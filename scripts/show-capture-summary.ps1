param(
  [string]$CaptureDirectory = ".\artifacts\capture"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $CaptureDirectory)) {
  throw "Capture directory not found: $CaptureDirectory"
}

$jobsDirectory = Join-Path $CaptureDirectory "jobs"
if (-not (Test-Path $jobsDirectory)) {
  throw "Capture jobs directory not found: $jobsDirectory"
}

Get-ChildItem -Path $jobsDirectory -Filter "job-*.json" |
  Sort-Object Name |
  ForEach-Object {
    Get-Content -Raw -Encoding UTF8 $_.FullName | ConvertFrom-Json
  } |
  Select-Object job_id, user, document_name, printer_name, submit_time_utc, status |
  Format-Table -AutoSize
