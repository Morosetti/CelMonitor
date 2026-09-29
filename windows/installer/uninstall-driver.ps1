<#
.SYNOPSIS
  Removes the CelMonitor virtual display driver (and optionally the trusted signing certificate). Run elevated.
#>
param(
    [switch]$RemoveCertificate
)
$ErrorActionPreference = "Stop"
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Execute este script em um PowerShell como Administrador."
}
$root = Resolve-Path "$PSScriptRoot\..\.."
$devcon = Get-ChildItem "$root\windows\driver\packages" -Recurse -Filter devcon.exe |
    Where-Object FullName -match '\\x64\\' | Select-Object -First 1 -ExpandProperty FullName

if ($devcon) { & $devcon remove "Root\CelMonIdd" }

# Remove the package from the driver store.
$drivers = Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like "*celmonidd.inf" }
foreach ($d in $drivers) {
    Write-Host "Removendo $($d.Driver) do driver store"
    pnputil /delete-driver $d.Driver /uninstall /force
}

if ($RemoveCertificate) {
    foreach ($store in "Root", "TrustedPublisher") {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Subject -eq "CN=CelMonitor Local Driver Signing" |
            ForEach-Object { Write-Host "Removendo certificado de $store"; Remove-Item $_.PSPath }
    }
}
