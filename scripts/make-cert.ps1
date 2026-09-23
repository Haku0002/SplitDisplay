# Creates the local code-signing certificate used to sign the SplitDisplay driver.
# The private key stays non-exportable in CurrentUser\My; only the public .cer is written out.
$ErrorActionPreference = 'Stop'
$subject = 'CN=SplitDisplay Local Driver Signing'
$out = Join-Path $PSScriptRoot '..\out'
New-Item -ItemType Directory -Force $out | Out-Null

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq $subject | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
        -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
}
Export-Certificate -Cert $cert -FilePath (Join-Path $out 'SplitDisplay.cer') -Type CERT | Out-Null
"thumbprint: $($cert.Thumbprint)"
