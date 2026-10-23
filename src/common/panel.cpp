#include "panel.h"

#include <wingdi.h>
#include <winternl.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Display.h>
#include <winrt/Windows.Devices.Display.Core.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
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

// Writes to an INI file through a temp copy + atomic replace. config.ini is read by other
// processes (the running compositor, the settings window) all the time; rewriting it in
// place section by section let a reader observe a half-written file (new panel count,
// panels not yet written). Falls back to in-place writes if the copy fails.
struct AtomicIni
{
    std::wstring path;
    std::wstring temp;
    bool staged = false;

    explicit AtomicIni(std::wstring target) : path(std::move(target))
    {
        temp = path + L".tmp" + std::to_wstring(GetCurrentProcessId());
        staged = CopyFileW(path.c_str(), temp.c_str(), FALSE) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    ~AtomicIni()
    {
        if (staged) DeleteFileW(temp.c_str());
    }
    const wchar_t* File() const { return staged ? temp.c_str() : path.c_str(); }
    bool Commit()
    {
        if (!staged) return true; // wrote in place already
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, temp.c_str()); // flush the profile cache
        bool ok = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES
            ? ReplaceFileW(path.c_str(), temp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE
            : MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
        if (ok) staged = false;
        return ok;
    }
};

bool SaveConfigValue(const wchar_t* key, const std::wstring& value)
{
    wchar_t dir[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramData%\\SplitDisplay", dir, MAX_PATH);
    CreateDirectoryW(dir, nullptr);
    AtomicIni ini(ConfigPath());
    // Quoted so leading/trailing spaces (EDID names) survive.
    return WritePrivateProfileStringW(L"SplitDisplay", key, (L"\"" + value + L"\"").c_str(), ini.File()) != FALSE && ini.Commit();
}

#pragma region Panel selection

static std::vector<PanelConfig> g_override; // from --panel
static bool g_hasOverride = false;

static std::wstring Trim(std::wstring s)
{
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(0, 1);
    return s;
}

static std::wstring ReadIni(const wchar_t* section, const wchar_t* key)
{
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(section, key, L"", buf, 1024, ConfigPath().c_str());
    return buf;
}

std::vector<PanelConfig> LoadPanels()
{
    std::vector<PanelConfig> out;
    int count = _wtoi(ReadIni(L"SplitDisplay", L"panels").c_str());
    for (int i = 1; i <= count && i <= 16; i++)
    {
        std::wstring sec = L"Panel" + std::to_wstring(i);
        PanelConfig c;
        c.id = ReadIni(sec.c_str(), L"id");
        c.name = Trim(ReadIni(sec.c_str(), L"name"));
        c.layout = ReadIni(sec.c_str(), L"layout");
        // refresh_mhz (v0.4+) supersedes the whole-Hz refresh key.
        c.refreshMilliHz = _wtoi(ReadIni(sec.c_str(), L"refresh_mhz").c_str());
        if (!c.refreshMilliHz) c.refreshMilliHz = _wtoi(ReadIni(sec.c_str(), L"refresh").c_str()) * 1000;
        {
            std::wstring s = ReadIni(sec.c_str(), L"scales");
            for (size_t at = 0; at < s.size();)
            {
                size_t comma = s.find(L',', at);
                c.scales.push_back(_wtoi(s.substr(at, comma == std::wstring::npos ? comma : comma - at).c_str()));
                if (comma == std::wstring::npos) break;
                at = comma + 1;
            }
        }
        if (!c.id.empty() || !c.name.empty()) out.push_back(c);
    }
    if (count == 0)
    {
        // v0.1: one panel in the [SplitDisplay] section.
        PanelConfig c;
        c.id = ReadIni(L"SplitDisplay", L"panel_id");
        c.name = Trim(ReadIni(L"SplitDisplay", L"panel"));
        c.layout = ReadIni(L"SplitDisplay", L"layout");
        if (!c.id.empty() || !c.name.empty()) out.push_back(c);
    }
    return out;
}

void SavePanels(const std::vector<PanelConfig>& panels)
{
    wchar_t dir[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramData%\\SplitDisplay", dir, MAX_PATH);
    CreateDirectoryW(dir, nullptr);
    AtomicIni ini(ConfigPath());
    const wchar_t* path = ini.File();
    for (int i = 1; i <= 16; i++)
    {
        std::wstring sec = L"Panel" + std::to_wstring(i);
        WritePrivateProfileStringW(sec.c_str(), nullptr, nullptr, path); // drop the whole section
        if (i > (int)panels.size()) continue;
        const auto& c = panels[i - 1];
        WritePrivateProfileStringW(sec.c_str(), L"id", (L"\"" + c.id + L"\"").c_str(), path);
        WritePrivateProfileStringW(sec.c_str(), L"name", (L"\"" + c.name + L"\"").c_str(), path);
        WritePrivateProfileStringW(sec.c_str(), L"layout", (L"\"" + c.layout + L"\"").c_str(), path);
        WritePrivateProfileStringW(sec.c_str(), L"refresh_mhz", std::to_wstring(c.refreshMilliHz).c_str(), path);
        // Kept in whole Hz so an older build still finds a sane rate after a downgrade.
        WritePrivateProfileStringW(sec.c_str(), L"refresh", std::to_wstring((c.refreshMilliHz + 500) / 1000).c_str(), path);
        std::wstring scales;
        for (int s : c.scales) scales += (scales.empty() ? L"" : L",") + std::to_wstring(s);
        WritePrivateProfileStringW(sec.c_str(), L"scales", scales.c_str(), path);
    }
    // The count goes last: even on the in-place fallback path a concurrent reader sees the
    // old count with complete old sections, never the new count with missing ones.
    WritePrivateProfileStringW(L"SplitDisplay", L"panels", (L"\"" + std::to_wstring(panels.size()) + L"\"").c_str(), path);
    // Retire the v0.1 keys once migrated.
    for (const wchar_t* key : { L"panel_id", L"panel", L"layout" }) WritePrivateProfileStringW(L"SplitDisplay", key, nullptr, path);
    ini.Commit();
}

bool Matches(const PanelConfig& c, const PanelTarget& t)
{
    if (t.isVirtual) return false;
    if (!c.id.empty()) return _wcsicmp(t.monitorDevicePath.c_str(), c.id.c_str()) == 0;
    std::wstring n = Trim(t.friendlyName);
    return !c.name.empty() && n.size() >= c.name.size() && _wcsnicmp(n.c_str(), c.name.c_str(), c.name.size()) == 0;
}

PanelConfig ConfigFor(const PanelTarget& t)
{
    PanelConfig c;
    c.id = t.monitorDevicePath;
    c.name = Trim(t.friendlyName);
    return c;
}

std::vector<PanelConfig> SelectedPanels()
{
    return g_hasOverride ? g_override : LoadPanels();
}

void ParsePanelOption(int& argc, wchar_t** argv)
{
    for (int i = 1; i + 1 < argc; i++)
    {
        if (_wcsicmp(argv[i], L"--panel") != 0) continue;
        PanelConfig named;
        named.name = Trim(argv[i + 1]);
        g_override = { named };
        // Keep the layout configured for that display, if any.
        for (auto& c : LoadPanels())
        {
            if (_wcsnicmp(c.name.c_str(), g_override[0].name.c_str(), g_override[0].name.size()) == 0)
            {
                g_override[0].layout = c.layout;
                g_override[0].refreshMilliHz = c.refreshMilliHz;
                g_override[0].scales = c.scales;
            }
        }
        g_hasOverride = true;
        for (int j = i; j + 2 <= argc; j++) argv[j] = argv[j + 2];
        argc -= 2;
        return;
    }
}

#pragma endregion

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
    // "Split 1" .. "Split 16" from SplitDisplayIdd
    if (name.rfind(L"Split ", 0) != 0 || name.size() < 7 || name.size() > 8) return false;
    int n = _wtoi(name.c_str() + 6);
    return n >= 1 && n <= 16 && std::to_wstring(n) == name.substr(6);
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

            DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof(src);
            src.header.adapterId = p.sourceInfo.adapterId;
            src.header.id = p.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS) t.gdiName = src.viewGdiDeviceName;
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
            auto monitor = dt.TryGetMonitor();
            if (!monitor) continue;
            UINT32 wmm = 0, hmm = 0;
            if (auto phys = monitor.PhysicalSizeInInches())
            {
                auto v = phys.Value();
                wmm = (UINT32)(v.Width * 25.4 + 0.5);
                hmm = (UINT32)(v.Height * 25.4 + 0.5);
            }
            bool known = false;
            for (auto& e : out)
                if (e.adapter.LowPart == id.LowPart && e.adapter.HighPart == id.HighPart && e.targetId == dt.AdapterRelativeId())
                {
                    known = true;
                    // CCD has no physical size; DisplayCore's monitor object is the only source.
                    e.widthMm = wmm;
                    e.heightMm = hmm;
                }
            if (known) continue;
            PanelTarget t;
            t.adapter = { id.LowPart, id.HighPart };
            t.targetId = dt.AdapterRelativeId();
            t.friendlyName = monitor.DisplayName();
            t.monitorDevicePath = monitor.DeviceId();
            auto native = monitor.NativeResolutionInRawPixels();
            t.width = (UINT32)native.Width;
            t.height = (UINT32)native.Height;
            t.widthMm = wmm;
            t.heightMm = hmm;
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

std::vector<int> SupportedRefreshRatesMilli(const PanelTarget& t)
{
    std::vector<int> rates;
    if (t.gdiName.empty() || !t.width) return rates;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(t.gdiName.c_str(), i, &dm); i++)
    {
        if (dm.dmPelsWidth != t.width || dm.dmPelsHeight != t.height || dm.dmDisplayFrequency <= 1) continue;
        int milli = (int)dm.dmDisplayFrequency * 1000;
        if (std::find(rates.begin(), rates.end(), milli) == rates.end()) rates.push_back(milli);
    }
    // The current mode's exact rate is known from CCD as a rational; DEVMODE would round
    // 59.94 to 59 or 60, and pacing the virtual monitors at either drifts against the panel.
    int currentMilli = (int)std::lround(t.refreshHz * 1000);
    if (currentMilli > 1000)
    {
        int closest = ClosestRate(rates, currentMilli);
        if (std::abs(closest - currentMilli) < 1000)
        {
            std::replace(rates.begin(), rates.end(), closest, currentMilli);
        }
        else if (std::find(rates.begin(), rates.end(), currentMilli) == rates.end())
        {
            rates.push_back(currentMilli);
        }
    }
    std::sort(rates.rbegin(), rates.rend());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

int ClosestRate(const std::vector<int>& rates, int want)
{
    int best = want;
    int bestDiff = INT_MAX;
    for (int r : rates)
    {
        if (std::abs(r - want) < bestDiff)
        {
            bestDiff = std::abs(r - want);
            best = r;
        }
    }
    return best;
}

std::optional<PanelTarget> FindTarget(const PanelConfig& c)
{
    std::optional<PanelTarget> best;
    for (auto& t : EnumTargets())
        if (Matches(c, t) && (!best || (t.active && !best->active))) best = t;
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
