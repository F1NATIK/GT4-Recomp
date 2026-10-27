# Builds GT4.exe and the launcher (GT4RecompilationFull.exe) from this repository with Visual Studio's C++ compiler.
# Needs Visual Studio 2022 or newer with "Desktop development with C++" (it brings CMake and Ninja) and an internet
# connection the first time (CMake fetches raylib, glslang, volk and the Vulkan headers).
#
# The game code is NOT in this repository. Make it from your own disc first (docs/BUILDING.md), then:
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Generated <ps2_recomp output folder> `
#       -VuDir <translated VU folder> -LateFunctions <gt4_late_functions.cpp>
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Recompiler     (only the ps2_recomp tool)
param(
    [string]$Generated = '',
    [string]$VuDir = '',
    [string]$LateFunctions = '',
    [switch]$Recompiler,
    [string]$BuildDir = '',
    [int]$Jobs = 0
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$fwd = { param($p) (Resolve-Path -LiteralPath $p).Path.Replace('\', '/') }

# The Visual Studio x64 developer environment (vswhere ships with every Visual Studio installer).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio not found: install Visual Studio 2022 or newer with 'Desktop development with C++'" }
$vs = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (-not $vs) { throw "Visual Studio has no C++ tools: add 'Desktop development with C++' in the Visual Studio Installer" }
$devenv = & cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set"
foreach ($l in $devenv) { if ($l -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $vsCmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    if (Test-Path "$vsCmake\CMake\bin\cmake.exe") { $env:PATH = "$vsCmake\CMake\bin;$vsCmake\Ninja;$env:PATH" }
    else { throw "CMake not found: in the Visual Studio Installer add 'C++ CMake tools for Windows'" }
}
$j = if ($Jobs -gt 0) { $Jobs } else { [Environment]::ProcessorCount }

if ($Recompiler) {
    $build = if ($BuildDir) { $BuildDir } else { Join-Path $repo 'build\tools' }
    & cmake -S (Join-Path $repo 'recomp') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
        -DPS2X_BUILD_RECOMP=ON -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
    if ($LASTEXITCODE) { throw "cmake configure failed ($LASTEXITCODE)" }
    & cmake --build $build --parallel $j
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
    exit 0
}

if (-not $Generated -or -not (Test-Path (Join-Path $Generated 'register_functions.cpp'))) {
    throw "-Generated must be the ps2_recomp output folder made from your own disc (it has register_functions.cpp): see docs/BUILDING.md"
}
if (-not $VuDir -or -not (Test-Path (Join-Path $VuDir 'vu1_images.cpp'))) { throw "-VuDir must hold vu1_images.cpp: see docs/BUILDING.md" }
if (-not $LateFunctions -or -not (Test-Path $LateFunctions)) { throw "-LateFunctions must be gt4_late_functions.cpp: see docs/BUILDING.md" }

$build = if ($BuildDir) { $BuildDir } else { Join-Path $repo 'build\gt4' }
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    & cmake -S (Join-Path $repo 'recomp') -B $build -G Ninja `
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS2X_GAME=GT4 -DGT3_VULKAN=ON -DGT4_STOCK_BUILD=OFF -DGT3_STOCK_BUILD=OFF `
        -DPS2X_BUILD_RUNTIME=ON -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF `
        -DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_RUNNER_UNITY_BUILD=ON -DPS2X_ENABLE_RUNNER_PCH=ON `
        "-DPS2X_GENERATED_DIR=$(& $fwd $Generated)" `
        "-DPS2X_VU_GENERATED_DIR=$(& $fwd $VuDir)" `
        "-DPS2X_LATE_FUNCTIONS_CPP=$(& $fwd $LateFunctions)"
    if ($LASTEXITCODE) { throw "cmake configure failed ($LASTEXITCODE)" }
}
& cmake --build $build --target ps2EntryRunner gt4RecompilationFull --parallel $j
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
Get-ChildItem $build -Recurse -Include GT4.exe, GT4RecompilationFull.exe | ForEach-Object { "built: $($_.FullName)" }
