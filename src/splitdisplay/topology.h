#pragma once
#include <Windows.h>

// Windows' per-monitor scale in percent for the source driving the given target (0 if unknown).
int GetScalePercentForTarget(LUID adapter, UINT32 targetId);

// Applies a scale percentage to every active source whose monitor name starts with namePrefix.
void SetScalePercentForMonitors(const wchar_t* namePrefix, int percent);

// Stacks "Split Upper" at (0,0) (primary) and "Split Lower" directly below it.
// Returns true once both were found and the layout was applied.
bool ArrangeVirtualMonitors();

// Returns true if the target with this name is part of the desktop.
bool IsTargetActive(const wchar_t* namePrefix);

// Forces an extend topology across connected displays (used as a last-resort recovery).
void ForceExtendTopology();
