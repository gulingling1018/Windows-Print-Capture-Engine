#requires -Version 5.1
param(
  [string]$Preset,
  [string]$BuildPreset,
  [string]$TestPreset,
  [string]$VisualStudioRoot,
  [ValidateSet('Debug','Release')][string]$Configuration='Debug',
  [switch]$WithQt,
  [string[]]$ConfigureArguments=@()
)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/Wpce.Process.psm1') -Force
try {
  $repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
  $presets = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakePresets.json') -Raw | ConvertFrom-Json
  if (-not $Preset) { $Preset = if($WithQt) {'vs2026-x64-qt'} else {'vs2026-x64'} }
  $suffix = $Configuration.ToLowerInvariant()
  if (-not $BuildPreset) { $BuildPreset = if($Preset -eq 'vs2026-x64-qt') {"vs2026-qt-$suffix"} else {"vs2026-$suffix"} }
  if (-not $TestPreset) { $TestPreset=$BuildPreset }
  $configure = @($presets.configurePresets | Where-Object {$_.name -eq $Preset})
  $build = @($presets.buildPresets | Where-Object {$_.name -eq $BuildPreset})
  $test = @($presets.testPresets | Where-Object {$_.name -eq $TestPreset})
  if ($configure.Count -ne 1 -or $build.Count -ne 1 -or $test.Count -ne 1 -or
      $build[0].configurePreset -ne $Preset -or $test[0].configurePreset -ne $Preset -or
      $build[0].configuration -ne $Configuration -or $test[0].configuration -ne $Configuration) {
    throw 'Configure/build/test presets must reference the same directory and configuration.'
  }
  if ($WithQt -and $configure[0].cacheVariables.WPCE_BUILD_UI -ne 'ON') { throw '-WithQt requires a Qt configure preset.' }
  foreach ($argument in $ConfigureArguments) {
    if ($argument -match '^(--preset|-B|-S|--build|--install)' -or $argument -match '^-DWPCE_BUILD_(UI|TESTS)') {
      throw "Configure override would change the validated build contract: $argument"
    }
  }
  Reset-WpceProcessLanguage
  Push-Location -LiteralPath $repoRoot
  try {
    $arguments=@('--preset',$Preset)+$ConfigureArguments
    if ($VisualStudioRoot) {
      if (-not (Test-Path -LiteralPath $VisualStudioRoot -PathType Container)) { throw 'Visual Studio root does not exist.' }
      $arguments += "-DCMAKE_GENERATOR_INSTANCE=$VisualStudioRoot"
    }
    Invoke-WpceNativeStep 'CMake configure' { cmake @arguments }
    Invoke-WpceNativeStep 'CMake build' { cmake --build --preset $BuildPreset }
    Invoke-WpceNativeStep 'CTest' { ctest --preset $TestPreset --no-tests=error }
  } finally { Pop-Location }
} catch { Write-Error $_ -ErrorAction Continue; exit 1 }
