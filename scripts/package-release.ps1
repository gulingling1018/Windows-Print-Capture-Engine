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

Import-Module (Join-Path $PSScriptRoot 'lib/Wpce.Process.psm1') -Force
Set-Alias -Name Invoke-NativeStep -Value Invoke-WpceNativeStep
Reset-WpceProcessLanguage

Write-Host "=== WPCE Portable Release Packaging ===" -ForegroundColor Cyan
Write-Host "Output: $OutputDir"

# ────────────────────────────────────────────────────────────────
# 1. Build
# ────────────────────────────────────────────────────────────────
if (-not $SkipBuild) {
  Write-Host "`n[1/6] Configuring CMake ..." -ForegroundColor Yellow
  Invoke-NativeStep "CMake configure" {
    cmake -S "$repoRoot" -B "$BuildDir" `
      -G "Visual Studio 18 2026" `
      -A x64 `
      -DCMAKE_PREFIX_PATH="$QtPrefixPath" `
      -DWPCE_BUILD_TESTS=OFF `
      -DWPCE_BUILD_UI=ON
  }

  Write-Host "`n[2/6] Building Release ..." -ForegroundColor Yellow
  Invoke-NativeStep "CMake Release build" {
    cmake --build "$BuildDir" --config Release `
      --target wpce_ui wpce wpce_xps_filter wpce_sim --parallel 1
  }
}
else {
  Write-Host "`n[1-2/6] Build skipped by request; packaging existing exact Release paths." -ForegroundColor Yellow
}

# ────────────────────────────────────────────────────────────────
# 3. Collect binaries
# ────────────────────────────────────────────────────────────────
Write-Host "`n[3/6] Collecting binaries ..." -ForegroundColor Yellow

$null = New-Item -ItemType Directory -Force -Path "$OutputDir"
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path

# Use deterministic target paths. A recursive "first match" can silently pick
# an obsolete executable left by an earlier failed build.
$guiExe = Join-Path $BuildDir "src/ui/Release/wpce_ui.exe"
if (-not (Test-Path -LiteralPath $guiExe -PathType Leaf)) {
  throw "Release GUI executable not found at the expected path: $guiExe"
}

$guiExe = (Resolve-Path -LiteralPath $guiExe).Path
$guiInfo = Get-Item -LiteralPath $guiExe
Write-Host "  GUI source: $($guiInfo.FullName)"
Write-Host "  GUI stamp : $($guiInfo.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"
Write-Host "  GUI size  : $($guiInfo.Length) bytes"

# Copy main executables.
$packagedGui = Join-Path $OutputDir "wpce_ui.exe"
if (Test-Path -LiteralPath $packagedGui -PathType Leaf) {
  try {
    $probe = [System.IO.File]::Open(
      $packagedGui,
      [System.IO.FileMode]::Open,
      [System.IO.FileAccess]::ReadWrite,
      [System.IO.FileShare]::None)
    $probe.Dispose()
  }
  catch {
    $owners = @(Get-CimInstance Win32_Process -Filter "Name = 'wpce_ui.exe'" -ErrorAction SilentlyContinue |
      Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $packagedGui } |
      ForEach-Object { "PID $($_.ProcessId)" })
    $ownerText = if ($owners.Count -gt 0) { $owners -join ', ' } else { 'unknown process' }
    throw "Cannot replace the packaged GUI because it is running or locked ($ownerText): $packagedGui. Close it and rerun packaging."
  }
}
Copy-Item -LiteralPath $guiExe -Destination $packagedGui -Force

# The GUI calls the same versioned installation backend as the CLI.
$scriptDestination=Join-Path $OutputDir 'scripts'
$null=New-Item -ItemType Directory -Path $scriptDestination -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install-xps-filter.ps1') -Destination $scriptDestination -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'lib') -Destination $scriptDestination -Recurse -Force

$optionalBinaries = @(
  @{ Source = (Join-Path $BuildDir "src/Release/wpce.exe"); Name = "wpce.exe" },
  @{ Source = (Join-Path $BuildDir "tools/Release/wpce_sim.exe"); Name = "wpce_sim.exe" },
  @{ Source = (Join-Path $BuildDir "src/Release/wpce_xps_filter.dll"); Name = "wpce_xps_filter.dll" }
)
foreach ($binary in $optionalBinaries) {
  if (Test-Path -LiteralPath $binary.Source -PathType Leaf) {
    Copy-Item -LiteralPath $binary.Source -Destination (Join-Path $OutputDir $binary.Name) -Force
    Write-Host "  Copied: $($binary.Name)"
  }
  else {
    Write-Host "  Optional binary not found: $($binary.Source)" -ForegroundColor Yellow
  }
}

# Qt's build-tree deployment may provide the DirectX shader compiler beside
# the source executable even when the Qt SDK bin directory does not. Copy it
# before the package-level windeployqt pass so that deployment is complete.
foreach ($runtimeName in @("dxcompiler.dll", "dxil.dll")) {
  $runtimeSource = Join-Path $guiInfo.Directory.FullName $runtimeName
  if (Test-Path -LiteralPath $runtimeSource -PathType Leaf) {
    Copy-Item -LiteralPath $runtimeSource -Destination (Join-Path $OutputDir $runtimeName) -Force
    Write-Host "  Copied: $runtimeName"
  }
}

$sourceHash = (Get-FileHash -LiteralPath $guiExe -Algorithm SHA256).Hash
$packagedHash = (Get-FileHash -LiteralPath $packagedGui -Algorithm SHA256).Hash
if ($sourceHash -ne $packagedHash) {
  throw "GUI executable hash mismatch immediately after copy. Packaging stopped."
}
Write-Host "  GUI SHA-256: $sourceHash"

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
  # DirectX compiler DLLs are copied explicitly from the already-deployed
  # build output / Qt bin paths, so suppress windeployqt's SDK-path probing.
  Invoke-NativeStep "windeployqt" {
    & $wdqExe --release --no-translations --no-compiler-runtime `
      --no-system-d3d-compiler --no-system-dxc-compiler $targetExe
  }
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

# Non-default Visual Studio locations are recorded by the generated CMake
# cache even when vswhere and developer-shell variables are unavailable.
if (-not $vsRoot) {
  $cachePath = Join-Path $BuildDir "CMakeCache.txt"
  if (Test-Path -LiteralPath $cachePath) {
    $instanceLine = Select-String -LiteralPath $cachePath `
      -Pattern '^CMAKE_GENERATOR_INSTANCE:INTERNAL=' | Select-Object -First 1
    if ($instanceLine) {
      $vsRoot = ($instanceLine.Line -split '=', 2)[1]
    }
  }
}

# Helper: scan a base path for CRT DLLs.
$findCrt = {
  param($base)
  $dirs = Get-ChildItem -Path $base -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending
  foreach ($d in $dirs) {
    if (Test-Path "$($d.FullName)\msvcp140.dll") { return $d.FullName }
  }
  return $null
}

if ($vsRoot) {
  $msvcBase = Get-ChildItem -Path "$vsRoot\VC\Redist\MSVC" -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
    Sort-Object Name -Descending | Select-Object -First 1
  if ($msvcBase) { $vcredistDir = & $findCrt (Join-Path $msvcBase.FullName "x64") }
}

# Fallback: scan the whole VC tools tree from a detected compiler.
if (-not $vcredistDir) {
  $clExe = Get-Command cl.exe -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($clExe) {
    $toolsBase = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $clExe.Source))
    $redistVersions = Get-ChildItem -Path "$toolsBase\Redist\MSVC" -Directory -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
      Sort-Object Name -Descending | Select-Object -First 1
    if ($redistVersions) { $vcredistDir = & $findCrt (Join-Path $redistVersions.FullName "x64") }
  }
}

# Last fallback: walk the whole C: drive for the VC CRT (slow, only if needed).
if (-not $vcredistDir) {
  $candidate = Get-ChildItem -Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio" -Recurse -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
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

# The application itself writes the current schema defaults on first launch.
# Do not embed a second hand-maintained template here: it previously drifted
# from AppConfig and caused fresh portable packages to start with invalid data.
$configPath = "$OutputDir/wpce_config.json"
if (-not (Test-Path $configPath)) {
  Write-Host "  Config will be generated from AppConfig defaults on first launch."
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

Optional XPS page capture
-------------------------
Application startup only probes installation state; it never installs or
restarts printing. Open "XPS 设置" for explicit installation actions.
Installation needs an elevated x64 process. Restarting Spooler interrupts
printing and must be explicitly selected. A verified configuration still
requires a test print on each supported driver.

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
  scripts/               Shared XPS installation/probe backend
  Launch-WPCE.bat         GUI launcher
  Launch-CLI.bat          CLI launcher

Support
-------
See project documentation for details.
"@ | Set-Content -Path "$OutputDir/README.txt" -Encoding utf8

if (-not (Test-Path -LiteralPath (Join-Path $OutputDir "platforms/qwindows.dll") -PathType Leaf)) {
  throw "Qt platform plugin is missing after deployment: platforms/qwindows.dll"
}
$finalHash = (Get-FileHash -LiteralPath $packagedGui -Algorithm SHA256).Hash
if ($finalHash -ne $sourceHash) {
  throw "Packaged GUI hash changed during dependency deployment. Packaging stopped."
}
Write-Host "`n=== Package complete ===" -ForegroundColor Green
Write-Host "Output: $OutputDir" -ForegroundColor Green
Write-Host "Application: $((Resolve-Path -LiteralPath $packagedGui).Path)" -ForegroundColor Green
Write-Host "SHA-256:    $finalHash" -ForegroundColor Green
Write-Host "Files:" -ForegroundColor Green
Push-Location $OutputDir
try {
  Get-ChildItem -Recurse -File | ForEach-Object {
    $rel = Resolve-Path -Relative $_.FullName
    $kb = [math]::Round($_.Length / 1KB, 0)
    Write-Host ("  {0,7:N0} KB  {1}" -f $kb, $rel)
  }
} finally { Pop-Location }
