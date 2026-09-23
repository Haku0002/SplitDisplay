// panel_test: feasibility spike for taking exclusive ownership of the Sculptor panel.
//
//   panel_test status            list targets and specialization state
//   panel_test run [seconds]     remove panel from desktop, draw test pattern, give it back
//   panel_test revert            give the panel back to the desktop
//
// "run" spawns a detached watchdog copy of itself that kills this process and reverts the
// specialization if we are not done within seconds+15, so a hang cannot leave the panel dark.

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

#include "log.h"
#include "panel.h"

namespace wdc = winrt::Windows::Devices::Display::Core;
namespace wdd = winrt::Windows::Devices::Display;
namespace wgd = winrt::Windows::Graphics::DirectX;

static std::atomic_bool g_stop = false;

static BOOL WINAPI OnCtrl(DWORD)
{
    g_stop = true;
    return TRUE;
}

static const wchar_t* UsageName(wdd::DisplayMonitorUsageKind k)
{
    switch (k)
    {
    case wdd::DisplayMonitorUsageKind::Standard: return L"Standard";
    case wdd::DisplayMonitorUsageKind::HeadMounted: return L"HeadMounted";
    case wdd::DisplayMonitorUsageKind::SpecialPurpose: return L"SpecialPurpose";
    }
    return L"?";
}

static int CmdStatus()
{
    for (auto& t : EnumTargets())
    {
        auto s = GetSpecialization(t.adapter, t.targetId);
        Log(L"target %08X:%08X/%u '%s' active=%d %ux%u@%.2f spec(ok=%d en=%d mon=%d sys=%d)",
            t.adapter.HighPart, t.adapter.LowPart, t.targetId, t.friendlyName.c_str(), t.active, t.width, t.height, t.refreshHz,
            s.ok, s.enabled, s.availableForMonitor, s.availableForSystem);
    }
    return 0;
}

static int RevertTarget(LUID luid, UINT32 targetId)
{
    auto s = GetSpecialization(luid, targetId);
    if (s.ok && !s.enabled)
    {
        Log(L"revert: panel already on desktop");
        return 0;
    }
    LONG r = SetSpecialization(luid, targetId, false);
    Log(L"revert: SetSpecialization(false) -> %ld", r);
    return r == ERROR_SUCCESS ? 0 : 1;
}

static int CmdRevert()
{
    auto p = FindPanel();
    if (!p)
    {
        Log(L"revert: Sculptor not found");
        return 1;
    }
    return RevertTarget(p->adapter, p->targetId);
}

static int CmdWatchdog(DWORD parentPid, LUID luid, UINT32 targetId, DWORD timeoutSec)
{
    HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, parentPid);
    DWORD w = parent ? WaitForSingleObject(parent, timeoutSec * 1000) : WAIT_FAILED;
    if (w == WAIT_TIMEOUT)
    {
        Log(L"watchdog: parent %lu overran %lus, terminating it", parentPid, timeoutSec);
        TerminateProcess(parent, 0xDEAD);
        WaitForSingleObject(parent, 5000);
    }
    if (parent) CloseHandle(parent);
    // The parent reverts on a clean exit; this covers crashes and hangs. Retry a few times
    // because the display stack can be busy right after the owner process dies.
    for (int i = 0; i < 10; i++)
    {
        if (RevertTarget(luid, targetId) == 0) return 0;
        Sleep(1000);
    }
    return 1;
}

static void SpawnWatchdog(const PanelTarget& p, DWORD timeoutSec)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t cmd[512];
    swprintf_s(cmd, L"\"%s\" watchdog %lu %lu %ld %u %lu", exe, GetCurrentProcessId(), p.adapter.LowPart, p.adapter.HighPart, p.targetId, timeoutSec);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi) ||
        CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
    {
        Log(L"watchdog pid %lu armed (%lus)", pi.dwProcessId, timeoutSec);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else
    {
        throw std::runtime_error("failed to start watchdog");
    }
}

static wdc::DisplayTarget WaitForOwnableTarget(const wdc::DisplayManager& mgr, LUID luid, UINT32 targetId, int timeoutMs)
{
    for (int waited = 0; waited <= timeoutMs; waited += 250)
    {
        for (auto&& t : mgr.GetCurrentTargets())
        {
            auto id = t.Adapter().Id();
            if (id.LowPart == luid.LowPart && id.HighPart == luid.HighPart && t.AdapterRelativeId() == targetId && t.IsConnected())
            {
                if (t.UsageKind() != wdd::DisplayMonitorUsageKind::Standard)
                {
                    Log(L"DisplayCore target ready, usage=%s after %dms", UsageName(t.UsageKind()), waited);
                    return t;
                }
            }
        }
        Sleep(250);
    }
    return nullptr;
}

struct Renderer
{
    winrt::com_ptr<ID3D11Device5> dev;
    winrt::com_ptr<ID3D11DeviceContext4> ctx;
    winrt::com_ptr<ID3D11Fence> fence;
    UINT64 fenceValue = 0;
    winrt::com_ptr<ID3D11Texture2D> tex[2];
    winrt::com_ptr<ID3D11RenderTargetView> rtv[2];

    void Create(LUID luid)
    {
        winrt::com_ptr<IDXGIFactory6> factory;
        factory.capture(&CreateDXGIFactory2, 0);
        winrt::com_ptr<IDXGIAdapter4> adapter;
        adapter.capture(factory, &IDXGIFactory6::EnumAdapterByLuid, luid);
        DXGI_ADAPTER_DESC3 ad{};
        adapter->GetDesc3(&ad);
        Log(L"render adapter: %s", ad.Description);

        winrt::com_ptr<ID3D11Device> d;
        winrt::com_ptr<ID3D11DeviceContext> c;
        D3D_FEATURE_LEVEL fl;
        winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d.put(), &fl, c.put()));
        dev = d.as<ID3D11Device5>();
        ctx = c.as<ID3D11DeviceContext4>();
        fence.capture(dev, &ID3D11Device5::CreateFence, 0, D3D11_FENCE_FLAG_SHARED);
    }

    void Open(const wdc::DisplayDevice& device, wdc::DisplaySurface* surfaces)
    {
        auto interop = device.as<IDisplayDeviceInterop>();
        for (int i = 0; i < 2; i++)
        {
            winrt::handle h;
            winrt::check_hresult(interop->CreateSharedHandle(surfaces[i].as<::IInspectable>().get(), nullptr, GENERIC_ALL, nullptr, h.put()));
            tex[i].capture(dev, &ID3D11Device5::OpenSharedResource1, h.get());
            D3D11_TEXTURE2D_DESC td{};
            tex[i]->GetDesc(&td);
            D3D11_RENDER_TARGET_VIEW_DESC vd{};
            vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            vd.Format = td.Format;
            winrt::check_hresult(dev->CreateRenderTargetView(tex[i].get(), &vd, rtv[i].put()));
        }
    }

    wdc::DisplayFence DisplayFenceFor(const wdc::DisplayDevice& device)
    {
        auto interop = device.as<IDisplayDeviceInterop>();
        winrt::handle h;
        winrt::check_hresult(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, h.put()));
        winrt::com_ptr<::IInspectable> f;
        f.capture(interop, &IDisplayDeviceInterop::OpenSharedHandle, h.get());
        return f.as<wdc::DisplayFence>();
    }

    // Top half red, bottom half blue, each with a white bar sweeping at a different speed.
    UINT64 Draw(int i, UINT w, UINT h, UINT64 frame)
    {
        const float red[4] = { 0.75f, 0.05f, 0.05f, 1 };
        const float blue[4] = { 0.05f, 0.15f, 0.75f, 1 };
        const float white[4] = { 1, 1, 1, 1 };
        const float black[4] = { 0, 0, 0, 1 };
        LONG half = (LONG)h / 2;
        D3D11_RECT top{ 0, 0, (LONG)w, half };
        D3D11_RECT bottom{ 0, half, (LONG)w, (LONG)h };
        ctx->ClearView(rtv[i].get(), red, &top, 1);
        ctx->ClearView(rtv[i].get(), blue, &bottom, 1);

        LONG x1 = (LONG)((frame * 16) % w);
        LONG x2 = (LONG)((frame * 8) % w);
        D3D11_RECT bar1{ x1, 0, x1 + 48, half };
        D3D11_RECT bar2{ x2, half, x2 + 48, (LONG)h };
        ctx->ClearView(rtv[i].get(), white, &bar1, 1);
        ctx->ClearView(rtv[i].get(), white, &bar2, 1);

        // A 2px black seam marks exactly where the split will be.
        D3D11_RECT seam{ 0, half - 1, (LONG)w, half + 1 };
        ctx->ClearView(rtv[i].get(), black, &seam, 1);

        ctx->Signal(fence.get(), ++fenceValue);
        return fenceValue;
    }
};

static int RenderOn(const wdc::DisplayManager& mgr, const wdc::DisplayTarget& target, DWORD seconds)
{
    auto targets = winrt::single_threaded_vector<wdc::DisplayTarget>({ target });
    auto acq = mgr.TryAcquireTargetsAndCreateEmptyState(targets);
    if (acq.ErrorCode() != wdc::DisplayManagerResult::Success)
    {
        Log(L"TryAcquireTargetsAndCreateEmptyState failed: result=%d hr=0x%08X", (int)acq.ErrorCode(), (UINT)acq.ExtendedErrorCode());
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
        double hz = (double)v.Numerator / v.Denominator;
        auto r = m.SourceResolution();
        Log(L"  mode %dx%d @ %.3f Hz fmt=%d", r.Width, r.Height, hz, (int)m.SourcePixelFormat());
        if (std::abs(hz - 100) < bestDiff)
        {
            bestDiff = std::abs(hz - 100);
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

    auto reread = mgr.TryAcquireTargetsAndReadCurrentState(targets);
    state = reread.State();
    path = state.GetPathForTarget(target);
    auto res = path.SourceResolution().Value();
    auto rate = path.PresentationRate().Value().VerticalSyncRate;
    Log(L"applied: %dx%d @ %.3f Hz", res.Width, res.Height, (double)rate.Numerator / rate.Denominator);

    auto device = mgr.CreateDisplayDevice(target.Adapter());
    Renderer r;
    auto aid = target.Adapter().Id();
    r.Create({ aid.LowPart, aid.HighPart });

    auto source = device.CreateScanoutSource(target);
    auto pool = device.CreateTaskPool();
    winrt::Windows::Graphics::DirectX::Direct3D11::Direct3DMultisampleDescription ms{ 1, 0 };
    wdc::DisplayPrimaryDescription desc{ (uint32_t)res.Width, (uint32_t)res.Height, path.SourcePixelFormat(), wgd::DirectXColorSpace::RgbFullG22NoneP709, false, ms };
    wdc::DisplaySurface prim[2] = { device.CreatePrimary(target, desc), device.CreatePrimary(target, desc) };
    wdc::DisplayScanout scan[2] = { device.CreateSimpleScanout(source, prim[0], 0, 1), device.CreateSimpleScanout(source, prim[1], 0, 1) };
    r.Open(device, prim);
    auto dfence = r.DisplayFenceFor(device);

    LARGE_INTEGER freq, start, prev, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    prev = start;
    double minMs = 1e9, maxMs = 0;
    int late = 0;
    UINT64 frame = 0;
    const double expectMs = 1000.0 * rate.Denominator / rate.Numerator;

    while (!g_stop)
    {
        UINT64 fv = r.Draw((int)(frame & 1), res.Width, res.Height, frame);
        auto task = pool.CreateTask();
        task.SetScanout(scan[frame & 1]);
        task.SetWait(dfence, fv);
        pool.ExecuteTask(task);
        device.WaitForVBlank(source);

        QueryPerformanceCounter(&now);
        double dt = 1000.0 * (now.QuadPart - prev.QuadPart) / freq.QuadPart;
        prev = now;
        if (frame > 5)
        {
            minMs = std::min(minMs, dt);
            maxMs = std::max(maxMs, dt);
            if (dt > expectMs * 1.5) late++;
        }
        frame++;
        if ((now.QuadPart - start.QuadPart) / freq.QuadPart >= (LONGLONG)seconds) break;
    }
    double total = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
    Log(L"presented %llu frames in %.2fs = %.2f fps (vblank interval min %.2fms max %.2fms, %d late)", frame, total, frame / total, minMs, maxMs, late);

    mgr.ReleaseTarget(target);
    return 0;
}

static int CmdRun(DWORD seconds)
{
    auto panel = FindPanel();
    if (!panel)
    {
        Log(L"Sculptor not connected");
        return 1;
    }
    Log(L"panel %08X:%08X/%u '%s' active=%d %ux%u@%.2f", panel->adapter.HighPart, panel->adapter.LowPart, panel->targetId,
        panel->friendlyName.c_str(), panel->active, panel->width, panel->height, panel->refreshHz);

    int others = CountOtherActiveDisplays(*panel);
    if (others < 1)
    {
        Log(L"REFUSING: Sculptor is the only desktop display. Open the laptop lid / attach another display first.");
        return 1;
    }
    Log(L"%d other desktop display(s) active - OK", others);

    auto spec = GetSpecialization(panel->adapter, panel->targetId);
    if (!spec.availableForMonitor || !spec.availableForSystem)
    {
        Log(L"specialization not available (mon=%d sys=%d)", spec.availableForMonitor, spec.availableForSystem);
        return 1;
    }

    SpawnWatchdog(*panel, seconds + 15);

    int rc = 0;
    LONG r = SetSpecialization(panel->adapter, panel->targetId, true);
    Log(L"SetSpecialization(true) -> %ld", r);
    if (r != ERROR_SUCCESS) return 1;

    try
    {
        winrt::init_apartment();
        auto mgr = wdc::DisplayManager::Create(wdc::DisplayManagerOptions::None);
        auto target = WaitForOwnableTarget(mgr, panel->adapter, panel->targetId, 10000);
        if (!target)
        {
            Log(L"target never became ownable");
            rc = 5;
        }
        else
        {
            rc = RenderOn(mgr, target, seconds);
        }
        mgr.Close();
    }
    catch (winrt::hresult_error const& e)
    {
        Log(L"WinRT error 0x%08X: %s", (UINT)e.code(), e.message().c_str());
        rc = 6;
    }
    catch (std::exception const& e)
    {
        Log(L"error: %S", e.what());
        rc = 7;
    }

    RevertTarget(panel->adapter, panel->targetId);
    return rc;
}

int wmain(int argc, wchar_t** argv)
{
    LogInit(L"panel_test.log");
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    std::wstring cmd = argc > 1 ? argv[1] : L"status";

    if (cmd == L"status") return CmdStatus();
    if (cmd == L"revert") return CmdRevert();
    if (cmd == L"run") return CmdRun(argc > 2 ? _wtoi(argv[2]) : 15);
    if (cmd == L"watchdog" && argc >= 7)
    {
        LUID l{ (DWORD)wcstoul(argv[3], nullptr, 10), (LONG)wcstol(argv[4], nullptr, 10) };
        return CmdWatchdog(wcstoul(argv[2], nullptr, 10), l, wcstoul(argv[5], nullptr, 10), wcstoul(argv[6], nullptr, 10));
    }
    Log(L"usage: panel_test status | run [seconds] | revert");
    return 1;
}
