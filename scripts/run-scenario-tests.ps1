<#
.SYNOPSIS
  WPCE Scenario Test Harness — runs one scenario at a time and validates output.
  Designed for PowerShell 5.1 compatibility.
#>
param(
  [string]$Scenario = "all",
  [string]$BuildDir = "build\vs2026-x64"
)

$ErrorActionPreference = "Continue"
$root = $PSScriptRoot | Split-Path -Parent
$wpce = Join-Path $root "$BuildDir\src\Debug\wpce.exe"
$sim  = Join-Path $root "$BuildDir\tools\Debug\wpce_sim.exe"

if (-not (Test-Path $wpce)) { Write-Host "ERROR: wpce.exe not found at $wpce"; exit 1 }
if (-not (Test-Path $sim))  { Write-Host "ERROR: wpce_sim.exe not found at $sim"; exit 1 }

# ------------------------------------------------------------------
function Purge-Queue($pn) {
  try {
    Add-Type -AssemblyName System.Printing -ErrorAction SilentlyContinue
    $ps = New-Object System.Printing.PrintServer
    $pq = New-Object System.Printing.PrintQueue $ps, $pn
    $pq.Refresh()
    foreach ($j in $pq.GetPrintJobInfoCollection()) { try { $j.Cancel() } catch {} }
  } catch {}
}

function Run-One {
  param($Name, $Printer, $SimArgs, $Dur, $Stall, $ExpectMin, $ExpectPhases)

  $outDir = Join-Path $root "artifacts\sim-$Name"
  Write-Host ""
  Write-Host ("=" * 60)
  Write-Host "SCENARIO: $Name"
  Write-Host ("=" * 60)

  # Clean
  Purge-Queue $Printer
  if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }

  # Start watcher
  Write-Host "[Watcher] Starting --watch-printer `"$Printer`" --duration $Dur --stall-timeout $Stall"
  $j = Start-Job -ScriptBlock {
    param($e,$p,$o,$d,$s) & $e --watch-printer $p $o --duration $d --stall-timeout $s 2>&1
  } -Arg $wpce,$Printer,$outDir,$Dur,$Stall

  Start-Sleep 3
  if ($j.State -ne "Running") {
    $err = Receive-Job $j; Remove-Job $j -Force
    Write-Host "[FAIL] Watcher did not start: $err"
    return @{Name=$Name;Passed=$false;Jobs=0;Error=$err}
  }

  # Run simulator
  Write-Host "[Simulator] $SimArgs"
  $simArgsArr = $SimArgs -split ' '
  & $sim --target-printer $Printer $simArgsArr 2>&1 | ForEach-Object { Write-Host "  $_" }

  # Wait
  Write-Host "[Wait] ${Dur}s + 30s grace..."
  Wait-Job $j -Timeout ($Dur + 30) | Out-Null
  if ($j.State -eq "Running") { Stop-Job $j }
  $output = Receive-Job $j; Remove-Job $j -Force

  # Check error
  $hasError = $false
  $errText = ""
  if ($output -match "stopped with an error") {
    $hasError = $true
    $errText = ($output | Select-String "error" | Select-Object -First 3) -join "; "
    Write-Host "[WARN] Watcher reported error: $errText"
  }

  # Parse lifecycle
  $sumPath = Join-Path $outDir "lifecycle-summary.json"
  if (-not (Test-Path $sumPath)) {
    Write-Host "[FAIL] Missing lifecycle-summary.json (hasError=$hasError)"
    return @{Name=$Name;Passed=$false;Jobs=0;Error="Missing summary"}
  }

  try {
    $sum = Get-Content -Raw -Encoding UTF8 $sumPath | ConvertFrom-Json
    $jobs = $sum.session_jobs.Count
    $phaseOk = 0; $phaseBad = 0; $badList = @()
    foreach ($jb in $sum.session_jobs) {
      if ($jb.final_phase -in $ExpectPhases) { $phaseOk++ }
      else { $phaseBad++; if ($phaseBad -le 3) { $badList += "Job$($jb.job_id):$($jb.final_phase)" } }
    }

    # Also check per-job lifecycle files
    $ljFiles = Get-ChildItem (Join-Path $outDir "jobs") -Filter "*-lifecycle.json" -ErrorAction SilentlyContinue
    $hasTiming = $false
    foreach ($ljf in $ljFiles) {
      $lj = Get-Content -Raw -Encoding UTF8 $ljf.FullName | ConvertFrom-Json
      if ($lj.total_duration_ms -gt 0) { $hasTiming = $true }
    }

    $ok = ($jobs -ge $ExpectMin) -and ($phaseOk -gt 0) -and (-not $hasError)

    Write-Host "[RESULT] Jobs=$jobs (min=$ExpectMin) PhaseOK=$phaseOk/$jobs Timing=$hasTiming Error=$hasError"
    if ($badList.Count -gt 0) { Write-Host "  Unexpected phases: $badList" }
    if ($jobs -gt 0) {
      # Show phase breakdown
      $phases = ($sum.session_jobs | ForEach-Object { $_.final_phase } | Group-Object | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join " "
      Write-Host "  Phase breakdown: $phases"
    }
    $status = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "[$status]"
    return @{Name=$Name;Passed=$ok;Jobs=$jobs;PhaseOK=$phaseOk;Error=$(if($hasError){$errText}else{""})}
  } catch {
    Write-Host "[FAIL] Parse error: $_"
    return @{Name=$Name;Passed=$false;Jobs=0;Error=$_.Exception.Message}
  }
}

# ------------------------------------------------------------------
Write-Host "============================================================"
Write-Host "  WPCE Scenario Integration Tests"
Write-Host "============================================================"
Write-Host "  wpce: $wpce"
Write-Host "  sim:  $sim"

$all = @(
  @{n="normal";           p="WPCE Sim Printer";  a="--scenario normal --job-count 5";         d=20; s=10; min=5;  ph=@("completed")},
  @{n="fast-disappear";   p="WPCE Sim Printer";  a="--scenario fast-disappear --job-count 5"; d=25; s=10; min=5;  ph=@("cancelled","completed")},
  @{n="cancel-mid";       p="WPCE Sim Printer";  a="--scenario cancel-mid --job-count 3";     d=25; s=10; min=3;  ph=@("cancelled","completed")},
  @{n="stall";            p="WPCE Sim Printer";  a="--scenario stall --job-count 2";          d=25; s=8;  min=2;  ph=@("stalled","completed")},
  @{n="printer-error";    p="WPCE Fault Printer";a="--scenario printer-error --job-count 3";  d=45; s=15; min=3;  ph=@("completed","failed","stalled")},
  @{n="high-concurrency"; p="WPCE Sim Printer";  a="--scenario high-concurrency --job-count 50";d=35;s=10; min=50; ph=@("completed","cancelled")},
  @{n="param-variety";    p="WPCE Sim Printer";  a="--scenario param-variety";                d=25; s=10; min=5;  ph=@("spooling","printing","completed")}
)

$run = if ($Scenario -eq "all") { $all } else { $all | Where-Object { $_.n -eq $Scenario } }
if ($run.Count -eq 0) { Write-Host "Unknown scenario: $Scenario"; exit 1 }

$results = @()
foreach ($s in $run) {
  $r = Run-One $s.n $s.p $s.a $s.d $s.s $s.min $s.ph
  $results += $r
}

# Summary
Write-Host ""
Write-Host "============================================================"
Write-Host "  SUMMARY"
Write-Host "============================================================"
$p = ($results | Where-Object { $_.Passed }).Count
$f = ($results | Where-Object { -not $_.Passed }).Count
foreach ($r in $results) {
  $st = if ($r.Passed) { "PASS" } else { "FAIL" }
  Write-Host "  [$st] $($r.Name) -- Jobs=$($r.Jobs) PhaseOK=$($r.PhaseOK)"
  if ($r.Error) { Write-Host "       Error: $($r.Error)" }
}
Write-Host ""
Write-Host "  Total=$($results.Count) Passed=$p Failed=$f"
if ($f -gt 0) { exit 1 }
Write-Host "  All passed!"