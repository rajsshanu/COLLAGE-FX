param(
    [switch]$Uninstall,
    [switch]$NoCacheReset
)
$ErrorActionPreference = "Stop"

$source = Join-Path $PSScriptRoot "dist\MixedMedia.ofx.bundle"
$programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
$pluginsDir = Join-Path $programFiles "Common Files\OFX\Plugins"
$target = Join-Path $pluginsDir "MixedMedia.ofx.bundle"

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    if ($Uninstall) { $arguments += "-Uninstall" }
    if ($NoCacheReset) { $arguments += "-NoCacheReset" }
    Write-Host "Requesting administrator access..."
    Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $arguments
    exit
}

if (Get-Process Resolve -ErrorAction SilentlyContinue) {
    throw "Close DaVinci Resolve before installing or uninstalling Mixed Media."
}

if ($Uninstall) {
    if (Test-Path $target) {
        Remove-Item $target -Recurse -Force
        Write-Host "Removed $target" -ForegroundColor Green
    } else {
        Write-Host "Mixed Media is not installed."
    }
    if (-not $NoCacheReset) {
        $cache = Join-Path $env:APPDATA "Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml"
        if (Test-Path $cache) {
            Remove-Item $cache -Force
            Write-Host "Removed Resolve OFX plugin cache."
        }
    }
    Write-Host "Restart DaVinci Resolve."
    exit
}

if (-not (Test-Path $source)) {
    throw "Build output not found: $source`nRun build.ps1 first."
}

New-Item -ItemType Directory -Force -Path $pluginsDir | Out-Null
if (Test-Path $target) {
    Remove-Item $target -Recurse -Force
}
Copy-Item $source $target -Recurse -Force

if (-not $NoCacheReset) {
    $cache = Join-Path $env:APPDATA "Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml"
    if (Test-Path $cache) {
        Remove-Item $cache -Force
        Write-Host "Removed Resolve OFX plugin cache."
    }
}

Write-Host ""
Write-Host "Installed Mixed Media to:" -ForegroundColor Green
Write-Host "  $target"
Write-Host ""
Write-Host "Restart DaVinci Resolve, then open:" -ForegroundColor Cyan
Write-Host "  Effects > OpenFX > Mixed Media > Paper Cutout"
Write-Host "  Effects > OpenFX > Mixed Media > Halftone Screen"
Write-Host ""
Write-Host "GPU log: $env:LOCALAPPDATA\MixedMediaOFX\mixedmedia.log"
