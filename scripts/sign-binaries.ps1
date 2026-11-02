# Signs SplitDisplay binaries (.exe / .dll / .cat) when a signing certificate is configured:
#   1. $env:SD_SIGN_PFX_BASE64 (or $env:SD_SIGN_PFX_PATH) + $env:SD_SIGN_PASSWORD  (CI / release PFX)
#   2. $env:SD_SIGN_THUMBPRINT                                                     (cert in Windows store)
#   3. -UseDevCert                                                                 (local developer cert)
# If no certificate is configured, exits 0 without failing the build (install.ps1 signs on the target machine).
param(
    [Parameter(Mandatory)][string[]]$Files,
    [switch]$UseDevCert
)

$ErrorActionPreference = 'Stop'
$timestampUrl = "http://timestamp.digicert.com"
$cert = $null
$tempCertThumbprint = $null

try {
    if ($env:SD_SIGN_PFX_BASE64 -or $env:SD_SIGN_PFX_PATH) {
        $pfxPath = if ($env:SD_SIGN_PFX_PATH) { $env:SD_SIGN_PFX_PATH } else {
            $tmp = Join-Path $env:TEMP ("sd-sign-" + [guid]::NewGuid() + ".pfx")
            [IO.File]::WriteAllBytes($tmp, [Convert]::FromBase64String($env:SD_SIGN_PFX_BASE64))
            $tmp
        }
        $sec = if ($env:SD_SIGN_PASSWORD) { ConvertTo-SecureString $env:SD_SIGN_PASSWORD -AsPlainText -Force } else { [securestring]::new() }
        $cert = Import-PfxCertificate -FilePath $pfxPath -CertStoreLocation Cert:\CurrentUser\My -Password $sec
        $tempCertThumbprint = $cert.Thumbprint
        if (-not $env:SD_SIGN_PFX_PATH -and (Test-Path $pfxPath)) { Remove-Item $pfxPath -Force -ErrorAction SilentlyContinue }
    } elseif ($env:SD_SIGN_THUMBPRINT) {
        $cert = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
            Where-Object { $_.Thumbprint -ieq $env:SD_SIGN_THUMBPRINT } | Select-Object -First 1
        if (-not $cert) { throw "Certificate $env:SD_SIGN_THUMBPRINT not found in CurrentUser\My or LocalMachine\My" }
    } elseif ($UseDevCert -or $env:SD_DEV_SIGN -eq "1") {
        $devSubj = "CN=SplitDisplay Local Dev Signing"
        $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -eq $devSubj -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
        if (-not $cert) {
            $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $devSubj -CertStoreLocation Cert:\CurrentUser\My `
                -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
            $cer = Join-Path $env:TEMP "SplitDisplay-Dev.cer"
            Export-Certificate -Cert $cert -FilePath $cer | Out-Null
            try {
                Import-Certificate -FilePath $cer -CertStoreLocation Cert:\CurrentUser\Root | Out-Null
                Import-Certificate -FilePath $cer -CertStoreLocation Cert:\CurrentUser\TrustedPublisher | Out-Null
            } catch { }
            Remove-Item $cer -Force -ErrorAction SilentlyContinue
        }
    }

    if (-not $cert) {
        Write-Host "No release signing certificate configured (set SD_SIGN_PFX_BASE64 or SD_SIGN_THUMBPRINT); skipping pre-signing."
        return
    }

    foreach ($f in $Files) {
        if (-not (Test-Path $f)) { continue }
        Unblock-File -LiteralPath $f -ErrorAction SilentlyContinue
        $sig = Set-AuthenticodeSignature -LiteralPath $f -Certificate $cert -HashAlgorithm SHA256 -TimestampServer $timestampUrl -ErrorAction SilentlyContinue
        if (-not $sig -or $sig.Status -eq "UnknownError") {
            # Fallback without timestamp server when offline or using a local dev certificate.
            $sig = Set-AuthenticodeSignature -LiteralPath $f -Certificate $cert -HashAlgorithm SHA256
        }
        Write-Host "Signed $(Split-Path $f -Leaf): $($sig.Status)"
    }
} finally {
    if ($tempCertThumbprint) {
        Remove-Item "Cert:\CurrentUser\My\$tempCertThumbprint" -DeleteKey -ErrorAction SilentlyContinue
    }
}
