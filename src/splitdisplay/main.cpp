// splitdisplay: turns physical displays into several native Windows monitors each.
//
//   splitdisplay run [--test SECONDS]   plug virtual monitors, take over the panels, composite;
//                                       without --test it keeps running and heals itself
//   splitdisplay stop                   ask a running instance to exit (it reverts on the way out)
//   splitdisplay revert                 give the panels back to the desktop, unplug virtual monitors
//   splitdisplay configure --panel NAME split only the display called NAME (config.ini)
//   splitdisplay autodetect             select a display if none is selected yet
//   splitdisplay autostart on|off       create/enable or disable the logon task
//   splitdisplay watchdog ...           (internal) guards a running compositor
//   splitdisplay                        (no arguments) open the settings window
//
// Any command accepts --panel "<monitor name prefix>" to work on that display only (not saved).
//
// Emergency exit while running: Ctrl+Alt+Shift+F12.

#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Display.h>
#include <winrt/Windows.Devices.Display.Core.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Devices.Display.Core.Interop.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "autostart.h"
#include "layout.h"
#include "log.h"
#include "names.h"
#include "panel.h"
#include "shm.h"
#include "topology.h"

namespace wdc = winrt::Windows::Devices::Display::Core;
namespace wdd = winrt::Windows::Devices::Display;
namespace wgd = winrt::Windows::Graphics::DirectX;

static std::atomic_bool g_stop = false;
static std::atomic_bool g_abort = false;   // end the session (a panel was lost, or a restart is needed)
static std::atomic_bool g_restart = false; // end the session and start a new one right away
static HANDLE g_stopEvent = nullptr;
static volatile LONG64* g_heartbeat = nullptr;
// Panel workers report here; the main thread only beats while every worker is alive.
static thread_local std::atomic<ULONGLONG>* t_workerBeat = nullptr;

static void Beat()
{
    if (t_workerBeat) *t_workerBeat = GetTickCount64();
    else if (g_heartbeat) InterlockedExchange64(g_heartbeat, (LONG64)GetTickCount64());
}

static bool StopRequested()
{
    if (!g_stop && g_stopEvent && WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) g_stop = true;
    return g_stop;
}

static bool ShouldExit()
{
    return StopRequested() || g_abort;
}

// Sleeps in small steps, keeping the heartbeat alive. Returns false if the session should end.
static bool Nap(DWORD ms)
{
    for (DWORD t = 0; t < ms; t += 100)
    {
        Beat();
        if (ShouldExit()) return false;
        Sleep(100);
    }
    return !ShouldExit();
}

static BOOL WINAPI OnCtrl(DWORD)
{
    g_stop = true;
    return TRUE;
}

static void EnableDebugPrivilege()
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp{ 1 };
    LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid);
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    CloseHandle(tok);
}

static std::wstring HeartbeatName(DWORD pid)
{
    return L"Local\\SplitDisplay.Hb." + std::to_wstring(pid);
}

static std::wstring VirtualName(size_t index)
{
    return L"Split " + std::to_wstring(index + 1);
}

static bool SameTarget(const PanelTarget& a, const PanelTarget& b)
{
    return a.adapter.LowPart == b.adapter.LowPart && a.adapter.HighPart == b.adapter.HighPart && a.targetId == b.targetId;
}

#pragma region Runtime state (read by the settings window)

static std::wstring RuntimePath()
{
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramData%\\SplitDisplay\\runtime.ini", p, MAX_PATH);
    return p;
}

struct PanelSession
{
    PanelConfig cfg;
    PanelTarget target;
    std::vector<RECT> regions; // on the panel
    UINT first = 0;            // index of its first virtual monitor
    int scale = 0;
    int refresh = 0;           // Hz for the panel and all of its split monitors
    std::vector<int> rates;    // rates the panel supports (offered on the split monitors)
};

static void WriteRuntime(const std::vector<PanelSession>& ps)
{
    auto path = RuntimePath();
    DeleteFileW(path.c_str());
    WritePrivateProfileStringW(L"Runtime", L"panels", std::to_wstring(ps.size()).c_str(), path.c_str());
    for (size_t i = 0; i < ps.size(); i++)
    {
        auto sec = L"Panel" + std::to_wstring(i + 1);
        auto& p = ps[i];
        auto put = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(sec.c_str(), k, v.c_str(), path.c_str()); };
        put(L"id", p.target.monitorDevicePath);
        put(L"x", std::to_wstring(p.target.position.x));
        put(L"y", std::to_wstring(p.target.position.y));
        put(L"w", std::to_wstring(p.target.width));
        put(L"h", std::to_wstring(p.target.height));
        put(L"first", std::to_wstring(p.first));
        put(L"count", std::to_wstring(p.regions.size()));
    }
}

static void ClearRuntime()
{
    DeleteFileW(RuntimePath().c_str());
}

#pragma endregion

#pragma region Panels

struct PanelScan
{
    std::vector<std::pair<PanelConfig, PanelTarget>> ready; // on the desktop, can be split
    std::vector<PanelTarget> stale;                          // still removed from the desktop by us
};

// Resolves the configured panels to connected targets. A name-only entry must match exactly one
// active display (a foldable over DP shows up as two monitors with the same name and is left
// alone); it is then remembered by device path.
static PanelScan ScanPanels()
{
    PanelScan scan;
    auto panels = SelectedPanels();
    bool fromConfig = !panels.empty() && LoadPanels().size() == panels.size();
    if (panels.empty())
    {
        auto guess = AutodetectPanel();
        if (!guess) return scan;
        panels = { ConfigFor(*guess) };
        SavePanels(panels);
        fromConfig = true;
        Log(L"no display selected, auto-detected '%s'", guess->friendlyName.c_str());
    }

    auto targets = EnumTargets();
    bool upgraded = false;
    for (auto& c : panels)
    {
        std::vector<PanelTarget> active;
        for (auto& t : targets)
        {
            if (!Matches(c, t)) continue;
            if (!t.active)
            {
                // Off the desktop: ours if it is still removed via the override (e.g. after a crash).
                if (GetSpecialization(t.adapter, t.targetId).enabled) scan.stale.push_back(t);
                continue;
            }
            active.push_back(t);
        }
        if (active.size() != 1 || !active[0].width || !active[0].height) continue;
        if (c.id.empty())
        {
            c.id = active[0].monitorDevicePath;
            upgraded = true;
        }
        bool dup = false;
        for (auto& r : scan.ready) dup |= SameTarget(r.second, active[0]);
        if (!dup) scan.ready.push_back({ c, active[0] });
    }
    if (upgraded && fromConfig) SavePanels(panels);
    return scan;
}

static std::vector<RECT> RegionsFor(const PanelConfig& c, UINT width, UINT height)
{
    LayoutNode layout;
    if (c.layout.empty() || !ParseLayout(c.layout, layout) || CountRegions(layout) < 1) layout = DefaultLayout((int)width, (int)height);
    std::vector<RECT> out;
    for (auto& r : ResolveLayout(layout, (int)width, (int)height)) out.push_back(r.rc);
    return out;
}

#pragma endregion

#pragma region Revert / watchdog

static bool TargetActive(LUID luid, UINT32 targetId)
{
    for (auto& t : EnumTargets())
        if (t.adapter.LowPart == luid.LowPart && t.adapter.HighPart == luid.HighPart && t.targetId == targetId) return t.active;
    return false;
}

static void UnplugVirtual()
{
    SharedView v;
    if (!v.Open()) return;
    InterlockedExchange(&v->desiredPlugged, 0);
    v.Kick();
    auto anyPlugged = [&] { return std::any_of(std::begin(v->mon), std::end(v->mon), [](auto& m) { return m.plugged != 0; }); };
    for (int i = 0; i < 50 && anyPlugged(); i++) Sleep(100);
    Log(L"revert: virtual monitors unplugged%s", anyPlugged() ? L" (some still plugged)" : L"");
}

// Idempotent: panels back on the desktop first, then remove the virtual monitors.
static void RevertTargets(const std::vector<std::pair<LUID, UINT32>>& targets)
{
    for (auto& [luid, id] : targets)
    {
        for (int i = 0; i < 10; i++)
        {
            LONG r = SetSpecialization(luid, id, false);
            if (r == ERROR_SUCCESS) Log(L"revert: panel %u returned to the desktop", id);
            if (r == ERROR_SUCCESS || r == ERROR_GEN_FAILURE) break; // GEN_FAILURE: nothing to undo
            Log(L"revert: SetSpecialization(false) -> %ld", r);
            Sleep(500);
        }
    }
    for (auto& [luid, id] : targets)
        for (int i = 0; i < 20 && !TargetActive(luid, id); i++) Sleep(250);

    UnplugVirtual();
    ClearRuntime();

    bool allActive = true;
    for (auto& [luid, id] : targets) allActive &= TargetActive(luid, id);
    if (!allActive)
    {
        Sleep(1000);
        allActive = true;
        for (auto& [luid, id] : targets) allActive &= TargetActive(luid, id);
        if (!allActive) ForceExtendTopology();
    }
    Log(L"revert: done, all panels active=%d", allActive);
}

static std::vector<std::pair<LUID, UINT32>> Keys(const std::vector<PanelTarget>& ts)
{
    std::vector<std::pair<LUID, UINT32>> out;
    for (auto& t : ts) out.push_back({ t.adapter, t.targetId });
    return out;
}

static int CmdRevert()
{
    std::vector<PanelTarget> ts;
    auto panels = SelectedPanels();
    for (auto& t : EnumTargets())
        for (auto& c : panels)
            if (Matches(c, t)) ts.push_back(t);
    RevertTargets(Keys(ts));
    return ts.empty() ? 1 : 0;
}

static int CmdStop()
{
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
    if (!e)
    {
        Log(L"stop: no running instance");
        return 1;
    }
    SetEvent(e);
    CloseHandle(e);
    Log(L"stop: signalled");
    return 0;
}

// watchdog <parentPid> <maxSeconds> <luidLow>:<luidHigh>:<targetId> ...
static int CmdWatchdog(int argc, wchar_t** argv)
{
    DWORD parentPid = wcstoul(argv[2], nullptr, 10);
    DWORD maxSeconds = wcstoul(argv[3], nullptr, 10);
    std::vector<std::pair<LUID, UINT32>> targets;
    for (int i = 4; i < argc; i++)
    {
        unsigned long lo = 0, id = 0;
        long hi = 0;
        if (swscanf_s(argv[i], L"%lu:%ld:%lu", &lo, &hi, &id) == 3) targets.push_back({ LUID{ lo, hi }, (UINT32)id });
    }

    HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, parentPid);
    const wchar_t* reason = L"compositor exited";
    if (parent)
    {
        volatile LONG64* hb = nullptr;
        ULONGLONG start = GetTickCount64();
        for (;;)
        {
            if (WaitForSingleObject(parent, 250) == WAIT_OBJECT_0) break;
            if (!hb)
            {
                HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, HeartbeatName(parentPid).c_str());
                if (map) hb = (volatile LONG64*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(LONG64));
            }
            ULONGLONG now = GetTickCount64();
            LONG64 last = hb ? *hb : 0;
            if (last && now - (ULONGLONG)last > 6000)
            {
                reason = L"heartbeat lost";
                TerminateProcess(parent, 0xDEAD);
                break;
            }
            if (maxSeconds && now - start > maxSeconds * 1000ULL)
            {
                reason = L"test time limit";
                TerminateProcess(parent, 0xDEAD);
                break;
            }
        }
        WaitForSingleObject(parent, 5000);
    }
    Log(L"watchdog: %s, reverting %zu panel(s)", reason, targets.size());
    RevertTargets(targets);
    return 0;
}

static void SpawnWatchdog(const std::vector<PanelTarget>& ts, DWORD maxSeconds)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" watchdog " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(maxSeconds);
    for (auto& t : ts)
        cmd += L" " + std::to_wstring(t.adapter.LowPart) + L":" + std::to_wstring(t.adapter.HighPart) + L":" + std::to_wstring(t.targetId);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi) &&
        !CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
        throw std::runtime_error("failed to start watchdog");
    Log(L"watchdog pid %lu armed for %zu panel(s) (limit %lus)", pi.dwProcessId, ts.size(), maxSeconds);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

#pragma endregion

#pragma region Frame source

// Consumer side of one virtual monitor's texture ring (protocol.h).
struct FrameSource
{
    int index = 0;
    LONG gen = -1;
    winrt::com_ptr<ID3D11Texture2D> tex[SD_BUFFERS];
    winrt::com_ptr<ID3D11Fence> fence;
    UINT width = 0, height = 0;
    LONG64 lastSeq = -1;
    bool reserved = false;

    void Drop()
    {
        for (auto& t : tex) t = nullptr;
        fence = nullptr;
        gen = -1;
    }

    bool Refresh(ID3D11Device5* dev, SdShared* s, HANDLE driverProc)
    {
        auto& m = s->mon[index];
        LONG g1 = m.handleGeneration;
        Drop();
        if ((g1 & 1) || m.fenceHandle == 0) return false;
        MemoryBarrier();

        auto open = [&](UINT64 remote, auto&& fn) -> bool {
            HANDLE h = nullptr;
            if (!DuplicateHandle(driverProc, (HANDLE)(ULONG_PTR)remote, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
            HRESULT hr = fn(h);
            CloseHandle(h);
            return SUCCEEDED(hr);
        };
        bool ok = true;
        for (int i = 0; i < SD_BUFFERS && ok; i++)
            ok = open(m.texHandle[i], [&](HANDLE h) { return dev->OpenSharedResource1(h, IID_PPV_ARGS(tex[i].put())); });
        if (ok) ok = open(m.fenceHandle, [&](HANDLE h) { return dev->OpenSharedFence(h, IID_PPV_ARGS(fence.put())); });

        MemoryBarrier();
        if (!ok || m.handleGeneration != g1)
        {
            Drop();
            return false;
        }
        width = m.width;
        height = m.height;
        gen = g1;
        Log(L"mon%d: attached ring gen=%ld %ux%u fmt=%u", index, gen, width, height, m.format);
        return true;
    }

    // Returns the buffer index now reserved for reading, or -1.
    int Acquire(SdShared* s, UINT64& fenceValue)
    {
        auto& m = s->mon[index];
        for (int attempt = 0; attempt < 3; attempt++)
        {
            LONG l = m.latest;
            if (l < 0 || l >= SD_BUFFERS) return -1;
            InterlockedExchange(&m.readerIndex, l);
            if (m.latest == l)
            {
                fenceValue = m.fenceValue[l];
                reserved = true;
                return l;
            }
            InterlockedExchange(&m.readerIndex, -1);
        }
        return -1;
    }

    void Release(SdShared* s)
    {
        if (!reserved) return;
        InterlockedExchange(&s->mon[index].readerIndex, -1);
        reserved = false;
    }
};

#pragma endregion

#pragma region Compositor

enum class Outcome
{
    Stop,        // user asked to stop, test finished, or another panel ended the session
    DisplaysOff, // Windows powered the virtual monitors off; panel released so it can sleep
    Lost,        // panel/GPU went away; caller reverts and retries
};

static wdc::DisplayTarget WaitForOwnableTarget(const wdc::DisplayManager& mgr, LUID luid, UINT32 targetId, int timeoutMs)
{
    for (int waited = 0; waited <= timeoutMs; waited += 250)
    {
        Beat();
        for (auto&& t : mgr.GetCurrentTargets())
        {
            auto id = t.Adapter().Id();
            if (id.LowPart == luid.LowPart && id.HighPart == luid.HighPart && t.AdapterRelativeId() == targetId && t.IsConnected() &&
                t.UsageKind() != wdd::DisplayMonitorUsageKind::Standard)
                return t;
        }
        Sleep(250);
    }
    return nullptr;
}

static std::atomic_bool g_hotkeyQuit = false;

static void HotkeyThread()
{
    if (!RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F12))
        Log(L"warning: could not register Ctrl+Alt+Shift+F12 (%lu)", GetLastError());
    while (!g_hotkeyQuit)
    {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_HOTKEY)
            {
                Log(L"emergency hotkey pressed");
                g_stop = true;
            }
        }
    }
    UnregisterHotKey(nullptr, 1);
}

static bool AnyVirtualSignal(SdShared* s, const PanelSession& p)
{
    for (UINT i = p.first; i < p.first + p.regions.size() && i < SD_MAX_MONITORS; i++)
        if (s->mon[i].fenceHandle != 0 && s->mon[i].latest >= 0) return true;
    return false;
}

// Drives one panel until the session ends. Runs on its own thread.
static Outcome Composite(SharedView& shm, const PanelSession& p, ULONGLONG deadline)
{
    const PanelTarget& panel = p.target;
    auto mgr = wdc::DisplayManager::Create(wdc::DisplayManagerOptions::None);
    auto target = WaitForOwnableTarget(mgr, panel.adapter, panel.targetId, 10000);
    if (!target)
    {
        Log(L"panel %u never became ownable", panel.targetId);
        return Outcome::Lost;
    }

    auto targets = winrt::single_threaded_vector<wdc::DisplayTarget>({ target });
    auto acq = mgr.TryAcquireTargetsAndCreateEmptyState(targets);
    if (acq.ErrorCode() != wdc::DisplayManagerResult::Success)
    {
        Log(L"acquire failed: result=%d hr=0x%08X", (int)acq.ErrorCode(), (UINT)acq.ExtendedErrorCode());
        return Outcome::Lost;
    }
    auto state = acq.State();
    auto path = state.ConnectTarget(target);
    path.IsInterlaced(false);
    path.Scaling(wdc::DisplayPathScaling::Identity);
    path.SourcePixelFormat(wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized);

    wdc::DisplayModeInfo best{ nullptr };
    double bestDiff = 1e9;
    for (auto&& m : path.FindModes(wdc::DisplayModeQueryOptions::OnlyPreferredResolution))
    {
        auto v = m.PresentationRate().VerticalSyncRate;
        double diff = std::abs((double)v.Numerator / v.Denominator - p.refresh);
        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = m;
        }
    }
    if (!best)
    {
        Log(L"no usable mode");
        mgr.ReleaseTarget(target);
        return Outcome::Lost;
    }
    path.ApplyPropertiesFromMode(best);
    auto applied = state.TryApply(wdc::DisplayStateApplyOptions::None);
    if (applied.Status() != wdc::DisplayStateOperationStatus::Success)
    {
        Log(L"TryApply failed: status=%d hr=0x%08X", (int)applied.Status(), (UINT)applied.ExtendedErrorCode());
        mgr.ReleaseTarget(target);
        return Outcome::Lost;
    }
    state = mgr.TryAcquireTargetsAndReadCurrentState(targets).State();
    path = state.GetPathForTarget(target);
    auto res = path.SourceResolution().Value();
    auto rate = path.PresentationRate().Value().VerticalSyncRate;
    Log(L"panel %u owned: %dx%d @ %.3f Hz", panel.targetId, res.Width, res.Height, (double)rate.Numerator / rate.Denominator);
    Beat();

    winrt::com_ptr<IDXGIFactory6> factory;
    factory.capture(&CreateDXGIFactory2, 0);
    winrt::com_ptr<IDXGIAdapter4> adapter;
    adapter.capture(factory, &IDXGIFactory6::EnumAdapterByLuid, panel.adapter);
    winrt::com_ptr<ID3D11Device> d0;
    winrt::com_ptr<ID3D11DeviceContext> c0;
    winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d0.put(), nullptr, c0.put()));
    auto dev = d0.as<ID3D11Device5>();
    auto ctx = c0.as<ID3D11DeviceContext4>();
    winrt::com_ptr<ID3D11Fence> fence;
    fence.capture(dev, &ID3D11Device5::CreateFence, 0, D3D11_FENCE_FLAG_SHARED);
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto device = mgr.CreateDisplayDevice(target.Adapter());
    auto interop = device.as<IDisplayDeviceInterop>();
    auto source = device.CreateScanoutSource(target);
    auto pool = device.CreateTaskPool();
    winrt::Windows::Graphics::DirectX::Direct3D11::Direct3DMultisampleDescription ms{ 1, 0 };
    wdc::DisplayPrimaryDescription desc{ (uint32_t)res.Width, (uint32_t)res.Height, path.SourcePixelFormat(), wgd::DirectXColorSpace::RgbFullG22NoneP709, false, ms };

    wdc::DisplaySurface prim[2] = { device.CreatePrimary(target, desc), device.CreatePrimary(target, desc) };
    wdc::DisplayScanout scan[2] = { device.CreateSimpleScanout(source, prim[0], 0, 1), device.CreateSimpleScanout(source, prim[1], 0, 1) };
    winrt::com_ptr<ID3D11Texture2D> primTex[2];
    winrt::com_ptr<ID3D11RenderTargetView> primRtv[2];
    for (int i = 0; i < 2; i++)
    {
        winrt::handle h;
        winrt::check_hresult(interop->CreateSharedHandle(prim[i].as<::IInspectable>().get(), nullptr, GENERIC_ALL, nullptr, h.put()));
        primTex[i].capture(dev, &ID3D11Device5::OpenSharedResource1, h.get());
        winrt::check_hresult(dev->CreateRenderTargetView(primTex[i].get(), nullptr, primRtv[i].put()));
    }
    wdc::DisplayFence displayFence{ nullptr };
    {
        winrt::handle h;
        winrt::check_hresult(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, h.put()));
        winrt::com_ptr<::IInspectable> f;
        f.capture(interop, &IDisplayDeviceInterop::OpenSharedHandle, h.get());
        displayFence = f.as<wdc::DisplayFence>();
    }

    EnableDebugPrivilege();
    HANDLE driverProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, shm->driverPid);
    if (!driverProc)
    {
        Log(L"cannot open driver host process %lu: %lu", shm->driverPid, GetLastError());
        CloseHandle(fenceEvent);
        mgr.ReleaseTarget(target);
        return Outcome::Lost;
    }

    std::vector<FrameSource> src(p.regions.size());
    for (int i = 0; i < (int)src.size(); i++)
    {
        src[i].index = (int)p.first + i;
        LUID a = shm->mon[src[i].index].adapter;
        if ((a.LowPart || a.HighPart) && (a.LowPart != panel.adapter.LowPart || a.HighPart != panel.adapter.HighPart))
            Log(L"warning: virtual monitor %d renders on GPU %08X:%08X but the panel is on %08X:%08X; cross-GPU copy is not supported, it will stay black",
                src[i].index, a.HighPart, a.LowPart, panel.adapter.HighPart, panel.adapter.LowPart);
    }
    const float black[4] = { 0, 0, 0, 1 };

    LARGE_INTEGER freq, t0, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    UINT64 vblanks = 0, presents = 0, stalls = 0;
    UINT64 pendingFence = 0; // our copies that still hold ring reservations
    bool presented = false;
    ULONGLONG darkSince = 0;
    bool poweredOff = false;
    Outcome outcome = Outcome::Stop;

    while (!ShouldExit())
    {
        Beat();
        device.WaitForVBlank(source);
        auto status = source.Status();
        if (status == wdc::DisplaySourceStatus::PoweredOff)
        {
            // Windows blanked all displays (idle timeout / sleep). The session stays intact;
            // everything is repainted once power returns.
            if (!poweredOff) Log(L"panel %u powered off by Windows, waiting", panel.targetId);
            poweredOff = true;
            presented = false;
            darkSince = 0;
            if (!Nap(100)) break;
            if (deadline && GetTickCount64() >= deadline) break;
            continue;
        }
        if (poweredOff && status == wdc::DisplaySourceStatus::Active)
        {
            Log(L"panel %u powered on again", panel.targetId);
            poweredOff = false;
        }
        if (status != wdc::DisplaySourceStatus::Active)
        {
            Log(L"panel %u source status %d", panel.targetId, static_cast<int>(status));
            outcome = Outcome::Lost;
            break;
        }
        vblanks++;

        // Copies issued last frame are almost always done by now; only wait if they are not.
        if (pendingFence && fence->GetCompletedValue() < pendingFence)
        {
            stalls++;
            fence->SetEventOnCompletion(pendingFence, fenceEvent);
            WaitForSingleObject(fenceEvent, 100);
        }
        for (auto& s : src) s.Release(shm.get());
        pendingFence = 0;

        // Windows turned this panel's virtual monitors off: let the panel lose signal and sleep too.
        if (!AnyVirtualSignal(shm.get(), p))
        {
            if (!darkSince) darkSince = GetTickCount64();
            if (GetTickCount64() - darkSince > 2000)
            {
                outcome = Outcome::DisplaysOff;
                break;
            }
        }
        else
        {
            darkSince = 0;
        }

        bool changed = !presented;
        for (auto& s : src)
        {
            auto& m = shm->mon[s.index];
            if (s.gen != m.handleGeneration || m.frameSeq != s.lastSeq) changed = true;
        }

        if (changed)
        {
            int back = (int)(presents & 1);
            for (size_t k = 0; k < src.size(); k++)
            {
                auto& s = src[k];
                auto& m = shm->mon[s.index];
                if (s.gen != m.handleGeneration) s.Refresh(dev.get(), shm.get(), driverProc);

                // Region on the panel, clipped to the scanout size.
                RECT rc = p.regions[k];
                rc.right = std::min<LONG>(rc.right, res.Width);
                rc.bottom = std::min<LONG>(rc.bottom, res.Height);
                if (rc.right <= rc.left || rc.bottom <= rc.top) continue;
                D3D11_RECT area{ rc.left, rc.top, rc.right, rc.bottom };
                LONG64 seq = m.frameSeq;
                UINT64 fv = 0;
                int idx = s.gen >= 0 ? s.Acquire(shm.get(), fv) : -1;
                if (idx < 0)
                {
                    ctx->ClearView(primRtv[back].get(), black, &area, 1);
                    s.lastSeq = seq;
                    continue;
                }
                s.lastSeq = seq;
                ctx->Wait(s.fence.get(), fv);
                D3D11_BOX box{ 0, 0, 0, std::min<UINT>(s.width, (UINT)(rc.right - rc.left)), std::min<UINT>(s.height, (UINT)(rc.bottom - rc.top)), 1 };
                ctx->CopySubresourceRegion(primTex[back].get(), 0, (UINT)rc.left, (UINT)rc.top, 0, s.tex[idx].get(), 0, &box);
            }
            ctx->Signal(fence.get(), ++fenceValue);
            ctx->Flush();
            pendingFence = fenceValue;

            auto task = pool.CreateTask();
            task.SetScanout(scan[back]);
            task.SetWait(displayFence, fenceValue);
            pool.ExecuteTask(task);
            presents++;
            presented = true;
        }

        if (deadline && GetTickCount64() >= deadline) break;
    }

    if (pendingFence)
    {
        fence->SetEventOnCompletion(pendingFence, fenceEvent);
        WaitForSingleObject(fenceEvent, 250);
    }
    for (auto& s : src)
    {
        s.Release(shm.get());
        s.Drop();
    }

    QueryPerformanceCounter(&now);
    double total = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
    Log(L"panel %u composited %.1fs: %llu vblanks (%.2f Hz), %llu presents, %llu copy stalls", panel.targetId, total, vblanks,
        vblanks / std::max(total, 0.001), presents, stalls);

    CloseHandle(driverProc);
    CloseHandle(fenceEvent);
    try
    {
        mgr.ReleaseTarget(target);
    }
    catch (...)
    {
    }
    return outcome;
}

// One panel's thread: composite, sit out display power-off, repeat.
static Outcome PanelWorker(SharedView& shm, const PanelSession& p, ULONGLONG deadline)
{
    try
    {
        winrt::init_apartment();
        for (;;)
        {
            Outcome o = Composite(shm, p, deadline);
            if (o != Outcome::DisplaysOff) return o;

            // Panel released (no signal -> it sleeps). Resume on the first new frame.
            Log(L"panel %u: virtual displays off, panel released", p.target.targetId);
            while (!AnyVirtualSignal(shm.get(), p))
            {
                if (!Nap(200)) return Outcome::Stop;
                if (deadline && GetTickCount64() >= deadline) return Outcome::Stop;
            }
            Log(L"panel %u: virtual displays back on", p.target.targetId);
        }
    }
    catch (winrt::hresult_error const& e)
    {
        Log(L"panel %u: WinRT error 0x%08X: %s", p.target.targetId, (UINT)e.code(), e.message().c_str());
    }
    catch (std::exception const& e)
    {
        Log(L"panel %u: error: %S", p.target.targetId, e.what());
    }
    return Outcome::Lost;
}

// One full split session: plug every panel's monitors, take the panels over, composite until
// stop / loss. Returns true if the session ended because a stop was requested.
static bool RunSession(std::vector<PanelSession>& ps, ULONGLONG deadline)
{
    SharedView shm;
    if (!shm.Open()) throw std::runtime_error("SplitDisplay driver not loaded");

    // Start from a clean slate: monitors left plugged by an earlier session would keep old sizes.
    if (shm->desiredPlugged)
    {
        InterlockedExchange(&shm->desiredPlugged, 0);
        shm.Kick();
        for (int i = 0; i < 50 && std::any_of(std::begin(shm->mon), std::end(shm->mon), [](auto& m) { return m.plugged != 0; }); i++)
            if (!Nap(100)) return true;
    }

    // Where every other display sits now, so they can be put back after the desktop changes.
    std::vector<PlacedDisplay> others;
    for (auto& t : EnumTargets())
    {
        bool mine = false;
        for (auto& p : ps) mine |= SameTarget(p.target, t);
        if (t.active && !t.isVirtual && !mine) others.push_back({ t.monitorDevicePath, t.position });
    }

    SdConfig cfg{};
    cfg.renderAdapter = ps[0].target.adapter;
    std::vector<RECT> desktop; // where each virtual monitor goes on the Windows desktop
    for (auto& p : ps)
    {
        p.first = cfg.count;
        std::wstring desc;
        for (auto& r : p.regions)
        {
            SdMonitorConfig m{ (UINT32)(r.right - r.left), (UINT32)(r.bottom - r.top), (UINT32)p.refresh, 0, {} };
            for (int hz : p.rates)
                if (m.rateCount < SD_MAX_RATES) m.rates[m.rateCount++] = (UINT32)hz;
            cfg.mon[cfg.count++] = m;
            desktop.push_back({ p.target.position.x + r.left, p.target.position.y + r.top, p.target.position.x + r.right, p.target.position.y + r.bottom });
            desc += L" " + std::to_wstring(r.right - r.left) + L"x" + std::to_wstring(r.bottom - r.top) + L"@" + std::to_wstring(r.left) + L"," +
                    std::to_wstring(r.top);
        }
        std::wstring rates;
        for (int hz : p.rates) rates += (rates.empty() ? L"" : L"/") + std::to_wstring(hz);
        Log(L"panel '%s' %ux%u@%d Hz (supports %s) at (%ld,%ld), scale %d%%, monitors %u..%u:%s", p.cfg.name.c_str(), p.target.width,
            p.target.height, p.refresh, rates.empty() ? L"?" : rates.c_str(), p.target.position.x, p.target.position.y, p.scale, p.first + 1,
            cfg.count, desc.c_str());
    }

    // Counters survive from earlier sessions, so require fresh frames after this plug.
    LONG64 base[SD_MAX_MONITORS];
    for (int i = 0; i < SD_MAX_MONITORS; i++) base[i] = shm->mon[i].frameSeq;
    shm->config = cfg;
    MemoryBarrier();
    InterlockedExchange(&shm->desiredPlugged, 1);
    shm.Kick();
    auto live = [&] {
        for (UINT i = 0; i < cfg.count; i++)
            if (!shm->mon[i].plugged || shm->mon[i].frameSeq <= base[i] || !IsTargetActive(VirtualName(i).c_str())) return false;
        return true;
    };
    std::vector<std::wstring> allNames;
    for (UINT i = 0; i < cfg.count; i++) allNames.push_back(VirtualName(i));
    for (int i = 0; i < 100 && !live(); i++)
    {
        // Plugged but left off the desktop (a saved topology): switch them on ourselves,
        // and as a last resort extend the desktop to every connected display.
        if (i == 15) ActivateMonitors(allNames);
        if (i == 60) ForceExtendTopology();
        if (!Nap(100)) return true;
    }
    if (!live()) throw std::runtime_error("virtual monitors did not come up");
    Log(L"%u virtual monitors live", cfg.count);
    for (auto& p : ps)
    {
        std::vector<std::wstring> names;
        for (UINT i = 0; i < p.regions.size(); i++) names.push_back(VirtualName(p.first + i));
        if (p.scale) SetScalePercent(names, p.scale);
        // Windows restores the rate it last saw on each monitor; make them all the panel's rate, so a
        // later difference can only be a user change (which then applies to the whole panel).
        ForceRefresh(names, p.refresh);
    }
    Beat();

    for (auto& p : ps)
    {
        LONG r = SetSpecialization(p.target.adapter, p.target.targetId, true);
        Log(L"panel '%s' removed from desktop -> %ld", p.cfg.name.c_str(), r);
        if (r != ERROR_SUCCESS) throw std::runtime_error("could not remove a panel from the desktop");
    }
    for (auto& p : ps)
        for (int i = 0; i < 40 && TargetActive(p.target.adapter, p.target.targetId); i++)
            if (!Nap(250)) return true;
    // The split monitors take each panel's place in the desktop; everything else stays put.
    for (int i = 0; i < 10 && !ArrangeMonitors(desktop, others); i++)
        if (!Nap(300)) return true;
    WriteRuntime(ps);

    // One thread per panel, each paced by its own panel's vblank.
    g_abort = false;
    std::vector<std::unique_ptr<std::atomic<ULONGLONG>>> beats;
    std::vector<Outcome> results(ps.size(), Outcome::Stop);
    std::vector<std::thread> workers;
    for (size_t i = 0; i < ps.size(); i++)
    {
        beats.push_back(std::make_unique<std::atomic<ULONGLONG>>(GetTickCount64()));
        workers.emplace_back([&, i] {
            t_workerBeat = beats[i].get();
            results[i] = PanelWorker(shm, ps[i], deadline);
            *beats[i] = ULLONG_MAX; // finished: never counts as stale
            if (results[i] == Outcome::Lost) g_abort = true;
        });
    }
    // Beat only while every worker is alive, so a hung panel thread trips the watchdog.
    // Every 2 s, glue each panel's monitors back together if Windows moved them apart.
    ULONGLONG nextAlign = GetTickCount64() + 2000;
    int corrections = 0;
    ULONGLONG correctionWindow = 0;
    for (;;)
    {
        if (GetTickCount64() >= nextAlign)
        {
            nextAlign = GetTickCount64() + 2000;
            if (GetTickCount64() - correctionWindow > 60000)
            {
                correctionWindow = GetTickCount64();
                corrections = 0;
            }
            // If Windows keeps undoing it, stop fighting for this minute.
            for (auto& p : ps)
                if (corrections < 5 && KeepGroupAligned(p.first, p.regions)) corrections++;

            // A split monitor's refresh rate was changed (e.g. in Settings): the whole panel follows.
            for (auto& p : ps)
            {
                if (g_restart) break;
                for (UINT i = 0; i < p.regions.size(); i++)
                {
                    int hz = CurrentRefreshOf(VirtualName(p.first + i));
                    if (!hz || std::abs(hz - p.refresh) <= 1) continue;
                    Log(L"'%s' split monitor %u switched to %d Hz: switching the whole display from %d Hz", p.cfg.name.c_str(), p.first + i + 1, hz,
                        p.refresh);
                    auto panels = LoadPanels();
                    for (auto& c : panels)
                        if (Matches(c, p.target)) c.refresh = hz;
                    SavePanels(panels);
                    g_restart = true;
                    g_abort = true;
                    break;
                }
            }
        }

        bool allDone = true, allFresh = true;
        ULONGLONG t = GetTickCount64();
        for (auto& b : beats)
        {
            ULONGLONG v = *b;
            if (v != ULLONG_MAX) allDone = false;
            if (v != ULLONG_MAX && t > v && t - v > 3000) allFresh = false;
        }
        if (allDone) break;
        if (allFresh) Beat();
        if (StopRequested()) g_abort = g_abort.load(); // workers see the stop themselves
        Sleep(100);
    }
    for (auto& w : workers) w.join();
    if (g_restart) return false; // caller reverts and starts over without counting a failure
    for (auto r : results)
        if (r == Outcome::Lost) return false;
    return true;
}

static int CmdRun(DWORD testSeconds)
{
    HANDLE instance = CreateMutexW(nullptr, TRUE, kInstanceMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        Log(L"already running");
        return 1;
    }
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);

    HANDLE hbMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG64), HeartbeatName(GetCurrentProcessId()).c_str());
    g_heartbeat = (volatile LONG64*)MapViewOfFile(hbMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG64));
    Beat();

    std::thread hotkeys(HotkeyThread);
    winrt::init_apartment();

    ULONGLONG deadline = testSeconds ? GetTickCount64() + testSeconds * 1000ULL : 0;
    bool watchdogArmed = false;
    int failures = 0;
    ULONGLONG firstFailure = 0;

    while (!StopRequested())
    {
        auto scan = ScanPanels();
        if (!scan.stale.empty())
        {
            Log(L"%zu panel(s) found off the desktop (stale state), restoring them first", scan.stale.size());
            RevertTargets(Keys(scan.stale));
            if (!Nap(1000)) break;
            continue;
        }
        if (scan.ready.empty())
        {
            // Nothing selected is connected (or it shows up as separate DP monitors): nothing to do.
            if (!Nap(2000)) break;
            continue;
        }

        SharedView probe;
        if (!probe.Open())
        {
            Log(L"driver not loaded, waiting");
            if (!Nap(5000)) break;
            continue;
        }
        probe.Close();

        // All split panels must share one GPU (the virtual monitors render on a single adapter),
        // and together they may use at most SD_MAX_MONITORS virtual monitors.
        std::vector<PanelSession> ps;
        UINT used = 0;
        LUID gpu = scan.ready[0].second.adapter;
        for (auto& [cfg, t] : scan.ready)
        {
            if (t.adapter.LowPart != gpu.LowPart || t.adapter.HighPart != gpu.HighPart)
            {
                Log(L"skipping '%s': it is on a different GPU than '%s'", cfg.name.c_str(), scan.ready[0].first.name.c_str());
                continue;
            }
            auto regions = RegionsFor(cfg, t.width, t.height);
            if (used + regions.size() > SD_MAX_MONITORS)
            {
                Log(L"skipping '%s': more than %d split monitors in total", cfg.name.c_str(), SD_MAX_MONITORS);
                continue;
            }
            used += (UINT)regions.size();
            auto rates = SupportedRefreshRates(t);
            int want = cfg.refresh ? cfg.refresh : (int)std::lround(t.refreshHz);
            ps.push_back({ cfg, t, regions, 0, GetScalePercentForTarget(t.adapter, t.targetId), ClosestRate(rates, want), rates });
        }
        std::vector<PanelTarget> sessionTargets;
        for (auto& p : ps) sessionTargets.push_back(p.target);

        if (!watchdogArmed)
        {
            SpawnWatchdog(sessionTargets, testSeconds ? testSeconds + 30 : 0);
            watchdogArmed = true;
        }

        bool stopped = false;
        try
        {
            stopped = RunSession(ps, deadline);
        }
        catch (winrt::hresult_error const& e)
        {
            Log(L"WinRT error 0x%08X: %s", (UINT)e.code(), e.message().c_str());
        }
        catch (std::exception const& e)
        {
            Log(L"error: %S", e.what());
        }
        RevertTargets(Keys(sessionTargets));
        if (stopped || StopRequested() || (deadline && GetTickCount64() >= deadline)) break;
        if (g_restart)
        {
            g_restart = false;
            Log(L"restarting the split with the new settings");
            continue;
        }

        // Back off on repeated failures so a persistent problem cannot flicker the screen forever.
        ULONGLONG t = GetTickCount64();
        if (!firstFailure || t - firstFailure > 120000)
        {
            firstFailure = t;
            failures = 0;
        }
        if (++failures >= 3)
        {
            Log(L"3 failures within 2 minutes, giving up; the displays stay unsplit");
            break;
        }
        Log(L"session lost, retrying in 3s");
        if (!Nap(3000)) break;
    }

    g_hotkeyQuit = true;
    hotkeys.join();
    Log(L"exit");
    CloseHandle(instance);
    return 0;
}

#pragma endregion

int RunGui();

int wmain(int argc, wchar_t** argv)
{
    LogInit(L"splitdisplay.log");
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    ParsePanelOption(argc, argv);
    if (argc == 1) return RunGui();
    std::wstring cmd = argv[1];

    if (cmd == L"configure")
    {
        // Split only the named display; remembered by device path when the name is unambiguous.
        auto sel = SelectedPanels();
        if (sel.empty())
        {
            Log(L"configure: pass --panel NAME");
            return 1;
        }
        std::vector<PanelTarget> hits;
        for (auto& t : EnumTargets())
            if (Matches(sel[0], t)) hits.push_back(t);
        PanelConfig c = hits.size() == 1 ? ConfigFor(hits[0]) : sel[0];
        c.layout = sel[0].layout;
        SavePanels({ c });
        Log(L"configure: split '%s' %s", c.name.c_str(), c.id.empty() ? L"(by name)" : c.id.c_str());
        return 0;
    }
    if (cmd == L"autodetect")
    {
        // Keeps an existing selection; otherwise picks the only tall display, or the only display.
        if (!LoadPanels().empty())
        {
            Log(L"autodetect: keeping the existing selection");
            return 0;
        }
        auto t = AutodetectPanel();
        if (!t)
        {
            Log(L"autodetect: several displays, choose in the settings window");
            return 2;
        }
        SavePanels({ ConfigFor(*t) });
        Log(L"autodetect: selected '%s' (%s)", t->friendlyName.c_str(), t->monitorDevicePath.c_str());
        return 0;
    }
    if (cmd == L"autostart" && argc > 2)
    {
        bool on = _wcsicmp(argv[2], L"on") == 0;
        return SetAutostart(on) ? 0 : 1;
    }
    if (cmd == L"run")
    {
        DWORD test = 0;
        if (argc > 3 && std::wstring(argv[2]) == L"--test") test = (DWORD)_wtoi(argv[3]);
        return CmdRun(test);
    }
    if (cmd == L"stop") return CmdStop();
    if (cmd == L"revert") return CmdRevert();
    if (cmd == L"watchdog" && argc >= 4) return CmdWatchdog(argc, argv);
    Log(L"usage: splitdisplay [--panel NAME] run [--test SECONDS] | stop | revert | configure | autodetect | autostart on|off");
    return 1;
}
