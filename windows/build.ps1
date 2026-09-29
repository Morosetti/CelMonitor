<#
.SYNOPSIS
  Builds the Windows side: driver (WDK/NuGet) and host app (CMake). Output goes to build\.
.EXAMPLE
  .\windows\build.ps1               # everything, Release
  .\windows\build.ps1 -App          # host app only
#>
param(
    [switch]$Driver,
    [switch]$App,
    [string]$Configuration = "Release"
)
$ErrorActionPreference = "Stop"
if (-not $Driver -and -not $App) { $Driver = $true; $App = $true }
$root = Resolve-Path "$PSScriptRoot\.."

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio Build Tools com C++ nao encontrado (veja docs/COMPILAR.md)." }
$msbuild = Join-Path $vs "MSBuild\Current\Bin\amd64\MSBuild.exe"   # 64-bit: the WDK InfVerif task needs it
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

# MSBuild refuses to spawn tools when the environment block exceeds 64 KB; drop oversized variables.
foreach ($v in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
    if ($v.Value.Length -gt 8192 -and $v.Key -ne "PATH") { [Environment]::SetEnvironmentVariable($v.Key, $null) }
}

if ($Driver) {
    $drv = Join-Path $root "windows\driver"
    if (-not (Test-Path "$drv\packages\Microsoft.Windows.WDK.x64*")) {
        nuget restore "$drv\packages.config" -PackagesDirectory "$drv\packages" -NonInteractive
        if ($LASTEXITCODE) { throw "nuget restore falhou" }
    }
    & $msbuild "$drv\CelMonIdd\CelMonIdd.vcxproj" /p:Configuration=$Configuration /p:Platform=x64 /nologo /v:m
    if ($LASTEXITCODE) { throw "build do driver falhou" }

    # WinUSB package for phones in accessory mode (INF only; the catalog is generated here).
    $aoaOut = Join-Path $root "build\driver\$Configuration\CelMonAoa"
    New-Item -ItemType Directory -Force $aoaOut | Out-Null
    Copy-Item "$drv\CelMonAoa\CelMonAoa.inf" $aoaOut -Force
    Remove-Item "$aoaOut\*.cat" -ErrorAction SilentlyContinue
    $inf2cat = Get-ChildItem "$drv\packages" -Recurse -Filter Inf2Cat.exe | Select-Object -First 1 -ExpandProperty FullName
    & $inf2cat /driver:$aoaOut /os:10_X64 | Out-Null
    if (-not (Test-Path "$aoaOut\celmonaoa.cat")) { throw "Inf2Cat falhou para CelMonAoa.inf" }
    Write-Host "CelMonAoa -> $aoaOut"
}

if ($App) {
    $out = Join-Path $root "build\host"
    if (-not (Test-Path "$out\CMakeCache.txt")) {
        & $cmake -S (Join-Path $root "windows\host") -B $out -A x64
        if ($LASTEXITCODE) { throw "cmake configure falhou" }
    }
    & $cmake --build $out --config $Configuration -- /v:m /nologo
    if ($LASTEXITCODE) { throw "build do host falhou" }
}
