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
// Exclusive mode owns each panel through DisplayManager; window mode (Windows editions without
// display specialization, or mode=window in config.ini) shows the split monitors in a window on it.
//
// Emergency exit while running: Ctrl+Alt+Shift+F12.

#include <Windows.h>
#include <DbgHelp.h>
#include <Icm.h>
#include <Shellapi.h>
#include <avrt.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <slpublic.h>
#include <WtsApi32.h>
#include <winternl.h> // NTSTATUS for d3dkmthk.h
#include <d3dkmthk.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Display.h>
#include <winrt/Windows.Devices.Display.Core.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Devices.Display.Core.Interop.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "autostart.h"
#include "frames.h"
#include "guard.h"
#include "ring.h"
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
static bool g_windowMode = false;          // this session composites into a window (see "Window mode")
static HANDLE g_stopEvent = nullptr;
// Per-panel session state shared between the session loop and the panel workers,
// indexed by PanelSession::index.
static std::atomic<int> g_desiredMilliHz[SD_MAX_MONITORS]; // refresh rate the panel should run at now
static std::atomic<int> g_rotations[SD_MAX_MONITORS];      // DISPLAYCONFIG_ROTATION per virtual monitor
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

// Only Windows editions licensed for specialized displays (e.g. Enterprise, Pro for Workstations)
// let a DisplayManager client own a display removed from the desktop. On the others (e.g. Home,
// Pro) the panel can still be removed, but acquiring it fails with access denied.
static bool SpecializedDisplaysLicensed()
{
    DWORD enabled = 0;
    return SUCCEEDED(SLGetWindowsInformationDWORD(L"Display-Specialized-Displays-Enabled", &enabled)) && enabled != 0;
}

// True while this session is locked: the lock screen and the sign-in screen after it run on the
// Winlogon desktop, which only SYSTEM may draw on. A window-mode compositor cannot put anything
// there, so its panel would stay black until the user signs in.
static bool SessionLocked()
{
    WTSINFOEXW* info = nullptr;
    DWORD bytes = 0;
    bool locked = false;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, (LPWSTR*)&info, &bytes) && info)
    {
        // SessionFlags is WTS_SESSIONSTATE_LOCK (0) or WTS_SESSIONSTATE_UNLOCK (1), not a bit mask.
        if (info->Level == 1) locked = info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
        WTSFreeMemory(info);
    }
    return locked;
}

// True while Windows shows the desktop this process runs on. Ctrl+Alt+Del, a UAC prompt on the secure
// desktop and confirmation dialogs that programs raise on a desktop of their own all switch away from
// it; a secure desktop cannot even be opened from here (access denied).
static bool OnInputDesktop()
{
    HDESK in = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!in) return false;
    wchar_t shown[64] = {}, mine[64] = {};
    DWORD len = 0;
    bool same = GetUserObjectInformationW(in, UOI_NAME, shown, sizeof(shown), &len) &&
                GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, mine, sizeof(mine), &len) && _wcsicmp(shown, mine) == 0;
    CloseDesktop(in);
    return same;
}

// Why a window-mode compositor cannot reach its panels right now, or nullptr if it can. Its window
// only shows on its own desktop, and not even there while the lock screen is up.
static const wchar_t* HiddenReason()
{
    if (SessionLocked()) return L"session locked";
    if (!OnInputDesktop()) return L"another desktop is up (Ctrl+Alt+Del or a secure prompt)";
    return nullptr;
}

// config.ini [SplitDisplay] mode = auto (default) | exclusive | window
static bool ChooseWindowMode()
{
    auto mode = LoadConfigValue(L"mode");
    if (_wcsicmp(mode.c_str(), L"window") == 0) return true;
    if (_wcsicmp(mode.c_str(), L"exclusive") == 0) return false;
    return !SpecializedDisplaysLicensed();
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
    int index = 0;             // slot in the session arrays (g_desiredMilliHz, beats, ...)
    int scale = 0;             // scale inherited from the physical panel, percent
    std::vector<int> scales;   // per-region scale actually in effect (persisted on user change)
    int refreshMilliHz = 0;    // milli-Hz for the panel and all of its split monitors (session start)
    std::vector<int> rates;    // rates the panel supports in milli-Hz (offered on the split monitors)
};

static void WriteRuntime(const std::vector<PanelSession>& ps)
{
    auto path = RuntimePath();
    // Written to a temp file and renamed over the original: the settings window reads this
    // file every second and must never see it half-written.
    auto temp = path + L".tmp" + std::to_wstring(GetCurrentProcessId());
    DeleteFileW(temp.c_str());
    const wchar_t* file = temp.c_str();
    WritePrivateProfileStringW(L"Runtime", L"panels", std::to_wstring(ps.size()).c_str(), file);
    for (size_t i = 0; i < ps.size(); i++)
    {
        auto sec = L"Panel" + std::to_wstring(i + 1);
        auto& p = ps[i];
        auto put = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(sec.c_str(), k, v.c_str(), file); };
        put(L"id", p.target.monitorDevicePath);
        put(L"x", std::to_wstring(p.target.position.x));
        put(L"y", std::to_wstring(p.target.position.y));
        put(L"w", std::to_wstring(p.target.width));
        put(L"h", std::to_wstring(p.target.height));
        put(L"first", std::to_wstring(p.first));
        put(L"count", std::to_wstring(p.regions.size()));
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, file); // flush
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(temp.c_str());
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
    for (int i = 0; i < 50 && anyPlugged(); i++)
    {
        Beat();
        Sleep(100);
    }
    Log(L"revert: virtual monitors unplugged%s", anyPlugged() ? L" (some still plugged)" : L"");
}

// A panel to give back to the desktop. The adapter LUID changes when the GPU driver is reset or
// updated; the monitor device path does not, so it finds the panel again.
struct PanelKey
{
    LUID adapter;
    UINT32 targetId;
    std::wstring path;
};

// Maps each key to the target its panel is on now. A panel that is not listed (e.g. for a moment during
// a GPU reset) is waited for a few seconds, then dropped: there is nothing to give back.
static std::vector<std::pair<LUID, UINT32>> CurrentKeys(const std::vector<PanelKey>& keys)
{
    std::vector<std::pair<LUID, UINT32>> out;
    std::vector<bool> found(keys.size());
    for (int attempt = 0; attempt < 16; attempt++)
    {
        auto targets = EnumTargets();
        for (size_t k = 0; k < keys.size(); k++)
        {
            if (found[k]) continue;
            auto& key = keys[k];
            for (auto& t : targets)
            {
                bool same = t.targetId == key.targetId && t.adapter.LowPart == key.adapter.LowPart && t.adapter.HighPart == key.adapter.HighPart;
                bool moved = !same && !key.path.empty() && _wcsicmp(t.monitorDevicePath.c_str(), key.path.c_str()) == 0;
                if (!same && !moved) continue;
                if (moved) Log(L"revert: panel %u is now target %u on adapter %08X:%08X", key.targetId, t.targetId, t.adapter.HighPart, t.adapter.LowPart);
                out.push_back({ t.adapter, t.targetId });
                found[k] = true;
                break;
            }
        }
        if (std::all_of(found.begin(), found.end(), [](bool f) { return f; })) return out;
        Beat();
        Sleep(250);
    }
    for (size_t k = 0; k < keys.size(); k++)
        if (!found[k]) Log(L"revert: panel %u is not connected", keys[k].targetId);
    return out;
}

// Idempotent: panels back on the desktop first, then remove the virtual monitors.
static void RevertTargets(const std::vector<PanelKey>& keys)
{
    auto targets = CurrentKeys(keys);
    for (auto& [luid, id] : targets)
    {
        auto spec = GetSpecialization(luid, id);
        if (spec.ok && !spec.enabled) continue; // never removed (window mode) or already back
        for (int i = 0; i < 10; i++)
        {
            Beat();
            LONG r = SetSpecialization(luid, id, false);
            if (r == ERROR_SUCCESS) Log(L"revert: panel %u returned to the desktop", id);
            if (r == ERROR_SUCCESS || r == ERROR_GEN_FAILURE) break; // GEN_FAILURE: nothing to undo
            Log(L"revert: SetSpecialization(false) -> %ld", r);
            Sleep(500);
        }
    }
    for (auto& [luid, id] : targets)
        for (int i = 0; i < 20 && !TargetActive(luid, id); i++)
        {
            Beat();
            Sleep(250);
        }

    UnplugVirtual();
    ClearRuntime();

    bool allActive = true;
    for (auto& [luid, id] : targets) allActive &= TargetActive(luid, id);
    if (!allActive)
    {
        Beat();
        Sleep(1000);
        Beat();
        allActive = true;
        for (auto& [luid, id] : targets) allActive &= TargetActive(luid, id);
        if (!allActive) ForceExtendTopology();
    }
    Log(L"revert: done, all panels active=%d", allActive);
}

static std::vector<PanelKey> Keys(const std::vector<PanelTarget>& ts)
{
    std::vector<PanelKey> out;
    for (auto& t : ts) out.push_back({ t.adapter, t.targetId, t.monitorDevicePath });
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
    std::vector<PanelKey> targets; // each "lo:hi:id", then the monitor device path
    for (int i = 4; i < argc; i++)
    {
        unsigned long lo = 0, id = 0;
        long hi = 0;
        if (swscanf_s(argv[i], L"%lu:%ld:%lu", &lo, &hi, &id) != 3) continue;
        std::wstring path = i + 1 < argc && wcschr(argv[i + 1], L'#') ? argv[++i] : L"";
        targets.push_back({ LUID{ lo, hi }, (UINT32)id, path });
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
        cmd += L" " + std::to_wstring(t.adapter.LowPart) + L":" + std::to_wstring(t.adapter.HighPart) + L":" + std::to_wstring(t.targetId) + L" \"" +
               t.monitorDevicePath + L"\"";
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

#pragma region Compositor

enum class Outcome
{
    Stop,           // user asked to stop, test finished, or another panel ended the session
    DisplaysOff,    // Windows powered the virtual monitors off; panel released so it can sleep
    RefreshChanged, // panel rate changed and could not be re-applied in place; restart this panel only
    Lost,           // panel/GPU/driver went away; caller reverts and retries
};

// The driver updates driverHeartbeat once a second. If the UMDF host died and restarted,
// the shared section and its textures outlive it: without this check the compositor would
// keep scanning out the last frozen frame forever with every heartbeat looking healthy.
// True while the driver is alive; throttled so callers can ask every frame.
static bool DriverAlive(SharedView& shm, ULONGLONG& nextCheck)
{
    ULONGLONG now = GetTickCount64();
    if (now < nextCheck) return true;
    nextCheck = now + 500;
    LONG64 beat = SdReadI64(&shm->driverHeartbeat);
    return !beat || now - (ULONGLONG)beat <= 5000;
}

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
//
// Frames are copied into the idle primary as soon as the driver signals them and the
// scanout task is queued at once, so the flip takes effect at the next vblank; the
// WaitForVBlank after each submit only paces submissions to the panel rate. (The old
// order -- wait for the vblank, then copy -- made every frame that arrived mid-cycle
// miss the vblank it could have made.)
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
        if (acq.ErrorCode() == wdc::DisplayManagerResult::TargetAccessDenied && !SpecializedDisplaysLicensed())
            Log(L"this Windows edition cannot own a display; set mode=auto or mode=window in config.ini");
        return Outcome::Lost;
    }
    int activeMilliHz = g_desiredMilliHz[p.index].load();
    if (!activeMilliHz) activeMilliHz = p.refreshMilliHz;
    auto pickMode = [&](const wdc::DisplayPath& ph, int milliHz) {
        wdc::DisplayModeInfo best{ nullptr };
        double bestDiff = 1e9;
        for (auto&& m : ph.FindModes(wdc::DisplayModeQueryOptions::OnlyPreferredResolution))
        {
            auto v = m.PresentationRate().VerticalSyncRate;
            double diff = std::abs((double)v.Numerator / v.Denominator - milliHz / 1000.0);
            if (diff < bestDiff)
            {
                bestDiff = diff;
                best = m;
            }
        }
        return best;
    };
    auto state = acq.State();
    auto path = state.ConnectTarget(target);
    path.IsInterlaced(false);
    path.Scaling(wdc::DisplayPathScaling::Identity);
    path.SourcePixelFormat(wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized);

    auto best = pickMode(path, activeMilliHz);
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

    // Switches the owned path to another refresh rate without releasing the panel, so a
    // rate change costs one mode switch instead of a teardown. False = caller restarts.
    auto reapplyRate = [&](int milliHz) {
        try
        {
            auto read = mgr.TryAcquireTargetsAndReadCurrentState(targets);
            if (read.ErrorCode() != wdc::DisplayManagerResult::Success) return false;
            auto st = read.State();
            auto ph = st.GetPathForTarget(target);
            auto mode = pickMode(ph, milliHz);
            if (!mode) return false;
            ph.ApplyPropertiesFromMode(mode);
            return st.TryApply(wdc::DisplayStateApplyOptions::None).Status() == wdc::DisplayStateOperationStatus::Success;
        }
        catch (...)
        {
            return false;
        }
    };

    winrt::com_ptr<IDXGIFactory6> factory;
    factory.capture(&CreateDXGIFactory2, 0);
    winrt::com_ptr<IDXGIAdapter4> adapter;
    adapter.capture(factory, &IDXGIFactory6::EnumAdapterByLuid, panel.adapter);
    winrt::com_ptr<ID3D11Device> d0;
    winrt::com_ptr<ID3D11DeviceContext> c0;
    winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d0.put(), nullptr, c0.put()));
    auto dev = d0.as<ID3D11Device5>();
    auto ctx = c0.as<ID3D11DeviceContext4>();
    // Same reasoning as window mode: these copies sit on the path of every frame.
    dev.as<IDXGIDevice>()->SetGPUThreadPriority(7);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    StretchBlitter blitter;
    bool blitterOk = blitter.Init(dev.get());
    if (!blitterOk) Log(L"panel %u: stretch blitter unavailable, scaled modes will be cropped", panel.targetId);
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
    FrameEventWaiter waiter(src.size());

    LARGE_INTEGER freq, t0, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    UINT64 vblanks = 0, presents = 0, stalls = 0;
    UINT64 pendingFence = 0; // our copies that still hold ring reservations
    int nextPrimary = 0;
    bool presented = false;
    ULONGLONG darkSince = 0;
    ULONGLONG loopStart = GetTickCount64();
    ULONGLONG nextDriverCheck = 0;
    bool poweredOff = false;
    Outcome outcome = Outcome::Stop;

    while (!ShouldExit())
    {
        Beat();
        if (!DriverAlive(shm, nextDriverCheck))
        {
            Log(L"panel %u: driver heartbeat lost, ending the session", panel.targetId);
            outcome = Outcome::Lost;
            break;
        }
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
            // The source can report a transient status while the path is being applied.
            if (!presented && GetTickCount64() - loopStart < 3000)
            {
                if (!Nap(50)) break;
                continue;
            }
            Log(L"panel %u source status %d", panel.targetId, static_cast<int>(status));
            outcome = Outcome::Lost;
            break;
        }

        int desiredMilliHz = g_desiredMilliHz[p.index].load();
        if (desiredMilliHz && desiredMilliHz != activeMilliHz)
        {
            if (reapplyRate(desiredMilliHz))
            {
                Log(L"panel %u switched to %s Hz in place", panel.targetId, FormatHz(desiredMilliHz).c_str());
                activeMilliHz = desiredMilliHz;
                presented = false;
            }
            else
            {
                Log(L"panel %u: could not re-apply %s Hz in place, restarting this panel", panel.targetId, FormatHz(desiredMilliHz).c_str());
                outcome = Outcome::RefreshChanged;
                break;
            }
        }

        // Copies issued last frame are almost always done by now; only wait if they are not.
        // The reservations are only released once the copies have finished, or the driver
        // could overwrite a ring buffer that is still being read (window mode already did).
        if (pendingFence && fence->GetCompletedValue() < pendingFence)
        {
            stalls++;
            fence->SetEventOnCompletion(pendingFence, fenceEvent);
            WaitForSingleObject(fenceEvent, 100);
            if (fence->GetCompletedValue() < pendingFence) continue;
        }
        for (auto& s1 : src) s1.Release(shm.get());
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
            if (!Nap(100)) break;
            if (deadline && GetTickCount64() >= deadline) break;
            continue;
        }
        darkSince = 0;

        if (!presented || FramesChanged(shm.get(), src))
        {
            int back = nextPrimary;
            nextPrimary ^= 1;
            int rotations[SD_MAX_MONITORS] = {};
            for (size_t k = 0; k < src.size() && p.first + k < SD_MAX_MONITORS; k++) rotations[k] = g_rotations[p.first + k].load();
            DrawRegions(dev.get(), ctx.get(), shm.get(), driverProc, src, p.regions, primTex[back].get(), primRtv[back].get(), (UINT)res.Width,
                (UINT)res.Height, back, blitterOk ? &blitter : nullptr, rotations);
            ctx->Signal(fence.get(), ++fenceValue);
            ctx->Flush();
            pendingFence = fenceValue;

            auto task = pool.CreateTask();
            task.SetScanout(scan[back]);
            task.SetWait(displayFence, fenceValue);
            pool.ExecuteTask(task);
            presents++;
            presented = true;
            device.WaitForVBlank(source); // pace to the panel; the flip lands on this edge
            vblanks++;
        }
        else
        {
            HANDLE wake[SD_MAX_MONITORS];
            DWORD n = waiter.Build(src, wake, ARRAYSIZE(wake));
            if (FramesChanged(shm.get(), src)) continue;
            if (n) WaitForMultipleObjects(n, wake, FALSE, 100);
            else
            {
                device.WaitForVBlank(source);
                vblanks++;
            }
        }

        if (deadline && GetTickCount64() >= deadline) break;
    }

    if (pendingFence)
    {
        fence->SetEventOnCompletion(pendingFence, fenceEvent);
        WaitForSingleObject(fenceEvent, 250);
    }
    for (auto& s1 : src)
    {
        s1.Release(shm.get());
        s1.Drop();
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


#pragma region Window mode

// Without display specialization (see SpecializedDisplaysLicensed) a panel cannot be taken off the
// desktop and owned. Window mode leaves every panel on the desktop instead, parked diagonally off
// the bottom-right corner of all other displays so only one corner touches them, and shows its
// split monitors in a topmost window covering it. A guard thread keeps the cursor and application
// windows off the parked panels. Costs one DWM composition step compared to exclusive mode.

// Where the panel's scanout is right now: 0..99 = percent of the active area, 100 = vertical blank.
// Used by test runs to show when split monitor frames arrive relative to the panel's refresh.
struct ScanlineProbe
{
    D3DKMT_GETSCANLINE query{};
    UINT height = 0;

    bool Open(const std::wstring& gdiName, UINT activeHeight)
    {
        D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME o{};
        wcsncpy_s(o.DeviceName, gdiName.c_str(), _TRUNCATE);
        if (D3DKMTOpenAdapterFromGdiDisplayName(&o) != 0) return false;
        query.hAdapter = o.hAdapter;
        query.VidPnSourceId = o.VidPnSourceId;
        height = activeHeight;
        return true;
    }

    int Percent()
    {
        if (!height || D3DKMTGetScanLine(&query) != 0) return -1;
        return query.InVerticalBlank ? 100 : (int)std::min<UINT>(99, query.ScanLine * 100 / height);
    }

    ~ScanlineProbe()
    {
        if (!query.hAdapter) return;
        D3DKMT_CLOSEADAPTER c{ query.hAdapter };
        D3DKMTCloseAdapter(&c);
    }
};

static LRESULT CALLBACK CompositorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Window-mode counterpart of Composite: shows one panel's split monitors in a topmost window
// covering the (parked) panel, presenting through a flip-model swap chain on the panel's GPU.
static Outcome CompositeWindow(SharedView& shm, const PanelSession& p, ULONGLONG deadline)
{
    const PanelTarget& panel = p.target;
    PanelLocator where{ panel.adapter, panel.targetId };
    RECT rc{};
    if (!where.Rect(rc))
    {
        Log(L"panel %u is not on the desktop", panel.targetId);
        return Outcome::Lost;
    }

    winrt::com_ptr<IDXGIFactory6> factory;
    factory.capture(&CreateDXGIFactory2, 0);
    winrt::com_ptr<IDXGIAdapter4> adapter;
    adapter.capture(factory, &IDXGIFactory6::EnumAdapterByLuid, panel.adapter);
    winrt::com_ptr<ID3D11Device> d0;
    winrt::com_ptr<ID3D11DeviceContext> c0;
    winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d0.put(), nullptr, c0.put()));
    auto dev = d0.as<ID3D11Device5>();
    auto ctx = c0.as<ID3D11DeviceContext4>();
    // Our copies are tiny and sit on the path of every frame: let them jump the GPU queue.
    dev.as<IDXGIDevice>()->SetGPUThreadPriority(7);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    StretchBlitter blitter;
    bool blitterOk = blitter.Init(dev.get());
    winrt::com_ptr<ID3D11Fence> fence;
    fence.capture(dev, &ID3D11Device5::CreateFence, 0, D3D11_FENCE_FLAG_NONE);
    UINT64 fenceValue = 0;
    winrt::handle fenceEvent{ CreateEventW(nullptr, FALSE, FALSE, nullptr) };

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = CompositorWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kCompositorWindowClass;
    RegisterClassExW(&wc); // fails harmlessly if another panel thread registered it
    // Exactly covering the panel, so Windows scans the swap chain out directly (independent flip)
    // instead of compositing it once more. Filters of the GPU vendor's app (e.g. NVIDIA RTX Dynamic
    // Vibrance) then treat SplitDisplay like a game; exclude splitdisplay.exe there.
    UINT width = (UINT)(rc.right - rc.left), height = (UINT)(rc.bottom - rc.top);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kCompositorWindowClass, L"SplitDisplay", WS_POPUP, rc.left, rc.top,
        (int)width, (int)height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) throw std::runtime_error("could not create the compositor window");
    struct WindowOwner // destroyed on every way out, including exceptions (e.g. a GPU reset mid-resize)
    {
        HWND h;
        ~WindowOwner() { DestroyWindow(h); }
    } windowOwner{ hwnd };

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Three buffers: one on screen, one waiting for the next vblank, one to draw the next frame into.
    sd.BufferCount = 3;
    // FLIP_SEQUENTIAL, not FLIP_DISCARD: contents of each buffer survive its present, so a
    // later pass can leave regions (and rects) that have not changed untouched instead of
    // recopying the whole panel every frame -- with DISCARD they would be undefined.
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // With a waitable swap chain, Present never blocks on frame latency: a present right after the
    // previous one must not wait for that one to reach the screen.
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    winrt::com_ptr<IDXGISwapChain1> swap;
    winrt::check_hresult(factory->CreateSwapChainForHwnd(dev.get(), hwnd, &sd, nullptr, nullptr, swap.put()));
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    CloseHandle(swap.as<IDXGISwapChain2>()->GetFrameLatencyWaitableObject()); // not waited on, see above
    // With a D3D11 flip-model swap chain, buffer 0 is always the current back buffer.
    winrt::com_ptr<ID3D11Texture2D> back;
    back.capture(swap, &IDXGISwapChain1::GetBuffer, 0);
    winrt::com_ptr<ID3D11RenderTargetView> backRtv;
    winrt::check_hresult(dev->CreateRenderTargetView(back.get(), nullptr, backRtv.put()));
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    Log(L"panel %u: window mode, compositor window at (%ld,%ld) %ux%u", panel.targetId, rc.left, rc.top, width, height);

    EnableDebugPrivilege();
    winrt::handle driverProc{ OpenProcess(PROCESS_DUP_HANDLE, FALSE, shm->driverPid) };
    if (!driverProc)
    {
        Log(L"cannot open driver host process %lu: %lu", shm->driverPid, GetLastError());
        return Outcome::Lost;
    }
    std::vector<FrameSource> src(p.regions.size());
    FrameEventWaiter waiter(src.size());
    ULONGLONG nextDriverCheck = 0;
    auto swap3 = swap.try_as<IDXGISwapChain3>();
    for (int i = 0; i < (int)src.size(); i++)
    {
        src[i].index = (int)p.first + i;
        LUID a = shm->mon[src[i].index].adapter;
        if ((a.LowPart || a.HighPart) && (a.LowPart != panel.adapter.LowPart || a.HighPart != panel.adapter.HighPart))
            Log(L"warning: virtual monitor %d renders on GPU %08X:%08X but the panel is on %08X:%08X; cross-GPU copy is not supported, it will stay black",
                src[i].index, a.HighPart, a.LowPart, panel.adapter.HighPart, panel.adapter.LowPart);
    }

    LARGE_INTEGER freq, t0, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    UINT64 presents = 0, stalls = 0;
    UINT64 pendingFence = 0;
    bool presented = false;
    ULONGLONG nextCheck = 0, nextStats = 0;
    std::vector<LONG64> statsSeq(src.size());
    std::vector<LONGLONG> presentQpc(64); // test runs: when each present was issued
    ScanlineProbe scan;
    UINT64 arrivalAt[11] = {}; // test runs: panel scanout position when frames arrive, in 10% steps + vblank
    double presentMs = 0; // test runs: time spent copying + presenting
    if (deadline) scan.Open(GdiNameOfTarget(panel.adapter, panel.targetId), height);
    UINT64 statsPresents = 0;
    Outcome outcome = Outcome::Stop;

    while (!ShouldExit())
    {
        Beat();
        if (!DriverAlive(shm, nextDriverCheck))
        {
            Log(L"panel %u: driver heartbeat lost, ending the session", panel.targetId);
            outcome = Outcome::Lost;
            break;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

        // Notice when the desktop layout moves the panel, and stay above other topmost windows
        // (e.g. a secondary taskbar on the panel).
        if (GetTickCount64() >= nextCheck)
        {
            nextCheck = GetTickCount64() + 500;
            RECT cur{};
            if (!where.Rect(cur))
            {
                Log(L"panel %u left the desktop", panel.targetId);
                outcome = Outcome::Lost;
                break;
            }
            if (!EqualRect(&cur, &rc))
            {
                // Windows moved the panel off its parking spot or changed its resolution (a split monitor
                // switched off, Win+P, display settings). Following it would leave it among the other
                // displays, so the session ends and the next one parks it and lays out the split again.
                // Counted like a lost panel, so a layout that keeps changing cannot restart forever.
                Log(L"panel %u moved to (%ld,%ld) %ldx%ld, setting up the split again", panel.targetId, cur.left, cur.top, cur.right - cur.left,
                    cur.bottom - cur.top);
                outcome = Outcome::Lost;
                break;
            }
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        if (pendingFence && fence->GetCompletedValue() < pendingFence)
        {
            stalls++;
            fence->SetEventOnCompletion(pendingFence, fenceEvent.get());
            WaitForSingleObject(fenceEvent.get(), 100);
            // Still copying: the ring buffers it reads stay reserved, or the driver could overwrite them.
            if (fence->GetCompletedValue() < pendingFence) continue;
        }
        for (auto& s : src) s.Release(shm.get());
        pendingFence = 0;

        // Windows turned the split monitors off; it blanks the panel itself, as it is a desktop monitor.
        if (!AnyVirtualSignal(shm.get(), p))
        {
            presented = false;
            if (!Nap(100)) break;
            if (deadline && GetTickCount64() >= deadline) break;
            continue;
        }

        // Present as soon as a split monitor frame arrives, with sync interval 0 on a swap chain that
        // does not allow tearing: the flip still waits for the vblank, but a newer present replaces one
        // that is still waiting. So every vblank shows the newest frame that made it in time, whatever
        // the phase between the split monitors and the panel, and nothing queues up behind a late frame.
        bool ready = !presented || FramesChanged(shm.get(), src);
        LARGE_INTEGER tArrive;
        QueryPerformanceCounter(&tArrive);
        if (ready)
        {
            if (deadline)
            {
                int at = scan.Percent();
                if (at >= 0) arrivalAt[at / 10]++;
            }
            // No blanket clear: the regions tile the panel, buffers keep their contents
            // (FLIP_SEQUENTIAL), and DrawRegions tracks per buffer what is already there --
            // it blacks out exactly the regions that have no frame yet.
            int rotations[SD_MAX_MONITORS] = {};
            for (size_t k2 = 0; k2 < src.size() && p.first + k2 < SD_MAX_MONITORS; k2++) rotations[k2] = g_rotations[p.first + k2].load();
            int targetIndex = swap3 ? (int)swap3->GetCurrentBackBufferIndex() : 0;
            DrawRegions(dev.get(), ctx.get(), shm.get(), driverProc.get(), src, p.regions, back.get(), backRtv.get(), width, height,
                targetIndex, blitterOk ? &blitter : nullptr, rotations);
            ctx->Signal(fence.get(), ++fenceValue);
            pendingFence = fenceValue;
            LARGE_INTEGER presentAt;
            QueryPerformanceCounter(&presentAt);
            HRESULT hr = swap->Present(0, 0);
            LARGE_INTEGER tDone;
            QueryPerformanceCounter(&tDone);
            presentMs += (tDone.QuadPart - tArrive.QuadPart) * 1000.0 / freq.QuadPart;
            UINT presentId = 0;
            if (deadline && SUCCEEDED(swap->GetLastPresentCount(&presentId))) presentQpc[presentId % presentQpc.size()] = presentAt.QuadPart;
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            {
                Log(L"panel %u: GPU device lost (0x%08X)", panel.targetId, (UINT)hr);
                outcome = Outcome::Lost;
                break;
            }
            presents++;
            presented = true;
        }
        else
        {
            // Sleep until a split monitor finishes its next frame: the driver signals the monitor's
            // fence once per frame, so a new frame is picked up right away instead of at the next vblank.
            // Each event is armed once per frame (a wait that stays pending is not registered again), and
            // the frames are checked again afterwards, as one may have completed just before arming.
            HANDLE wake[SD_MAX_MONITORS];
            DWORD n = waiter.Build(src, wake, ARRAYSIZE(wake));
            if (FramesChanged(shm.get(), src)) continue;
            // The events wake us the moment a frame lands; the timeout is only a safety
            // net (deadline and panel checks below), so it can be long and idle-cheap.
            if (n) WaitForMultipleObjects(n, wake, FALSE, 100);
            else Sleep(2);
        }

        // Test runs report the frame rates every 10 s: split monitor frames in, presents out.
        if (deadline && GetTickCount64() >= nextStats)
        {
            LONG64 busiest = 0;
            for (size_t k = 0; k < src.size(); k++)
            {
                LONG64 seq = shm->mon[src[k].index].frameSeq;
                busiest = std::max(busiest, seq - statsSeq[k]);
                statsSeq[k] = seq;
            }
            // Present -> start of scanout of the last present the display reports.
            double toScreenMs = -1;
            DXGI_FRAME_STATISTICS fs{};
            if (SUCCEEDED(swap->GetFrameStatistics(&fs)) && fs.SyncQPCTime.QuadPart && presentQpc[fs.PresentCount % presentQpc.size()])
                toScreenMs = (fs.SyncQPCTime.QuadPart - presentQpc[fs.PresentCount % presentQpc.size()]) * 1000.0 / freq.QuadPart;
            if (nextStats)
            {
                wchar_t hist[160] = {};
                for (int b = 0; b < 11; b++)
                    swprintf_s(hist + wcslen(hist), 160 - wcslen(hist), L" %llu", arrivalAt[b]);
                UINT64 n = std::max<UINT64>(1, presents - statsPresents);
                Log(L"panel %u: busiest split monitor %.1f frames/s, %.1f presents/s, present->scanout %.1f ms, copy+present %.2f ms, presents at scanout 0-10%%..90-100%%,vblank:%s",
                    panel.targetId, busiest / 10.0, (presents - statsPresents) / 10.0, toScreenMs, presentMs / n, hist);
            }
            memset(arrivalAt, 0, sizeof(arrivalAt));
            presentMs = 0;
            statsPresents = presents;
            nextStats = GetTickCount64() + 10000;
        }

        if (deadline && GetTickCount64() >= deadline) break;
    }

    if (pendingFence)
    {
        fence->SetEventOnCompletion(pendingFence, fenceEvent.get());
        WaitForSingleObject(fenceEvent.get(), 250);
    }
    for (auto& s : src)
    {
        s.Release(shm.get());
        s.Drop();
    }
    QueryPerformanceCounter(&now);
    double total = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
    Log(L"panel %u composited %.1fs in window mode: %llu presents (%.2f/s), %llu copy stalls", panel.targetId, total, presents,
        presents / std::max(total, 0.001), stalls);

    return outcome;
}

#pragma endregion

// One panel's thread: composite, sit out display power-off, repeat.
static Outcome PanelWorker(SharedView& shm, const PanelSession& p, ULONGLONG deadline)
{
    // MMCSS like the driver's frame threads; every panel worker is latency-critical,
    // not just the window-mode one that used to be the only privileged thread.
    DWORD avTask = 0;
    HANDLE avHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &avTask);
    struct AvRevert
    {
        HANDLE h;
        ~AvRevert() { if (h) AvRevertMmThreadCharacteristics(h); }
    } avRevert{ avHandle };
    try
    {
        winrt::init_apartment();
        if (g_windowMode) return CompositeWindow(shm, p, deadline);
        for (;;)
        {
            Outcome o = Composite(shm, p, deadline);
            if (o == Outcome::RefreshChanged)
            {
                // Only this panel restarts; the virtual monitors were never unplugged.
                Log(L"panel %u: restarting compositor for the new refresh rate", p.target.targetId);
                continue;
            }
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

// Gives the split monitors the panel's ICC profile. Without this, a calibrated panel's
// colours shift the moment it is split, because the virtual monitors get the system
// default (sRGB) association. Best effort: association keys off GDI device names, which
// Windows renumbers between sessions, so this runs at every session start. Per-user scope
// matches how calibration tools install display profiles.
static void InheritColorProfiles(const std::vector<PanelSession>& ps)
{
    for (auto& p : ps)
    {
        if (p.target.gdiName.empty()) continue;
        DWORD bytes = 0;
        if (!WcsGetDefaultColorProfileSize(WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER, p.target.gdiName.c_str(), CPT_ICC, CPST_NONE, 0, &bytes) || !bytes)
            continue;
        std::wstring profile(bytes / sizeof(wchar_t) + 1, L'\0');
        if (!WcsGetDefaultColorProfile(WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER, p.target.gdiName.c_str(), CPT_ICC, CPST_NONE, 0, bytes,
                profile.data()))
            continue;
        profile.resize(wcslen(profile.c_str()));
        if (profile.empty()) continue;
        for (UINT i = 0; i < p.regions.size(); i++)
        {
            auto gdi = GdiNameOfMonitor(VirtualName(p.first + i));
            if (gdi.empty()) continue;
            if (WcsAssociateColorProfileWithDevice(WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER, profile.c_str(), gdi.c_str()))
            {
                WcsSetDefaultColorProfile(WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER, gdi.c_str(), CPT_ICC, CPST_NONE, 0, profile.c_str());
                Log(L"color profile '%s' inherited by Split %u", profile.c_str(), p.first + i + 1);
            }
        }
    }
}

// One full split session: plug every panel's monitors, take the panels over, composite until
// stop / loss. Returns true if the session ended because a stop was requested.
static bool RunSession(std::vector<PanelSession>& ps, ULONGLONG deadline)
{
    SharedView shm;
    if (!shm.Open()) throw std::runtime_error("SplitDisplay driver not loaded");
    for (size_t i = 0; i < ps.size() && i < SD_MAX_MONITORS; i++)
    {
        ps[i].index = (int)i;
        g_desiredMilliHz[i] = ps[i].refreshMilliHz;
    }
    for (auto& rot : g_rotations) rot = (int)DISPLAYCONFIG_ROTATION_IDENTITY;

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
    std::vector<RECT> otherRects;
    for (auto& t : EnumTargets())
    {
        bool mine = false;
        for (auto& p : ps) mine |= SameTarget(p.target, t);
        if (t.active && !t.isVirtual && !mine)
        {
            others.push_back({ t.monitorDevicePath, t.position });
            otherRects.push_back({ t.position.x, t.position.y, t.position.x + (LONG)t.width, t.position.y + (LONG)t.height });
        }
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
            SdMonitorConfig m{ (UINT32)(r.right - r.left), (UINT32)(r.bottom - r.top), (UINT32)p.refreshMilliHz, 0, {}, 0, 0 };
            // The region's share of the real panel, so its EDID reports true physical size
            // (and with it a correct Windows scaling recommendation) on any panel.
            if (p.target.widthMm && p.target.width) m.widthMm = (UINT32)((UINT64)p.target.widthMm * (r.right - r.left) / p.target.width);
            if (p.target.heightMm && p.target.height) m.heightMm = (UINT32)((UINT64)p.target.heightMm * (r.bottom - r.top) / p.target.height);
            for (int hz : p.rates)
                if (m.rateCount < SD_MAX_RATES) m.ratesMilliHz[m.rateCount++] = (UINT32)hz;
            cfg.mon[cfg.count++] = m;
            desktop.push_back({ p.target.position.x + r.left, p.target.position.y + r.top, p.target.position.x + r.right, p.target.position.y + r.bottom });
            desc += L" " + std::to_wstring(r.right - r.left) + L"x" + std::to_wstring(r.bottom - r.top) + L"@" + std::to_wstring(r.left) + L"," +
                    std::to_wstring(r.top);
        }
        std::wstring rates;
        for (int hz : p.rates) rates += (rates.empty() ? L"" : L"/") + FormatHz(hz);
        Log(L"panel '%s' %ux%u@%s Hz (supports %s) at (%ld,%ld), scale %d%%, monitors %u..%u:%s", p.cfg.name.c_str(), p.target.width,
            p.target.height, FormatHz(p.refreshMilliHz).c_str(), rates.empty() ? L"?" : rates.c_str(), p.target.position.x, p.target.position.y, p.scale,
            p.first + 1, cfg.count, desc.c_str());
    }

    // Counters survive from earlier sessions, so require fresh frames after this plug.
    LONG64 base[SD_MAX_MONITORS];
    for (int i = 0; i < SD_MAX_MONITORS; i++) base[i] = (LONG64)SdReadI64(&shm->mon[i].frameSeq);
    SdWriteConfig(shm.get(), cfg);
    InterlockedIncrement(&shm->modesGeneration);
    InterlockedExchange(&shm->desiredPlugged, 1);
    shm.Kick();
    auto live = [&] {
        for (UINT i = 0; i < cfg.count; i++)
            if (!shm->mon[i].plugged || SdReadI64(&shm->mon[i].frameSeq) <= base[i] || !IsTargetActive(VirtualName(i).c_str())) return false;
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
        p.scales.resize(p.regions.size());
        for (UINT i = 0; i < p.regions.size(); i++)
        {
            names.push_back(VirtualName(p.first + i));
            // Per region, remembered across sessions; a region without a stored scale
            // inherits the physical panel's, as before. Changes made later in Windows
            // Settings are captured by the session loop instead of being overwritten.
            int want = p.scales[i] ? p.scales[i] : p.scale;
            p.scales[i] = want;
            if (want) SetScalePercent({ names.back() }, want);
        }
        // Windows restores the rate it last saw on each monitor; make them all the panel's rate, so a
        // later difference can only be a user change (which then applies to the whole panel).
        ForceRefresh(names, p.refreshMilliHz);
    }
    Beat();

    if (g_windowMode)
    {
        // Windows restores the layout it saved for this set of monitors, which can switch a panel off
        // (e.g. one saved by an exclusive-mode session). Window mode needs the panels on the desktop.
        for (auto& p : ps)
        {
            // Right after the split monitors appear, Windows may still refuse (ERROR_GEN_FAILURE): retry.
            for (int i = 0; i < 20 && !TargetActive(p.target.adapter, p.target.targetId); i++)
            {
                if (i % 4 == 0) ActivateTarget(p.target.adapter, p.target.targetId);
                if (!Nap(250)) return true;
            }
            if (!TargetActive(p.target.adapter, p.target.targetId)) throw std::runtime_error("could not put a panel back on the desktop");
        }

        // The panels stay on the desktop: park them one after another off the bottom-right corner of
        // everything else, so each touches the rest of the desktop at a single corner only.
        RECT box = desktop[0];
        for (auto& r : desktop) UnionRect(&box, &box, &r);
        for (auto& r : otherRects) UnionRect(&box, &box, &r);
        POINT park{ box.right, box.bottom };
        for (auto& p : ps)
        {
            others.push_back({ p.target.monitorDevicePath, park });
            Log(L"panel '%s' parked at (%ld,%ld)", p.cfg.name.c_str(), park.x, park.y);
            park.x += (LONG)p.target.width;
            park.y += (LONG)p.target.height;
        }
    }
    else
    {
        for (auto& p : ps)
        {
            LONG r = SetSpecialization(p.target.adapter, p.target.targetId, true);
            Log(L"panel '%s' removed from desktop -> %ld", p.cfg.name.c_str(), r);
            if (r != ERROR_SUCCESS) throw std::runtime_error("could not remove a panel from the desktop");
        }
        for (auto& p : ps)
            for (int i = 0; i < 40 && TargetActive(p.target.adapter, p.target.targetId); i++)
                if (!Nap(250)) return true;
    }
    // The split monitors take each panel's place in the desktop; everything else stays put.
    for (int i = 0; i < 10 && !ArrangeMonitors(desktop, others); i++)
        if (!Nap(300)) return true;
    // Moving a panel can make Windows fall back to 60 Hz; it must run at the rate of its split monitors.
    if (g_windowMode)
        for (auto& p : ps) ForceRefreshTarget(p.target.adapter, p.target.targetId, p.refreshMilliHz);
    WriteRuntime(ps);
    InheritColorProfiles(ps);

    WindowGuard guard;
    if (g_windowMode)
    {
        std::vector<ParkedPanel> parked;
        for (auto& p : ps) parked.push_back({ PanelLocator{ p.target.adapter, p.target.targetId }, VirtualName(p.first), desktop[p.first], {} });
        guard.Start(std::move(parked));
    }
    // The split is back: undo the pile-up Windows made of the windows while it was gone.
    RestoreWindowLayout();

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
    ULONGLONG nextVisible = 0;
    int hiddenPolls = 0;
    int corrections = 0;
    ULONGLONG correctionWindow = 0;
    ULONGLONG superviseStart = GetTickCount64();
    std::vector<ULONGLONG> rateGrace(ps.size(), superviseStart + 10000); // let rate switches settle before judging them
    std::map<std::pair<int, int>, std::pair<int, int>> scaleCandidates;  // (panel, region) -> (percent, matching polls)
    for (;;)
    {
        // The session was locked, or Windows switched to another desktop (Ctrl+Alt+Del, a secure
        // prompt). A window-mode panel would stay black meanwhile, so end the session and give the
        // panels back whole; the next one starts once the desktop is back. Two polls in a row, so a
        // single failed query cannot tear the split down.
        if (g_windowMode && !g_restart && GetTickCount64() >= nextVisible)
        {
            nextVisible = GetTickCount64() + 150;
            const wchar_t* why = HiddenReason();
            hiddenPolls = why ? hiddenPolls + 1 : 0;
            // Held briefly, so a prompt that is dismissed right away costs nothing.
            if (hiddenPolls >= 4)
            {
                Log(L"%s: giving the panels back meanwhile", why);
                SaveWindowLayout();
                g_restart = true;
                g_abort = true;
            }
        }

        if (GetTickCount64() >= nextAlign)
        {
            nextAlign = GetTickCount64() + 2000;
            if (GetTickCount64() - correctionWindow > 60000)
            {
                correctionWindow = GetTickCount64();
                corrections = 0;
            }
            // One CCD pass answers every question below (drifted? rates? scales? rotations?);
            // this used to be a full QueryDisplayConfig per monitor per question.
            auto snap = SnapshotMonitors();
            for (auto& e : snap)
            {
                if (e.name.rfind(L"Split ", 0) != 0) continue;
                int n = _wtoi(e.name.c_str() + 6);
                if (n >= 1 && n <= SD_MAX_MONITORS) g_rotations[n - 1] = (int)e.rotation;
            }
            for (auto& p : ps)
            {
                // If Windows keeps undoing it, stop fighting for this minute.
                if (corrections < 5 && !p.regions.empty())
                {
                    const MonitorSnapshot* first = FindInSnapshot(snap, VirtualName(p.first));
                    bool drift = false, complete = first != nullptr;
                    if (first)
                    {
                        LONG ox = first->position.x - p.regions[0].left, oy = first->position.y - p.regions[0].top;
                        for (UINT i = 0; i < p.regions.size(); i++)
                        {
                            auto* e = FindInSnapshot(snap, VirtualName(p.first + i));
                            if (!e) complete = false;
                            else if (e->position.x != ox + p.regions[i].left || e->position.y != oy + p.regions[i].top) drift = true;
                        }
                    }
                    if (complete && drift && KeepGroupAligned(p.first, p.regions)) corrections++;
                }
                int desired = g_desiredMilliHz[p.index].load();
                if (g_windowMode && corrections < 5)
                {
                    for (auto& e : snap)
                    {
                        if (e.adapter.LowPart != p.target.adapter.LowPart || e.adapter.HighPart != p.target.adapter.HighPart ||
                            e.targetId != p.target.targetId || !e.refreshMilliHz)
                            continue;
                        if (std::abs(e.refreshMilliHz - desired) <= 100) continue;
                        Log(L"parked panel '%s' runs at %s Hz, switching it back to %s Hz", p.cfg.name.c_str(),
                            FormatHz(e.refreshMilliHz).c_str(), FormatHz(desired).c_str());
                        ForceRefreshTarget(p.target.adapter, p.target.targetId, desired);
                        corrections++;
                    }
                }

                // A split monitor's refresh rate was changed (e.g. in Settings): the whole panel
                // follows, in place -- the driver updates its mode lists and the exclusive
                // compositor re-applies its mode, so nothing is unplugged and nothing flickers
                // beyond the panel's own mode switch.
                for (UINT i = 0; i < p.regions.size(); i++)
                {
                    auto* e = FindInSnapshot(snap, VirtualName(p.first + i));
                    if (!e || !e->refreshMilliHz || std::abs(e->refreshMilliHz - desired) <= 100) continue;
                    if (GetTickCount64() < rateGrace[p.index])
                    {
                        ForceRefresh({ e->name }, desired); // still settling from the last switch: nudge, don't flip-flop
                        continue;
                    }
                    int milliHz = e->refreshMilliHz;
                    Log(L"'%s' split monitor %u switched to %s Hz: switching the whole display from %s Hz", p.cfg.name.c_str(), p.first + i + 1,
                        FormatHz(milliHz).c_str(), FormatHz(desired).c_str());
                    p.refreshMilliHz = milliHz;
                    g_desiredMilliHz[p.index] = milliHz;
                    rateGrace[p.index] = GetTickCount64() + 10000;
                    auto panels = LoadPanels();
                    for (auto& c : panels)
                        if (Matches(c, p.target)) c.refreshMilliHz = milliHz;
                    SavePanels(panels);
                    SdConfig liveCfg;
                    if (SdReadConfig(shm.get(), liveCfg))
                    {
                        for (UINT k = 0; k < p.regions.size() && p.first + k < (UINT)SD_MAX_MONITORS; k++)
                            liveCfg.mon[p.first + k].refreshMilliHz = (UINT32)milliHz;
                        SdWriteConfig(shm.get(), liveCfg);
                        InterlockedIncrement(&shm->modesGeneration);
                        shm.Kick();
                    }
                    std::vector<std::wstring> names;
                    for (UINT k = 0; k < p.regions.size(); k++) names.push_back(VirtualName(p.first + k));
                    ForceRefresh(names, milliHz);
                    if (g_windowMode) ForceRefreshTarget(p.target.adapter, p.target.targetId, milliHz);
                    break;
                }

                // Scale changes made in Settings belong to the user, not to the panel: remember
                // them per region (two matching polls, so a transition cannot be captured),
                // instead of overwriting them at every session start.
                if (GetTickCount64() - superviseStart > 15000)
                {
                    bool scaleDirty = false;
                    for (UINT i = 0; i < p.regions.size(); i++)
                    {
                        auto* e = FindInSnapshot(snap, VirtualName(p.first + i));
                        if (!e || !e->scalePercent || i >= p.scales.size()) continue;
                        auto& cand = scaleCandidates[{ p.index, (int)i }];
                        if (e->scalePercent == p.scales[i])
                        {
                            cand = { 0, 0 };
                            continue;
                        }
                        if (cand.first != e->scalePercent) cand = { e->scalePercent, 1 };
                        else cand.second++;
                        if (cand.second < 2) continue;
                        Log(L"'%s' split monitor %u scale changed to %d%%: remembering it", p.cfg.name.c_str(), p.first + i + 1, e->scalePercent);
                        p.scales[i] = e->scalePercent;
                        cand = { 0, 0 };
                        scaleDirty = true;
                    }
                    if (scaleDirty)
                    {
                        auto panels = LoadPanels();
                        for (auto& c : panels)
                            if (Matches(c, p.target)) c.scales = p.scales;
                        SavePanels(panels);
                    }
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
    guard.Stop();
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
    std::vector<std::wstring> watchdogCovered; // monitor device paths some watchdog already knows
    bool loggedOtherSession = false;
    int failures = 0;
    ULONGLONG firstFailure = 0;

    while (!StopRequested())
    {
        // Fast user switching: the instance mutex is per-session, so a second user's logon
        // task would start a second compositor that mistakes this session's specialized
        // panels for stale leftovers and reverts them. Only the console session composes;
        // everyone else waits until they become the console session.
        {
            DWORD mine = 0;
            bool console = ProcessIdToSessionId(GetCurrentProcessId(), &mine) && mine == WTSGetActiveConsoleSessionId();
            if (!console)
            {
                if (!loggedOtherSession)
                {
                    Log(L"another session is at the console; waiting");
                    loggedOtherSession = true;
                }
                if (!Nap(2000)) break;
                continue;
            }
            loggedOtherSession = false;
        }
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
            auto rates = SupportedRefreshRatesMilli(t);
            int want = cfg.refreshMilliHz ? cfg.refreshMilliHz : (int)std::lround(t.refreshHz * 1000);
            PanelSession session;
            session.cfg = cfg;
            session.target = t;
            session.regions = regions;
            session.scale = GetScalePercentForTarget(t.adapter, t.targetId);
            session.scales = cfg.scales;
            session.refreshMilliHz = ClosestRate(rates, want);
            session.rates = rates;
            ps.push_back(std::move(session));
        }
        std::vector<PanelTarget> sessionTargets;
        for (auto& p : ps) sessionTargets.push_back(p.target);

        // Arm again when a panel joins that no living watchdog has heard of (a display plugged
        // in mid-run and picked up by a later session); the older watchdog simply stays as a
        // second, now-redundant pair of hands -- reverting is idempotent.
        bool uncovered = false;
        for (auto& t : sessionTargets)
        {
            bool known = false;
            for (auto& covered : watchdogCovered) known |= _wcsicmp(covered.c_str(), t.monitorDevicePath.c_str()) == 0;
            uncovered |= !known;
        }
        if (!watchdogArmed || uncovered)
        {
            SpawnWatchdog(sessionTargets, testSeconds ? testSeconds + 30 : 0);
            watchdogArmed = true;
            for (auto& t : sessionTargets) watchdogCovered.push_back(t.monitorDevicePath);
        }

        g_windowMode = ChooseWindowMode();

        // Nothing of a window-mode split reaches the panel while the session is locked or another
        // desktop is up, so leave the displays whole until the desktop is back: the lock screen, the
        // sign-in screen and secure prompts then show on them as on ordinary monitors. Exclusive mode
        // scans the panel out itself, whatever desktop is up.
        if (const wchar_t* why = g_windowMode ? HiddenReason() : nullptr)
        {
            Log(L"%s: leaving the displays whole meanwhile", why);
            while (HiddenReason() && !(deadline && GetTickCount64() >= deadline))
                if (!Nap(100)) break;
            if (ShouldExit() || (deadline && GetTickCount64() >= deadline)) break;
            Log(L"desktop back");
            if (!Nap(150)) break; // the panel rescan below gives Windows the rest of its moment
            continue;
        }

        Log(L"mode: %s", g_windowMode ? (SpecializedDisplaysLicensed() ? L"window (config.ini)" : L"window (this Windows edition cannot own a display)")
                                      : L"exclusive");

        bool stopped = false;
        ULONGLONG sessionStart = GetTickCount64();
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
        g_abort = false; // a lost panel ended that session only; otherwise Nap() below would end the run
        if (stopped || StopRequested() || (deadline && GetTickCount64() >= deadline)) break;
        if (g_restart)
        {
            g_restart = false;
            Log(L"restarting the split");
            continue;
        }

        // Back off on repeated failures so a persistent problem cannot flicker the screen forever. A
        // session that ran for a minute before it was lost (a GPU driver update, a replugged cable)
        // starts the count over.
        ULONGLONG t = GetTickCount64();
        if (!firstFailure || t - firstFailure > 120000 || t - sessionStart > 60000)
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

// A crash leaves a minidump next to the logs. The watchdog restores the display either way,
// but without a dump a field crash is undiagnosable.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    static volatile LONG inside = 0;
    if (InterlockedExchange(&inside, 1)) return EXCEPTION_EXECUTE_HANDLER;
    wchar_t dir[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramData%\\SplitDisplay", dir, MAX_PATH);
    CreateDirectoryW(dir, nullptr);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\crash-%lu-%llu.dmp", dir, GetCurrentProcessId(), GetTickCount64());
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
            (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory), &mei, nullptr, nullptr);
        CloseHandle(f);
        Log(L"crashed; minidump written to %s", path);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool IsElevated()
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD len = 0;
    bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &len) && e.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

static std::wstring QuoteArg(const std::wstring& arg)
{
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (wchar_t c : arg)
    {
        if (c == L'\\' || c == L'"') out += L'\\';
        out += c;
    }
    return out + L"\"";
}

// The binary is asInvoker: the settings window (and the links it opens) run unprivileged,
// and only the commands that truly need it (composing, reverting, the logon task) ask for
// elevation by relaunching themselves. Returns the elevated process's exit code when
// waited for, 0 when detached (the run command: the elevated twin is the real instance).
static int RelaunchElevated(int argc, wchar_t** argv, bool wait)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring params;
    for (int i = 1; i < argc; i++) params += (params.empty() ? L"" : L" ") + QuoteArg(argv[i]);
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei))
    {
        Log(L"elevation declined or failed: %lu", GetLastError());
        return 1;
    }
    int code = 0;
    if (sei.hProcess)
    {
        if (wait)
        {
            WaitForSingleObject(sei.hProcess, INFINITE);
            DWORD ec = 1;
            GetExitCodeProcess(sei.hProcess, &ec);
            code = (int)ec;
        }
        CloseHandle(sei.hProcess);
    }
    return code;
}

int wmain(int argc, wchar_t** argv)
{
    LogInit(L"splitdisplay.log");
    SetUnhandledExceptionFilter(CrashFilter);
    // Elevation must be decided before ParsePanelOption rewrites argv, so the elevated twin
    // receives the original command line including --panel.
    {
        std::wstring rawCmd;
        for (int i = 1; i < argc; i++)
        {
            if (_wcsicmp(argv[i], L"--panel") == 0) i++; // skip its value
            else rawCmd = argv[i];
            if (!rawCmd.empty()) break;
        }
        bool needsAdmin = rawCmd == L"run" || rawCmd == L"revert" || rawCmd == L"autostart" || rawCmd == L"watchdog";
        if (needsAdmin && !IsElevated()) return RelaunchElevated(argc, argv, rawCmd != L"run" && rawCmd != L"watchdog");
    }
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
