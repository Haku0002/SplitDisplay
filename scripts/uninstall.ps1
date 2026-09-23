# Removes the everyday install (elevated). Leaves the driver and certificate in place;
# remove those with: sdctl driver-remove, pnputil /delete-driver oemNN.inf, scripts\untrust-cert.ps1
$dest = Join-Path $env:ProgramFiles 'SplitDisplay'
if (Test-Path (Join-Path $dest 'splitdisplay.exe')) {
    & (Join-Path $dest 'splitdisplay.exe') stop
    Get-Process splitdisplay -ErrorAction SilentlyContinue | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
    & (Join-Path $dest 'splitdisplay.exe') revert
}
Unregister-ScheduledTask -TaskName 'SplitDisplay' -Confirm:$false -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
'uninstalled'
