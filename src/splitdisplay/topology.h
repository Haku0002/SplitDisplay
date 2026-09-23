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

// Returns true if the target with this name is part of the desktop.
bool IsTargetActive(const wchar_t* namePrefix);

// Forces an extend topology across connected displays (used as a last-resort recovery).
void ForceExtendTopology();
