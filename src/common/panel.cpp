#include "panel.h"

#include <wingdi.h>

// {5B1C8E0A-6F43-4E7B-9C5D-2E1A7F3B9D10} / {5B1C8E0A-6F43-4E7B-9C5D-2E1A7F3B9D11}
static constexpr GUID kSpecializationType = { 0x5b1c8e0a, 0x6f43, 0x4e7b, { 0x9c, 0x5d, 0x2e, 0x1a, 0x7f, 0x3b, 0x9d, 0x10 } };
static constexpr GUID kSpecializationSubType = { 0x5b1c8e0a, 0x6f43, 0x4e7b, { 0x9c, 0x5d, 0x2e, 0x1a, 0x7f, 0x3b, 0x9d, 0x11 } };

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
    DISPLAYCONFIG_SET_MONITOR_SPECIALIZATION s{};
    s.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_MONITOR_SPECIALIZATION;
    s.header.size = sizeof(s);
    s.header.adapterId = adapter;
    s.header.id = targetId;
    s.isSpecializationEnabled = enable ? 1 : 0;
    s.specializationType = kSpecializationType;
    s.specializationSubType = kSpecializationSubType;
    wcscpy_s(s.specializationApplicationName, L"SplitDisplay");
    return DisplayConfigSetDeviceInfo(&s.header);
}
