# Run this YOURSELF in an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File D:\Github\SplitDisplay\scripts\trust-cert.ps1
#
# Adds the SplitDisplay public certificate to LocalMachine\Root and LocalMachine\TrustedPublisher so
# Windows accepts the self-signed driver package without test-signing mode.
# Undo:  scripts\untrust-cert.ps1
$ErrorActionPreference = 'Stop'
$cer = Join-Path $PSScriptRoot '..\out\SplitDisplay.cer'
if (-not (Test-Path $cer)) { throw "missing $cer - run make-cert.ps1 first" }

$c = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($cer)
Write-Host "Certificate : $($c.Subject)"
Write-Host "Thumbprint  : $($c.Thumbprint)"
Write-Host "Valid until : $($c.NotAfter)"
Write-Host "EKU         : $(($c.EnhancedKeyUsageList | ForEach-Object FriendlyName) -join ', ')"
$ok = Read-Host "Trust this certificate for driver installation on this PC? (y/n)"
if ($ok -ne 'y') { Write-Host 'Aborted.'; exit 1 }

Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
Write-Host 'Done. Certificate trusted in Root and TrustedPublisher.'
