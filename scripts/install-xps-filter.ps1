#requires -Version 5.1
<#
.SYNOPSIS
  Explicit XPS filter installation or read-only health probe, using one shared service.
.DESCRIPTION
  -Probe and -DryRun never modify files, registry or services and need no elevation.
  Installation requires x64 administrator PowerShell. Spooler restart is opt-in.
  Targets are compatible XML pipelines associated with installed v4 drivers;
  this does not enable page capture for arbitrary printers.
#>
param(
  [string]$OutputDir=(Join-Path $PSScriptRoot '../data'),
  [string]$DllPath,
  [string]$BuildDirectory,
  [string]$InstallDirectory=(Join-Path $env:ProgramFiles 'WPCE'),
  [switch]$DryRun,
  [switch]$Probe,
  [switch]$RestartSpooler,
  [switch]$AsJson
)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)
$OutputEncoding=[Console]::OutputEncoding
try {
  if(-not [Environment]::Is64BitProcess){throw 'Use x64 PowerShell for the x64 print filter.'}
  if($Probe -and ($DryRun -or $RestartSpooler)){throw '-Probe cannot install or restart services.'}
  if($DllPath -and $BuildDirectory){throw 'Use either -DllPath or -BuildDirectory.'}
  if(-not $DllPath){
    if($BuildDirectory){$DllPath=Join-Path $BuildDirectory 'src/Release/wpce_xps_filter.dll'}
    else{$DllPath=Join-Path (Split-Path $PSScriptRoot -Parent) 'wpce_xps_filter.dll'}
  }
  $DllPath=[IO.Path]::GetFullPath($DllPath)
  $OutputDir=[IO.Path]::GetFullPath($OutputDir)
  $InstallDirectory=[IO.Path]::GetFullPath($InstallDirectory)
  Import-Module (Join-Path $PSScriptRoot 'lib/Wpce.Install.psm1') -Force
  if($Probe){$result=Test-WpceFilterInstallation $DllPath $InstallDirectory $OutputDir}
  else{$result=Invoke-WpceFilterInstallation $DllPath $InstallDirectory $OutputDir -DryRun:$DryRun -RestartSpooler:$RestartSpooler}
}catch{
  $result=[pscustomobject]@{schema_version=1;success=$false;ready=$false;status='failed';message=$_.Exception.Message;targets=@();errors=@($_.Exception.Message);restart_required=$false}
}
if($AsJson){$result | ConvertTo-Json -Depth 12 -Compress}
else{
  Write-Host "[$($result.status)] $($result.message)"
  foreach($target in $result.targets){Write-Host ($target | ConvertTo-Json -Compress)}
  if($result.restart_required){Write-Host 'Spooler was NOT restarted. Arrange an interruption window, then explicitly rerun with -RestartSpooler.'}
}
if(-not $result.success){exit 1}
