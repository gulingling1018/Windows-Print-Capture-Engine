param(
  [string]$Preset = "vs2026-x64",
  [string]$BuildPreset = "vs2026-debug",
  [string]$TestPreset = "vs2026-debug",
  [string]$VisualStudioRoot = "D:\download\Microsoft Visual Studio18_Community",
  [switch]$WithQt = $false
)

$ErrorActionPreference = "Stop"

if ($WithQt) {
  $Preset = "vs2026-x64-qt"
  $BuildPreset = "vs2026-debug"
  $TestPreset = "vs2026-debug"
}

$configureArgs = @("--preset", $Preset)
if (Test-Path $VisualStudioRoot) {
  $configureArgs += "-DCMAKE_GENERATOR_INSTANCE=$VisualStudioRoot"
}

cmake @configureArgs
cmake --build --preset $BuildPreset
ctest --preset $TestPreset
