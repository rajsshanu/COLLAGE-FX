# Build the self-contained Mixed Media Studio installer (Windows x64).
# Requires the Studio app and the OFX bundle in dist\, which are built by
# build-studio.ps1 and build.ps1.
param(
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

$studio = Join-Path $root "dist\MixedMediaStudio.exe"
if (-not (Test-Path $studio)) {
    Write-Host "Studio app not built yet; building it first..." -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "build-studio.ps1")
    if ($LASTEXITCODE -ne 0) { throw "Studio build failed." }
}

$plugin = Join-Path $root "dist\MixedMedia.ofx.bundle\Contents\Win64\MixedMedia.ofx"
if (-not (Test-Path $plugin)) {
    Write-Host "OFX plugin not built yet; building it first..." -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "build.ps1") -SkipTests
    if ($LASTEXITCODE -ne 0) { throw "OFX plugin build failed." }
}

New-Item -ItemType Directory -Force build, build\setup, dist | Out-Null

$out = Join-Path $root "dist\MixedMediaSetup.exe"
$res = Join-Path $root "build\setup\Setup.res"
$configFlags = if ($Debug) { "/Od /Zi /MTd /D_DEBUG" } else { "/O2 /Ob2 /MT /DNDEBUG" }

$bat = Join-Path $root "build\build_setup.bat"
$batText = @"
@echo off
call "$vcvars" >nul
rc /nologo /fo "$res" "$root\installer\Setup.rc"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /utf-8 $configFlags /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
 /I"$root\installer" ^
 /Fo"$root\build\setup\\" ^
 "$root\installer\Setup.cpp" ^
 /Fe:"$out" /link /SUBSYSTEM:WINDOWS /MACHINE:X64 /MANIFEST:NO ^
 comctl32.lib shlwapi.lib shell32.lib ole32.lib uuid.lib gdiplus.lib uxtheme.lib advapi32.lib user32.lib gdi32.lib ^
 "$res"
"@
Set-Content -Path $bat -Value $batText -Encoding ASCII
& cmd.exe /d /c $bat
if ($LASTEXITCODE -ne 0) { throw "Setup build failed (exit $LASTEXITCODE)." }
if (-not (Test-Path $out)) { throw "Build did not produce $out." }

Write-Host "[done] Mixed Media Studio installer built." -ForegroundColor Green
Write-Host "Installer: $out"
