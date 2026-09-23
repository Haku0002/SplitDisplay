# Removes the SplitDisplay certificate from the machine trust stores (elevated).
$subject = 'CN=SplitDisplay Local Driver Signing'
foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Subject -eq $subject | ForEach-Object {
        Remove-Item $_.PSPath
        "removed $($_.Thumbprint) from LocalMachine\$store"
    }
}
