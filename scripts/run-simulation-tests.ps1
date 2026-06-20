<#
.SYNOPSIS
  WPCE Simulation Integration Tests
  Runs watcher + simulator for each scenario and validates lifecycle output.
#>

param(
  [string]$BuildDir = "build\vs2026-x64",
  [string]$Scenario = "all",
  [int]$Duration = 30,
  [int]$StallTimeout = 10
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot | Split-Path -Parent

$wpceExe = Join-Path $projectRoot "$BuildDir\src\Debug\wpce.exe"
$simExe  = Join-Path $projectRoot "$BuildDir\tools\Debug\wpce_sim.exe"

if (-not (Test-Path $wpceExe)) {
  Write-Error "wpce.exe not found at: $wpceExe"
  exit 1
}
if (-not (Test-Path $simExe)) {
  Write-Error "wpce_sim.exe not found at: $simExe"
  exit 1
}

# ----------------------------------------------------------------------
function Test-Normal {
  return @{ Name="normal"; Printer="WPCE Sim Printer"; SimArgs="--scenario normal --job-count 5"; Dur=25; Stall=10; Expect=5; Phase="completed" }
}
function Test-FastDisappear {
  return @{ Name="fast-disappear"; Printer="WPCE Sim Printer"; SimArgs="--scenario fast-disappear --job-count 5"; Dur=30; Stall=10; Expect=5; Phase=@("cancelled","completed") }
}
function Test-CancelMid {
  return @{ Name="cancel-mid"; Printer="WPCE Sim Printer"; SimArgs="--scenario cancel-mid --job-count 3"; Dur=30; Stall=10; Expect=3; Phase=@("cancelled","completed") }
}
function Test-Stall {
  return @{ Name="stall"; Printer="WPCE Sim Printer"; SimArgs="--scenario stall --job-count 2"; Dur=30; Stall=10; Expect=2; Phase="stalled" }
}
function Test-PrinterError {
  return @{ Name="printer-error"; Printer="WPCE Fault Printer"; SimArgs="--scenario printer-error --job-count 3"; Dur=45; Stall=15; Expect=3; Phase=@("completed","failed","stalled") }
}
function Test-HighConcurrency {
  return @{ Name="high-concurrency"; Printer="WPCE Sim Printer"; SimArgs="--scenario high-concurrency --job-count 50"; Dur=35; Stall=10; Expect=50; Phase=@("completed","cancelled") }
}
function Test-ParamVariety {
  return @{ Name="param-variety"; Printer="WPCE Sim Printer"; SimArgs="--scenario param-variety"; Dur=30; Stall=10; Expect=5; Phase=@("spooling","printing","completed") }
}

# ----------------------------------------------------------------------
function Invoke-ScenarioTest {
  param($Cfg)

  $name   = $Cfg.Name
  $printer = $Cfg.Printer
  $simArgs = $Cfg.SimArgs
  $dur     = $Cfg.Dur
  $stall   = $Cfg.Stall
  $expect  = $Cfg.Expect
  $expectPhase = $Cfg.Phase

  $outDir = Join-Path $projectRoot "artifacts\sim-test-$name"

  Write-Host ""
  Write-Host ("-" * 70)
  Write-Host "Scenario: $name"
  Write-Host "Description: $($Cfg.Description)" -ErrorAction SilentlyContinue
  Write-Host ("-" * 70)

  if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }

  # Start watcher in background.
  Write-Host "Starting watcher..."
  $watcherJob = Start-Job -ScriptBlock {
    param($Exe, $Printer, $Out, $Dur, $Stall)
    & $Exe --watch-printer $Printer $Out --duration $Dur --stall-timeout $Stall 2>&1
  } -ArgumentList $wpceExe, $printer, $outDir, $dur, $stall

  Start-Sleep -Seconds 3

  if ($watcherJob.State -ne "Running") {
    $err = Receive-Job $watcherJob
    Remove-Job $watcherJob -Force
    Write-Host "FAIL: Watcher failed to start"
    Write-Host $err
    return @{ Name=$name; Passed=$false; Error="Watcher startup failed"; Jobs=0 }
  }

  # Run simulator.
  Write-Host "Running simulator..."
  $simSplitArgs = $simArgs -split '\s+'
  $simFullArgs = @("--target-printer", $printer) + $simSplitArgs
  $simOutput = & $simExe $simFullArgs 2>&1
  if ($simOutput) {
    $simOutput | ForEach-Object { Write-Host "  $_" }
  }

  # Wait for watcher to exit.
  Write-Host "Waiting for watcher (--duration ${dur}s)..."
  $null = Wait-Job $watcherJob -Timeout ($dur + 30)
  if ($watcherJob.State -eq "Running") {
    Stop-Job $watcherJob
    Remove-Job $watcherJob -Force
    Write-Host "FAIL: Watcher timeout"
    return @{ Name=$name; Passed=$false; Error="Watcher timeout"; Jobs=0 }
  }

  $watcherOutput = Receive-Job $watcherJob 2>&1
  Remove-Job $watcherJob -Force

  # Extract Summary line.
  $summaryLine = $watcherOutput | Select-String "Observed (\d+) job" | Select-Object -First 1
  $queuedLine  = $watcherOutput | Select-String "Queued (\d+)" | Select-Object -First 1
  Write-Host "  $summaryLine"
  Write-Host "  $queuedLine"

  # Validate lifecycle-summary.json.
  $summaryPath = Join-Path $outDir "lifecycle-summary.json"
  if (-not (Test-Path $summaryPath)) {
    Write-Host "FAIL: Missing lifecycle-summary.json"
    return @{ Name=$name; Passed=$false; Error="Missing lifecycle-summary.json"; Jobs=0 }
  }

  try {
    $summary = Get-Content -Raw -Encoding UTF8 $summaryPath | ConvertFrom-Json
    $actualJobs = $summary.session_jobs.Count

    $phaseOk = 0
    $phaseBad = 0
    $badJobs = @()
    $expectList = if ($expectPhase -is [array]) { $expectPhase } else { @($expectPhase) }

    foreach ($job in $summary.session_jobs) {
      if ($job.final_phase -in $expectList) {
        $phaseOk++
      } else {
        $phaseBad++
        if ($phaseBad -le 3) {
          $badJobs += "Job $($job.job_id): $($job.final_phase)"
        }
      }
    }

    Write-Host "  Jobs: $actualJobs (expected >=$expect)"
    Write-Host "  Phase OK: $phaseOk/$actualJobs (expected $expectList)"
    if ($badJobs.Count -gt 0) {
      Write-Host "  Bad phases: $badJobs"
    }

    $passed = ($actualJobs -ge $expect) -and ($phaseOk -gt 0)
    if (-not $passed) {
      return @{ Name=$name; Passed=$false; Error="Jobs=$actualJobs expect>=$expect, PhaseOK=$phaseOk"; Jobs=$actualJobs }
    }

    Write-Host "  PASSED"
    return @{ Name=$name; Passed=$true; Error=""; Jobs=$actualJobs }

  } catch {
    Write-Host "FAIL: $_"
    return @{ Name=$name; Passed=$false; Error=$_.Exception.Message; Jobs=0 }
  }
}

# ----------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------

Write-Host "============================================================"
Write-Host "  WPCE Simulation Integration Tests"
Write-Host "============================================================"
Write-Host "  wpce: $wpceExe"
Write-Host "  sim:  $simExe"

$allScenarios = @(
  (Test-Normal),
  (Test-FastDisappear),
  (Test-CancelMid),
  (Test-Stall),
  (Test-PrinterError),
  (Test-HighConcurrency),
  (Test-ParamVariety)
)

$toRun = if ($Scenario -eq "all") { $allScenarios } else { $allScenarios | Where-Object { $_.Name -eq $Scenario } }

if ($toRun.Count -eq 0) {
  Write-Error "No scenarios matched: $Scenario"
  exit 1
}

$results = @()
foreach ($cfg in $toRun) {
  $r = Invoke-ScenarioTest $cfg
  $results += $r
}

# Summary
Write-Host ""
Write-Host "============================================================"
Write-Host "  TEST RESULTS SUMMARY"
Write-Host "============================================================"

$passed = ($results | Where-Object { $_.Passed }).Count
$failed = ($results | Where-Object { -not $_.Passed }).Count

foreach ($r in $results) {
  $status = if ($r.Passed) { "PASS" } else { "FAIL" }
  Write-Host "  [$status] $($r.Name) -- $($r.Jobs) jobs"
  if (-not $r.Passed) {
    Write-Host "          $($r.Error)"
  }
}

Write-Host ""
Write-Host "  Total:  $($results.Count)"
Write-Host "  Passed: $passed"
Write-Host "  Failed: $failed"

if ($failed -gt 0) {
  exit 1
}
Write-Host "  All scenarios passed!"
exit 0
