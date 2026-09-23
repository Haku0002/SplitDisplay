// splitdisplay: turns the single 2560x2880 Sculptor panel into two native Windows monitors.
//
//   splitdisplay run [--test SECONDS]   plug virtual monitors, take over the panel, composite
//   splitdisplay revert                 give the panel back to the desktop, unplug virtual monitors
//   splitdisplay watchdog ...           (internal) guards a running compositor
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

#include "log.h"
#include "panel.h"
#include "shm.h"
#include "topology.h"

namespace wdc = winrt::Windows::Devices::Display::Core;
namespace wdd = winrt::Windows::Devices::Display;
namespace wgd = winrt::Windows::Graphics::DirectX;

static std::atomic_bool g_stop = false;
static volatile LONG64* g_heartbeat = nullptr;

static void Beat()
{
    if (g_heartbeat) InterlockedExchange64(g_heartbeat, (LONG64)GetTickCount64());
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

#pragma region Revert / watchdog

static bool PanelActive(LUID luid, UINT32 targetId)
{
    for (auto& t : EnumTargets())
        if (t.adapter.LowPart == luid.LowPart && t.adapter.HighPart == luid.HighPart && t.targetId == targetId) return t.active;
    return false;
}

// Idempotent: panel back on the desktop first, then remove the virtual monitors.
static void RevertAll(LUID luid, UINT32 targetId)
{
    // Always send the disable packet: the documented GET query does not reflect this override.
    for (int i = 0; i < 10; i++)
    {
        LONG r = SetSpecialization(luid, targetId, false);
        Log(L"revert: SetSpecialization(false) -> %ld", r);
        if (r == ERROR_SUCCESS) break;
        Sleep(500);
    }
    for (int i = 0; i < 20 && !PanelActive(luid, targetId); i++) Sleep(250);

    SharedView v;
    if (v.Open())
    {
        InterlockedExchange(&v->desiredPlugged, 0);
        v.Kick();
        for (int i = 0; i < 50 && (v->mon[0].plugged || v->mon[1].plugged); i++) Sleep(100);
        Log(L"revert: virtual monitors unplugged (%ld,%ld)", v->mon[0].plugged, v->mon[1].plugged);
    }

    if (!PanelActive(luid, targetId))
    {
        Sleep(1000);
        if (!PanelActive(luid, targetId)) ForceExtendTopology();
    }
    Log(L"revert: panel active=%d", PanelActive(luid, targetId));
}

static int CmdRevert()
{
    // After specialization the panel is no longer an active path, but EnumTargets still lists it.
    auto p = FindPanel();
    if (!p)
    {
        Log(L"revert: Sculptor not found");
        return 1;
    }
    RevertAll(p->adapter, p->targetId);
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
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    LONG64 lastSeq = -1;
    UINT64 consumed = 0;

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
        if ((g1 & 1) || m.fenceHandle == 0) return false;
        Drop();
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
        format = (DXGI_FORMAT)m.format;
        gen = g1;
        Log(L"mon%d: attached ring gen=%ld %ux%u fmt=%u", index, gen, width, height, format);
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
                return l;
            }
            InterlockedExchange(&m.readerIndex, -1);
        }
        return -1;
    }

    void Release(SdShared* s)
    {
        InterlockedExchange(&s->mon[index].readerIndex, -1);
    }
};

#pragma endregion

#pragma region Compositor

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

static int Composite(SharedView& shm, const PanelTarget& panel, DWORD testSeconds)
{
    auto mgr = wdc::DisplayManager::Create(wdc::DisplayManagerOptions::None);
    auto target = WaitForOwnableTarget(mgr, panel.adapter, panel.targetId, 10000);
    if (!target)
    {
        Log(L"panel never became ownable");
        return 5;
    }

    auto targets = winrt::single_threaded_vector<wdc::DisplayTarget>({ target });
    auto acq = mgr.TryAcquireTargetsAndCreateEmptyState(targets);
    if (acq.ErrorCode() != wdc::DisplayManagerResult::Success)
    {
        Log(L"acquire failed: result=%d hr=0x%08X", (int)acq.ErrorCode(), (UINT)acq.ExtendedErrorCode());
        return 2;
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
        return 3;
    }
    path.ApplyPropertiesFromMode(best);
    auto applied = state.TryApply(wdc::DisplayStateApplyOptions::None);
    if (applied.Status() != wdc::DisplayStateOperationStatus::Success)
    {
        Log(L"TryApply failed: status=%d hr=0x%08X", (int)applied.Status(), (UINT)applied.ExtendedErrorCode());
        return 4;
    }
    state = mgr.TryAcquireTargetsAndReadCurrentState(targets).State();
    path = state.GetPathForTarget(target);
    auto res = path.SourceResolution().Value();
    auto rate = path.PresentationRate().Value().VerticalSyncRate;
    Log(L"panel owned: %dx%d @ %.3f Hz", res.Width, res.Height, (double)rate.Numerator / rate.Denominator);
    Beat();

    // D3D on the panel's GPU.
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
        mgr.ReleaseTarget(target);
        return 6;
    }

    FrameSource src[SD_MONITORS];
    for (int i = 0; i < SD_MONITORS; i++) src[i].index = i;
    const UINT halfH = (UINT)res.Height / SD_MONITORS;
    const float black[4] = { 0, 0, 0, 1 };

    LARGE_INTEGER freq, t0, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    UINT64 vblanks = 0, presents = 0;
    double gpuWaitMaxMs = 0;
    bool presented = false;

    while (!g_stop)
    {
        Beat();
        device.WaitForVBlank(source);
        vblanks++;

        bool changed = !presented;
        for (auto& s : src)
        {
            if (s.gen != shm->mon[s.index].handleGeneration) changed = true;
            if (shm->mon[s.index].frameSeq != s.lastSeq) changed = true;
        }

        if (changed)
        {
            int back = (int)(presents & 1);
            int reserved[SD_MONITORS];
            for (auto& s : src)
            {
                reserved[s.index] = -1;
                auto& m = shm->mon[s.index];
                if (s.gen != m.handleGeneration) s.Refresh(dev.get(), shm.get(), driverProc);

                D3D11_RECT area{ 0, (LONG)(s.index * halfH), res.Width, (LONG)((s.index + 1) * halfH) };
                LONG64 seq = m.frameSeq;
                UINT64 fv = 0;
                int idx = s.gen >= 0 ? s.Acquire(shm.get(), fv) : -1;
                if (idx < 0)
                {
                    ctx->ClearView(primRtv[back].get(), black, &area, 1);
                    continue;
                }
                reserved[s.index] = idx;
                s.lastSeq = seq;
                s.consumed++;
                ctx->Wait(s.fence.get(), fv);
                D3D11_BOX box{ 0, 0, 0, std::min<UINT>(s.width, (UINT)res.Width), std::min<UINT>(s.height, halfH), 1 };
                ctx->CopySubresourceRegion(primTex[back].get(), 0, 0, s.index * halfH, 0, s.tex[idx].get(), 0, &box);
            }
            ctx->Signal(fence.get(), ++fenceValue);
            ctx->Flush();

            // Hold the reservations until our copies are done on the GPU.
            LARGE_INTEGER w0, w1;
            QueryPerformanceCounter(&w0);
            fence->SetEventOnCompletion(fenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, 250);
            QueryPerformanceCounter(&w1);
            gpuWaitMaxMs = std::max(gpuWaitMaxMs, 1000.0 * (w1.QuadPart - w0.QuadPart) / freq.QuadPart);
            for (auto& s : src)
                if (reserved[s.index] >= 0) s.Release(shm.get());

            auto task = pool.CreateTask();
            task.SetScanout(scan[back]);
            task.SetWait(displayFence, fenceValue);
            pool.ExecuteTask(task);
            presents++;
            presented = true;
        }

        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
        if (testSeconds && elapsed >= testSeconds) break;
    }

    double total = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
    Log(L"ran %.1fs: %llu vblanks (%.2f Hz), %llu presents, frames used upper=%llu lower=%llu, max GPU copy wait %.2fms",
        total, vblanks, vblanks / std::max(total, 0.001), presents, src[0].consumed, src[1].consumed, gpuWaitMaxMs);

    for (auto& s : src) s.Drop();
    CloseHandle(driverProc);
    CloseHandle(fenceEvent);
    mgr.ReleaseTarget(target);
    return 0;
}

static int CmdRun(DWORD testSeconds)
{
    auto panel = FindPanel();
    if (!panel || !panel->active)
    {
        Log(L"Sculptor not found or not on the desktop (run 'splitdisplay revert' first?)");
        return 1;
    }
    if (panel->height % 2)
    {
        Log(L"unexpected panel height %u", panel->height);
        return 1;
    }
    auto spec = GetSpecialization(panel->adapter, panel->targetId);
    if (!spec.availableForMonitor || !spec.availableForSystem)
    {
        Log(L"panel cannot be removed from the desktop on this system");
        return 1;
    }
    SharedView shm;
    if (!shm.Open())
    {
        Log(L"SplitDisplay driver not loaded");
        return 1;
    }
    Log(L"panel %ux%u@%.2f, driver pid %lu", panel->width, panel->height, panel->refreshHz, shm->driverPid);

    HANDLE hbMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG64), HeartbeatName(GetCurrentProcessId()).c_str());
    g_heartbeat = (volatile LONG64*)MapViewOfFile(hbMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG64));
    Beat();

    int scale = GetScalePercentForTarget(panel->adapter, panel->targetId);
    SpawnWatchdog(*panel, testSeconds ? testSeconds + 30 : 0);

    std::thread hotkeys(HotkeyThread);

    int rc = 0;
    try
    {
        // 1. Virtual monitors, half the panel each, rendered on the panel's GPU.
        // Counters survive from earlier sessions, so require fresh frames after this plug.
        LONG64 base0 = shm->mon[0].frameSeq, base1 = shm->mon[1].frameSeq;
        shm->config = { panel->width, panel->height / 2, (UINT32)std::lround(panel->refreshHz), panel->adapter };
        MemoryBarrier();
        InterlockedExchange(&shm->desiredPlugged, 1);
        shm.Kick();
        auto live = [&] {
            return shm->mon[0].plugged && shm->mon[1].plugged && shm->mon[0].frameSeq > base0 && shm->mon[1].frameSeq > base1 &&
                   IsTargetActive(L"Split Upper") && IsTargetActive(L"Split Lower");
        };
        for (int i = 0; i < 100 && !live(); i++)
        {
            Beat();
            Sleep(100);
        }
        if (!live()) throw std::runtime_error("virtual monitors did not come up");
        Log(L"virtual monitors live");
        if (scale) SetScalePercentForMonitors(L"Split ", scale);
        Beat();

        // 2. Take the panel off the desktop; Windows now only has the two virtual monitors.
        LONG r = SetSpecialization(panel->adapter, panel->targetId, true);
        Log(L"SetSpecialization(true) -> %ld", r);
        if (r != ERROR_SUCCESS) throw std::runtime_error("specialization failed");
        for (int i = 0; i < 40 && IsTargetActive(L"Sculptor"); i++)
        {
            Beat();
            Sleep(250);
        }
        for (int i = 0; i < 10 && !ArrangeVirtualMonitors(); i++)
        {
            Beat();
            Sleep(300);
        }
        Beat();

        // 3. Drive the panel ourselves.
        winrt::init_apartment();
        rc = Composite(shm, *panel, testSeconds);
    }
    catch (winrt::hresult_error const& e)
    {
        Log(L"WinRT error 0x%08X: %s", (UINT)e.code(), e.message().c_str());
        rc = 7;
    }
    catch (std::exception const& e)
    {
        Log(L"error: %S", e.what());
        rc = 8;
    }

    RevertAll(panel->adapter, panel->targetId);
    g_hotkeyQuit = true;
    hotkeys.join();
    return rc;
}

#pragma endregion

int wmain(int argc, wchar_t** argv)
{
    LogInit(L"splitdisplay.log");
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    std::wstring cmd = argc > 1 ? argv[1] : L"";

    if (cmd == L"run")
    {
        DWORD test = 0;
        if (argc > 3 && std::wstring(argv[2]) == L"--test") test = (DWORD)_wtoi(argv[3]);
        return CmdRun(test);
    }
    if (cmd == L"revert") return CmdRevert();
    if (cmd == L"watchdog" && argc >= 7)
    {
        LUID l{ (DWORD)wcstoul(argv[3], nullptr, 10), (LONG)wcstol(argv[4], nullptr, 10) };
        return CmdWatchdog(wcstoul(argv[2], nullptr, 10), l, wcstoul(argv[5], nullptr, 10), wcstoul(argv[6], nullptr, 10));
    }
    Log(L"usage: splitdisplay run [--test SECONDS] | revert");
    return 1;
}
