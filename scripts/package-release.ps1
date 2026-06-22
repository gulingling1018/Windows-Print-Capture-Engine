#requires -Version 5.1

<#
.SYNOPSIS
  Build and package WPCE for portable deployment.
.DESCRIPTION
  1. Configures and builds the Qt6 GUI release.
  2. Runs windeployqt to collect Qt runtime dependencies.
  3. Copies MSVC redistributable DLLs.
  4. Creates a default wpce_config.json template.
  5. Generates launch batch files and README.
  6. Outputs a self-contained folder ready for copy-to-USB deployment.
#>

param(
  [string]$OutputDir = "$PSScriptRoot/../release/wpce-portable",

  [string]$BuildDir = "$PSScriptRoot/../build/vs2026-x64-qt-release",

  [string]$QtPrefixPath = "C:/Qt/6.8.2/msvc2022_64",

  [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = Resolve-Path "$PSScriptRoot/.."

Write-Host "=== WPCE Portable Release Packaging ===" -ForegroundColor Cyan
Write-Host "Output: $OutputDir"

# ────────────────────────────────────────────────────────────────
# 1. Build
# ────────────────────────────────────────────────────────────────
if (-not $SkipBuild) {
  Write-Host "`n[1/6] Configuring CMake ..." -ForegroundColor Yellow
  cmake -S "$repoRoot" -B "$BuildDir" `
    -G "Visual Studio 18 2026" `
    -A x64 `
    -DCMAKE_PREFIX_PATH="$QtPrefixPath" `
    -DWPCE_BUILD_TESTS=OFF `
    -DWPCE_BUILD_UI=ON

  Write-Host "`n[2/6] Building Release ..." -ForegroundColor Yellow
  cmake --build "$BuildDir" --config Release --parallel
}

# ────────────────────────────────────────────────────────────────
# 3. Collect binaries
# ────────────────────────────────────────────────────────────────
Write-Host "`n[3/6] Collecting binaries ..." -ForegroundColor Yellow

$null = New-Item -ItemType Directory -Force -Path "$OutputDir"

# Find the actual build output directory (VS places in Release/ subfolder).
$exeDir = Get-ChildItem -Path "$BuildDir" -Recurse -Filter "wpce_ui.exe" -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName -match '\\Release\\' } |
  Select-Object -First 1

if (-not $exeDir) {
  Write-Host "ERROR: wpce_ui.exe not found in build tree." -ForegroundColor Red
  Write-Host "Searched: $BuildDir" -ForegroundColor Red
  exit 1
}

$exeDir = $exeDir.Directory.FullName
Write-Host "  Found binaries in: $exeDir"

# Copy main executables.
Copy-Item "$exeDir/wpce_ui.exe" "$OutputDir/" -Force
Copy-Item "$exeDir/wpce.exe" "$OutputDir/" -Force -ErrorAction SilentlyContinue
Copy-Item "$exeDir/wpce_sim.exe" "$OutputDir/" -Force -ErrorAction SilentlyContinue
Copy-Item "$exeDir/wpce_xps_filter.dll" "$OutputDir/" -Force -ErrorAction SilentlyContinue

# ────────────────────────────────────────────────────────────────
# 4. windeployqt
# ────────────────────────────────────────────────────────────────
Write-Host "`n[4/6] Running windeployqt ..." -ForegroundColor Yellow

$windeployqt = Get-ChildItem -Path "$QtPrefixPath/bin" -Filter "windeployqt.exe" -ErrorAction SilentlyContinue |
  Select-Object -First 1

if (-not $windeployqt) {
  Write-Host "WARNING: windeployqt not found at $QtPrefixPath/bin" -ForegroundColor Yellow
  Write-Host "  Skipping Qt dependency collection. Copy Qt DLLs manually." -ForegroundColor Yellow
}
else {
  $wdqExe = $windeployqt.FullName
  $targetExe = "$OutputDir\wpce_ui.exe"
  # --no-opengl-sw and --no-system-d3d-compiler are removed:
  # those DLLs are required on some systems for the DirectX backend.
  & $wdqExe --release --no-translations --no-compiler-runtime $targetExe
  Write-Host "  Qt dependencies collected."

  # windeployqt doesn't always copy D3Dcompiler_47.dll and opengl32sw.dll
  # from the Qt bin directory. Copy them manually to guarantee launch.
  $qtBin = Split-Path -Parent $wdqExe
  foreach ($dll in @("D3Dcompiler_47.dll","opengl32sw.dll")) {
    $src = Join-Path $qtBin $dll
    if (Test-Path $src) {
      Copy-Item $src "$OutputDir\" -Force
      Write-Host "  Copied: $dll"
    }
  }

  # Create qt.conf so Qt finds its plugins relative to the exe.
  $qtConf = @'
[Paths]
Plugins = .
'@
  Out-File -FilePath "$OutputDir\qt.conf" -InputObject $qtConf -Encoding ascii
  Write-Host "  Created: qt.conf"
}

# ────────────────────────────────────────────────────────────────
# 5. MSVC redistributables
# ────────────────────────────────────────────────────────────────
Write-Host "`n[5/6] Copying MSVC runtime ..." -ForegroundColor Yellow

$vcredistDir = $null

# Strategy: search for msvcp140.dll near the VS installation.
# First try the VS install root from environment or vswhere.
$vsRoot = $env:VSINSTALLDIR
if (-not $vsRoot) {
  $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vsWhere) {
    $vsRoot = & $vsWhere -latest -property installationPath 2>$null
  }
}
# Also try VS170COMNTOOLS.
if (-not $vsRoot -and $env:VS170COMNTOOLS) {
  $vsRoot = Split-Path -Parent (Split-Path -Parent $env:VS170COMNTOOLS)
}

# Helper: scan a base path for CRT DLLs.
$findCrt = {
  param($base)
  $dirs = Get-ChildItem -Path $base -Recurse -Directory -Filter "Microsoft.VC143.CRT" -ErrorAction SilentlyContinue
  foreach ($d in $dirs) {
    if (Test-Path "$($d.FullName)\msvcp140.dll") { return $d.FullName }
  }
  return $null
}

if ($vsRoot) {
  $msvcBase = Get-ChildItem -Path "$vsRoot\VC\Redist\MSVC" -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
  if ($msvcBase) { $vcredistDir = & $findCrt $msvcBase.FullName }
}

# Fallback: scan the whole VC tools tree from a detected compiler.
if (-not $vcredistDir) {
  $clExe = Get-Command cl.exe -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($clExe) {
    $toolsBase = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $clExe.Source))
    $vcredistDir = & $findCrt "$toolsBase\Redist\MSVC"
  }
}

# Last fallback: walk the whole C: drive for the VC CRT (slow, only if needed).
if (-not $vcredistDir) {
  $candidate = Get-ChildItem -Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio" -Recurse -Directory -Filter "Microsoft.VC143.CRT" -ErrorAction SilentlyContinue |
    Where-Object { Test-Path "$($_.FullName)\msvcp140.dll" } | Select-Object -First 1
  if ($candidate) { $vcredistDir = $candidate.FullName }
}

if (-not $vcredistDir) {
  Write-Host "  WARNING: Could not find MSVC redist." -ForegroundColor Yellow
  Write-Host "  Target machines MUST have the VC++ 2022+ redist installed." -ForegroundColor Yellow
}
else {
  Copy-Item "$vcredistDir\msvcp140*.dll" "$OutputDir\" -Force -ErrorAction SilentlyContinue
  Copy-Item "$vcredistDir\vcruntime140*.dll" "$OutputDir\" -Force -ErrorAction SilentlyContinue
  Copy-Item "$vcredistDir\concrt140.dll" "$OutputDir\" -Force -ErrorAction SilentlyContinue
  Copy-Item "$vcredistDir\vccorlib140.dll" "$OutputDir\" -Force -ErrorAction SilentlyContinue
  Write-Host "  VC++ runtime copied from: $vcredistDir"
}

# ────────────────────────────────────────────────────────────────
# 6. Configuration & launchers
# ────────────────────────────────────────────────────────────────
Write-Host "`n[6/6] Creating launchers and config ..." -ForegroundColor Yellow

# Default config (only if not already present).
$configPath = "$OutputDir/wpce_config.json"
if (-not (Test-Path $configPath)) {
  $defaultConfig = @'
{
  "version": 1,
  "storage": {
    "output_root": "./data",
    "use_timestamp_subdir": true,
    "queue_capacity": 256,
    "retention_days": 90
  },
  "pricing": {
    "single_sided_per_page": 0.15,
    "double_sided_per_sheet": 0.25,
    "color_surcharge": 0.50,
    "large_paper_surcharge": 0.30,
    "high_quality_surcharge": 0.10,
    "volume_discounts": [
      {"min_pages": 100, "discount": 0.95},
      {"min_pages": 500, "discount": 0.90},
      {"min_pages": 1000, "discount": 0.85}
    ],
    "currency": "CNY"
  },
  "merchant": {
    "name": "",
    "phone": "",
    "address": ""
  },
  "monitoring": {
    "stall_timeout_seconds": 30,
    "perf_log_enabled": false
  },
  "window": {
    "width": 1280,
    "height": 760
  }
}
'@
  Set-Content -Path $configPath -Value $defaultConfig -Encoding utf8
  Write-Host "  Default config: $configPath"
}

# Launch batch for GUI.
@'
@echo off
chcp 65001 >nul
title WPCE Print Capture
echo ============================================
echo   WPCE Windows Print Capture Engine
echo ============================================
echo.
echo Configuration: %~dp0wpce_config.json
echo Data directory: see storage.output_root in config
echo.
echo Starting...
start "" "%~dp0wpce_ui.exe"
'@ | Out-File -FilePath "$OutputDir/Launch-WPCE.bat" -Encoding utf8

# Launch batch for CLI (single printer).
@'
@echo off
chcp 65001 >nul
title WPCE CLI Watcher
echo ============================================
echo   WPCE CLI Watcher
echo ============================================
echo.
echo Usage: wpce --watch-printer "PRINTER_NAME" [output_dir]
echo.
echo List available printers:
wmic printer get Name
echo.
set /p PRINTER="Enter printer name to watch: "
if "%PRINTER%"=="" goto :end
echo Starting watcher for "%PRINTER%" ...
wpce --watch-printer "%PRINTER%" "./data"
:end
pause
'@ | Out-File -FilePath "$OutputDir/Launch-CLI.bat" -Encoding utf8

# README.
@"
WPCE Windows Print Capture Engine v0.1.0
=========================================

Portable deployment — no installation required.

Quick start
-----------
1. Double-click `Launch-WPCE.bat` to launch the GUI console.
2. The app will auto-detect local printers and begin monitoring.
3. Captured data is saved to the directory specified in `wpce_config.json`
   (default: `./data/` relative to this folder).

Configuration
-------------
Edit `wpce_config.json` to change:
  - Data storage path (storage.output_root)
  - Print pricing (pricing.*)
  - Merchant info (merchant.*)
  - Monitoring parameters (monitoring.*)

Or use the "设置" (Settings) button inside the GUI application.

Requirements
------------
- Windows 10 / 11 (x64)
- Visual C++ Redistributable 2022+ (included in this folder)
- At least one locally installed printer

Files
-----
  wpce_ui.exe            GUI console
  wpce.exe               CLI watcher
  wpce_config.json       Configuration file
  Qt6*.dll               Qt 6 runtime libraries
  msvcp140*.dll          Visual C++ runtime
  vcruntime140*.dll      Visual C++ runtime
  platforms/             Qt platform plugins
  styles/                Qt style plugins
  Launch-WPCE.bat         GUI launcher
  Launch-CLI.bat          CLI launcher

Support
-------
See project documentation for details.
"@ | Set-Content -Path "$OutputDir/README.txt" -Encoding utf8

Write-Host "`n=== Package complete ===" -ForegroundColor Green
Write-Host "Output: $OutputDir" -ForegroundColor Green
Write-Host "Files:" -ForegroundColor Green
Push-Location $OutputDir
try {
  Get-ChildItem -Recurse -File | ForEach-Object {
    $rel = Resolve-Path -Relative $_.FullName
    $kb = [math]::Round($_.Length / 1KB, 0)
    Write-Host ("  {0,7:N0} KB  {1}" -f $kb, $rel)
  }
} finally { Pop-Location }
