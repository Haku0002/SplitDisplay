// sdctl: install/remove the SplitDisplay driver and drive its virtual monitors by hand.
//
//   sdctl driver-install <path\to\SplitDisplayIdd.inf>
//   sdctl driver-remove
//   sdctl plug [width height hz]     default: half of the Sculptor panel, on its GPU
//   sdctl unplug
//   sdctl status

#include <Windows.h>
#include <SetupAPI.h>
#include <newdev.h>
#include <devguid.h>
#include <cfgmgr32.h>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "log.h"
#include "panel.h"
#include "shm.h"

static const wchar_t kHwId[] = L"Root\\SplitDisplayIdd";

static std::vector<SP_DEVINFO_DATA> FindOurDevices(HDEVINFO set)
{
    std::vector<SP_DEVINFO_DATA> out;
    SP_DEVINFO_DATA dev{ sizeof(dev) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++)
    {
        wchar_t buf[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, (BYTE*)buf, sizeof(buf) - 4, nullptr)) continue;
        for (wchar_t* p = buf; *p; p += wcslen(p) + 1)
            if (_wcsicmp(p, kHwId) == 0) out.push_back(dev);
    }
    return out;
}

static int DriverInstall(const wchar_t* inf)
{
    wchar_t full[MAX_PATH];
    GetFullPathNameW(inf, MAX_PATH, full, nullptr);

    CreateDirectoryW(L"C:\\ProgramData\\SplitDisplay", nullptr);

    HDEVINFO all = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    bool exists = !FindOurDevices(all).empty();
    SetupDiDestroyDeviceInfoList(all);

    if (!exists)
    {
        HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr);
        SP_DEVINFO_DATA dev{ sizeof(dev) };
        if (!SetupDiCreateDeviceInfoW(set, L"Display", &GUID_DEVCLASS_DISPLAY, L"SplitDisplay Virtual Panel", nullptr, DICD_GENERATE_ID, &dev))
        {
            Log(L"SetupDiCreateDeviceInfo failed %lu", GetLastError());
            return 1;
        }
        wchar_t hw[64] = {};
        wcscpy_s(hw, kHwId); // double-NUL terminated by the zero init
        SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, (BYTE*)hw, (DWORD)((wcslen(hw) + 2) * sizeof(wchar_t)));
        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev))
        {
            Log(L"DIF_REGISTERDEVICE failed %lu", GetLastError());
            return 1;
        }
        SetupDiDestroyDeviceInfoList(set);
        Log(L"root device created");
    }

    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHwId, full, INSTALLFLAG_FORCE, &reboot))
    {
        Log(L"UpdateDriverForPlugAndPlayDevices failed %lu (0x%08X)", GetLastError(), GetLastError());
        return 1;
    }
    Log(L"driver installed%s", reboot ? L" (reboot requested)" : L"");

    // Installing an INF with a Root\ hardware id can make PnP create a second node; keep only one.
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    auto devs = FindOurDevices(set);
    for (size_t i = 1; i < devs.size(); i++)
    {
        BOOL rb = FALSE;
        Log(L"removing duplicate device node: %s", DiUninstallDevice(nullptr, set, &devs[i], 0, &rb) ? L"ok" : L"failed");
    }
    SetupDiDestroyDeviceInfoList(set);
    return 0;
}

static int DriverRemove()
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    int n = 0;
    for (auto& dev : FindOurDevices(set))
    {
        BOOL reboot = FALSE;
        if (DiUninstallDevice(nullptr, set, &dev, 0, &reboot)) n++;
        else Log(L"DiUninstallDevice failed %lu", GetLastError());
    }
    SetupDiDestroyDeviceInfoList(set);
    Log(L"removed %d device(s); remove the package with: pnputil /enum-drivers  then  pnputil /delete-driver oemNN.inf", n);
    return 0;
}

static bool OpenShm(SharedView& v)
{
    if (!v.Open())
    {
        Log(L"driver shared memory not found (driver not loaded?) err=%lu", GetLastError());
        return false;
    }
    return true;
}

static int Plug(int argc, wchar_t** argv)
{
    SharedView v;
    if (!OpenShm(v)) return 1;

    // Default: two halves of the panel. "plug W H HZ" plugs two monitors of W x H.
    SdConfig cfg{};
    cfg.count = 2;
    SdMonitorConfig m{ 2560, 1440, 100, 0, {} };
    auto panels = SelectedPanels();
    if (auto p = panels.empty() ? AutodetectPanel() : FindTarget(panels[0]))
    {
        cfg.renderAdapter = p->adapter;
        if (p->width && p->height) m.width = p->width, m.height = p->height / 2;
        if (p->refreshHz > 1) m.refreshHz = (UINT32)std::lround(p->refreshHz);
    }
    if (argc >= 5) m = { (UINT32)_wtoi(argv[2]), (UINT32)_wtoi(argv[3]), (UINT32)_wtoi(argv[4]), 0, {} };
    cfg.mon[0] = cfg.mon[1] = m;
    v->config = cfg;
    MemoryBarrier();
    InterlockedExchange(&v->desiredPlugged, 1);
    v.Kick();
    Log(L"plug requested: 2 x %ux%u@%u on adapter %08X:%08X", m.width, m.height, m.refreshHz, cfg.renderAdapter.HighPart, cfg.renderAdapter.LowPart);

    for (int i = 0; i < 50; i++)
    {
        if (v->mon[0].plugged && v->mon[1].plugged)
        {
            Log(L"both monitors plugged");
            return 0;
        }
        Sleep(100);
    }
    Log(L"timeout waiting for monitors (plugged=%ld,%ld)", v->mon[0].plugged, v->mon[1].plugged);
    return 1;
}

static int Unplug()
{
    SharedView v;
    if (!OpenShm(v)) return 1;
    InterlockedExchange(&v->desiredPlugged, 0);
    v.Kick();
    for (int i = 0; i < 50 && (v->mon[0].plugged || v->mon[1].plugged); i++) Sleep(100);
    Log(L"unplugged (plugged=%ld,%ld)", v->mon[0].plugged, v->mon[1].plugged);
    return 0;
}

static int Status()
{
    SharedView v;
    if (!OpenShm(v)) return 1;
    auto* s = v.get();
    Log(L"driver pid %lu, heartbeat %lld ms ago, desiredPlugged=%ld, %u monitors on adapter %08X:%08X", s->driverPid,
        (LONG64)GetTickCount64() - s->driverHeartbeat, s->desiredPlugged, s->config.count,
        s->config.renderAdapter.HighPart, s->config.renderAdapter.LowPart);
    for (UINT i = 0; i < s->config.count && i < SD_MAX_MONITORS; i++)
    {
        auto& m = s->mon[i];
        Log(L"  mon%u plugged=%ld gen=%ld latest=%ld frames=%lld %ux%u fmt=%u adapter %08X:%08X", i, m.plugged, m.handleGeneration, m.latest,
            m.frameSeq, m.width, m.height, m.format, m.adapter.HighPart, m.adapter.LowPart);
    }
    for (auto& t : EnumTargets())
        Log(L"  target %08X:%08X/%u '%s' active=%d %ux%u@%.2f", t.adapter.HighPart, t.adapter.LowPart, t.targetId, t.friendlyName.c_str(), t.active,
            t.width, t.height, t.refreshHz);
    return 0;
}

// Exercises the remove-from-desktop packet on the invisible "Split 2" monitor only.
static int SpecTest()
{
    if (Plug(1, nullptr) != 0) return 1;
    Sleep(1500);
    std::optional<PanelTarget> lower;
    for (auto& t : EnumTargets())
        if (t.friendlyName.rfind(L"Split 2", 0) == 0) lower = t;
    int rc = 1;
    if (lower)
    {
        auto s = GetSpecialization(lower->adapter, lower->targetId);
        Log(L"Split 2: active=%d spec(ok=%d en=%d mon=%d sys=%d)", lower->active, s.ok, s.enabled, s.availableForMonitor, s.availableForSystem);
        auto activeNow = [&] {
            for (auto& t : EnumTargets())
                if (t.targetId == lower->targetId && t.adapter.LowPart == lower->adapter.LowPart) return t.active;
            return false;
        };
        LONG r1 = SetSpecialization(lower->adapter, lower->targetId, true);
        Sleep(2000);
        bool afterEnable = activeNow();
        s = GetSpecialization(lower->adapter, lower->targetId);
        Log(L"enable -> %ld, active=%d, GET en=%d", r1, afterEnable, s.enabled);
        LONG r2 = SetSpecialization(lower->adapter, lower->targetId, false);
        Sleep(2000);
        bool afterDisable = activeNow();
        Log(L"disable -> %ld, active=%d", r2, afterDisable);
        LONG r3 = SetSpecialization(lower->adapter, lower->targetId, false);
        Log(L"second disable -> %ld", r3);
        rc = (r1 == 0 && !afterEnable && r2 == 0 && afterDisable) ? 0 : 2;
        Log(L"spec-test %s", rc == 0 ? L"PASSED" : L"FAILED");
    }
    Unplug();
    return rc;
}

int wmain(int argc, wchar_t** argv)
{
    LogInit(L"sdctl.log");
    ParsePanelOption(argc, argv);
    std::wstring cmd = argc > 1 ? argv[1] : L"status";
    if (cmd == L"driver-install" && argc > 2) return DriverInstall(argv[2]);
    if (cmd == L"driver-remove") return DriverRemove();
    if (cmd == L"plug") return Plug(argc, argv);
    if (cmd == L"unplug") return Unplug();
    if (cmd == L"status") return Status();
    if (cmd == L"spec-test") return SpecTest();
    Log(L"usage: sdctl driver-install <inf> | driver-remove | plug [w h hz] | unplug | status");
    return 1;
}
