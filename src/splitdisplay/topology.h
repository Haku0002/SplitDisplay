#pragma once
#include <Windows.h>

#include <vector>

// Windows' per-monitor scale in percent for the source driving the given target (0 if unknown).
int GetScalePercentForTarget(LUID adapter, UINT32 targetId);

// Applies a scale percentage to every active source whose monitor name starts with namePrefix.
void SetScalePercentForMonitors(const wchar_t* namePrefix, int percent);

#include <string>

struct PlacedDisplay
{
    std::wstring devicePath;
    POINT position;
};

// Places virtual monitor "Split <i+1>" at origin + regions[i].left/top (the panel's old desktop
// position, so the split mirrors the panel), and puts every display in `others` back at its saved
// position. Returns true once the arrangement is in effect.
bool ArrangeRegions(const std::vector<RECT>& regions, POINT origin, const std::vector<PlacedDisplay>& others);

// Returns true if the target with this name is part of the desktop.
bool IsTargetActive(const wchar_t* namePrefix);

// Forces an extend topology across connected displays (used as a last-resort recovery).
void ForceExtendTopology();
