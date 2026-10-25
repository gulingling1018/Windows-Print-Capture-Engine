#requires -Version 5.1
param([string]$Case='All')
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '../scripts/lib/Wpce.Install.psm1') -Force
Import-Module (Join-Path $PSScriptRoot '../scripts/lib/Wpce.Install.Native.psm1') -Force
function Assert-Wpce {param($Condition,[string]$Message) if(-not $Condition){throw $Message}}
function New-FakeInstallation {
  $state=@{files=@{};writes=@();registration=@{exists=$false;path=$null;threading=$null;description=$null};environment='old';environmentKind='ExpandString';service='Running';
    fault='';fired=$false;admin=$true;stops=0;starts=0;discover=@('C:/fake/driver/pipeline.xml')}
  $state.files['C:/fake/release/filter.dll']=[Text.Encoding]::UTF8.GetBytes('fake x64 DLL')
  $state.original=[Text.Encoding]::UTF8.GetBytes('<Filters><Filter dll="original.dll" clsid="{00000000-0000-0000-0000-000000000001}"><Input guid="{b8cf8530-5562-47c4-ab67-b1f69ecf961e}"/><Output guid="{4368d8a2-4181-4a9f-b295-3d9a38bb9ba0}"/></Filter></Filters>')
  $state.files['C:/fake/driver/pipeline.xml']=$state.original
  $ops=@{
    Exists={param($p) $state.files.ContainsKey($p.Replace('\','/'))}.GetNewClosure()
    Read={param($p) $p=$p.Replace('\','/');if(-not $state.files.ContainsKey($p)){throw "Missing fake file: $p"};return ,$state.files[$p]}.GetNewClosure()
    Write={param($p,$b)
      $p=$p.Replace('\','/')
      $state.writes+=@($p)
      if(-not $state.fired -and (($state.fault -eq 'Copy' -and $p -like '*wpce_xps_filter.dll') -or
        ($state.fault -eq 'Backup' -and $p -like '*_install-transactions*'))){$state.fired=$true;throw 'Injected write failure'}
      if(-not $state.fired -and $state.fault -in @('Partial','Rollback') -and $p -like '*pipeline.xml'){
        $state.fired=$true;$state.files[$p]=[Text.Encoding]::UTF8.GetBytes('partial');throw 'Injected partial write'
      }
      if($state.fired -and $state.fault -eq 'Rollback' -and $p -like '*pipeline.xml'){throw 'Injected rollback failure'}
      if(-not $state.fired -and $state.fault -eq 'PartialSecond' -and $p -like '*driver2*'){
        $state.fired=$true;$state.files[$p]=[Text.Encoding]::UTF8.GetBytes('partial');throw 'Second pipeline write failed'
      }
      if(-not $state.fired -and $state.fault -eq 'Record' -and $p -like '*filter-installation.json'){$state.fired=$true;throw 'Record write failed'}
      if(-not $state.fired -and $state.fault -eq 'BackupCorruption' -and $p -like '*_install-transactions*'){
        $state.fired=$true;$record=[Text.Encoding]::UTF8.GetString($b)|ConvertFrom-Json;$record.snapshots[-1].original=''
        $state.files[$p]=[Text.Encoding]::UTF8.GetBytes(($record|ConvertTo-Json -Depth 15 -Compress));return
      }
      $state.files[$p]=[byte[]]$b.Clone()
    }.GetNewClosure()
    Delete={param($p) $state.files.Remove($p.Replace('\','/'))}.GetNewClosure()
    ValidateDll={param($p) if($state.fault -eq 'InvalidDll'){throw 'Wrong PE architecture'}}.GetNewClosure()
    Admin={$state.admin}.GetNewClosure()
    Lock={New-Object IO.MemoryStream}
    PrepareOutput={param($p) $state.writes+=@($p)}.GetNewClosure()
    Discover={return $state.discover}.GetNewClosure()
    Pending={param($p)
      foreach($key in @($state.files.Keys | Where-Object {$_ -like '*_install-transactions*'})){
        $record=[Text.Encoding]::UTF8.GetString($state.files[$key]) | ConvertFrom-Json
        if($record.status -notin @('committed','rolled_back')){$key}
      }
    }.GetNewClosure()
    Registration={$state.registration}.GetNewClosure()
    RestoreRegistration={param($old) $state.registration=$old}.GetNewClosure()
    Register={param($dll) $state.registration=@{exists=$true;path=$dll;threading='Both';description='WPCE'};if($state.fault -eq 'Register'){throw 'Registration failed'}}.GetNewClosure()
    Environment={$state.environment}.GetNewClosure()
    SnapshotEnvironment={return @{value=$state.environment;kind=$state.environmentKind}}.GetNewClosure()
    RestoreEnvironment={param($old) $state.environment=$old.value;$state.environmentKind=$old.kind}.GetNewClosure()
    SetEnvironment={param($v) $state.environment=$v;$state.environmentKind='String'}.GetNewClosure()
    ServiceState={$state.service}.GetNewClosure()
    StopService={$state.stops++;$state.service='Stopped'}.GetNewClosure()
    StartService={$state.starts++;$state.service='Running';if($state.fault -eq 'Start' -and $state.starts -eq 1){throw 'Start failed'}}.GetNewClosure()
    WaitService={param($desired,$seconds)
      if($state.fault -eq 'StopTimeout' -and $desired -eq 'Stopped'){throw 'Stop timeout'}
      if($state.fault -eq 'StartTimeout' -and $desired -eq 'Running'){throw 'Start timeout'}
      if($state.service -ne $desired){throw 'Wrong service state'}
    }.GetNewClosure()
  }
  return @{state=$state;ops=$ops}
}
function Invoke-FakeInstall {param($Fake,[switch]$DryRun,[switch]$Restart)
  Invoke-WpceFilterInstallation 'C:/fake/release/filter.dll' 'C:/fake/install' 'C:/fake/output' -Operations $Fake.ops -DryRun:$DryRun -RestartSpooler:$Restart
}
function Assert-OriginalPipeline {param($Fake)
  Assert-Wpce ([Convert]::ToBase64String($Fake.state.files['C:/fake/driver/pipeline.xml']) -eq [Convert]::ToBase64String($Fake.state.original)) 'Original pipeline bytes were not restored.'
}
$cases=@{}
$cases.DryRun={
  $f=New-FakeInstallation;$result=Invoke-FakeInstall $f -DryRun
  Assert-Wpce ($result.success -and $result.status -eq 'dry_run') 'Dry run failed'
  Assert-Wpce ($f.state.writes.Count -eq 0 -and $f.state.stops -eq 0 -and -not $f.state.registration.exists) 'DryRun modified state'
}
$cases.ZeroTargets={
  $f=New-FakeInstallation;$f.state.discover=@();$r=Invoke-FakeInstall $f
  Assert-Wpce (-not $r.success -and $f.state.writes.Count -eq 0) 'Zero targets accepted'
}
$cases.MissingDll={
  $f=New-FakeInstallation;$f.state.files.Remove('C:/fake/release/filter.dll');$r=Invoke-FakeInstall $f
  Assert-Wpce (-not $r.success -and $f.state.writes.Count -eq 0) 'Missing DLL accepted'
}
$cases.InvalidDll={
  $f=New-FakeInstallation;$f.state.fault='InvalidDll';$r=Invoke-FakeInstall $f
  Assert-Wpce (-not $r.success -and $f.state.writes.Count -eq 0) 'Invalid DLL accepted'
}
$cases.NoAdmin={
  $f=New-FakeInstallation;$f.state.admin=$false;$r=Invoke-FakeInstall $f
  Assert-Wpce (-not $r.success -and $f.state.writes.Count -eq 0) 'Unprivileged install accepted'
}
$cases.XmlCompatibility={
  foreach($xml in @('<Filters><Filter><Input guid="wrong"/></Filter></Filters>','<Unknown/>','<!DOCTYPE x [<!ENTITY y SYSTEM "file:///secret">]><Filters>&y;</Filters>')){
    $f=New-FakeInstallation;$f.state.files['C:/fake/driver/pipeline.xml']=[Text.Encoding]::UTF8.GetBytes($xml)
    $r=Invoke-FakeInstall $f;Assert-Wpce (-not $r.success -and $f.state.writes.Count -eq 0) 'Unsupported/unsafe XML accepted'
  }
}
$cases.BackupFailure={
  $f=New-FakeInstallation;$f.state.fault='Backup';$r=Invoke-FakeInstall $f
  Assert-Wpce (-not $r.success) 'Backup failure accepted'
  Assert-Wpce (@($f.state.writes | Where-Object {$_ -notlike '*_install-transactions*'}).Count -eq 0) 'Target modified before verified backup'
  Assert-OriginalPipeline $f
}
$cases.BackupCorruption={
  $f=New-FakeInstallation;$f.state.fault='BackupCorruption';$r=Invoke-FakeInstall $f
  Assert-Wpce (-not $r.success) 'Corrupted backup accepted';Assert-OriginalPipeline $f
  Assert-Wpce (@($f.state.writes | Where-Object {$_ -notlike '*_install-transactions*'}).Count -eq 0) 'Changed target after corrupted backup'
}
foreach($fault in @('Copy','Register','Partial','StopTimeout','Start','StartTimeout')){
  $cases[$fault+'Failure']={
    $f=New-FakeInstallation;$f.state.fault=$fault;$r=Invoke-FakeInstall $f -Restart
    Assert-Wpce (-not $r.success -and -not $r.ready) "Failure was accepted: $fault / $($r.message)"
    Assert-OriginalPipeline $f
    Assert-Wpce (-not $f.state.registration.exists -and $f.state.environment -eq 'old' -and $f.state.environmentKind -eq 'ExpandString') 'Registration/environment value or registry kind not restored'
    Assert-Wpce (-not $f.state.files.ContainsKey('C:/fake/install/filter-installation.json')) 'Failure left success record'
    if($fault -in @('StopTimeout','Start','StartTimeout')){Assert-Wpce ($f.state.starts -gt 0) 'Service recovery was not attempted'}
  }.GetNewClosure()
}
$cases.RollbackFailure={
  $f=New-FakeInstallation;$f.state.fault='Rollback';$r=Invoke-FakeInstall $f
  Assert-Wpce (-not $r.success -and $r.message -match 'Rollback file failed') 'Rollback error hidden'
  $pending=@(& $f.ops.Pending 'C:/fake/install');Assert-Wpce ($pending.Count -eq 1) 'Recovery journal lost'
  $again=Invoke-FakeInstall $f;Assert-Wpce (-not $again.success) 'Unrecovered install permitted'
}
$cases.PendingRestart={
  $f=New-FakeInstallation;$r=Invoke-FakeInstall $f
  Assert-Wpce ($r.success -and -not $r.ready -and $r.restart_required -and $f.state.stops -eq 0) "Unexpected readiness: $($r.message)"
}
$cases.ExplicitRestart={
  $f=New-FakeInstallation;$r=Invoke-FakeInstall $f -Restart
  Assert-Wpce ($r.success -and $r.ready -and $f.state.stops -eq 1 -and $f.state.starts -eq 1) "Explicit restart failed: $($r.message)"
}
$cases.ProbeRevalidates={
  $f=New-FakeInstallation;$r=Invoke-FakeInstall $f -Restart;Assert-Wpce $r.success $r.message
  $f.state.files['C:/fake/driver/pipeline.xml']=$f.state.original
  $r=Test-WpceFilterInstallation 'C:/fake/release/filter.dll' 'C:/fake/install' 'C:/fake/output' $f.ops
  Assert-Wpce (-not $r.ready -and -not $r.success) 'Changed pipeline accepted by record alone'
}
$cases.LegacySentinel={
  $f=New-FakeInstallation;$f.state.files['C:/fake/install/wpce_filter_installed']=[byte[]]@(1)
  $r=Test-WpceFilterInstallation 'C:/fake/release/filter.dll' 'C:/fake/install' 'C:/fake/output' $f.ops
  Assert-Wpce (-not $r.ready -and $r.status -eq 'not_installed' -and $f.state.writes.Count -eq 0) 'Legacy sentinel trusted'
}
$cases.XmlIdempotency={
  $f=New-FakeInstallation;$bytes=Get-WpcePipelineXml $f.state.original 'C:/fake/a&b/filter.dll'
  $twice=Get-WpcePipelineXml $bytes 'C:/fake/a&b/filter.dll'
  Assert-Wpce (Get-WpcePipelineXml $twice 'C:/fake/a&b/filter.dll' -Verify) 'XML namespace/escaping round trip failed'
  $bad=[Text.Encoding]::UTF8.GetBytes('<Filters/>')
  Assert-Wpce (-not (Get-WpcePipelineXml $bad 'C:/fake/filter.dll' -Verify)) 'Empty pipeline reported configured'
}
$cases.StandardStreamInput={
  $f=New-FakeInstallation
  $stream=[Text.Encoding]::UTF8.GetBytes([Text.Encoding]::UTF8.GetString($f.state.original).Replace('{b8cf8530-5562-47c4-ab67-b1f69ecf961e}','{4d47a67c-66cc-4430-850e-daf466fe5bc4}'))
  $updated=Get-WpcePipelineXml $stream 'C:/fake/filter.dll'
  Assert-Wpce (Get-WpcePipelineXml $updated 'C:/fake/filter.dll' -Verify) 'Standard stream interface was rejected'
  Assert-Wpce ([Text.Encoding]::UTF8.GetString($updated).Contains('{4d47a67c-66cc-4430-850e-daf466fe5bc4}')) 'Downstream interface was changed'
}
$cases.AtomicUnicode={
  $root=Join-Path ([IO.Path]::GetTempPath()) ('wpce-install-'+[guid]::NewGuid().ToString('N'))
  $path=Join-Path $root '中文目录/配置.xml'
  try{
    Write-WpceAtomicBytes $path ([byte[]]@(1,2,3));Write-WpceAtomicBytes $path ([byte[]]@(4,5))
    Assert-Wpce ([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -eq 'BAU=') 'Atomic Unicode replacement failed'
  }finally{
    if(-not [IO.Path]::GetFullPath($root).StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe test cleanup'}
    if([IO.Directory]::Exists($root)){Remove-Item -LiteralPath $root -Recurse -Force}
  }
}
$cases.MultipleTargetRollback={
  $f=New-FakeInstallation;$second='C:/fake/driver2/pipeline.xml';$f.state.discover+=@($second);$f.state.files[$second]=$f.state.original
  $f.state.fault='PartialSecond';$r=Invoke-FakeInstall $f
  Assert-Wpce (-not $r.success) 'Partial target failure accepted';Assert-OriginalPipeline $f
  Assert-Wpce ([Convert]::ToBase64String($f.state.files[$second]) -eq [Convert]::ToBase64String($f.state.original)) 'Second pipeline not restored'
}
$cases.RecordFailure={
  $f=New-FakeInstallation;$f.state.fault='Record';$r=Invoke-FakeInstall $f -Restart
  Assert-Wpce (-not $r.success) 'Record failure accepted';Assert-OriginalPipeline $f
  Assert-Wpce (-not $f.state.files.ContainsKey('C:/fake/install/filter-installation.json')) 'Failed install advertised by record'
}
$cases.SourceHashChange={
  $f=New-FakeInstallation;$r=Invoke-FakeInstall $f -Restart;Assert-Wpce $r.success $r.message
  $f.state.files['C:/fake/release/filter.dll']=[Text.Encoding]::UTF8.GetBytes('new release')
  $r=Test-WpceFilterInstallation 'C:/fake/release/filter.dll' 'C:/fake/install' 'C:/fake/output' $f.ops
  Assert-Wpce (-not $r.success -and -not $r.ready) 'New release trusted stale installation'
}
$cases.IdenticalDllDoesNotRewriteLoadedBinary={
  $f=New-FakeInstallation;$f.state.files['C:/fake/install/wpce_xps_filter.dll']=$f.state.files['C:/fake/release/filter.dll']
  $f.state.fault='Copy';$r=Invoke-FakeInstall $f
  Assert-Wpce ($r.success -and -not $f.state.fired) 'Identical installed DLL was unnecessarily replaced'
}
$cases.CliMissingDllDryRun={
  $root=Join-Path ([IO.Path]::GetTempPath()) ('wpce-cli-'+[guid]::NewGuid().ToString('N'))
  $cli=Join-Path $PSScriptRoot '../scripts/install-xps-filter.ps1'
  $output=& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $cli -DllPath (Join-Path $root 'missing.dll') -OutputDir (Join-Path $root 'data') -InstallDirectory (Join-Path $root 'install') -DryRun -AsJson
  $code=$LASTEXITCODE;$result=$output | ConvertFrom-Json
  Assert-Wpce ($code -ne 0 -and -not $result.success -and -not [IO.Directory]::Exists($root)) 'CLI DryRun created a directory or hid missing DLL'
}
$selected=@(if($Case -eq 'All'){$cases.Keys | Sort-Object}else{$Case})
foreach($name in $selected){if(-not $cases.ContainsKey($name)){throw "Unknown case: $name"}; & $cases[$name];Write-Host "PASS: $name"}
Write-Host "Install service tests passed: $($selected.Count)"
