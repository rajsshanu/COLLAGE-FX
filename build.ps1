# Build Mixed Media OFX for DaVinci Resolve on Windows x64.
# Requires Visual Studio Build Tools (C++ workload) and Python 3.
param(
    [switch]$SkipTests,
    [switch]$Debug
)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

function Find-VsInstall {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $path = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1)
        if (-not $path) {
            $path = (& $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1)
        }
        if ($path) { return $path }
    }
    $base = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio"
    if (Test-Path $base) {
        $found = Get-ChildItem $base -Directory |
            ForEach-Object { Join-Path $_.FullName "BuildTools" } |
            Where-Object { Test-Path (Join-Path $_ "VC\Auxiliary\Build\vcvars64.bat") } |
            Select-Object -First 1
        if ($found) { return $found }
    }
    return $null
}

$vsPath = Find-VsInstall
if (-not $vsPath) {
    throw "Visual Studio C++ Build Tools not found. Install 'Desktop development with C++'."
}
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at $vcvars"
}

New-Item -ItemType Directory -Force build, build\obj, generated | Out-Null

Write-Host "[1/3] Compiling CUDA kernels to embedded PTX..." -ForegroundColor Cyan
python tools\compile_ptx.py
if ($LASTEXITCODE -ne 0) { throw "PTX compilation failed." }

Write-Host "[2/3] Building MixedMedia.ofx..." -ForegroundColor Cyan
$bundle = Join-Path $root "dist\MixedMedia.ofx.bundle\Contents\Win64"
New-Item -ItemType Directory -Force $bundle | Out-Null
$out = Join-Path $bundle "MixedMedia.ofx"
$obj = Join-Path $root "build\obj"
$objFwd = ($obj -replace '\\', '/') + '/'

$sources = Get-ChildItem (Join-Path $root "src") -Filter *.cpp | ForEach-Object { "`"$($_.FullName)`"" }
$sources = $sources -join " "
$includes = @(
    "/I`"$root\src`"",
    "/I`"$root\generated`"",
    "/I`"$root\vendor\openfx\include`""
) -join " "

$configFlags = if ($Debug) { "/Od /Zi /MTd /D_DEBUG" } else { "/O2 /Ob2 /MT /DNDEBUG" }
$bat = Join-Path $root "build\build_plugin.bat"
$batText = @"
@echo off
call "$vcvars" >nul
cl /nologo /LD /std:c++17 /EHsc /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS $configFlags $includes /Fo"$objFwd" $sources /Fe:"$out" /link /MACHINE:X64 /SUBSYSTEM:WINDOWS
"@
Set-Content -Path $bat -Value $batText -Encoding ASCII
& cmd.exe /d /c $bat
if ($LASTEXITCODE -ne 0) { throw "C++ build failed (exit $LASTEXITCODE)." }
if (-not (Test-Path $out)) { throw "Build did not produce $out." }
Remove-Item (Join-Path $bundle "MixedMedia.lib"), (Join-Path $bundle "MixedMedia.exp") -Force -ErrorAction SilentlyContinue

if (-not $SkipTests) {
    Write-Host "[3/3] Building and running tests..." -ForegroundColor Cyan
    $testObj = Join-Path $root "build\testobj"
    New-Item -ItemType Directory -Force $testObj, "$testObj\render", "$testObj\gpu", "$testObj\host" | Out-Null
    $testObjFwd = $testObj -replace '\\', '/'
    $inc = "/I`"$root\src`" /I`"$root\generated`" /I`"$root\vendor\openfx\include`""
    $common = "/nologo /O2 /Ob2 /std:c++17 /EHsc /W4 /utf-8 /MT /DNDEBUG /D_CRT_SECURE_NO_WARNINGS $inc"
    $testBat = Join-Path $root "build\build_tests.bat"
    $testText = @"
@echo off
call "$vcvars" >nul
cl $common /Fo"$testObjFwd/render/" "$root\tests\render_cli.cpp" "$root\src\CpuRender.cpp" /Fe:"$root\build\render_cli.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
cl $common /Fo"$testObjFwd/gpu/" "$root\tests\gpu_parity.cpp" "$root\src\CudaBackend.cpp" "$root\src\CpuRender.cpp" "$root\src\Log.cpp" /Fe:"$root\build\gpu_parity.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
cl $common /Fo"$testObjFwd/host/" "$root\tests\ofx_host_test.cpp" /Fe:"$root\build\ofx_host_test.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
"@
    Set-Content -Path $testBat -Value $testText -Encoding ASCII
    & cmd.exe /d /c $testBat
    if ($LASTEXITCODE -ne 0) { throw "Test build failed (exit $LASTEXITCODE)." }

    & "$root\build\ofx_host_test.exe" $out
    if ($LASTEXITCODE -ne 0) { throw "OFX host test failed." }

    & "$root\build\gpu_parity.exe" 256 256
    if ($LASTEXITCODE -eq 77) {
        Write-Host "CUDA parity test skipped: no NVIDIA CUDA device." -ForegroundColor Yellow
    } elseif ($LASTEXITCODE -ne 0) {
        throw "CPU/GPU parity test failed."
    }
}

$bundlePath = Join-Path $root "dist\MixedMedia.ofx.bundle"
$zipPath = Join-Path $root "dist\MixedMedia-ofx-windows-x64.zip"
Compress-Archive -Path $bundlePath -DestinationPath $zipPath -Force

Write-Host "[done] Mixed Media OFX built." -ForegroundColor Green
Write-Host "Plugin: $out"
Write-Host "Bundle: $bundlePath"
Write-Host "Zip:    $zipPath"
