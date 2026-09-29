<#
.SYNOPSIS
  Installs the CelMonitor virtual display driver. Run from an elevated PowerShell.

.DESCRIPTION
  1. Trusts the local signing certificate (LocalMachine\Root + TrustedPublisher) so Windows accepts the driver
     package without test-signing mode. Only code signed by that certificate is affected.
  2. Creates the Root\CelMonIdd device (or updates its driver if it already exists).
  The virtual monitor itself only appears while the CelMonitor app has a phone connected.
#>
param(
    [string]$Configuration = "Release"
)
$ErrorActionPreference = "Stop"
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Execute este script em um PowerShell como Administrador."
}

$root = Resolve-Path "$PSScriptRoot\..\.."
$pkg = Join-Path $root "build\driver\$Configuration\CelMonIdd"
$cer = Join-Path $root "build\driver\CelMonitorDriver.cer"
$devcon = Get-ChildItem "$root\windows\driver\packages" -Recurse -Filter devcon.exe |
    Where-Object FullName -match '\\x64\\' | Select-Object -First 1 -ExpandProperty FullName
if (-not (Test-Path "$pkg\celmonidd.cat")) { throw "Pacote do driver nao encontrado em $pkg" }
if (-not (Test-Path $cer)) { throw "Certificado nao encontrado ($cer). Rode sign-driver.ps1 antes." }
if (-not $devcon) { throw "devcon.exe nao encontrado (restaure windows\driver\packages.config)." }

$sig = Get-AuthenticodeSignature "$pkg\celmonidd.cat"
if ($sig.SignerCertificate -eq $null) { throw "O catalogo nao esta assinado. Rode sign-driver.ps1 antes." }

Write-Host "Confiando no certificado de assinatura local..."
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

$existing = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains "Root\CelMonIdd" }
if ($existing) {
    Write-Host "Atualizando driver do dispositivo existente..."
    & $devcon update "$pkg\CelMonIdd.inf" "Root\CelMonIdd"
} else {
    Write-Host "Criando o dispositivo Root\CelMonIdd..."
    & $devcon install "$pkg\CelMonIdd.inf" "Root\CelMonIdd"
}
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1) { throw "devcon falhou (codigo $LASTEXITCODE)" }
if ($LASTEXITCODE -eq 1) { Write-Warning "O Windows pediu reinicializacao para concluir a instalacao." }

Start-Sleep -Seconds 2
Get-PnpDevice | Where-Object { $_.HardwareID -contains "Root\CelMonIdd" } | Format-Table Status, Class, FriendlyName, InstanceId -AutoSize

# WinUSB for phones in accessory mode (direct USB connection, no adb in the data path).
$aoa = Join-Path $root "build\driver\$Configuration\CelMonAoa"
if (Test-Path "$aoa\celmonaoa.cat") {
    if ((Get-AuthenticodeSignature "$aoa\celmonaoa.cat").SignerCertificate -eq $null) { throw "celmonaoa.cat nao esta assinado. Rode sign-driver.ps1 antes." }
    Write-Host "Instalando o driver USB do modo acessorio (CelMonAoa)..."
    pnputil /add-driver "$aoa\CelMonAoa.inf" /install
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "pnputil falhou (codigo $LASTEXITCODE)" }
} else {
    Write-Warning "Pacote CelMonAoa nao encontrado; a conexao direta (AOA) ficara indisponivel, so ADB."
}
