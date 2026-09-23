#include "panel.h"

#include <wingdi.h>
#include <winternl.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Display.h>
#include <winrt/Windows.Devices.Display.Core.h>

#include <cwctype>

// Settings > Display > Advanced display > "Remove display from desktop" does not use the
// documented SET_MONITOR_SPECIALIZATION packet. It hands SystemSettingsAdminFlows.exe a
// "SpecializeDisplay" command, which sends this undocumented packet (type -23, 48 bytes).
// Reverse engineered from SettingsHandlers_PCDisplay.dll / SystemSettingsAdminFlows.exe with
// public symbols. Requires elevation.

// {F196C02F-F86F-4F9A-AA15-E9CEBDFE3B96}, ntddvdeo.h
static constexpr GUID kPseudoSpecialized = { 0xf196c02f, 0xf86f, 0x4f9a, { 0xaa, 0x15, 0xe9, 0xce, 0xbd, 0xfe, 0x3b, 0x96 } };
static constexpr int kSetMonitorOverride = -23;

#pragma pack(push, 4)
struct MonitorOverridePacket
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header; // 20 bytes
    GUID overrideType;                       // offset 20
    INT32 enable;                            // offset 36
    UINT64 hash;                             // offset 40
};
#pragma pack(pop)
static_assert(sizeof(MonitorOverridePacket) == 48);

extern "C" NTSYSAPI NTSTATUS NTAPI RtlHashUnicodeString(PCUNICODE_STRING String, BOOLEAN CaseInSensitive, ULONG HashAlgorithm, PULONG HashValue);

// Hash of StableMonitorId + "{GUID}", computed as two 32-bit halves exactly like Settings does.
static UINT64 OverrideHash(const std::wstring& stableMonitorId)
{
    wchar_t guid[64];
    StringFromGUID2(kPseudoSpecialized, guid, 64);
    std::wstring s = stableMonitorId + guid;
    if (s.size() > 0x140) s.resize(0x140); // Settings uses a 0x288-byte buffer

    USHORT total = (USHORT)(s.size() * sizeof(wchar_t));
    USHORT half = (USHORT)((total >> 1) & 0x7ffe);

    ULONG lo = 0, hi = 0;
    UNICODE_STRING u{ half, half, s.data() };
    RtlHashUnicodeString(&u, TRUE, 0, &lo);
    if (total > 2)
    {
        UNICODE_STRING v{ (USHORT)(total - half), (USHORT)(total - half), (PWSTR)((BYTE*)s.data() + half) };
        RtlHashUnicodeString(&v, TRUE, 0, &hi);
    }
    return ((UINT64)hi << 32) | lo;
}

static std::wstring StableMonitorId(LUID adapter, UINT32 targetId)
{
    try
    {
        auto mgr = winrt::Windows::Devices::Display::Core::DisplayManager::Create(winrt::Windows::Devices::Display::Core::DisplayManagerOptions::None);
        for (auto&& t : mgr.GetCurrentTargets())
        {
            auto id = t.Adapter().Id();
            if (id.LowPart == adapter.LowPart && id.HighPart == adapter.HighPart && t.AdapterRelativeId() == targetId)
            {
                std::wstring s(t.StableMonitorId());
                mgr.Close();
                return s;
            }
        }
        mgr.Close();
    }
    catch (...)
    {
    }
    return L"";
}

static std::wstring ConfigPath()
{
    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramData%\\SplitDisplay\\config.ini", path, MAX_PATH);
    return path;
}

std::wstring LoadConfigValue(const wchar_t* key)
{
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(L"SplitDisplay", key, L"", buf, 1024, ConfigPath().c_str());
    return buf;
}

bool SaveConfigValue(const wchar_t* key, const std::wstring& value)
{
    wchar_t dir[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramData%\\SplitDisplay", dir, MAX_PATH);
    CreateDirectoryW(dir, nullptr);
    // Quoted so leading/trailing spaces (EDID names) survive.
    return WritePrivateProfileStringW(L"SplitDisplay", key, (L"\"" + value + L"\"").c_str(), ConfigPath().c_str()) != FALSE;
}

#pragma region Panel selection

static std::wstring g_panelId;   // monitor device path
static std::wstring g_panelName; // fallback: name prefix

static std::wstring Trim(std::wstring s)
{
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(0, 1);
    return s;
}

static bool StartsWithNoCase(const std::wstring& s, const std::wstring& prefix)
{
    return !prefix.empty() && s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

std::wstring PanelId()
{
    return g_panelId;
}

std::wstring PanelNamePrefix()
{
    return g_panelName;
}

bool HasPanelSelection()
{
    return !g_panelId.empty() || !g_panelName.empty();
}

void SetPanelNamePrefix(const std::wstring& name)
{
    g_panelName = Trim(name);
}

void SelectPanel(const PanelTarget& t)
{
    g_panelId = t.monitorDevicePath;
    g_panelName = Trim(t.friendlyName);
    SaveConfigValue(L"panel_id", g_panelId);
    SaveConfigValue(L"panel", g_panelName);
}

bool MatchesPanel(const PanelTarget& t)
{
    if (t.isVirtual) return false;
    if (!g_panelId.empty()) return _wcsicmp(t.monitorDevicePath.c_str(), g_panelId.c_str()) == 0;
    return StartsWithNoCase(Trim(t.friendlyName), g_panelName);
}

void ParsePanelOption(int& argc, wchar_t** argv)
{
    g_panelId = LoadConfigValue(L"panel_id");
    g_panelName = Trim(LoadConfigValue(L"panel"));
    for (int i = 1; i + 1 < argc; i++)
    {
        if (_wcsicmp(argv[i], L"--panel") != 0) continue;
        // An explicit name overrides the stored selection.
        g_panelName = Trim(argv[i + 1]);
        g_panelId.clear();
        for (int j = i; j + 2 <= argc; j++) argv[j] = argv[j + 2];
        argc -= 2;
        return;
    }
}

std::wstring ConnectorName(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t)
{
    switch (t)
    {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return L"HDMI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return L"DisplayPort";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return L"Built-in (eDP)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_USB_TUNNEL: return L"USB-C";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return L"Built-in";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return L"DVI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return L"VGA";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return L"Wireless";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED: return L"Indirect";
    default: return L"";
    }
}

#pragma endregion

static bool QueryAll(UINT32 flags, std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    for (int attempt = 0; attempt < 5; attempt++)
    {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(flags, &np, &nm) != ERROR_SUCCESS) return false;
        paths.resize(np);
        modes.resize(nm);
        LONG r = QueryDisplayConfig(flags, &np, paths.data(), &nm, modes.data(), nullptr);
        if (r == ERROR_INSUFFICIENT_BUFFER) continue;
        if (r != ERROR_SUCCESS) return false;
        paths.resize(np);
        modes.resize(nm);
        return true;
    }
    return false;
}

static bool IsOurVirtualName(const std::wstring& name)
{
    // "Split 1" .. "Split 8" from SplitDisplayIdd
    return name.size() == 7 && name.rfind(L"Split ", 0) == 0 && name[6] >= L'1' && name[6] <= L'8';
}

std::vector<PanelTarget> EnumTargets()
{
    std::vector<PanelTarget> out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryAll(QDC_ALL_PATHS, paths, modes)) return out;

    for (auto& p : paths)
    {
        if (!p.targetInfo.targetAvailable) continue;

        // QDC_ALL_PATHS lists every source x target combination; keep one entry per target,
        // preferring the active path.
        PanelTarget* existing = nullptr;
        for (auto& e : out)
            if (e.adapter.LowPart == p.targetInfo.adapterId.LowPart && e.adapter.HighPart == p.targetInfo.adapterId.HighPart && e.targetId == p.targetInfo.id)
                existing = &e;
        bool active = (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        if (existing && (existing->active || !active)) continue;

        PanelTarget t;
        t.adapter = p.targetInfo.adapterId;
        t.targetId = p.targetInfo.id;
        t.active = active;
        t.connector = p.targetInfo.outputTechnology;

        DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = t.adapter;
        name.header.id = t.targetId;
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS)
        {
            t.friendlyName = name.monitorFriendlyDeviceName;
            t.monitorDevicePath = name.monitorDevicePath;
            if (t.connector == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER) t.connector = name.outputTechnology;
        }
        t.isVirtual = IsOurVirtualName(Trim(t.friendlyName));

        if (active && p.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && p.sourceInfo.modeInfoIdx < modes.size())
        {
            auto& sm = modes[p.sourceInfo.modeInfoIdx].sourceMode;
            t.width = sm.width;
            t.height = sm.height;
            t.position = { sm.position.x, sm.position.y };
            t.primary = sm.position.x == 0 && sm.position.y == 0;
        }
        if (p.targetInfo.refreshRate.Denominator)
            t.refreshHz = (double)p.targetInfo.refreshRate.Numerator / p.targetInfo.refreshRate.Denominator;

        if (existing) *existing = t;
        else out.push_back(t);
    }

    // A display removed from the desktop disappears from CCD entirely; DisplayCore still lists it.
    try
    {
        namespace wdc = winrt::Windows::Devices::Display::Core;
        auto mgr = wdc::DisplayManager::Create(wdc::DisplayManagerOptions::None);
        for (auto&& dt : mgr.GetCurrentTargets())
        {
            if (!dt.IsConnected()) continue;
            auto id = dt.Adapter().Id();
            bool known = false;
            for (auto& e : out)
                if (e.adapter.LowPart == id.LowPart && e.adapter.HighPart == id.HighPart && e.targetId == dt.AdapterRelativeId()) known = true;
            if (known) continue;
            auto monitor = dt.TryGetMonitor();
            if (!monitor) continue;
            PanelTarget t;
            t.adapter = { id.LowPart, id.HighPart };
            t.targetId = dt.AdapterRelativeId();
            t.friendlyName = monitor.DisplayName();
            t.monitorDevicePath = monitor.DeviceId();
            auto native = monitor.NativeResolutionInRawPixels();
            t.width = (UINT32)native.Width;
            t.height = (UINT32)native.Height;
            switch (monitor.PhysicalConnector())
            {
            case winrt::Windows::Devices::Display::DisplayMonitorPhysicalConnectorKind::Hdmi: t.connector = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI; break;
            case winrt::Windows::Devices::Display::DisplayMonitorPhysicalConnectorKind::DisplayPort: t.connector = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL; break;
            default: break;
            }
            t.active = false;
            t.isVirtual = IsOurVirtualName(Trim(t.friendlyName));
            out.push_back(t);
        }
        mgr.Close();
    }
    catch (...)
    {
    }
    return out;
}

std::optional<PanelTarget> FindPanel()
{
    std::optional<PanelTarget> best;
    for (auto& t : EnumTargets())
        if (MatchesPanel(t) && (!best || (t.active && !best->active))) best = t;
    return best;
}

std::optional<PanelTarget> AutodetectPanel()
{
    std::vector<PanelTarget> real, tall;
    for (auto& t : EnumTargets())
    {
        if (t.isVirtual || !t.active || !t.width || !t.height) continue;
        real.push_back(t);
        if (t.height > t.width) tall.push_back(t);
    }
    if (tall.size() == 1) return tall[0];
    if (real.size() == 1) return real[0];
    return std::nullopt;
}

SpecializationState GetSpecialization(LUID adapter, UINT32 targetId)
{
    DISPLAYCONFIG_GET_MONITOR_SPECIALIZATION q{};
    q.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_MONITOR_SPECIALIZATION;
    q.header.size = sizeof(q);
    q.header.adapterId = adapter;
    q.header.id = targetId;
    SpecializationState s;
    s.ok = DisplayConfigGetDeviceInfo(&q.header) == ERROR_SUCCESS;
    s.enabled = q.isSpecializationEnabled;
    s.availableForMonitor = q.isSpecializationAvailableForMonitor;
    s.availableForSystem = q.isSpecializationAvailableForSystem;
    return s;
}

LONG SetSpecialization(LUID adapter, UINT32 targetId, bool enable)
{
    std::wstring stable = StableMonitorId(adapter, targetId);
    if (stable.empty()) return ERROR_NOT_FOUND;

    MonitorOverridePacket p{};
    p.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)kSetMonitorOverride;
    p.header.size = sizeof(p);
    p.header.adapterId = adapter;
    p.header.id = targetId;
    p.overrideType = kPseudoSpecialized;
    p.enable = enable ? 1 : 0;
    p.hash = OverrideHash(stable);
    return DisplayConfigSetDeviceInfo(&p.header);
}
