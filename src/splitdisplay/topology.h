#pragma once
#include <Windows.h>

#include <string>
#include <vector>

// Windows' per-monitor scale in percent for the source driving the given target (0 if unknown).
int GetScalePercentForTarget(LUID adapter, UINT32 targetId);

// Applies a scale percentage to the active monitors with exactly these names.
void SetScalePercent(const std::vector<std::wstring>& names, int percent);

#include <string>

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

// Sets every named active monitor to `hz` (and saves it for Windows to restore next time).
void ForceRefresh(const std::vector<std::wstring>& names, int hz);

// Current refresh rate (Hz, rounded) of the active monitor with exactly this name, or 0.
int CurrentRefreshOf(const std::wstring& name);

// Adds the named (connected but inactive) monitors to the desktop, leaving every other display as
// it is. Windows restores a saved topology for a known set of monitors, which can keep freshly
// plugged split monitors switched off. Returns true if a change was applied.
bool ActivateMonitors(const std::vector<std::wstring>& names);

// Returns true if the target with this name is part of the desktop.
bool IsTargetActive(const wchar_t* namePrefix);

// Forces an extend topology across connected displays (used as a last-resort recovery).
void ForceExtendTopology();

// GDI device name (\.\DISPLAYn) of the source driving this target while it is on the desktop, else "".
std::wstring GdiNameOfTarget(LUID adapter, UINT32 targetId);
