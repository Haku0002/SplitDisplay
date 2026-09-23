# Builds a release zip: out\SplitDisplay-v<version>-x64.zip
# The driver ships unsigned; release\install.ps1 signs it on the user's machine.
param([Parameter(Mandatory)][string]$Version)   # e.g. 0.1.0

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$name = "SplitDisplay-v$Version-x64"
$stage = Join-Path $root "out\$name"
$zip = Join-Path $root "out\$name.zip"

cmake --build (Join-Path $root 'build') --config Release
if ($LASTEXITCODE) { throw 'build failed' }

Remove-Item -Recurse -Force $stage, $zip -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $stage 'driver') | Out-Null

Copy-Item (Join-Path $root 'build\Release\splitdisplay.exe'), (Join-Path $root 'build\Release\sdctl.exe') $stage
Copy-Item (Join-Path $root 'build\Release\SplitDisplayIdd.dll') (Join-Path $stage 'driver')

# Driver version: 1.<minor>.<patch>.0 so it always ranks above development builds (1.0.0.x).
$v = [version]$Version
$driverVer = "{0},1.{1}.{2}.{3}" -f (Get-Date -Format 'MM/dd/yyyy'), ($v.Major * 100 + $v.Minor), $v.Build, 0
(Get-Content (Join-Path $root 'driver\SplitDisplayIdd.inf')) -replace '^DriverVer=.*', "DriverVer=$driverVer" |
    Set-Content (Join-Path $stage 'driver\SplitDisplayIdd.inf') -Encoding ASCII

foreach ($f in 'Install.cmd', 'install.ps1', 'Uninstall.cmd', 'uninstall.ps1', 'README.txt') {
    Copy-Item (Join-Path $root "release\$f") $stage
}
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $stage 'THIRD_PARTY_NOTICES.txt')

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
"$zip"
"DriverVer=$driverVer"
"SHA256 $hash"
