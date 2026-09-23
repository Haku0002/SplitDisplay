# SplitDisplay installer (run through Install.cmd, which elevates).
#
# 1. Signs the bundled driver with a brand-new certificate created on THIS machine, trusts that
#    certificate, then deletes its private key. Nothing signed by anyone else is trusted, and
#    nobody (including you) can sign anything else with it afterwards.
# 2. Installs the virtual monitor driver.
# 3. Copies SplitDisplay to Program Files, picks the display to split and registers a logon task.
#    Optional: -Panel "<monitor name>" to choose the display without asking.
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
# Restores the single display first.
if (Test-Path (Join-Path $dest 'splitdisplay.exe')) {
    & (Join-Path $dest 'splitdisplay.exe') stop | Out-Null
    Get-Process splitdisplay -ErrorAction SilentlyContinue | Wait-Process -Timeout 20 -ErrorAction SilentlyContinue
}

# From here on, any failure restarts the previous SplitDisplay (if one was installed) and cleans up.
$pkg = $null
$cert = $null
try {

Step 'Signing the driver with a one-time local certificate'
$pkg = Join-Path $env:TEMP ("SplitDisplayDriver-" + [guid]::NewGuid())
New-Item -ItemType Directory $pkg | Out-Null
Copy-Item (Join-Path $here 'driver\SplitDisplayIdd.dll'), (Join-Path $here 'driver\SplitDisplayIdd.inf') $pkg
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\LocalMachine\My `
    -KeyExportPolicy NonExportable -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(30)
try {
    # Trust first: Set-AuthenticodeSignature reports Valid only for a chain the machine trusts.
    $cer = Join-Path $pkg 'SplitDisplay.cer'
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

    $sig = Set-AuthenticodeSignature (Join-Path $pkg 'SplitDisplayIdd.dll') -Certificate $cert -HashAlgorithm SHA256
    if ($sig.Status -ne 'Valid') { throw "signing the driver dll failed: $($sig.Status) $($sig.StatusMessage)" }
    New-FileCatalog -Path $pkg -CatalogFilePath (Join-Path $pkg 'SplitDisplayIdd.cat') -CatalogVersion 2.0 | Out-Null
    $sig = Set-AuthenticodeSignature (Join-Path $pkg 'SplitDisplayIdd.cat') -Certificate $cert -HashAlgorithm SHA256
    if ($sig.Status -ne 'Valid') { throw "signing the driver catalog failed: $($sig.Status) $($sig.StatusMessage)" }
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
# splitdisplay.exe is a windowed app, so wait for it explicitly.
$exe = Join-Path $dest 'splitdisplay.exe'
# Which display to split: -Panel "<name>" if given, otherwise auto-detect (keeps an earlier choice).
# With several candidates nothing is chosen here; the settings window opened below asks.
if ($Panel) {
    $r = Start-Process $exe -ArgumentList '--panel', "`"$Panel`"", 'configure' -Wait -PassThru
} else {
    $r = Start-Process $exe -ArgumentList 'autodetect' -Wait -PassThru
    if ($r.ExitCode -eq 2) { Write-Host 'Several displays found: pick the one to split in the settings window.' -ForegroundColor Yellow }
}
$r = Start-Process $exe -ArgumentList 'autostart', 'on' -Wait -PassThru
if ($r.ExitCode) { throw 'creating the logon task failed' }

}
catch {
    Write-Host "`nInstall failed: $_" -ForegroundColor Red
    if ($cert) {
        # Do not leave a trusted certificate behind for a driver that was not installed.
        foreach ($store in 'Root', 'TrustedPublisher') { Remove-Item "Cert:\LocalMachine\$store\$($cert.Thumbprint)" -ErrorAction SilentlyContinue }
    }
    if (Get-ScheduledTask -TaskName 'SplitDisplay' -ErrorAction SilentlyContinue) {
        Write-Host 'Restarting the previously installed SplitDisplay.'
        Start-ScheduledTask -TaskName 'SplitDisplay'
    }
    exit 1
}
finally {
    if ($pkg) { Remove-Item -Recurse -Force $pkg -ErrorAction SilentlyContinue }
}

Step 'Starting'
Start-ScheduledTask -TaskName 'SplitDisplay'
Start-Process (Join-Path $dest 'splitdisplay.exe') # settings window
Write-Host @"

Done. SplitDisplay now starts automatically after you sign in.
  Settings and layout:  "$dest\splitdisplay.exe" (opened now)
  Emergency exit:       Ctrl+Alt+Shift+F12
  Uninstall:            "$dest\Uninstall.cmd"
  Logs:                 $env:ProgramData\SplitDisplay\
"@ -ForegroundColor Green
