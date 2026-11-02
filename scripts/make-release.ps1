# Builds the release artifacts:
#   out\SplitDisplay-v<version>-x64.zip        (portable archive)
#   out\SplitDisplay-Setup-v<version>-x64.exe  (standalone single-file graphical installer)
# The driver ships unsigned; release\install.ps1 signs it on the user's machine.
param([Parameter(Mandatory)][string]$Version)   # e.g. 0.4.1

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$name = "SplitDisplay-v$Version-x64"
$stage = Join-Path $root "out\$name"
$zip = Join-Path $root "out\$name.zip"
$setupExe = Join-Path $root "out\SplitDisplay-Setup-v$Version-x64.exe"

# The one true version lives in CMakeLists.txt (manifest, VERSIONINFO and the settings window
# all read it from there); refuse to package a release whose name would disagree with the binaries.
$cmakeLists = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmakeLists -notmatch 'project\(SplitDisplay VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { throw 'could not read the project version from CMakeLists.txt' }
if ($Matches[1] -ne $Version) { throw "release version $Version != CMakeLists.txt version $($Matches[1]); bump the project version there first" }

cmake --build (Join-Path $root 'build') --config Release
if ($LASTEXITCODE) { throw 'build failed' }

Remove-Item -Recurse -Force $stage, $zip, $setupExe -ErrorAction SilentlyContinue
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

& (Join-Path $root 'scripts\sign-binaries.ps1') -Files (
    (Join-Path $stage 'splitdisplay.exe'),
    (Join-Path $stage 'sdctl.exe'),
    (Join-Path $stage 'driver\SplitDisplayIdd.dll')
)

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip

# Embed the release zip into SplitDisplaySetup.exe so users can also download a single-file installer.
$embeddedZip = Join-Path $root 'build\generated\package.zip'
Copy-Item $zip $embeddedZip -Force
$rcFile = Join-Path $root "build\generated\setup.rc"
if (Test-Path $rcFile) { (Get-Item $rcFile).LastWriteTime = Get-Date }
Get-ChildItem (Join-Path $root 'build') -Recurse -Filter 'setup.res' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
cmake --build (Join-Path $root 'build') --config Release --target SplitDisplaySetup
if ($LASTEXITCODE) { throw 'SplitDisplaySetup build failed' }
Copy-Item (Join-Path $root 'build\Release\SplitDisplaySetup.exe') $setupExe -Force
& (Join-Path $root 'scripts\sign-binaries.ps1') -Files @($setupExe)

# Symbols are published separately (never inside the installer) so dumps can be read later.
# Named so it sorts after the package: the GitHub API lists assets by name, and 0.5.0 updaters take
# the first .zip ("-x64_symbols" > "-x64.zip", whereas "-x64-symbols" sorted first).
$symZip = Join-Path $root "out\SplitDisplay-v$Version-x64_symbols.zip"
Remove-Item $symZip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $root 'build\Release\splitdisplay.pdb'), (Join-Path $root 'build\Release\sdctl.pdb'), (Join-Path $root 'build\Release\SplitDisplayIdd.pdb') -DestinationPath $symZip
"$symZip"

$hashZip = (Get-FileHash $zip -Algorithm SHA256).Hash
$hashExe = (Get-FileHash $setupExe -Algorithm SHA256).Hash
"$zip  (SHA256 $hashZip)"
"$setupExe  (SHA256 $hashExe)"
"DriverVer=$driverVer"
