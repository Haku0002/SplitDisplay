#include "panel.h"

#include <wingdi.h>
#include <winternl.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Display.Core.h>

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

        DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = t.adapter;
        name.header.id = t.targetId;
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS)
        {
            t.friendlyName = name.monitorFriendlyDeviceName;
            t.monitorDevicePath = name.monitorDevicePath;
        }

        if (active && p.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && p.sourceInfo.modeInfoIdx < modes.size())
        {
            auto& sm = modes[p.sourceInfo.modeInfoIdx].sourceMode;
            t.width = sm.width;
            t.height = sm.height;
        }
        if (p.targetInfo.refreshRate.Denominator)
            t.refreshHz = (double)p.targetInfo.refreshRate.Numerator / p.targetInfo.refreshRate.Denominator;

        if (existing) *existing = t;
        else out.push_back(t);
    }
    return out;
}

std::optional<PanelTarget> FindPanel()
{
    for (auto& t : EnumTargets())
        if (t.friendlyName.rfind(L"Sculptor", 0) == 0)
            return t;
    return std::nullopt;
}

int CountOtherActiveDisplays(const PanelTarget& panel)
{
    int n = 0;
    for (auto& t : EnumTargets())
    {
        bool same = t.adapter.LowPart == panel.adapter.LowPart && t.adapter.HighPart == panel.adapter.HighPart && t.targetId == panel.targetId;
        if (t.active && !same) n++;
    }
    return n;
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
