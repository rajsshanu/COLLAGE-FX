# Build the standalone Mixed Media Studio app (Windows x64).
# Requires Visual Studio Build Tools with the C++ workload.
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

New-Item -ItemType Directory -Force build, build\studio, dist | Out-Null

if (-not (Test-Path (Join-Path $root "studio\MixedMedia.ico"))) {
    Write-Host "Generating app icon..." -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\make_icon.ps1")
    if ($LASTEXITCODE -ne 0) { throw "Icon generation failed." }
}

$out = Join-Path $root "dist\MixedMediaStudio.exe"
$res = Join-Path $root "build\studio\Studio.res"
$configFlags = if ($Debug) { "/Od /Zi /MTd /D_DEBUG" } else { "/O2 /Ob2 /MT /DNDEBUG" }

$bat = Join-Path $root "build\build_studio.bat"
$batText = @"
@echo off
call "$vcvars" >nul
rc /nologo /fo "$res" "$root\studio\Studio.rc"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /utf-8 $configFlags /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
 /I"$root\src" /I"$root\studio" ^
 /Fo"$root\build\studio\\" ^
 "$root\studio\Studio.cpp" "$root\studio\ImageIO.cpp" "$root\src\CpuRender.cpp" ^
 /Fe:"$out" /link /SUBSYSTEM:WINDOWS /MACHINE:X64 /MANIFEST:NO "$res"
"@
Set-Content -Path $bat -Value $batText -Encoding ASCII
& cmd.exe /d /c $bat
if ($LASTEXITCODE -ne 0) { throw "Studio build failed (exit $LASTEXITCODE)." }
if (-not (Test-Path $out)) { throw "Build did not produce $out." }

Write-Host "[done] Mixed Media Studio built." -ForegroundColor Green
Write-Host "App: $out"
