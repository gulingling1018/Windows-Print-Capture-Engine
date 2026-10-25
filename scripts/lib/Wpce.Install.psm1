#requires -Version 5.1
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'Wpce.Install.Native.psm1') -Force

function Get-WpceDigest {
  param([byte[]]$Bytes)
  $sha=[Security.Cryptography.SHA256]::Create()
  try {return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant()}
  finally {$sha.Dispose()}
}
function ConvertTo-WpceBytes {
  param($Object)
  return ,([Text.UTF8Encoding]::new($false).GetBytes(($Object | ConvertTo-Json -Depth 15 -Compress)))
}
function Get-WpceInstallResult {
  param([string]$Status,[bool]$Success,[string]$Message,$Targets=@(),$Errors=@())
  return [pscustomobject]@{schema_version=1;status=$Status;success=$Success;ready=($Status -eq 'configured');
    message=$Message;targets=@($Targets | Where-Object {$null -ne $_});errors=@($Errors | Where-Object {$null -ne $_});restart_required=($Status -eq 'restart_required')}
}
function Test-WpceFilterInstallation {
  param([string]$DllSource,[string]$InstallDirectory,[string]$OutputDirectory,[hashtable]$Operations)
  if(-not $Operations){$Operations=New-WpceInstallOperations}
  $ops=$Operations
  try {
    $dll=Join-Path $InstallDirectory 'wpce_xps_filter.dll'
    $recordPath=Join-Path $InstallDirectory 'filter-installation.json'
    if(@(& $ops.Pending $InstallDirectory).Count){throw 'Unfinished installation transaction requires recovery; inspect _install-transactions.'}
    if(-not (& $ops.Exists $recordPath)){return Get-WpceInstallResult 'not_installed' $false 'No verified installation record; legacy sentinel is ignored.'}
    $record=[Text.Encoding]::UTF8.GetString((& $ops.Read $recordPath)) | ConvertFrom-Json
    if($record.schema_version -ne 1 -or $record.installer_version -ne 1){throw 'Unsupported installation record.'}
    if(-not (& $ops.Exists $DllSource) -or -not (& $ops.Exists $dll)){throw 'Expected source/installed DLL is missing.'}
    & $ops.ValidateDll $DllSource
    $hash=Get-WpceDigest (& $ops.Read $dll)
    if($hash -ne $record.dll_hash -or $hash -ne (Get-WpceDigest (& $ops.Read $DllSource))){throw 'DLL hash does not match this release.'}
    $registration=& $ops.Registration
    if(-not $registration.exists -or $registration.path -ine $dll -or $registration.threading -ine 'Both'){throw 'COM registration does not match the installed DLL.'}
    if((& $ops.Environment) -ine $OutputDirectory -or $record.output_directory -ine $OutputDirectory){throw 'Capture output directory does not match.'}
    $current=@(& $ops.Discover | Sort-Object -Unique)
    if(-not $current.Count -or @($record.targets).Count -ne $current.Count){throw 'Installed pipeline target set has changed or is empty.'}
    foreach($target in $record.targets){
      if($target.path -notin $current){throw 'Pipeline target has changed.'}
      $bytes=& $ops.Read $target.path
      if((Get-WpceDigest $bytes) -ne $target.hash -or -not (Get-WpcePipelineXml $bytes $dll -Verify)){throw "Pipeline verification failed: $($target.path)"}
    }
    if((& $ops.ServiceState) -ne 'Running'){throw 'Print Spooler is not running.'}
    if(-not $record.restart_confirmed){return Get-WpceInstallResult 'restart_required' $true 'Pipeline configuration verified; explicit Spooler restart is still required.' $record.targets}
    return Get-WpceInstallResult 'configured' $true 'Configuration verified for the listed targets; real filter loading/printing still requires a test print.' $record.targets
  }catch {return Get-WpceInstallResult 'unverified' $false $_.Exception.Message @() @($_.Exception.Message)}
}

function Invoke-WpceFilterInstallation {
  param([string]$DllSource,[string]$InstallDirectory,[string]$OutputDirectory,
        [switch]$DryRun,[switch]$RestartSpooler,[hashtable]$Operations)
  if(-not $Operations){$Operations=New-WpceInstallOperations}
  $ops=$Operations; $snapshots=@(); $plans=@(); $lock=$null; $transaction=$null; $journalPath=$null
  $registrationTouched=$false; $environmentTouched=$false; $restartAttempted=$false; $mutationsStarted=$false
  $dll=Join-Path $InstallDirectory 'wpce_xps_filter.dll'
  $recordPath=Join-Path $InstallDirectory 'filter-installation.json'
  try {
    if(-not (& $ops.Exists $DllSource)){throw 'Release DLL not found; pass -DllPath or an exact -BuildDirectory.'}
    & $ops.ValidateDll $DllSource
    $source=& $ops.Read $DllSource; $hash=Get-WpceDigest $source
    $targets=@(& $ops.Discover | Sort-Object -Unique)
    if(-not $targets.Count){throw 'No compatible installed v4 pipeline target found; nothing was installed.'}
    foreach($path in $targets){
      if($path -ieq $dll -or $path -ieq $recordPath -or $path -ieq $DllSource){throw 'Installation target collision.'}
      $original=& $ops.Read $path
      try{$updated=Get-WpcePipelineXml $original $dll}catch{throw "Unsupported pipeline $path : $($_.Exception.Message)"}
      $plans+=@{path=$path;bytes=$updated;hash=(Get-WpceDigest $updated)}
    }
    if(@(& $ops.Pending $InstallDirectory).Count){throw 'Unfinished installation transaction; preserve and recover it before retrying.'}
    if($DryRun){return Get-WpceInstallResult 'dry_run' $true 'Plan validated; no directories, registry, files or service state were changed.' ($plans | ForEach-Object {$_.path})}
    if(-not (& $ops.Admin)){throw 'Installation requires an elevated x64 administrator process.'}
    $lock=& $ops.Lock $InstallDirectory
    if(@(& $ops.Pending $InstallDirectory).Count){throw 'Another incomplete installation transaction exists.'}
    $oldRegistration=& $ops.Registration; $oldEnvironment=& $ops.SnapshotEnvironment
    if($RestartSpooler -and (& $ops.ServiceState) -ne 'Running'){throw 'Spooler must be running before an explicit restart.'}
    foreach($path in @($dll,$recordPath)+$targets){
      $exists=& $ops.Exists $path
      $snapshots+=@{path=$path;existed=$exists;original=$(if($exists){[Convert]::ToBase64String((& $ops.Read $path))}else{''})}
    }
    $transaction=@{schema_version=1;status='in_progress';created_utc=[DateTime]::UtcNow.ToString('o');
      snapshots=$snapshots;registration=$oldRegistration;environment=$oldEnvironment;errors=@()}
    $journalPath=Join-Path $InstallDirectory ('_install-transactions/'+[guid]::NewGuid().ToString('N')+'.json')
    $journalBytes=ConvertTo-WpceBytes $transaction
    & $ops.Write $journalPath $journalBytes
    # Durable byte-for-byte backups exist before any authoritative target changes.
    $persisted=& $ops.Read $journalPath
    if((Get-WpceDigest $persisted) -ne (Get-WpceDigest $journalBytes)){throw 'Transaction backup bytes did not match.'}
    $writtenJournal=[Text.Encoding]::UTF8.GetString($persisted) | ConvertFrom-Json
    if($writtenJournal.status -ne 'in_progress' -or @($writtenJournal.snapshots).Count -ne $snapshots.Count){throw 'Transaction backup verification failed.'}
    $mutationsStarted=$true
    & $ops.PrepareOutput $OutputDirectory
    if($RestartSpooler){
      $restartAttempted=$true
      & $ops.StopService; & $ops.WaitService 'Stopped' 30
    }
    if(-not (& $ops.Exists $dll) -or (Get-WpceDigest (& $ops.Read $dll)) -ne $hash){& $ops.Write $dll $source}
    if((Get-WpceDigest (& $ops.Read $dll)) -ne $hash){throw 'DLL copy verification failed.'}
    $registrationTouched=$true; & $ops.Register $dll
    $registered=& $ops.Registration
    if(-not $registered.exists -or $registered.path -ine $dll -or $registered.threading -ine 'Both'){throw 'COM registration verification failed.'}
    foreach($plan in $plans){
      & $ops.Write $plan.path $plan.bytes
      if((Get-WpceDigest (& $ops.Read $plan.path)) -ne $plan.hash){throw "Pipeline replacement verification failed: $($plan.path)"}
    }
    $environmentTouched=$true; & $ops.SetEnvironment $OutputDirectory
    if((& $ops.Environment) -ine $OutputDirectory){throw 'Machine environment verification failed.'}
    if($RestartSpooler){
      & $ops.StartService; & $ops.WaitService 'Running' 30
      if((& $ops.ServiceState) -ne 'Running'){throw 'Spooler did not reach Running.'}
    }
    $record=@{schema_version=1;installer_version=1;dll_hash=$hash;output_directory=$OutputDirectory;
      installed_utc=[DateTime]::UtcNow.ToString('o');restart_confirmed=[bool]$RestartSpooler;
      targets=@($plans | ForEach-Object {@{path=$_.path;hash=$_.hash}})}
    & $ops.Write $recordPath (ConvertTo-WpceBytes $record)
    $transaction.status='committed'; & $ops.Write $journalPath (ConvertTo-WpceBytes $transaction)
    $verified=Test-WpceFilterInstallation $DllSource $InstallDirectory $OutputDirectory $ops
    if(-not $verified.success){throw "Final installation verification failed: $($verified.message)"}
    return $verified
  }catch {
    $errors=@($_.Exception.Message)
    if($transaction){
      foreach($snapshot in $(if($mutationsStarted){$snapshots}else{@()})){
        try {
          if($snapshot.existed){
            $bytes=[Convert]::FromBase64String($snapshot.original)
            if(-not (& $ops.Exists $snapshot.path) -or (Get-WpceDigest (& $ops.Read $snapshot.path)) -ne (Get-WpceDigest $bytes)){& $ops.Write $snapshot.path $bytes}
            if((Get-WpceDigest (& $ops.Read $snapshot.path)) -ne (Get-WpceDigest $bytes)){throw 'Restored bytes did not match.'}
          }else{& $ops.Delete $snapshot.path}
        }catch{$errors+="Rollback file failed: $($snapshot.path): $($_.Exception.Message)"}
      }
      if($registrationTouched){try{
        & $ops.RestoreRegistration $transaction.registration
        $restored=& $ops.Registration;$old=$transaction.registration
        if($restored.exists -ne $old.exists -or $restored.path -ine $old.path -or $restored.threading -ine $old.threading -or $restored.description -ne $old.description){throw 'Restored COM values did not match.'}
      }catch{$errors+="Rollback registration failed: $($_.Exception.Message)"}}
      if($environmentTouched){try{
        & $ops.RestoreEnvironment $transaction.environment
        $restored=& $ops.SnapshotEnvironment
        if($restored.value -ne $transaction.environment.value -or $restored.kind -ne $transaction.environment.kind){throw 'Restored environment did not match.'}
      }catch{$errors+="Rollback environment failed: $($_.Exception.Message)"}}
      if($restartAttempted){try{
        if((& $ops.ServiceState) -eq 'Running'){& $ops.StopService;& $ops.WaitService 'Stopped' 30}
        & $ops.StartService; & $ops.WaitService 'Running' 30
      }catch{$errors+="Service recovery failed: $($_.Exception.Message)"}}
      $transaction.status=if($errors.Count -eq 1){'rolled_back'}else{'rollback_failed'}
      $transaction.errors=$errors
      try{& $ops.Write $journalPath (ConvertTo-WpceBytes $transaction)}catch{$errors+="Transaction status could not be saved: $($_.Exception.Message)"}
    }
    return Get-WpceInstallResult 'failed' $false ($errors -join ' | ') ($plans | ForEach-Object {$_.path}) $errors
  }finally{if($lock){$lock.Dispose()}}
}
Export-ModuleMember -Function Test-WpceFilterInstallation,Invoke-WpceFilterInstallation
