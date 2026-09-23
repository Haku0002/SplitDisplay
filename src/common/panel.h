#pragma once
#include <Windows.h>
#include <optional>
#include <objbase.h>
#include <string>
#include <vector>

// Monitor name (EDID friendly name prefix) of the physical panel to split. Defaults to the
// Sculptor foldable; override with --panel "<name>" on the command line.
const wchar_t* PanelNamePrefix();

// Consumes "--panel <name>" from argv (compacting it) and applies it.
void ParsePanelOption(int& argc, wchar_t** argv);

// A display target as seen by the DisplayConfig (CCD) API.
struct PanelTarget
{
    LUID adapter{};
    UINT32 targetId = 0;
    std::wstring friendlyName;
    std::wstring monitorDevicePath;
    bool active = false;       // part of the desktop right now
    UINT32 width = 0, height = 0;
    double refreshHz = 0;
};

// All targets with a connected monitor (active or not).
std::vector<PanelTarget> EnumTargets();

// The physical panel we split: friendly name starts with PanelNamePrefix().
std::optional<PanelTarget> FindPanel();

// Number of active desktop paths whose target is not the given one.
int CountOtherActiveDisplays(const PanelTarget& panel);

struct SpecializationState
{
    bool ok = false;
    bool enabled = false;
    bool availableForMonitor = false;
    bool availableForSystem = false;
};

SpecializationState GetSpecialization(LUID adapter, UINT32 targetId);

// enable=true removes the panel from the Windows desktop so a DisplayManager
// client can own it; enable=false gives it back to the desktop.
LONG SetSpecialization(LUID adapter, UINT32 targetId, bool enable);
