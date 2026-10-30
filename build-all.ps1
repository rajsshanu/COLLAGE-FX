# Build everything: OFX plugin, standalone Studio app and installer.
# For a faster pass without the OFX host/parity tests, use:
#   .\build-all.ps1 -SkipTests
param(
    [switch]$SkipTests
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "build.ps1") -SkipTests:$SkipTests
if ($LASTEXITCODE -ne 0) { throw "OFX build failed." }

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "build-studio.ps1")
if ($LASTEXITCODE -ne 0) { throw "Studio build failed." }

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "build-setup.ps1")
if ($LASTEXITCODE -ne 0) { throw "Installer build failed." }

Write-Host ""
Write-Host "All Windows deliverables are in dist\" -ForegroundColor Green
