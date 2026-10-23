# Downloads the Windows Driver Kit NuGet package into third_party\wdk (headers, libs, Inf2Cat).
# No system-wide WDK install is needed. The package hash is pinned: a changed hash means the
# package was re-published, and the build should stop until someone looks at why.
$ErrorActionPreference = 'Stop'
$version = '10.0.26100.6584'
$sha256 = 'C393D03DFB640B5C92F546B32F6770EF68CD3AAF691956E7D66D8E2C28A1B55E'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$dir = Join-Path $root 'third_party'
$wdk = Join-Path $dir 'wdk'
if (Test-Path (Join-Path $wdk 'c\Include')) { "WDK already present in $wdk"; return }

New-Item -ItemType Directory -Force $dir | Out-Null
$pkg = Join-Path $dir 'wdk.nupkg'
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/$version/microsoft.windows.wdk.x64.$version.nupkg"
"downloading $url"
Invoke-WebRequest $url -OutFile $pkg -UseBasicParsing
$hash = (Get-FileHash $pkg -Algorithm SHA256).Hash
if ($hash -ne $sha256) { Remove-Item $pkg; throw "WDK package hash mismatch: got $hash, expected $sha256" }
Expand-Archive $pkg -DestinationPath $wdk -Force
Remove-Item $pkg
"WDK $version extracted to $wdk"
