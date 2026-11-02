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
// then enables or disables its logon trigger (the task itself stays enabled for on-demand start).
bool SetAutostart(bool enabled);

// Starts the registered "SplitDisplay" scheduled task on demand, launching the elevated compositor
// from the unprivileged settings window without a UAC prompt. Returns false if the task is missing.
bool StartAutostartTask();
