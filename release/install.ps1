# SplitDisplay installer (run through Install.cmd, SplitDisplaySetup.exe, or in-app Upgrade).
#
# 1. Signs the bundled driver AND executables (splitdisplay.exe, sdctl.exe) with a brand-new
#    certificate created on THIS machine, trusts that certificate, then deletes its private key.
#    Nothing signed by anyone else is trusted,nobody can sign anything else with it afterwards,
#    and Windows UAC / Defender sees the installed binaries as validly signed.
# 2. Installs the virtual monitor driver.
# 3. Copies SplitDisplay to Program Files, picks the display to split and registers a logon task.
#    Optional: -Panel "<monitor name>" to choose the display without asking.
#    Optional: -NoOpenGui to skip opening the settings window at the end.
#    Optional: -PauseOnError to keep the window open on failure (in-app install / upgrade).
# Everything is also written to %ProgramData%\SplitDisplay\install.log.
param(
    [string]$Panel,
    [switch]$NoOpenGui,
    [switch]$PauseOnError
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$dest = Join-Path $env:ProgramFiles 'SplitDisplay'
$data = Join-Path $env:ProgramData 'SplitDisplay'
$subject = 'CN=SplitDisplay Local Driver Signing'

# Strip Mark-of-the-Web (Zone.Identifier) from downloaded/extracted files so SmartScreen and UAC
# do not flag installed binaries.
Get-ChildItem $here -Recurse -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue

# The settings window runs unprivileged and writes config/logs here; files created so far were
# made by an elevated process and are read-only for the user. Grant the installing user Modify
# and also grant Interactive Users Modify so any interactive session can save settings.
New-Item -ItemType Directory -Force $data | Out-Null
try { Start-Transcript -Path (Join-Path $data 'install.log') -Force | Out-Null } catch { }
$me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
icacls $data /grant "${me}:(OI)(CI)M" /grant "*S-1-5-4:(OI)(CI)M" /T /C 2>&1 | Out-Null

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }

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

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run Install.cmd (it asks for administrator rights).' }
foreach ($f in 'splitdisplay.exe', 'sdctl.exe', 'driver\SplitDisplayIdd.dll', 'driver\SplitDisplayIdd.inf') {
    if (-not (Test-Path (Join-Path $here $f))) { throw "missing $f next to install.ps1" }
}

Step 'Stopping a running SplitDisplay'
# Ask any running compositor to restore displays and exit, then close any open settings window.
$stopExe = if (Test-Path (Join-Path $dest 'splitdisplay.exe')) { Join-Path $dest 'splitdisplay.exe' } else { Join-Path $here 'splitdisplay.exe' }
try { & $stopExe stop | Out-Null } catch { }
for ($i = 0; $i -lt 40; $i++) {
    if (-not (Get-Process splitdisplay -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Milliseconds 150
}
Get-Process splitdisplay, sdctl -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# From here on, any failure restarts the previous SplitDisplay (if one was installed) and cleans up.
$pkg = $null
$cert = $null
$driverInstalled = $false
try {

Step 'Signing driver and executables with a one-time local certificate'
$pkg = Join-Path $env:TEMP ("SplitDisplayDriver-" + [guid]::NewGuid())
New-Item -ItemType Directory $pkg | Out-Null
$binStage = Join-Path $pkg "bin"
New-Item -ItemType Directory $binStage | Out-Null
Copy-Item (Join-Path $here 'driver\SplitDisplayIdd.dll'), (Join-Path $here 'driver\SplitDisplayIdd.inf') $pkg
Copy-Item (Join-Path $here 'splitdisplay.exe'), (Join-Path $here 'sdctl.exe') $binStage
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

    # Sign splitdisplay.exe and sdctl.exe too so UAC and Windows security checks see a verified
    # publisher instead of "Unknown publisher". A valid signature is kept only if it comes from
    # another publisher (a real CA-issued release signature). One from an earlier local install
    # must be replaced: that certificate is removed from the trusted stores below, which would
    # leave the binary signed by an untrusted root.
    foreach ($exeName in 'splitdisplay.exe', 'sdctl.exe') {
        $exePath = Join-Path $binStage $exeName
        $existing = Get-AuthenticodeSignature $exePath -ErrorAction SilentlyContinue
        $foreign = $existing -and $existing.Status -eq 'Valid' -and $existing.SignerCertificate.Subject -notlike "$subject*"
        if (-not $foreign) {
            $sig = Set-AuthenticodeSignature $exePath -Certificate $cert -HashAlgorithm SHA256
            if ($sig.Status -ne 'Valid') { throw "signing $exeName failed: $($sig.Status) $($sig.StatusMessage)" }
        }
    }
}
finally {
    # The private key is never needed again. The trusted root stays behind, so a key that
    # survives this would let its holder sign drivers forever: verify it is really gone.
    Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey -ErrorAction SilentlyContinue
    if (Get-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -ErrorAction SilentlyContinue) { throw 'could not delete the one-time signing key' }
}
Write-Host "Certificate $($cert.Thumbprint) trusted; driver + executables signed; private key deleted."

Step 'Installing the virtual monitor driver'
& (Join-Path $binStage 'sdctl.exe') driver-install (Join-Path $pkg 'SplitDisplayIdd.inf')
if ($LASTEXITCODE) { throw "driver install failed (see $env:ProgramData\SplitDisplay\sdctl.log)" }
# From here on the installed driver depends on this certificate: a later failure must keep it.
$driverInstalled = $true

# Driver packages from earlier installs are no longer needed (the one in use cannot be deleted).
foreach ($oem in Get-SplitDisplayDriverPackages) { pnputil /delete-driver $oem 2>&1 | Out-Null }

# Keep the newly created certificate in Root/TrustedPublisher so the signed driver and
# executables remain trusted; remove certificates from earlier installs.
foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -like "$subject*" -and $_.Thumbprint -ne $cert.Thumbprint } |
        ForEach-Object { Remove-Item $_.PSPath }
}

Step 'Installing SplitDisplay'
New-Item -ItemType Directory -Force $dest | Out-Null
New-Item -ItemType Directory -Force (Join-Path $dest 'driver') | Out-Null
Get-ChildItem $dest -Filter 'splitdisplay.old*.exe' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

# Copy the signed executables from $binStage and the signed driver from $pkg.
foreach ($exeName in 'splitdisplay.exe', 'sdctl.exe') {
    $src = Join-Path $binStage $exeName
    $dst = Join-Path $dest $exeName
    try {
        Copy-Item $src $dst -Force
    } catch {
        if (Test-Path $dst) {
            Move-Item $dst (Join-Path $dest "$exeName.old.$PID.exe") -Force -ErrorAction SilentlyContinue
            Copy-Item $src $dst -Force
        }
    }
}
Copy-Item (Join-Path $pkg 'SplitDisplayIdd.dll'), (Join-Path $pkg 'SplitDisplayIdd.inf'), (Join-Path $pkg 'SplitDisplayIdd.cat') (Join-Path $dest 'driver') -Force

if ((Resolve-Path $here).Path.TrimEnd('\') -ne (Resolve-Path $dest).Path.TrimEnd('\')) {
    foreach ($f in 'install.ps1', 'Install.cmd', 'uninstall.ps1', 'Uninstall.cmd', 'README.txt') {
        $src = Join-Path $here $f
        if (Test-Path $src) { Copy-Item $src (Join-Path $dest $f) -Force }
    }
}

# Create Start Menu shortcut so SplitDisplay appears in Start / search.
try {
    $lnkPath = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\SplitDisplay.lnk'
    $wsh = New-Object -ComObject WScript.Shell
    $sc = $wsh.CreateShortcut($lnkPath)
    $sc.TargetPath = Join-Path $dest 'splitdisplay.exe'
    $sc.WorkingDirectory = $dest
    $sc.Description = 'SplitDisplay virtual monitor compositor'
    $sc.Save()
} catch { }

Unregister-ScheduledTask -TaskName 'SplitDisplay Failsafe Revert' -Confirm:$false -ErrorAction SilentlyContinue
$exe = Join-Path $dest 'splitdisplay.exe'
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
    if ($cert -and -not $driverInstalled) {
        foreach ($store in 'Root', 'TrustedPublisher') { Remove-Item "Cert:\LocalMachine\$store\$($cert.Thumbprint)" -ErrorAction SilentlyContinue }
    }
    if (Get-ScheduledTask -TaskName 'SplitDisplay' -ErrorAction SilentlyContinue) {
        Write-Host 'Restarting the previously installed SplitDisplay.'
        Start-ScheduledTask -TaskName 'SplitDisplay'
    }
    Write-Host "Details: $data\install.log"
    try { Stop-Transcript | Out-Null } catch { }
    if ($PauseOnError) { Read-Host 'Press Enter to close' | Out-Null }
    exit 1
}
finally {
    if ($pkg) { Remove-Item -Recurse -Force $pkg -ErrorAction SilentlyContinue }
}

Step 'Starting'
Start-ScheduledTask -TaskName 'SplitDisplay'
if (-not $NoOpenGui) {
    # Launch through explorer.exe so the settings window opens under the normal interactive user token.
    Start-Process explorer.exe -ArgumentList "`"$exe`""
}
Write-Host @"

Done. SplitDisplay is installed and starts automatically after you sign in.
  Settings and layout:  "$dest\splitdisplay.exe"
  Emergency exit:       Ctrl+Alt+Shift+F12
  Uninstall:            "$dest\Uninstall.cmd"
  Logs:                 $env:ProgramData\SplitDisplay\
"@ -ForegroundColor Green
try { Stop-Transcript | Out-Null } catch { }
