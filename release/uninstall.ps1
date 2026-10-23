# SplitDisplay uninstaller (run through Uninstall.cmd, which elevates).
# Restores the display, removes the logon task, the driver, the certificate and the program files.
$ErrorActionPreference = 'Continue'

# Published names (oemNN.inf) of every SplitDisplayIdd package in the driver store.
# pnputil's text output is localized, so parse its XML; fall back to the text form on
# Windows versions whose pnputil has no /format option.
function Get-SplitDisplayDriverPackages {
    try {
        $text = (pnputil /enum-drivers /format xml 2>$null | Out-String)
        $start = $text.IndexOf('<?xml')
        if ($start -ge 0) {
            $doc = [xml]$text.Substring($start)
            return @($doc.PnpUtil.Driver | Where-Object { $_.OriginalName -ieq 'splitdisplayidd.inf' } | ForEach-Object { $_.DriverName })
        }
    } catch { }
    $names = @()
    $lines = pnputil /enum-drivers
    for ($i = 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match 'Original Name:\s+splitdisplayidd\.inf' -and $lines[$i - 1] -match 'Published Name:\s+(oem\d+\.inf)') { $names += $Matches[1] }
    }
    return $names
}

$dest = Join-Path $env:ProgramFiles 'SplitDisplay'
$exe = Join-Path $dest 'splitdisplay.exe'
$sdctl = Join-Path $dest 'sdctl.exe'
if (-not (Test-Path $sdctl)) { $sdctl = Join-Path $PSScriptRoot 'sdctl.exe' }

Write-Host '== Restoring the display'
if (Test-Path $exe) {
    & $exe stop | Out-Null
    Get-Process splitdisplay -ErrorAction SilentlyContinue | Wait-Process -Timeout 20 -ErrorAction SilentlyContinue
    & $exe revert | Out-Null
}
Unregister-ScheduledTask -TaskName 'SplitDisplay' -Confirm:$false -ErrorAction SilentlyContinue

Write-Host '== Removing the driver'
if (Test-Path $sdctl) { & $sdctl driver-remove | Out-Null }
foreach ($oem in Get-SplitDisplayDriverPackages) {
    pnputil /delete-driver $oem /uninstall | Out-Null
    Write-Host "  removed $oem"
}

Write-Host '== Removing the certificate'
foreach ($store in 'Root', 'TrustedPublisher', 'My') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -like 'CN=SplitDisplay Local Driver Signing*' } |
        ForEach-Object { Remove-Item $_.PSPath -DeleteKey -ErrorAction SilentlyContinue; Write-Host "  removed $($_.Thumbprint) from $store" }
}

Write-Host '== Removing program files'
Set-Location $env:TEMP
Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
Write-Host 'SplitDisplay removed. Logs remain in' "$env:ProgramData\SplitDisplay" -ForegroundColor Green
