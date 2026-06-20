param(
  [ValidateSet("smoke", "matrix", "concurrency", "pc-stress", "stalled", "cancel", "network-timeout", "all")]
  [string]$Scenario = "all",
  [string]$SimPrinterName = "WPCE Sim Printer",
  [string]$FaultPrinterName = "WPCE Fault Printer",
  [string]$WatcherExe = (Join-Path (Get-Location) "build\vs2026-x64\src\Debug\wpce.exe"),
  [string]$ArtifactRoot = (Join-Path (Get-Location) "artifacts\print-simulation"),
  [int]$JobCount = 24,
  [int]$Concurrency = 6,
  [int]$DurationSeconds = 14,
  [int]$StallTimeoutSeconds = 3,
  [switch]$SkipSetup
)

$ErrorActionPreference = "Stop"

function Quote-Arg {
  param([string]$Value)

  if ($null -eq $Value) { return '""' }
  if ($Value -notmatch '[\s"]') { return $Value }
  return '"' + ($Value -replace '"', '\"') + '"'
}

function Join-Arguments {
  param([string[]]$Arguments)
  return ($Arguments | ForEach-Object { Quote-Arg $_ }) -join " "
}

function New-ScenarioDirectory {
  param([string]$Name)

  $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
  $path = Join-Path $ArtifactRoot "$stamp-$Name"
  New-Item -ItemType Directory -Force -Path $path | Out-Null
  return (Resolve-Path $path).Path
}

function Clear-TestPrintJobs {
  param([string]$PrinterName)

  Get-PrintJob -PrinterName $PrinterName -ErrorAction SilentlyContinue |
    Remove-PrintJob -ErrorAction SilentlyContinue
}

function Set-TestPrinterPaused {
  param(
    [string]$PrinterName,
    [bool]$Paused
  )

  $escapedName = $PrinterName -replace "'", "''"
  $printer = Get-CimInstance Win32_Printer -Filter "Name='$escapedName'" -ErrorAction SilentlyContinue
  if (-not $printer) { return }

  $method = if ($Paused) { "Pause" } else { "Resume" }
  Invoke-CimMethod -InputObject $printer -MethodName $method -ErrorAction SilentlyContinue | Out-Null
}

function Send-TestPrintJob {
  param(
    [string]$PrinterName,
    [string]$DocumentName,
    [int]$PageCount = 1,
    [int]$Copies = 1,
    [bool]$Landscape = $false,
    [bool]$Color = $false,
    [string]$Duplex = "Simplex",
    [string]$PaperKind = "A4"
  )

  Add-Type -AssemblyName System.Drawing

  $doc = New-Object System.Drawing.Printing.PrintDocument
  $doc.PrinterSettings.PrinterName = $PrinterName
  $doc.DocumentName = $DocumentName
  $doc.PrinterSettings.Copies = [int16]$Copies
  $doc.PrinterSettings.Collate = $true
  $doc.DefaultPageSettings.Landscape = $Landscape
  $doc.DefaultPageSettings.Color = $Color

  try {
    $doc.PrinterSettings.Duplex = [System.Drawing.Printing.Duplex]::$Duplex
  } catch {
    # Some virtual drivers expose no duplex capability. The job is still useful.
  }

  foreach ($paperSize in $doc.PrinterSettings.PaperSizes) {
    if ($paperSize.Kind.ToString() -eq $PaperKind) {
      $doc.DefaultPageSettings.PaperSize = $paperSize
      break
    }
  }

  $state = [pscustomobject]@{ Page = 0 }
  $font = New-Object System.Drawing.Font("Segoe UI", 12)
  $brush = [System.Drawing.Brushes]::Black

  $handler = [System.Drawing.Printing.PrintPageEventHandler]{
    param($sender, $eventArgs)

    $state.Page++
    $text = @(
      $DocumentName,
      "Printer: $PrinterName",
      "Page: $($state.Page) / $PageCount",
      "Copies: $Copies",
      "Landscape: $Landscape",
      "Color: $Color",
      "Duplex: $Duplex",
      "Paper: $PaperKind",
      "Generated: $((Get-Date).ToString('o'))"
    ) -join [Environment]::NewLine

    $eventArgs.Graphics.DrawString($text, $font, $brush, 80, 80)
    $eventArgs.HasMorePages = $state.Page -lt $PageCount
  }

  try {
    $doc.add_PrintPage($handler)
    $doc.Print()
  } finally {
    $doc.remove_PrintPage($handler)
    $font.Dispose()
    $doc.Dispose()
  }
}

function Wait-ForPrintJob {
  param(
    [string]$PrinterName,
    [string]$DocumentName,
    [int]$TimeoutSeconds = 8
  )

  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  do {
    $job = Get-PrintJob -PrinterName $PrinterName -ErrorAction SilentlyContinue |
      Where-Object { $_.DocumentName -like "*$DocumentName*" } |
      Select-Object -First 1
    if ($job) { return $job }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)

  return $null
}

function Start-CpuLoad {
  param([int]$Seconds, [int]$Workers)

  $processes = @()
  for ($i = 0; $i -lt $Workers; ++$i) {
    $command = @"
`$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt `$end) {
  for (`$i = 0; `$i -lt 250000; `$i++) {
    [void][Math]::Sqrt(`$i)
  }
}
"@
    $processes += Start-Process -FilePath "powershell.exe" `
      -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", $command) `
      -PassThru `
      -WindowStyle Hidden
  }
  return $processes
}

function Stop-CpuLoad {
  param([object[]]$Processes)

  foreach ($process in $Processes) {
    if ($process -and -not $process.HasExited) {
      Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
  }
}

function Start-Watcher {
  param(
    [string]$PrinterName,
    [string]$OutputDir,
    [int]$Duration,
    [int]$StallTimeout
  )

  New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
  $stdout = Join-Path $OutputDir "watcher.stdout.log"
  $stderr = Join-Path $OutputDir "watcher.stderr.log"
  $perf = Join-Path $OutputDir "perf.csv"

  $args = @(
    "--watch-printer", $PrinterName,
    $OutputDir,
    "--stall-timeout", [string]$StallTimeout,
    "--duration", [string]$Duration,
    "--perf-log", $perf
  )

  $process = Start-Process -FilePath $WatcherExe `
    -ArgumentList (Join-Arguments $args) `
    -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr `
    -PassThru `
    -WindowStyle Hidden

  return [pscustomobject]@{
    Process = $process
    Stdout = $stdout
    Stderr = $stderr
    PerfLog = $perf
  }
}

function Complete-Watcher {
  param(
    [pscustomobject]$Watcher,
    [int]$TimeoutSeconds
  )

  $exited = Wait-Process -Id $Watcher.Process.Id -Timeout $TimeoutSeconds -ErrorAction SilentlyContinue
  $process = Get-Process -Id $Watcher.Process.Id -ErrorAction SilentlyContinue
  if ($process) {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
  }
}

function Read-ScenarioResult {
  param(
    [string]$Name,
    [string]$PrinterName,
    [string]$OutputDir,
    [pscustomobject]$Watcher,
    [int]$SubmittedJobs,
    [string]$DocumentPrefix
  )

  $jobArchives = @()
  foreach ($file in @(Get-ChildItem -Path (Join-Path $OutputDir "jobs") -Filter "job-*.json" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^job-\d+\.json$' })) {
    $content = Get-Content -Raw $file.FullName | ConvertFrom-Json
    if ([string]$content.document_name -like "$DocumentPrefix*") {
      $jobArchives += $file
    }
  }

  $lifecycleFiles = @(Get-ChildItem -Path (Join-Path $OutputDir "jobs") -Filter "job-*-lifecycle.json" -ErrorAction SilentlyContinue)
  $matchingLifecycle = @()
  $strayLifecycle = 0
  $phases = @{}
  foreach ($file in $lifecycleFiles) {
    $content = Get-Content -Raw $file.FullName | ConvertFrom-Json
    if ([string]$content.document_name -notlike "$DocumentPrefix*") {
      $strayLifecycle++
      continue
    }

    $matchingLifecycle += $file
    $phase = [string]$content.final_phase
    if (-not $phases.ContainsKey($phase)) { $phases[$phase] = 0 }
    $phases[$phase]++
  }

  $result = [ordered]@{
    scenario = $Name
    printer = $PrinterName
    output_dir = $OutputDir
    document_prefix = $DocumentPrefix
    submitted_jobs = $SubmittedJobs
    observed_snapshot_jobs = $jobArchives.Count
    lifecycle_jobs = $matchingLifecycle.Count
    stray_lifecycle_jobs = $strayLifecycle
    final_phases = $phases
    stdout = $Watcher.Stdout
    stderr = $Watcher.Stderr
    perf_log = $Watcher.PerfLog
  }

  $reportPath = Join-Path $OutputDir "scenario-report.json"
  $result | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 $reportPath
  return [pscustomobject]$result
}

function Invoke-WatcherScenario {
  param(
    [string]$Name,
    [string]$PrinterName,
    [int]$Duration,
    [string]$DocumentPrefix,
    [scriptblock]$Body
  )

  $outputDir = New-ScenarioDirectory -Name $Name
  Clear-TestPrintJobs -PrinterName $PrinterName
  $watcher = Start-Watcher -PrinterName $PrinterName -OutputDir $outputDir -Duration $Duration -StallTimeout $StallTimeoutSeconds
  Start-Sleep -Seconds 2

  $submitted = 0
  try {
    $submitted = & $Body
  } finally {
    Complete-Watcher -Watcher $watcher -TimeoutSeconds ($Duration + 15)
    Set-TestPrinterPaused -PrinterName $PrinterName -Paused $false
    Clear-TestPrintJobs -PrinterName $PrinterName
  }

  return Read-ScenarioResult -Name $Name -PrinterName $PrinterName -OutputDir $outputDir -Watcher $watcher -SubmittedJobs $submitted -DocumentPrefix $DocumentPrefix
}

function Run-Smoke {
  Invoke-WatcherScenario -Name "smoke" -PrinterName $SimPrinterName -Duration $DurationSeconds -DocumentPrefix "WPCE smoke" -Body {
    Send-TestPrintJob -PrinterName $SimPrinterName -DocumentName "WPCE smoke" -PageCount 1 -Copies 1
    return 1
  }
}

function Run-Matrix {
  Invoke-WatcherScenario -Name "matrix" -PrinterName $SimPrinterName -Duration ($DurationSeconds + 4) -DocumentPrefix "WPCE matrix" -Body {
    $cases = @(
      @{ Name = "portrait-a4-simplex"; Pages = 1; Copies = 1; Landscape = $false; Color = $false; Duplex = "Simplex"; Paper = "A4" },
      @{ Name = "landscape-a4-color"; Pages = 2; Copies = 1; Landscape = $true; Color = $true; Duplex = "Simplex"; Paper = "A4" },
      @{ Name = "multi-copy"; Pages = 1; Copies = 3; Landscape = $false; Color = $false; Duplex = "Simplex"; Paper = "A4" },
      @{ Name = "duplex-request"; Pages = 2; Copies = 1; Landscape = $false; Color = $false; Duplex = "Vertical"; Paper = "A4" }
    )
    foreach ($case in $cases) {
      Send-TestPrintJob -PrinterName $SimPrinterName `
        -DocumentName "WPCE matrix $($case.Name)" `
        -PageCount $case.Pages `
        -Copies $case.Copies `
        -Landscape $case.Landscape `
        -Color $case.Color `
        -Duplex $case.Duplex `
        -PaperKind $case.Paper
      Start-Sleep -Milliseconds 300
    }
    return $cases.Count
  }
}

function Run-Concurrency {
  Invoke-WatcherScenario -Name "concurrency" -PrinterName $SimPrinterName -Duration ($DurationSeconds + 8) -DocumentPrefix "WPCE concurrency" -Body {
    $jobs = @()
    for ($i = 0; $i -lt $JobCount; ++$i) {
      while (@($jobs | Where-Object { $_.State -eq "Running" }).Count -ge $Concurrency) {
        Start-Sleep -Milliseconds 100
      }

      $jobs += Start-Job -ScriptBlock ${function:Send-TestPrintJob} -ArgumentList @(
        $SimPrinterName,
        "WPCE concurrency $i",
        1 + ($i % 3),
        1 + ($i % 2),
        [bool]($i % 2),
        [bool](($i + 1) % 2),
        "Simplex",
        "A4"
      )
    }

    Wait-Job -Job $jobs -Timeout ($DurationSeconds + 4) | Out-Null
    Receive-Job -Job $jobs -ErrorAction SilentlyContinue | Out-Null
    Remove-Job -Job $jobs -Force -ErrorAction SilentlyContinue
    return $JobCount
  }
}

function Run-PcStress {
  Invoke-WatcherScenario -Name "pc-stress" -PrinterName $SimPrinterName -Duration ($DurationSeconds + 8) -DocumentPrefix "WPCE pc-stress" -Body {
    $workers = [Math]::Max(2, [Math]::Min(6, [Environment]::ProcessorCount - 1))
    $load = Start-CpuLoad -Seconds ($DurationSeconds + 2) -Workers $workers
    try {
      for ($i = 0; $i -lt [Math]::Min($JobCount, 12); ++$i) {
        Send-TestPrintJob -PrinterName $SimPrinterName -DocumentName "WPCE pc-stress $i" -PageCount 2
        Start-Sleep -Milliseconds 150
      }
    } finally {
      Stop-CpuLoad -Processes $load
    }
    return [Math]::Min($JobCount, 12)
  }
}

function Run-Stalled {
  Invoke-WatcherScenario -Name "stalled" -PrinterName $FaultPrinterName -Duration ($DurationSeconds + 8) -DocumentPrefix "WPCE stalled" -Body {
    Set-TestPrinterPaused -PrinterName $FaultPrinterName -Paused $true
    Start-Sleep -Milliseconds 500
    Send-TestPrintJob -PrinterName $FaultPrinterName -DocumentName "WPCE stalled" -PageCount 2
    return 1
  }
}

function Run-Cancel {
  Invoke-WatcherScenario -Name "cancel" -PrinterName $FaultPrinterName -Duration ($DurationSeconds + 6) -DocumentPrefix "WPCE cancel" -Body {
    $docName = "WPCE cancel"
    Set-TestPrinterPaused -PrinterName $FaultPrinterName -Paused $true
    Start-Sleep -Milliseconds 500
    Send-TestPrintJob -PrinterName $FaultPrinterName -DocumentName $docName -PageCount 3
    $job = Wait-ForPrintJob -PrinterName $FaultPrinterName -DocumentName $docName -TimeoutSeconds 8
    if ($job) {
      Start-Sleep -Seconds 1
      Remove-PrintJob -InputObject $job -ErrorAction SilentlyContinue
    }
    return 1
  }
}

function Run-NetworkTimeout {
  Invoke-WatcherScenario -Name "network-timeout" -PrinterName $FaultPrinterName -Duration ($DurationSeconds + 10) -DocumentPrefix "WPCE network-timeout" -Body {
    Set-TestPrinterPaused -PrinterName $FaultPrinterName -Paused $true
    Start-Sleep -Milliseconds 500
    for ($i = 0; $i -lt [Math]::Min($JobCount, 4); ++$i) {
      Send-TestPrintJob -PrinterName $FaultPrinterName -DocumentName "WPCE network-timeout $i" -PageCount 1
      Start-Sleep -Milliseconds 500
    }
    return [Math]::Min($JobCount, 4)
  }
}

if (-not (Test-Path $WatcherExe)) {
  throw "Watcher executable not found: $WatcherExe"
}

if (-not $SkipSetup) {
  & (Join-Path $PSScriptRoot "setup-virtual-printers.ps1") `
    -SimPrinterName $SimPrinterName `
    -FaultPrinterName $FaultPrinterName | Out-Host
}

$selected = if ($Scenario -eq "all") {
  @("smoke", "matrix", "concurrency", "pc-stress", "stalled", "cancel", "network-timeout")
} else {
  @($Scenario)
}

$results = @()
foreach ($item in $selected) {
  Write-Host "=== Scenario: $item ==="
  switch ($item) {
    "smoke" { $results += Run-Smoke }
    "matrix" { $results += Run-Matrix }
    "concurrency" { $results += Run-Concurrency }
    "pc-stress" { $results += Run-PcStress }
    "stalled" { $results += Run-Stalled }
    "cancel" { $results += Run-Cancel }
    "network-timeout" { $results += Run-NetworkTimeout }
  }
}

$summaryDir = New-Item -ItemType Directory -Force -Path $ArtifactRoot
$summaryPath = Join-Path $summaryDir.FullName "latest-summary.json"
$results | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 $summaryPath

$results | Select-Object scenario, printer, submitted_jobs, observed_snapshot_jobs, lifecycle_jobs, stray_lifecycle_jobs |
  Format-Table -AutoSize

Write-Host "Summary: $summaryPath"
