#pragma once
#include <Windows.h>

#include <string>
#include <vector>

// Windows' per-monitor scale in percent for the source driving the given target (0 if unknown).
int GetScalePercentForTarget(LUID adapter, UINT32 targetId);

// Applies a scale percentage to the active monitors with exactly these names.
void SetScalePercent(const std::vector<std::wstring>& names, int percent);

struct PlacedDisplay
{
    std::wstring devicePath;
    POINT position;
};

// Places virtual monitor "Split <i+1>" at desktop[i].left/top (each panel's old desktop position
// plus the region offset, so the split mirrors the panel) and puts every display in `others` back
// at its saved position. Returns true once the arrangement is in effect.
bool ArrangeMonitors(const std::vector<RECT>& desktop, const std::vector<PlacedDisplay>& others);

// Keeps one panel's split monitors ("Split first+1" ...) touching exactly as on the panel, anchored
// at the first one. Windows re-lays out the desktop after e.g. a refresh-rate change in Settings,
// which can leave gaps the cursor cannot cross. Returns true if a correction was applied.
bool KeepGroupAligned(UINT first, const std::vector<RECT>& regions);

// Sets every named active monitor to the given rate (and saves it for Windows to restore next
// time). The mode switch itself goes through ChangeDisplaySettingsEx, which only takes whole
// Hz and picks the closest offered mode; the offered modes carry the exact fractional rate.
void ForceRefresh(const std::vector<std::wstring>& names, int milliHz);

// Current refresh rate (milli-Hz) of the active monitor with exactly this name, or 0.
int CurrentRefreshMilliOf(const std::wstring& name);

// The same for one target (a physical panel: its name need not be unique, or may be empty).
void ForceRefreshTarget(LUID adapter, UINT32 targetId, int milliHz);
int CurrentRefreshMilliOfTarget(LUID adapter, UINT32 targetId);

// One QueryDisplayConfig pass describing every active monitor. The session loop used to query
// per monitor per question (aligned? rate of monitor k? scale? rotation?), each a full CCD
// round trip with per-path device-info calls; a snapshot answers all of them at once.
struct MonitorSnapshot
{
    std::wstring name;
    std::wstring devicePath;
    std::wstring gdiName;
    LUID adapter{};
    UINT32 targetId = 0;
    POINT position{};
    UINT32 width = 0, height = 0;
    int refreshMilliHz = 0;
    int scalePercent = 0;
    DISPLAYCONFIG_ROTATION rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
};
std::vector<MonitorSnapshot> SnapshotMonitors();
const MonitorSnapshot* FindInSnapshot(const std::vector<MonitorSnapshot>& snap, const std::wstring& name);

// Desktop rect of the active monitor with exactly this name.
bool DesktopRectOf(const std::wstring& name, RECT& out);

// Adds the named (connected but inactive) monitors to the desktop, leaving every other display as
// it is. Windows restores a saved topology for a known set of monitors, which can keep freshly
// plugged split monitors switched off. Returns true if a change was applied.
bool ActivateMonitors(const std::vector<std::wstring>& names);

// The same for one target (a physical panel).
bool ActivateTarget(LUID adapter, UINT32 targetId);

// Returns true if the target with this name is part of the desktop.
bool IsTargetActive(const wchar_t* namePrefix);

// Forces an extend topology across connected displays (used as a last-resort recovery).
void ForceExtendTopology();

// GDI device name (\\.\DISPLAYn) of the source driving this target while it is on the desktop, else "".
std::wstring GdiNameOfTarget(LUID adapter, UINT32 targetId);

// GDI device name of the active monitor with exactly this friendly name, else "".
std::wstring GdiNameOfMonitor(const std::wstring& name);

// "100" / "59.94" style formatting for a milli-Hz rate.
std::wstring FormatHz(int milliHz);
