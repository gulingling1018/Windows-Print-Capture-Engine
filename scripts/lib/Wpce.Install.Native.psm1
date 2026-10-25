#requires -Version 5.1
Set-StrictMode -Version Latest
$script:Clsid='{B1C8E3F0-2D4A-5B6C-7D8E-9F0A1B2C3D4E}'
$script:Key='SOFTWARE\Classes\CLSID\'+$script:Clsid

function Write-WpceAtomicBytes {
  param([string]$Path,[byte[]]$Bytes)
  $parent=[IO.Path]::GetDirectoryName($Path)
  $null=[IO.Directory]::CreateDirectory($parent)
  $temp=Join-Path $parent ([IO.Path]::GetFileName($Path)+'.'+[guid]::NewGuid().ToString('N')+'.tmp')
  try {
    $file=[IO.File]::Open($temp,'CreateNew','Write','None')
    try { $file.Write($Bytes,0,$Bytes.Length); $file.Flush($true) } finally {$file.Dispose()}
    if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temp,$Path,[Management.Automation.Language.NullString]::Value) }
    else { [IO.File]::Move($temp,$Path) }
  } finally {if([IO.File]::Exists($temp)) {[IO.File]::Delete($temp)}}
}

function Read-WpceXml {
  param([byte[]]$Bytes)
  $settings=New-Object System.Xml.XmlReaderSettings
  $settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit; $settings.XmlResolver=$null
  $stream=New-Object IO.MemoryStream(,$Bytes)
  $reader=[Xml.XmlReader]::Create($stream,$settings)
  try {
    $xml=New-Object Xml.XmlDocument
    $xml.PreserveWhitespace=$true; $xml.XmlResolver=$null; $xml.Load($reader)
    return ,$xml
  } finally {$reader.Dispose();$stream.Dispose()}
}

function Get-WpcePipelineXml {
  param([byte[]]$Bytes,[string]$DllPath,[switch]$Verify)
  $xml=Read-WpceXml $Bytes
  $root=$xml.DocumentElement
  if ($root.LocalName -ne 'Filters' -or $root.NamespaceURI -notin @('','http://schemas.microsoft.com/windows/2005/06/printing/FilterPipeline')) {
    throw 'Unsupported filter pipeline XML root.'
  }
  $filters=@($root.ChildNodes | Where-Object {$_.NodeType -eq 'Element' -and $_.LocalName -eq 'Filter'})
  $ours=@($filters | Where-Object {$_.GetAttribute('clsid') -ieq $script:Clsid})
  $provider='{b8cf8530-5562-47c4-ab67-b1f69ecf961e}'
  $consumer='{4368d8a2-4181-4a9f-b295-3d9a38bb9ba0}'
  if ($Verify) {
    if ($ours.Count -ne 1 -or $filters[0] -ne $ours[0] -or $ours[0].GetAttribute('dll') -ine $DllPath) {return $false}
    $input=$ours[0].SelectSingleNode("*[local-name()='Input']")
    $output=$ours[0].SelectSingleNode("*[local-name()='Output']")
    return $input -and $output -and $input.GetAttribute('guid') -ieq $provider -and $output.GetAttribute('guid') -ieq $consumer
  }
  foreach($node in $ours) {$null=$root.RemoveChild($node)}
  $first=@($root.ChildNodes | Where-Object {$_.NodeType -eq 'Element' -and $_.LocalName -eq 'Filter'})
  if (-not $first.Count) {throw 'No downstream filter; cannot verify input compatibility.'}
  $input=$first[0].SelectSingleNode("*[local-name()='Input']")
  # The pipeline manager bridges the standard document and byte-stream
  # interfaces (the Microsoft pipeline XML example uses both adjacently).
  $readStream='{4d47a67c-66cc-4430-850e-daf466fe5bc4}'
  if (-not $input -or $input.GetAttribute('guid') -notin @($provider,$readStream)) {throw 'Unknown downstream input interface; compatibility is unverified.'}
  $filter=$xml.CreateElement('Filter',$root.NamespaceURI)
  $filter.SetAttribute('dll',$DllPath); $filter.SetAttribute('clsid',$script:Clsid); $filter.SetAttribute('name','WPCE Capture')
  foreach($item in @(@('Input',$provider),@('Output',$consumer))) {
    $part=$xml.CreateElement($item[0],$root.NamespaceURI);$part.SetAttribute('guid',$item[1]);$null=$filter.AppendChild($part)
  }
  $null=$root.PrependChild($filter)
  $stream=New-Object IO.MemoryStream
  $settings=New-Object Xml.XmlWriterSettings
  $settings.Encoding=New-Object Text.UTF8Encoding($false);$settings.Indent=$false
  $writer=[Xml.XmlWriter]::Create($stream,$settings)
  try {$xml.Save($writer);$writer.Flush();return ,$stream.ToArray()} finally {$writer.Dispose();$stream.Dispose()}
}

function Get-WpceRegistration {
  $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($script:Key)
  if(-not $key) {return @{exists=$false;inproc_exists=$false;path=$null;threading=$null;description=$null}}
  try {
    $child=$key.OpenSubKey('InprocServer32')
    try {return @{exists=$true;inproc_exists=($null -ne $child);description=$key.GetValue('');path=$(if($child){$child.GetValue('')});threading=$(if($child){$child.GetValue('ThreadingModel')})}}
    finally {if($child){$child.Dispose()}}
  } finally {$key.Dispose()}
}
function Restore-WpceRegistration {
  param($Old)
  if(-not $Old.exists) {[Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree($script:Key,$false);return}
  $key=[Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($script:Key)
  try {
    if($null -ne $Old.description){$key.SetValue('',$Old.description)}else{$key.DeleteValue('',$false)}
    if(-not $Old.inproc_exists){$key.DeleteSubKeyTree('InprocServer32',$false);return}
    $child=$key.CreateSubKey('InprocServer32')
    try {
      foreach($pair in @(@('',$Old.path),@('ThreadingModel',$Old.threading))) {
        if($null -ne $pair[1]){$child.SetValue($pair[0],$pair[1])}else{$child.DeleteValue($pair[0],$false)}
      }
    } finally {$child.Dispose()}
  } finally {$key.Dispose()}
}
function New-WpceInstallOperations {
  return @{
    Exists={param($p) [IO.File]::Exists($p)}
    Read={param($p) ,[IO.File]::ReadAllBytes($p)}
    Write={param($p,$b) Write-WpceAtomicBytes $p $b}
    Delete={param($p) if([IO.File]::Exists($p)){[IO.File]::Delete($p)}}
    Admin={ $identity=[Security.Principal.WindowsIdentity]::GetCurrent();try {(New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)}finally{$identity.Dispose()} }
    ValidateDll={param($p)
      $b=[IO.File]::ReadAllBytes($p)
      if($b.Length -lt 256 -or $b[0] -ne 77 -or $b[1] -ne 90){throw 'Not a PE DLL.'}
      $offset=[BitConverter]::ToInt32($b,60)
      if($offset -lt 0 -or $offset+24 -gt $b.Length -or [BitConverter]::ToUInt32($b,$offset) -ne 17744 -or
        [BitConverter]::ToUInt16($b,$offset+4) -ne 34404 -or ([BitConverter]::ToUInt16($b,$offset+22) -band 8192) -eq 0){throw 'Expected an x64 PE DLL.'}
    }
    Discover={
      $names=@(Get-Printer -ErrorAction Stop | Select-Object -ExpandProperty DriverName -Unique)
      $paths=@()
      foreach($driver in @(Get-PrinterDriver -PrinterEnvironment 'Windows x64' -ErrorAction Stop)) {
        if($driver.Name -notin $names -or $driver.MajorVersion -ne 4){continue}
        $candidates=@($driver.DependentFiles | Where-Object {$_ -match '(?i)pipeline.*\.xml$'})
        if($driver.ConfigFile){$candidates+=Join-Path ([IO.Path]::GetDirectoryName($driver.ConfigFile)) 'FilterPipelineConfig.xml'}
        foreach($path in $candidates){if($path -and [IO.File]::Exists($path)){$paths += [IO.Path]::GetFullPath($path)}}
      }
      return @($paths | Sort-Object -Unique)
    }
    Registration={Get-WpceRegistration}
    RestoreRegistration={param($old) Restore-WpceRegistration $old}
    Register={param($dll)
      $process=New-Object Diagnostics.Process
      $process.StartInfo.FileName=Join-Path $env:SystemRoot 'System32/regsvr32.exe'
      $process.StartInfo.Arguments='/s "'+$dll+'"';$process.StartInfo.UseShellExecute=$false
      $process.StartInfo.CreateNoWindow=$true;$process.StartInfo.WindowStyle='Hidden'
      try {
        if(-not $process.Start()){throw 'regsvr32 could not start.'}
        if(-not $process.WaitForExit(30000)){$process.Kill();$process.WaitForExit();throw 'regsvr32 timed out.'}
        if($process.ExitCode -ne 0){throw "regsvr32 failed: $($process.ExitCode)"}
      }finally{$process.Dispose()}
    }
    Environment={[Environment]::GetEnvironmentVariable('WPCE_OUTPUT_DIR','Machine')}
    SnapshotEnvironment={
      $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\Environment')
      try{
        $value=$key.GetValue('WPCE_OUTPUT_DIR',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        return @{value=$value;kind=$(if($null -ne $value){$key.GetValueKind('WPCE_OUTPUT_DIR').ToString()}else{'String'})}
      }finally{$key.Dispose()}
    }
    RestoreEnvironment={param($old)
      $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\Environment',$true)
      try{if($null -eq $old.value){$key.DeleteValue('WPCE_OUTPUT_DIR',$false)}else{$key.SetValue('WPCE_OUTPUT_DIR',$old.value,[Microsoft.Win32.RegistryValueKind]$old.kind)}}
      finally{$key.Dispose()}
    }
    SetEnvironment={param($value) [Environment]::SetEnvironmentVariable('WPCE_OUTPUT_DIR',$value,'Machine')}
    PrepareOutput={param($p) $null=[IO.Directory]::CreateDirectory($p)}
    Lock={param($dir) $null=[IO.Directory]::CreateDirectory($dir); [IO.File]::Open((Join-Path $dir 'install.lock'),'OpenOrCreate','ReadWrite','None')}
    Pending={param($dir)
      $root=Join-Path $dir '_install-transactions'
      if([IO.Directory]::Exists($root)) {
        foreach($file in Get-ChildItem -LiteralPath $root -Filter '*.json') {
          $record=Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
          if($record.status -notin @('committed','rolled_back')) {$file.FullName}
        }
      }
    }
    ServiceState={$service=Get-Service -Name Spooler -ErrorAction Stop;try{$service.Refresh();return $service.Status.ToString()}finally{$service.Dispose()}}
    StopService={$service=Get-Service -Name Spooler -ErrorAction Stop;try{$service.Stop()}finally{$service.Dispose()}}
    StartService={$service=Get-Service -Name Spooler -ErrorAction Stop;try{$service.Refresh();if($service.Status -ne 'Running'){$service.Start()}}finally{$service.Dispose()}}
    WaitService={param($state,$seconds)
      $service=Get-Service -Name Spooler -ErrorAction Stop
      try{$service.WaitForStatus([ServiceProcess.ServiceControllerStatus]$state,[TimeSpan]::FromSeconds($seconds));$service.Refresh();if($service.Status.ToString() -ne $state){throw 'Unexpected final service state.'}}
      finally{$service.Dispose()}
    }
  }
}
Export-ModuleMember -Function Write-WpceAtomicBytes,Get-WpcePipelineXml,New-WpceInstallOperations
