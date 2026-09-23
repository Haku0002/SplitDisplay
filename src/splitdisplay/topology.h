#pragma once
#include <Windows.h>

#include <vector>

// Windows' per-monitor scale in percent for the source driving the given target (0 if unknown).
int GetScalePercentForTarget(LUID adapter, UINT32 targetId);

// Applies a scale percentage to every active source whose monitor name starts with namePrefix.
void SetScalePercentForMonitors(const wchar_t* namePrefix, int percent);

// Places virtual monitor "Split <i+1>" at regions[i].left/top, so the Windows desktop mirrors the
// panel (the region at 0,0 becomes the primary display). Returns true once all were found and
// the arrangement is in effect.
bool ArrangeRegions(const std::vector<RECT>& regions);

// Returns true if the target with this name is part of the desktop.
bool IsTargetActive(const wchar_t* namePrefix);

// Forces an extend topology across connected displays (used as a last-resort recovery).
void ForceExtendTopology();
