#include "topology.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "log.h"

// Undocumented DisplayConfig packets used by Settings > Display > Scale (see e.g. the SetDPI tool).
// Values are relative steps from the recommended scale.
static constexpr int kGetDpiScale = -3;
static constexpr int kSetDpiScale = -4;
static const int kDpiValues[] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };

struct DpiScaleGet
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 minScaleRel;
    INT32 curScaleRel;
    INT32 maxScaleRel;
};

struct DpiScaleSet
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 scaleRel;
};

static bool QueryActive(std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    for (int attempt = 0; attempt < 5; attempt++)
    {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return false;
        paths.resize(np);
        modes.resize(nm);
        LONG r = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
        if (r == ERROR_INSUFFICIENT_BUFFER) continue;
        if (r != ERROR_SUCCESS) return false;
        paths.resize(np);
        modes.resize(nm);
        return true;
    }
    return false;
}

static bool IsTarget(const DISPLAYCONFIG_PATH_INFO& p, LUID adapter, UINT32 targetId)
{
    return p.targetInfo.id == targetId && p.targetInfo.adapterId.LowPart == adapter.LowPart && p.targetInfo.adapterId.HighPart == adapter.HighPart;
}

static std::wstring TargetName(const DISPLAYCONFIG_PATH_INFO& p)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = p.targetInfo.adapterId;
    name.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) return L"";
    return name.monitorFriendlyDeviceName;
}

static bool StartsWith(const std::wstring& s, const wchar_t* prefix)
{
    return s.rfind(prefix, 0) == 0;
}

static int ScalePercentForSource(LUID adapter, UINT32 sourceId, int* recommendedIdx)
{
    DpiScaleGet g{};
    g.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)kGetDpiScale;
    g.header.size = sizeof(g);
    g.header.adapterId = adapter;
    g.header.id = sourceId;
    if (DisplayConfigGetDeviceInfo(&g.header) != ERROR_SUCCESS) return 0;
    int rec = std::abs(g.minScaleRel);
    if (recommendedIdx) *recommendedIdx = rec;
    int idx = rec + g.curScaleRel;
    if (idx < 0 || idx >= (int)std::size(kDpiValues)) return 0;
    return kDpiValues[idx];
}

int GetScalePercentForTarget(LUID adapter, UINT32 targetId)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryActive(paths, modes)) return 0;
    for (auto& p : paths)
    {
        if (p.targetInfo.id == targetId && p.targetInfo.adapterId.LowPart == adapter.LowPart && p.targetInfo.adapterId.HighPart == adapter.HighPart)
            return ScalePercentForSource(p.sourceInfo.adapterId, p.sourceInfo.id, nullptr);
    }
    return 0;
}

void SetScalePercent(const std::vector<std::wstring>& names, int percent)
{
    int want = -1;
    for (int i = 0; i < (int)std::size(kDpiValues); i++)
        if (kDpiValues[i] == percent) want = i;
    if (want < 0) return;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryActive(paths, modes)) return;
    for (auto& p : paths)
    {
        if (std::find(names.begin(), names.end(), TargetName(p)) == names.end()) continue;
        int rec = 0;
        int cur = ScalePercentForSource(p.sourceInfo.adapterId, p.sourceInfo.id, &rec);
        if (cur == percent) continue;
        DpiScaleSet s{};
        s.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)kSetDpiScale;
        s.header.size = sizeof(s);
        s.header.adapterId = p.sourceInfo.adapterId;
        s.header.id = p.sourceInfo.id;
        s.scaleRel = want - rec;
        LONG r = DisplayConfigSetDeviceInfo(&s.header);
        Log(L"scale '%s' %d%% -> %d%%: %ld", TargetName(p).c_str(), cur, percent, r);
    }
}

static std::wstring TargetDevicePath(const DISPLAYCONFIG_PATH_INFO& p)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = p.targetInfo.adapterId;
    name.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) return L"";
    return name.monitorDevicePath;
}

bool ArrangeMonitors(const std::vector<RECT>& regions, const std::vector<PlacedDisplay>& others)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryActive(paths, modes)) return false;

    std::vector<DISPLAYCONFIG_SOURCE_MODE*> slot(regions.size(), nullptr);
    std::vector<std::pair<DISPLAYCONFIG_SOURCE_MODE*, POINT>> keep; // other displays -> saved position
    for (auto& p : paths)
    {
        if (p.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || p.sourceInfo.modeInfoIdx >= modes.size()) continue;
        auto& mi = modes[p.sourceInfo.modeInfoIdx];
        if (mi.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
        auto n = TargetName(p);
        bool mine = false;
        for (size_t i = 0; i < regions.size(); i++)
        {
            if (n == L"Split " + std::to_wstring(i + 1))
            {
                slot[i] = &mi.sourceMode;
                mine = true;
            }
        }
        if (mine) continue;
        auto path = TargetDevicePath(p);
        for (auto& o : others)
            if (_wcsicmp(o.devicePath.c_str(), path.c_str()) == 0) keep.push_back({ &mi.sourceMode, o.position });
    }
    for (auto* s : slot)
        if (!s) return false;

    auto want = [&](size_t i) { return POINTL{ regions[i].left, regions[i].top }; };
    bool done = true;
    for (size_t i = 0; i < regions.size(); i++)
        if (slot[i]->position.x != want(i).x || slot[i]->position.y != want(i).y) done = false;
    for (auto& [mode, pos] : keep)
        if (mode->position.x != pos.x || mode->position.y != pos.y) done = false;
    if (done) return true;

    for (size_t i = 0; i < regions.size(); i++) slot[i]->position = want(i);
    for (auto& [mode, pos] : keep) mode->position = { pos.x, pos.y };

    LONG r = SetDisplayConfig((UINT32)paths.size(), paths.data(), (UINT32)modes.size(), modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    Log(L"arrange %zu monitors, %zu active paths: %ld", regions.size(), paths.size(), r);
    return false; // verify on the next call
}

bool KeepGroupAligned(UINT first, const std::vector<RECT>& regions)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (regions.empty() || !QueryActive(paths, modes)) return false;

    std::vector<DISPLAYCONFIG_SOURCE_MODE*> slot(regions.size(), nullptr);
    for (auto& p : paths)
    {
        if (p.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || p.sourceInfo.modeInfoIdx >= modes.size()) continue;
        auto& mi = modes[p.sourceInfo.modeInfoIdx];
        if (mi.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
        auto n = TargetName(p);
        for (size_t i = 0; i < regions.size(); i++)
            if (n == L"Split " + std::to_wstring(first + i + 1)) slot[i] = &mi.sourceMode;
    }
    for (auto* s : slot)
        if (!s) return false; // e.g. a monitor switched off with Win+P: nothing to glue

    // The group's first monitor is the anchor, so the whole group can still be moved around.
    LONG ox = slot[0]->position.x - regions[0].left, oy = slot[0]->position.y - regions[0].top;
    bool aligned = true;
    for (size_t i = 0; i < regions.size(); i++)
        if (slot[i]->position.x != ox + regions[i].left || slot[i]->position.y != oy + regions[i].top) aligned = false;
    if (aligned) return false;

    std::wstring was;
    for (size_t i = 0; i < regions.size(); i++)
    {
        was += L" (" + std::to_wstring(slot[i]->position.x) + L"," + std::to_wstring(slot[i]->position.y) + L")";
        slot[i]->position = { ox + regions[i].left, oy + regions[i].top };
    }
    LONG r = SetDisplayConfig((UINT32)paths.size(), paths.data(), (UINT32)modes.size(), modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    Log(L"split monitors %u..%zu drifted apart (was%s), re-aligned: %ld", first + 1, first + regions.size(), was.c_str(), r);
    return true;
}

static int RoundHz(const DISPLAYCONFIG_RATIONAL& r)
{
    return r.Denominator ? (int)((r.Numerator + r.Denominator / 2) / r.Denominator) : 0;
}

static void ForceRefreshWhere(const std::function<bool(const DISPLAYCONFIG_PATH_INFO&)>& wanted, int hz)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (hz <= 0 || !QueryActive(paths, modes)) return;
    for (auto& p : paths)
    {
        if (!wanted(p) || RoundHz(p.targetInfo.refreshRate) == hz) continue;
        auto n = TargetName(p);
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmFields = DM_DISPLAYFREQUENCY;
        dm.dmDisplayFrequency = (DWORD)hz;
        LONG r = ChangeDisplaySettingsExW(src.viewGdiDeviceName, &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
        Log(L"refresh '%s' %d -> %d Hz: %ld", n.c_str(), RoundHz(p.targetInfo.refreshRate), hz, r);
    }
}

void ForceRefresh(const std::vector<std::wstring>& names, int hz)
{
    ForceRefreshWhere([&](const DISPLAYCONFIG_PATH_INFO& p) { return std::find(names.begin(), names.end(), TargetName(p)) != names.end(); }, hz);
}

void ForceRefreshTarget(LUID adapter, UINT32 targetId, int hz)
{
    ForceRefreshWhere([&](const DISPLAYCONFIG_PATH_INFO& p) { return IsTarget(p, adapter, targetId); }, hz);
}

int CurrentRefreshOfTarget(LUID adapter, UINT32 targetId)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryActive(paths, modes)) return 0;
    for (auto& p : paths)
        if (IsTarget(p, adapter, targetId)) return RoundHz(p.targetInfo.refreshRate);
    return 0;
}

bool DesktopRectOf(const std::wstring& name, RECT& out)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryActive(paths, modes)) return false;
    for (auto& p : paths)
    {
        if (TargetName(p) != name || p.sourceInfo.modeInfoIdx >= modes.size()) continue;
        auto& mi = modes[p.sourceInfo.modeInfoIdx];
        if (mi.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
        auto& sm = mi.sourceMode;
        out = { sm.position.x, sm.position.y, sm.position.x + (LONG)sm.width, sm.position.y + (LONG)sm.height };
        return true;
    }
    return false;
}

int CurrentRefreshOf(const std::wstring& name)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryActive(paths, modes)) return 0;
    for (auto& p : paths)
    {
        if (TargetName(p) == name) return RoundHz(p.targetInfo.refreshRate);
    }
    return 0;
}

static bool ActivateWhere(const std::function<bool(const DISPLAYCONFIG_PATH_INFO&)>& wanted, const wchar_t* what)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> all;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    for (int attempt = 0; attempt < 5; attempt++)
    {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &np, &nm) != ERROR_SUCCESS) return false;
        all.resize(np);
        modes.resize(nm);
        LONG r = QueryDisplayConfig(QDC_ALL_PATHS, &np, all.data(), &nm, modes.data(), nullptr);
        if (r == ERROR_INSUFFICIENT_BUFFER) continue;
        if (r != ERROR_SUCCESS) return false;
        all.resize(np);
        modes.resize(nm);
        break;
    }

    auto sameSource = [](const DISPLAYCONFIG_PATH_INFO& a, const DISPLAYCONFIG_PATH_INFO& b) {
        return a.sourceInfo.adapterId.LowPart == b.sourceInfo.adapterId.LowPart && a.sourceInfo.adapterId.HighPart == b.sourceInfo.adapterId.HighPart &&
               a.sourceInfo.id == b.sourceInfo.id;
    };
    auto sameTarget = [](const DISPLAYCONFIG_PATH_INFO& a, const DISPLAYCONFIG_PATH_INFO& b) {
        return a.targetInfo.adapterId.LowPart == b.targetInfo.adapterId.LowPart && a.targetInfo.adapterId.HighPart == b.targetInfo.adapterId.HighPart &&
               a.targetInfo.id == b.targetInfo.id;
    };

    // Keep everything that is active now, and add one path (with a free source) per wanted target.
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    for (auto& p : all)
        if (p.flags & DISPLAYCONFIG_PATH_ACTIVE) paths.push_back(p);
    int added = 0;
    for (auto& p : all)
    {
        if ((p.flags & DISPLAYCONFIG_PATH_ACTIVE) || !p.targetInfo.targetAvailable) continue;
        if (!wanted(p)) continue;
        bool busy = false;
        for (auto& q : paths) busy |= sameSource(p, q) || sameTarget(p, q);
        if (busy) continue;
        DISPLAYCONFIG_PATH_INFO n = p;
        n.flags |= DISPLAYCONFIG_PATH_ACTIVE;
        n.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        n.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        paths.push_back(n);
        added++;
    }
    if (!added) return false;
    LONG r = SetDisplayConfig((UINT32)paths.size(), paths.data(), (UINT32)modes.size(), modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    Log(L"Windows left %d %s off the desktop; activating: %ld", added, what, r);
    return r == ERROR_SUCCESS;
}

bool ActivateMonitors(const std::vector<std::wstring>& names)
{
    return ActivateWhere([&](const DISPLAYCONFIG_PATH_INFO& p) { return std::find(names.begin(), names.end(), TargetName(p)) != names.end(); },
        L"split monitor(s)");
}

bool ActivateTarget(LUID adapter, UINT32 targetId)
{
    return ActivateWhere([&](const DISPLAYCONFIG_PATH_INFO& p) { return IsTarget(p, adapter, targetId); }, L"panel(s)");
}

bool IsTargetActive(const wchar_t* namePrefix)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryActive(paths, modes)) return false;
    for (auto& p : paths)
        if (StartsWith(TargetName(p), namePrefix)) return true;
    return false;
}

void ForceExtendTopology()
{
    LONG r = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
    Log(L"force extend topology: %ld", r);
}

std::wstring GdiNameOfTarget(LUID adapter, UINT32 targetId)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryActive(paths, modes)) return L"";
    for (auto& p : paths)
    {
        if (p.targetInfo.id != targetId || p.targetInfo.adapterId.LowPart != adapter.LowPart || p.targetInfo.adapterId.HighPart != adapter.HighPart)
            continue;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS) return src.viewGdiDeviceName;
    }
    return L"";
}
