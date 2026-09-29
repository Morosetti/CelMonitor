<#
.SYNOPSIS
  Signs the CelMonIdd driver package with a local self-signed certificate (development / personal use).

.DESCRIPTION
  Creates (once) a code-signing certificate "CelMonitor Local Driver Signing" in CurrentUser\My, exports its public
  part to build\driver\CelMonitorDriver.cer and signs CelMonIdd.dll + celmonidd.cat. No admin rights needed.
  Distribution to other PCs requires an EV certificate + Microsoft attestation signing instead (see docs).
#>
param(
    [string]$Configuration = "Release"
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\.."
$pkg = Join-Path $root "build\driver\$Configuration\CelMonIdd"
if (-not (Test-Path "$pkg\CelMonIdd.inf")) { throw "Driver package not found at $pkg. Build windows\driver first." }

$signtool = Get-ChildItem "$root\windows\driver\packages" -Recurse -Filter signtool.exe |
    Where-Object FullName -match '\\x64\\' | Select-Object -First 1 -ExpandProperty FullName
if (-not $signtool) { throw "signtool.exe not found (restore windows\driver\packages.config)." }

$subject = "CN=CelMonitor Local Driver Signing"
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date) } |
    Select-Object -First 1
if (-not $cert) {
    Write-Host "Creating signing certificate $subject"
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\CurrentUser\My `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5) -KeyExportPolicy NonExportable
}
$cer = Join-Path $root "build\driver\CelMonitorDriver.cer"
Export-Certificate -Cert $cert -FilePath $cer -Force | Out-Null

foreach ($f in @("$pkg\CelMonIdd.dll", "$pkg\celmonidd.cat")) {
    & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My $f
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $f" }
}
Write-Host "Signed with $($cert.Thumbprint). Public certificate: $cer"
