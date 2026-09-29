<#
.SYNOPSIS
  Builds everything and produces build\installer\CelMonitor-Setup-<version>.exe (one file for new users).
.DESCRIPTION
  1. Windows: driver + app (windows\build.ps1), then signs the driver packages (sign-driver.ps1).
  2. Android: release APK (gradle assembleRelease).
  3. Stages build\dist and compiles windows\installer\CelMonitor.iss with Inno Setup 6.
#>
param([string]$Version = "0.2.0")
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\.."

Write-Host "== Windows (driver + app)"
& "$root\windows\build.ps1"
& "$root\windows\installer\sign-driver.ps1"

Write-Host "== Android (APK)"
if (-not $env:JAVA_HOME -or -not (Test-Path "$env:JAVA_HOME\bin\java.exe")) {
    $jdk = Get-ChildItem "C:\Program Files\Microsoft" -Directory -Filter "jdk-17*" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($jdk) { $env:JAVA_HOME = $jdk.FullName } else { throw "JDK 17 não encontrado (defina JAVA_HOME)." }
}
if (-not $env:JAVA_TOOL_OPTIONS) {
    # Short AF_UNIX temp dir: the JDK fails with "Unable to establish loopback connection" under long/8.3 TEMP paths.
    New-Item -ItemType Directory -Force "$env:SystemDrive\jtmp" | Out-Null
    $env:JAVA_TOOL_OPTIONS = "-Djdk.net.unixdomain.tmpdir=$env:SystemDrive\jtmp"
}
# The JDK prints "Picked up JAVA_TOOL_OPTIONS" on stderr; with "Stop" Windows PowerShell would treat it as fatal
# when the output is redirected. The exit code decides.
$ErrorActionPreference = "Continue"
& "$root\android\gradlew.bat" -p "$root\android" testDebugUnitTest assembleRelease --console=plain -q
$ErrorActionPreference = "Stop"
if ($LASTEXITCODE) { throw "build do Android falhou" }

Write-Host "== Staging"
$dist = Join-Path $root "build\dist"
Remove-Item $dist -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$dist\drivers\CelMonIdd", "$dist\drivers\CelMonAoa", "$dist\docs" | Out-Null
Copy-Item "$root\build\host\Release\CelMonitor.exe", "$root\build\host\Release\celmon-cli.exe" $dist
Copy-Item "$root\android\app\build\outputs\apk\release\app-release.apk" "$dist\CelMonitor.apk"
Copy-Item "$root\build\driver\Release\CelMonIdd\*" "$dist\drivers\CelMonIdd"
Copy-Item "$root\build\driver\Release\CelMonAoa\*" "$dist\drivers\CelMonAoa"
Copy-Item "$root\build\driver\CelMonitorDriver.cer" "$dist\drivers"
Copy-Item "$root\docs\*.md" "$dist\docs" -ErrorAction SilentlyContinue

Write-Host "== Inno Setup"
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 não encontrado (winget install JRSoftware.InnoSetup)." }
& $iscc /Qp "/DAppVersion=$Version" "/DDistDir=$dist" "$root\windows\installer\CelMonitor.iss"
if ($LASTEXITCODE) { throw "Inno Setup falhou" }
Get-Item "$root\build\installer\CelMonitor-Setup-$Version.exe" | Select-Object FullName, @{n='MB';e={[math]::Round($_.Length/1MB,1)}}
