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

// Which display to split. Selected by monitor device path (from the settings window or
// "autodetect"); a name prefix (--panel, case-insensitive) is the fallback.
std::wstring PanelId();
std::wstring PanelNamePrefix();
bool HasPanelSelection();
void SelectPanel(const PanelTarget& t); // saves id + name to config.ini
void SetPanelNamePrefix(const std::wstring& name);
bool MatchesPanel(const PanelTarget& t);

// Loads the selection from config.ini, then consumes "--panel <name>" from argv (compacting it).
void ParsePanelOption(int& argc, wchar_t** argv);

// Picks a display when none is selected: the only taller-than-wide display, else the only display.
std::optional<PanelTarget> AutodetectPanel();

// The configured panel, preferring an active target over an inactive one.
std::optional<PanelTarget> FindPanel();

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
