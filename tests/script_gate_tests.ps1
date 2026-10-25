#requires -Version 5.1
param([string]$Case='All',[switch]$Inner,[string]$Fixture)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildScript=Join-Path $repo 'scripts/build-msvc.ps1'
$staticScript=Join-Path $repo 'scripts/run-static-analysis.ps1'
if($Inner){
  function global:cmake {
    Add-Content -LiteralPath (Join-Path $Fixture 'trace.txt') -Value ('cmake '+($args -join '|'))
    if(@([Environment]::GetEnvironmentVariables('Process').Keys | Where-Object {$_ -ieq 'LANG'}).Count){throw 'LANG variants were not normalized'}
    $global:LASTEXITCODE=if(($Case -eq 'ConfigureFailure' -and $args -notcontains '--build') -or ($Case -eq 'BuildFailure' -and $args -contains '--build')){7}else{0}
  }
  function global:ctest {
    Add-Content -LiteralPath (Join-Path $Fixture 'trace.txt') -Value ('ctest '+($args -join '|'))
    $global:LASTEXITCODE=if($Case -eq 'TestFailure'){8}else{0}
  }
  switch($Case){
    'QtDebug' { & $buildScript -WithQt;exit $LASTEXITCODE }
    'QtRelease' { & $buildScript -WithQt -Configuration Release;exit $LASTEXITCODE }
    'PresetMismatch' { & $buildScript -WithQt -BuildPreset vs2026-debug;exit $LASTEXITCODE }
    'DirectoryOverride' { & $buildScript -WithQt -ConfigureArguments @('-Bbad');exit $LASTEXITCODE }
    {$_ -in @('ConfigureFailure','BuildFailure','TestFailure')} { & $buildScript -WithQt;exit $LASTEXITCODE }
  }
  $parameters=@{SourceRoot=(Join-Path $Fixture 'source');BuildDirectory=(Join-Path $Fixture 'build');
    CppcheckPath=(Join-Path $Fixture 'cppcheck.cmd');ClangTidyPath=(Join-Path $Fixture 'clang-tidy.cmd');ReportPath=(Join-Path $Fixture 'report.json')}
  switch($Case){
    'MissingCppcheck' {$parameters.CppcheckPath=Join-Path $Fixture 'absent.exe'}
    'MissingClangTidy' {$parameters.ClangTidyPath=Join-Path $Fixture 'absent.exe'}
    'LocalDefect' {$parameters.Mode='Local';$parameters.Files=@('src/defect.cpp');$parameters.CppcheckPath=(Get-Command cppcheck -ErrorAction Stop).Source}
    'LocalClean' {$parameters.Mode='Local';$parameters.Files=@('src/clean.cpp')}
  }
  & $staticScript @parameters
  exit $LASTEXITCODE
}
$cases=@('QtDebug','QtRelease','PresetMismatch','DirectoryOverride','ConfigureFailure','BuildFailure','TestFailure',
  'MissingCppcheck','MissingClangTidy','MissingDatabase','WrongSource','WrongConfiguration','MissingTranslationUnit',
  'CppcheckFailure','ClangTidyFailure','StrictSuccess','LocalDefect','LocalClean','MissingFileApi','WrongQtDefinitions')
$selected=@(if($Case -eq 'All'){$cases}else{$Case})
foreach($name in $selected){
  if($name -notin $cases){throw "Unknown case: $name"}
  $root=Join-Path ([IO.Path]::GetTempPath()) ('wpce-gates-'+[guid]::NewGuid().ToString('N'))
  $null=New-Item -ItemType Directory -Path $root
  try{
    $source=Join-Path $root 'source';$build=Join-Path $root 'build'
    foreach($directory in @('src','tests','tools')){$null=New-Item -ItemType Directory -Path (Join-Path $source $directory)}
    $null=New-Item -ItemType Directory -Path $build
    [IO.File]::WriteAllText((Join-Path $source 'src/defect.cpp'),'int bad(){int zero=0;return 5/zero;}')
    [IO.File]::WriteAllText((Join-Path $source 'src/clean.cpp'),'int good(){return 5;}')
    $representatives=@('src/ui/main_window.cpp','src/billing/billing_service.cpp','src/filter/filter_pipeline.cpp','tests/billing_service_tests.cpp')
    foreach($relative in $representatives){
      $path=Join-Path $source $relative;$null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path));[IO.File]::WriteAllText($path,'int fixture(){return 1;}')
    }
    $cacheSource=if($name -eq 'WrongSource'){'C:/wrong'}else{$source}
    "CMAKE_HOME_DIRECTORY:INTERNAL=$cacheSource`nWPCE_BUILD_UI:BOOL=ON`nWPCE_BUILD_TESTS:BOOL=ON`n" | Set-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')
    $configuration=if($name -eq 'WrongConfiguration'){'Release'}else{'Debug'}
    $files=@('src/defect.cpp','src/clean.cpp')+$representatives
    if($name -eq 'MissingTranslationUnit'){$files=@('src/clean.cpp')+$representatives}
    $commands=@($files | ForEach-Object {@{directory=$build;file=(Join-Path $source $_);command=('C:/cl.exe -std:c++20 /DWIN32 /DQT_CORE_LIB /DCMAKE_INTDIR=\"'+$configuration+'\" /c '+$_)}})
    if($name -eq 'WrongQtDefinitions'){foreach($command in $commands){$command.command=$command.command.Replace('/DQT_CORE_LIB','')}}
    if($name -ne 'MissingDatabase'){$commands | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $build 'compile_commands.json')}
    if($name -ne 'MissingFileApi'){
      $reply=Join-Path $build '.cmake/api/v1/reply';$null=[IO.Directory]::CreateDirectory($reply)
      @{reply=@{'codemodel-v2'=@{jsonFile='codemodel.json'}}}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $reply 'index-fixture.json')
      @{paths=@{source=$source};configurations=@(@{name='Debug';targets=@(@{jsonFile='target.json'})})}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $reply 'codemodel.json')
      $modelSources=@((@('src/defect.cpp','src/clean.cpp')+$representatives)|ForEach-Object {@{path=$_}})
      $modelSources+=@(@{path='src/unused.h'},@{path='build/generated.cpp';isGenerated=$true})
      @{sources=$modelSources}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $reply 'target.json')
    }
    $cppCode=if($name -eq 'CppcheckFailure'){11}else{0}
    $tidyCode=if($name -eq 'ClangTidyFailure'){12}else{0}
    "@echo off`r`nexit /b $cppCode" | Set-Content -LiteralPath (Join-Path $root 'cppcheck.cmd') -Encoding ASCII
    "@echo off`r`nexit /b $tidyCode" | Set-Content -LiteralPath (Join-Path $root 'clang-tidy.cmd') -Encoding ASCII
    $ErrorActionPreference='Continue'
    try {
      $output=& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Case $name -Inner -Fixture $root 2>&1
      $code=$LASTEXITCODE
    } finally {$ErrorActionPreference='Stop'}
    $success=$name -in @('QtDebug','QtRelease','StrictSuccess','LocalClean')
    if(($code -eq 0) -ne $success){throw "Unexpected exit code $code for $name : $output"}
    $tracePath=Join-Path $root 'trace.txt'
    $trace=if(Test-Path -LiteralPath $tracePath){@(Get-Content -LiteralPath $tracePath)}else{@()}
    $expected=switch($name){'QtDebug'{3};'QtRelease'{3};'ConfigureFailure'{1};'BuildFailure'{2};'TestFailure'{3};default{0}}
    if($trace.Count -ne $expected){throw "Unexpected stage count for $name : $trace"}
    if($success -and $name -like 'Qt*'){
      $preset=if($name -eq 'QtDebug'){'vs2026-qt-debug'}else{'vs2026-qt-release'}
      if($trace[0] -notmatch 'vs2026-x64-qt' -or $trace[1] -notmatch $preset -or $trace[2] -notmatch $preset){throw 'Qt preset binding was lost'}
    }
    if($name -in @('StrictSuccess','LocalClean')){
      $report=Get-Content -LiteralPath (Join-Path $root 'report.json') -Raw | ConvertFrom-Json
      if($report.release_gate -ne ($name -eq 'StrictSuccess')){throw 'Local result incorrectly advertised release gate pass'}
    }
    $expectedError=switch($name){'CppcheckFailure'{'cppcheck failed with exit code 11'};'ClangTidyFailure'{'clang-tidy failed with exit code 12'};'LocalDefect'{'local cppcheck failed'};default{''}}
    if($expectedError){
      $report=Get-Content -LiteralPath (Join-Path $root 'report.json') -Raw | ConvertFrom-Json
      if($report.error -notlike "*$expectedError*"){throw "Expected analyzer failure was not reached: $($report.error)"}
    }
    Write-Host "PASS: $name"
  }finally{
    $absolute=[IO.Path]::GetFullPath($root)
    $tempPrefix=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if(-not $absolute.StartsWith($tempPrefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe test cleanup target'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
  }
}
Write-Host "Script gate tests passed: $($selected.Count)"
