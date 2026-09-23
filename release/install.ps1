# SplitDisplay installer (run through Install.cmd, which elevates).
#
# 1. Signs the bundled driver with a brand-new certificate created on THIS machine, trusts that
#    certificate, then deletes its private key. Nothing signed by anyone else is trusted, and
#    nobody (including you) can sign anything else with it afterwards.
# 2. Installs the virtual monitor driver.
# 3. Copies SplitDisplay to Program Files and registers a logon task that runs it.
param([string]$Panel)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$dest = Join-Path $env:ProgramFiles 'SplitDisplay'
$subject = 'CN=SplitDisplay Local Driver Signing'

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run Install.cmd (it asks for administrator rights).' }
foreach ($f in 'splitdisplay.exe', 'sdctl.exe', 'driver\SplitDisplayIdd.dll', 'driver\SplitDisplayIdd.inf') {
    if (-not (Test-Path (Join-Path $here $f))) { throw "missing $f next to install.ps1" }
}

Step 'Stopping a running SplitDisplay'
# Restores the single display first, so the real panel (not the virtual halves) is listed below.
if (Test-Path (Join-Path $dest 'splitdisplay.exe')) {
    & (Join-Path $dest 'splitdisplay.exe') stop | Out-Null
    Get-Process splitdisplay -ErrorAction SilentlyContinue | Wait-Process -Timeout 20 -ErrorAction SilentlyContinue
}

Step 'Choose the display to split'
$monitors = @(Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction SilentlyContinue | Where-Object Active |
    ForEach-Object { -join ($_.UserFriendlyName | Where-Object { $_ } | ForEach-Object { [char]$_ }) } |
    Where-Object { $_ -and $_ -notlike 'Split Upper*' -and $_ -notlike 'Split Lower*' } | Select-Object -Unique)
if (-not $Panel) {
    for ($i = 0; $i -lt $monitors.Count; $i++) { Write-Host "  [$i] $($monitors[$i])" }
    $default = ($monitors | Where-Object { $_ -like 'Sculptor*' } | Select-Object -First 1)
    if (-not $default -and $monitors.Count) { $default = $monitors[0] }
    $answer = Read-Host "Number of the display to split (Enter = '$default')"
    $Panel = if ($answer -match '^\d+$' -and [int]$answer -lt $monitors.Count) { $monitors[[int]$answer] } else { $default }
}
$Panel = $Panel.Trim()
if (-not $Panel) { throw 'no display selected' }
Write-Host "Display: '$Panel' (it is split only while it runs as one tall display, e.g. over HDMI)"

Step 'Signing the driver with a one-time local certificate'
$pkg = Join-Path $env:TEMP ("SplitDisplayDriver-" + [guid]::NewGuid())
New-Item -ItemType Directory $pkg | Out-Null
Copy-Item (Join-Path $here 'driver\SplitDisplayIdd.dll'), (Join-Path $here 'driver\SplitDisplayIdd.inf') $pkg
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\LocalMachine\My `
    -KeyExportPolicy NonExportable -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(30)
try {
    if ((Set-AuthenticodeSignature (Join-Path $pkg 'SplitDisplayIdd.dll') -Certificate $cert -HashAlgorithm SHA256).Status -ne 'Valid') { throw 'signing the driver dll failed' }
    New-FileCatalog -Path $pkg -CatalogFilePath (Join-Path $pkg 'SplitDisplayIdd.cat') -CatalogVersion 2.0 | Out-Null
    if ((Set-AuthenticodeSignature (Join-Path $pkg 'SplitDisplayIdd.cat') -Certificate $cert -HashAlgorithm SHA256).Status -ne 'Valid') { throw 'signing the driver catalog failed' }
    $cer = Join-Path $pkg 'SplitDisplay.cer'
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
}
finally {
    # The private key is never needed again.
    Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey -ErrorAction SilentlyContinue
}
Write-Host "Certificate $($cert.Thumbprint) trusted; private key deleted."

Step 'Installing the virtual monitor driver'
& (Join-Path $here 'sdctl.exe') driver-install (Join-Path $pkg 'SplitDisplayIdd.inf')
if ($LASTEXITCODE) { throw "driver install failed (see $env:ProgramData\SplitDisplay\sdctl.log)" }

# Certificates from earlier installs are no longer needed.
foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -like "$subject*" -and $_.Thumbprint -ne $cert.Thumbprint } |
        ForEach-Object { Remove-Item $_.PSPath }
}

Step 'Installing SplitDisplay'
New-Item -ItemType Directory -Force $dest | Out-Null
foreach ($f in 'splitdisplay.exe', 'sdctl.exe', 'uninstall.ps1', 'Uninstall.cmd', 'README.txt') {
    if (Test-Path (Join-Path $here $f)) { Copy-Item (Join-Path $here $f) $dest -Force }
}
Unregister-ScheduledTask -TaskName 'SplitDisplay Failsafe Revert' -Confirm:$false -ErrorAction SilentlyContinue
$user = "$env:USERDOMAIN\$env:USERNAME"
$action = New-ScheduledTaskAction -Execute (Join-Path $dest 'splitdisplay.exe') -Argument "--panel `"$Panel`" run"
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$trigger.Delay = 'PT3S'
$taskPrincipal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName 'SplitDisplay' -Action $action -Trigger $trigger -Principal $taskPrincipal -Settings $settings `
    -Description "Splits '$Panel' into two native monitors." -Force | Out-Null
Remove-Item -Recurse -Force $pkg -ErrorAction SilentlyContinue

Step 'Starting'
Start-ScheduledTask -TaskName 'SplitDisplay'
Write-Host @"

Done. SplitDisplay now starts automatically after you sign in.
  Emergency exit:  Ctrl+Alt+Shift+F12
  Stop:            "$dest\splitdisplay.exe" stop
  Uninstall:       "$dest\Uninstall.cmd"
  Logs:            $env:ProgramData\SplitDisplay\
"@ -ForegroundColor Green
