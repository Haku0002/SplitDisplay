#include "topology.h"

#include <algorithm>
#include <cstdlib>
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

void SetScalePercentForMonitors(const wchar_t* namePrefix, int percent)
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
        if (!StartsWith(TargetName(p), namePrefix)) continue;
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

bool ArrangeRegions(const std::vector<RECT>& regions, POINT origin, const std::vector<PlacedDisplay>& others)
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

    auto want = [&](size_t i) { return POINTL{ origin.x + regions[i].left, origin.y + regions[i].top }; };
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
    Log(L"arrange %zu regions, %zu active paths: %ld", regions.size(), paths.size(), r);
    return false; // verify on the next call
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
