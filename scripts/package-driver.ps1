# Builds the signed driver package in out\driver (dll + inf + cat).
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$pkg = Join-Path $root 'out\driver'
$subject = 'CN=SplitDisplay Local Driver Signing'
$signtool = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
$inf2cat = Join-Path $root 'third_party\wdk\c\bin\10.0.26100.0\x86\Inf2Cat.exe'

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq $subject | Select-Object -First 1
if (-not $cert) { throw 'signing cert missing - run make-cert.ps1' }

Remove-Item -Recurse -Force $pkg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $pkg | Out-Null
Copy-Item (Join-Path $root 'build\Release\SplitDisplayIdd.dll') $pkg
Copy-Item (Join-Path $root 'driver\SplitDisplayIdd.inf') $pkg

& $signtool sign /q /fd SHA256 /sha1 $cert.Thumbprint /s My (Join-Path $pkg 'SplitDisplayIdd.dll')
if ($LASTEXITCODE) { throw 'sign dll failed' }
& $inf2cat /driver:$pkg /os:10_GE_X64 /uselocaltime | Out-Host
if ($LASTEXITCODE) { throw 'inf2cat failed' }
& $signtool sign /q /fd SHA256 /sha1 $cert.Thumbprint /s My (Join-Path $pkg 'SplitDisplayIdd.cat')
if ($LASTEXITCODE) { throw 'sign cat failed' }
Get-ChildItem $pkg | Format-Table Name, Length
