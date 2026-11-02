#pragma once

#include <Windows.h>

#include <functional>
#include <string>

struct UpdateInfo
{
    bool ok = false;              // GitHub API call succeeded
    bool hasUpdate = false;       // remote version > running version
    bool isMajorOrMinor = false;  // remote major/minor > running major/minor
    int major = 0, minor = 0, patch = 0;
    std::wstring tag;             // e.g. L"v0.5.0"
    std::wstring version;         // e.g. L"0.5.0"
    std::wstring zipUrl;          // direct asset URL for SplitDisplay-v*-x64.zip
    std::wstring setupUrl;        // direct asset URL for SplitDisplay-Setup-v*-x64.exe (if present)
    std::wstring htmlUrl;         // release web page
    std::wstring error;           // non-empty when !ok
};

// Queries https://api.github.com/repos/Haku0002/SplitDisplay/releases/latest via WinHTTP.
UpdateInfo CheckForGitHubUpdate();

// True if SplitDisplay is installed under %ProgramFiles%SplitDisplay at >= the running binary's
// version AND the matching protocol driver is currently loaded.
bool IsInstalledAndCurrent();

// True if local driver + tool files are available next to this binary (release folder, installed
// folder, or a developer build tree).
bool CanInstallFromLocal();

// Stages the local binaries + driver into %TEMP% and launches the elevated installer. Once the
// elevated installer starts, it stops the current SplitDisplay, signs & installs the driver,
// updates %ProgramFiles%SplitDisplay, and reopens the new settings window.
bool RunLocalInstall(const std::function<void(const std::wstring&)>& onStatus, std::wstring& errOut);

// Downloads the release package from GitHub with live progress, extracts it to %TEMP%, and
// launches the elevated installer to upgrade in place.
bool RunGitHubUpgrade(const UpdateInfo& info, const std::function<void(const std::wstring&)>& onStatus, std::wstring& errOut);
