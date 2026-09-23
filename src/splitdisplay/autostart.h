#pragma once

// The "SplitDisplay" logon task, managed through the Task Scheduler COM API (schtasks output is
// localized and not safe to parse).

enum class AutostartState
{
    Missing,
    Enabled,
    Disabled,
};

AutostartState GetAutostart();

// Creates or updates the task so it runs "<this exe> run" elevated at the current user's logon,
// then enables or disables it. Returns false on failure.
bool SetAutostart(bool enabled);
