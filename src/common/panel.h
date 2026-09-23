#pragma once
#include <Windows.h>
#include <optional>
#include <objbase.h>
#include <string>
#include <vector>

// %ProgramData%\SplitDisplay\config.ini
std::wstring LoadConfigValue(const wchar_t* key);
bool SaveConfigValue(const wchar_t* key, const std::wstring& value);

// A connected display target (CCD, plus DisplayCore for displays removed from the desktop).
struct PanelTarget
{
    LUID adapter{};
    UINT32 targetId = 0;
    std::wstring friendlyName;      // EDID name, may carry trailing spaces
    std::wstring monitorDevicePath; // stable per monitor and port: the selection key
    bool active = false;            // part of the desktop right now
    bool primary = false;
    POINT position{};               // desktop position (active targets)
    UINT32 width = 0, height = 0;   // desktop mode, or native resolution when not active
    double refreshHz = 0;
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY connector = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
    bool isVirtual = false;         // one of our own "Split N" monitors
};

// All targets with a connected monitor (active or not).
std::vector<PanelTarget> EnumTargets();

// Displays to split, each with its own layout. Stored in config.ini as [Panel1]..[PanelN]
// (id = monitor device path, name, layout); the single-panel keys of v0.1 are migrated.
struct PanelConfig
{
    std::wstring id;     // monitor device path (preferred key)
    std::wstring name;   // EDID name; case-insensitive prefix match when id is empty
    std::wstring layout; // SerializeLayout() text; empty = default halves
};

std::vector<PanelConfig> LoadPanels();
void SavePanels(const std::vector<PanelConfig>& panels);
bool Matches(const PanelConfig& c, const PanelTarget& t);
PanelConfig ConfigFor(const PanelTarget& t);

// The panels to work on: "--panel <name>" from the command line (not saved), else config.ini.
std::vector<PanelConfig> SelectedPanels();
void ParsePanelOption(int& argc, wchar_t** argv);

// Picks a display when none is selected: the only taller-than-wide display, else the only display.
std::optional<PanelTarget> AutodetectPanel();

// The connected target for a panel config, preferring an active target over an inactive one.
std::optional<PanelTarget> FindTarget(const PanelConfig& c);

std::wstring ConnectorName(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t);

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
