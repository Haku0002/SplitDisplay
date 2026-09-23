# Installs SplitDisplay for everyday use (elevated):
#   - copies the tools to C:\Program Files\SplitDisplay
#   - registers the "SplitDisplay" logon task (runs elevated, no window)
#   - removes the development failsafe task, which would fight the compositor
# The driver itself is installed separately with: sdctl driver-install out\driver\SplitDisplayIdd.inf
#
# Uninstall: scripts\uninstall.ps1
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$dest = Join-Path $env:ProgramFiles 'SplitDisplay'

$running = Get-Process splitdisplay -ErrorAction SilentlyContinue
if ($running) {
    & (Join-Path $dest 'splitdisplay.exe') stop 2>$null
    $running | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
}

New-Item -ItemType Directory -Force $dest | Out-Null
foreach ($f in 'splitdisplay.exe', 'sdctl.exe') {
    Copy-Item (Join-Path $root "build\Release\$f") $dest -Force
}

Unregister-ScheduledTask -TaskName 'SplitDisplay Failsafe Revert' -Confirm:$false -ErrorAction SilentlyContinue

$user = "$env:COMPUTERNAME\$env:USERNAME"
$action = New-ScheduledTaskAction -Execute (Join-Path $dest 'splitdisplay.exe') -Argument 'run'
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$trigger.Delay = 'PT3S'
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName 'SplitDisplay' -Action $action -Trigger $trigger -Principal $principal -Settings $settings `
    -Description 'Splits the Sculptor HDMI panel into two native monitors.' -Force | Out-Null
"installed to $dest; logon task 'SplitDisplay' registered"
