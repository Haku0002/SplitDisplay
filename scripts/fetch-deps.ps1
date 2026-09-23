# Downloads the Windows Driver Kit NuGet package into third_party\wdk (headers, libs, Inf2Cat).
# No system-wide WDK install is needed.
$ErrorActionPreference = 'Stop'
$version = '10.0.26100.6584'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$dir = Join-Path $root 'third_party'
$wdk = Join-Path $dir 'wdk'
if (Test-Path (Join-Path $wdk 'c\Include')) { "WDK already present in $wdk"; return }

New-Item -ItemType Directory -Force $dir | Out-Null
$pkg = Join-Path $dir 'wdk.nupkg'
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/$version/microsoft.windows.wdk.x64.$version.nupkg"
"downloading $url"
Invoke-WebRequest $url -OutFile $pkg -UseBasicParsing
Expand-Archive $pkg -DestinationPath $wdk -Force
Remove-Item $pkg
"WDK $version extracted to $wdk"
