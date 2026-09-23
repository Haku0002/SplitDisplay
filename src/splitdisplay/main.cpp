// splitdisplay: turns the single 2560x2880 Sculptor panel into two native Windows monitors.
//
//   splitdisplay run [--test SECONDS]   plug virtual monitors, take over the panel, composite;
//                                       without --test it keeps running and heals itself
//   splitdisplay stop                   ask a running instance to exit (it reverts on the way out)
//   splitdisplay revert                 give the panel back to the desktop, unplug virtual monitors
//   splitdisplay configure --panel NAME save NAME as the display to split (config.ini)
//   splitdisplay autodetect             select a display if none is selected yet
//   splitdisplay autostart on|off       create/enable or disable the logon task
//   splitdisplay watchdog ...           (internal) guards a running compositor
//   splitdisplay                        (no arguments) open the settings window
//
// Any command accepts --panel "<monitor name prefix>"; otherwise the selection in config.ini.
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
static HANDLE g_stopEvent = nullptr;
static volatile LONG64* g_heartbeat = nullptr;

static void Beat()
{
    if (g_heartbeat) InterlockedExchange64(g_heartbeat, (LONG64)GetTickCount64());
}

static bool StopRequested()
{
    if (!g_stop && g_stopEvent && WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) g_stop = true;
    return g_stop;
}

// Sleeps in small steps, keeping the heartbeat alive. Returns false if a stop was requested.
static bool Nap(DWORD ms)
{
    for (DWORD t = 0; t < ms; t += 100)
    {
        Beat();
        if (StopRequested()) return false;
        Sleep(100);
    }
    return !StopRequested();
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

// The display to split: the selected monitor (by device path). With only a name selected, it must
// match exactly one connected monitor (a foldable over DP shows up as two monitors with the same
// name and is left alone); that match is then remembered by device path.
static std::optional<PanelTarget> FindCombinedPanel()
{
    if (!HasPanelSelection())
    {
        auto guess = AutodetectPanel();
        if (!guess) return std::nullopt;
        SelectPanel(*guess);
        Log(L"no display selected, auto-detected '%s' (%s)", guess->friendlyName.c_str(), guess->monitorDevicePath.c_str());
    }
    std::vector<PanelTarget> active;
    for (auto& t : EnumTargets())
    {
        if (!MatchesPanel(t)) continue;
        if (!t.active)
        {
            // Off the desktop: only ours if it is still removed via the override (e.g. left behind by a crash).
            if (GetSpecialization(t.adapter, t.targetId).enabled) return t;
            continue;
        }
        active.push_back(t);
    }
    if (active.size() != 1 || !active[0].width || !active[0].height) return std::nullopt;
    if (PanelId().empty()) SelectPanel(active[0]);
    return active[0];
}

// The configured layout resolved for this panel (default: two halves along the long side).
static std::vector<RECT> CurrentRegions(UINT width, UINT height)
{
    LayoutNode layout;
    std::wstring text = LoadConfigValue(L"layout");
    if (text.empty() || !ParseLayout(text, layout) || CountRegions(layout) < 1) layout = DefaultLayout((int)width, (int)height);
    std::vector<RECT> out;
    for (auto& r : ResolveLayout(layout, (int)width, (int)height)) out.push_back(r.rc);
    return out;
}

static std::wstring VirtualName(size_t index)
{
    return L"Split " + std::to_wstring(index + 1);
}

#pragma region Revert / watchdog

static bool PanelActive(LUID luid, UINT32 targetId)
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
    for (int i = 0; i < 50 && (v->mon[0].plugged || v->mon[1].plugged); i++) Sleep(100);
    Log(L"revert: virtual monitors unplugged (%ld,%ld)", v->mon[0].plugged, v->mon[1].plugged);
}

// Idempotent: panel back on the desktop first, then remove the virtual monitors.
static void RevertAll(LUID luid, UINT32 targetId)
{
    for (int i = 0; i < 10; i++)
    {
        LONG r = SetSpecialization(luid, targetId, false);
        if (r == ERROR_SUCCESS) Log(L"revert: panel returned to the desktop");
        if (r == ERROR_SUCCESS || r == ERROR_GEN_FAILURE) break; // GEN_FAILURE: nothing to undo
        Log(L"revert: SetSpecialization(false) -> %ld", r);
        Sleep(500);
    }
    for (int i = 0; i < 20 && !PanelActive(luid, targetId); i++) Sleep(250);

    UnplugVirtual();

    if (!PanelActive(luid, targetId))
    {
        Sleep(1000);
        if (!PanelActive(luid, targetId)) ForceExtendTopology();
    }
    Log(L"revert: panel active=%d", PanelActive(luid, targetId));
}

static int CmdRevert()
{
    bool any = false;
    for (auto& t : EnumTargets())
    {
        if (!MatchesPanel(t)) continue;
        any = true;
        RevertAll(t.adapter, t.targetId);
    }
    if (!any)
    {
        Log(L"revert: selected display not found");
        UnplugVirtual();
        return 1;
    }
    return 0;
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

static int CmdWatchdog(DWORD parentPid, LUID luid, UINT32 targetId, DWORD maxSeconds)
{
    HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, parentPid);
    if (!parent)
    {
        RevertAll(luid, targetId);
        return 1;
    }

    volatile LONG64* hb = nullptr;
    HANDLE map = nullptr;
    ULONGLONG start = GetTickCount64();
    const wchar_t* reason = L"compositor exited";

    for (;;)
    {
        if (WaitForSingleObject(parent, 250) == WAIT_OBJECT_0) break;
        if (!hb)
        {
            map = OpenFileMappingW(FILE_MAP_READ, FALSE, HeartbeatName(parentPid).c_str());
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
    Log(L"watchdog: %s, reverting", reason);
    RevertAll(luid, targetId);
    return 0;
}

static void SpawnWatchdog(const PanelTarget& p, DWORD maxSeconds)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t cmd[512];
    swprintf_s(cmd, L"\"%s\" watchdog %lu %lu %ld %u %lu", exe, GetCurrentProcessId(), p.adapter.LowPart, p.adapter.HighPart, p.targetId, maxSeconds);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi) &&
        !CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
        throw std::runtime_error("failed to start watchdog");
    Log(L"watchdog pid %lu armed (limit %lus)", pi.dwProcessId, maxSeconds);
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
    Stop,        // user asked to stop or test finished
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

static bool AnyVirtualSignal(SdShared* s)
{
    for (UINT i = 0; i < s->config.count && i < SD_MAX_MONITORS; i++)
        if (s->mon[i].fenceHandle != 0 && s->mon[i].latest >= 0) return true;
    return false;
}

static Outcome Composite(SharedView& shm, const PanelTarget& panel, const std::vector<RECT>& regions, ULONGLONG deadline)
{
    auto mgr = wdc::DisplayManager::Create(wdc::DisplayManagerOptions::None);
    auto target = WaitForOwnableTarget(mgr, panel.adapter, panel.targetId, 10000);
    if (!target)
    {
        Log(L"panel never became ownable");
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
        double diff = std::abs((double)v.Numerator / v.Denominator - panel.refreshHz);
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
    Log(L"panel owned: %dx%d @ %.3f Hz", res.Width, res.Height, (double)rate.Numerator / rate.Denominator);
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

    std::vector<FrameSource> src(regions.size());
    for (int i = 0; i < (int)src.size(); i++)
    {
        src[i].index = i;
        LUID a = shm->mon[i].adapter;
        if ((a.LowPart || a.HighPart) && (a.LowPart != panel.adapter.LowPart || a.HighPart != panel.adapter.HighPart))
            Log(L"warning: virtual monitor %d renders on GPU %08X:%08X but the panel is on %08X:%08X; cross-GPU copy is not supported, it will stay black",
                i, a.HighPart, a.LowPart, panel.adapter.HighPart, panel.adapter.LowPart);
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

    while (!StopRequested())
    {
        Beat();
        device.WaitForVBlank(source);
        auto status = source.Status();
        if (status == wdc::DisplaySourceStatus::PoweredOff)
        {
            // Windows blanked all displays (idle timeout / sleep). The session stays intact;
            // everything is repainted once power returns.
            if (!poweredOff) Log(L"panel powered off by Windows, waiting");
            poweredOff = true;
            presented = false;
            darkSince = 0;
            if (!Nap(100)) break;
            if (deadline && GetTickCount64() >= deadline) break;
            continue;
        }
        if (poweredOff && status == wdc::DisplaySourceStatus::Active)
        {
            Log(L"panel powered on again");
            poweredOff = false;
        }
        if (status != wdc::DisplaySourceStatus::Active)
        {
            Log(L"panel source status %d", static_cast<int>(status));
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

        // Windows turned the (virtual) displays off: let the panel lose signal and sleep too.
        if (!AnyVirtualSignal(shm.get()))
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
            for (auto& s : src)
            {
                auto& m = shm->mon[s.index];
                if (s.gen != m.handleGeneration) s.Refresh(dev.get(), shm.get(), driverProc);

                // Region on the panel, clipped to the scanout size.
                RECT rc = regions[s.index];
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
    Log(L"composited %.1fs: %llu vblanks (%.2f Hz), %llu presents, %llu copy stalls", total, vblanks, vblanks / std::max(total, 0.001), presents, stalls);

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

// One full split session: plug, take over, composite until stop / loss.
// Returns true if the session ended because a stop was requested.
static bool RunSession(const PanelTarget& panel, int scale, ULONGLONG deadline)
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
        if (t.active && !t.isVirtual && !MatchesPanel(t)) others.push_back({ t.monitorDevicePath, t.position });

    auto regions = CurrentRegions(panel.width, panel.height);
    SdConfig cfg{};
    cfg.count = (UINT32)regions.size();
    cfg.refreshHz = (UINT32)std::lround(panel.refreshHz);
    cfg.renderAdapter = panel.adapter;
    std::wstring desc;
    for (size_t i = 0; i < regions.size(); i++)
    {
        cfg.size[i] = { (UINT32)(regions[i].right - regions[i].left), (UINT32)(regions[i].bottom - regions[i].top) };
        desc += L" " + std::to_wstring(cfg.size[i].width) + L"x" + std::to_wstring(cfg.size[i].height) + L"@" +
                std::to_wstring(regions[i].left) + L"," + std::to_wstring(regions[i].top);
    }
    Log(L"layout: %u regions:%s", cfg.count, desc.c_str());

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
    for (int i = 0; i < 100 && !live(); i++)
        if (!Nap(100)) return true;
    if (!live()) throw std::runtime_error("virtual monitors did not come up");
    Log(L"virtual monitors live");
    if (scale) SetScalePercentForMonitors(L"Split ", scale);
    Beat();

    LONG r = SetSpecialization(panel.adapter, panel.targetId, true);
    Log(L"panel removed from desktop -> %ld", r);
    if (r != ERROR_SUCCESS) throw std::runtime_error("could not remove panel from desktop");
    for (int i = 0; i < 40 && PanelActive(panel.adapter, panel.targetId); i++)
        if (!Nap(250)) return true;
    // The split monitors take the panel's place in the desktop; everything else stays put.
    for (int i = 0; i < 10 && !ArrangeRegions(regions, panel.position, others); i++)
        if (!Nap(300)) return true;

    for (;;)
    {
        Outcome o = Composite(shm, panel, regions, deadline);
        if (o == Outcome::Stop) return true;
        if (o == Outcome::Lost) return false;

        // DisplaysOff: panel released (no signal -> it sleeps). Resume on the first new frame.
        Log(L"virtual displays off, panel released");
        while (!AnyVirtualSignal(shm.get()))
        {
            if (!Nap(200)) return true;
            if (deadline && GetTickCount64() >= deadline) return true;
        }
        Log(L"virtual displays back on");
    }
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
    int scale = 0;
    int failures = 0;
    ULONGLONG firstFailure = 0;

    while (!StopRequested())
    {
        auto panel = FindCombinedPanel();
        if (!panel)
        {
            // Not connected, or connected over DP as two real monitors: nothing to do.
            if (!Nap(2000)) break;
            continue;
        }
        if (!panel->active)
        {
            Log(L"panel found off the desktop (stale state), restoring it first");
            RevertAll(panel->adapter, panel->targetId);
            if (!Nap(1000)) break;
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

        int s = GetScalePercentForTarget(panel->adapter, panel->targetId);
        if (s) scale = s;
        Log(L"session start: panel %ux%u@%.2f, scale %d%%", panel->width, panel->height, panel->refreshHz, scale);
        if (!watchdogArmed)
        {
            SpawnWatchdog(*panel, testSeconds ? testSeconds + 30 : 0);
            watchdogArmed = true;
        }

        bool stopped = false;
        try
        {
            stopped = RunSession(*panel, scale, deadline);
        }
        catch (winrt::hresult_error const& e)
        {
            Log(L"WinRT error 0x%08X: %s", (UINT)e.code(), e.message().c_str());
        }
        catch (std::exception const& e)
        {
            Log(L"error: %S", e.what());
        }
        RevertAll(panel->adapter, panel->targetId);
        if (stopped || StopRequested() || (deadline && GetTickCount64() >= deadline)) break;

        // Back off on repeated failures so a persistent problem cannot flicker the screen forever.
        ULONGLONG t = GetTickCount64();
        if (!firstFailure || t - firstFailure > 120000)
        {
            firstFailure = t;
            failures = 0;
        }
        if (++failures >= 3)
        {
            Log(L"3 failures within 2 minutes, giving up; panel stays a single display");
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
        // Resolve the name to a device path when it matches exactly one connected display.
        std::vector<PanelTarget> hits;
        for (auto& t : EnumTargets())
            if (MatchesPanel(t)) hits.push_back(t);
        if (hits.size() == 1) SelectPanel(hits[0]);
        else SaveConfigValue(L"panel", PanelNamePrefix()), SaveConfigValue(L"panel_id", L"");
        Log(L"configure: display to split = '%s' %s", PanelNamePrefix().c_str(), hits.size() == 1 ? PanelId().c_str() : L"(by name)");
        return 0;
    }
    if (cmd == L"autodetect")
    {
        // Keeps an existing selection; otherwise picks the only tall display, or the only display.
        if (HasPanelSelection())
        {
            Log(L"autodetect: keeping '%s'", PanelNamePrefix().c_str());
            return 0;
        }
        auto t = AutodetectPanel();
        if (!t)
        {
            Log(L"autodetect: several displays, choose one in the settings window");
            return 2;
        }
        SelectPanel(*t);
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
    if (cmd == L"watchdog" && argc >= 7)
    {
        LUID l{ (DWORD)wcstoul(argv[3], nullptr, 10), (LONG)wcstol(argv[4], nullptr, 10) };
        return CmdWatchdog(wcstoul(argv[2], nullptr, 10), l, wcstoul(argv[5], nullptr, 10), wcstoul(argv[6], nullptr, 10));
    }
    Log(L"usage: splitdisplay [--panel NAME] run [--test SECONDS] | stop | revert | configure | autodetect | autostart on|off");
    return 1;
}
