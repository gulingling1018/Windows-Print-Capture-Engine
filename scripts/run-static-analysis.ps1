#requires -Version 5.1
param(
  [string]$SourceRoot=(Join-Path $PSScriptRoot '..'),
  [string]$BuildDirectory,
  [ValidateSet('Strict','Local')][string]$Mode='Strict',
  [ValidateSet('Debug','Release')][string]$Configuration='Debug',
  [string[]]$Files=@(),
  [string]$CppcheckPath='cppcheck',
  [string]$ClangTidyPath='clang-tidy',
  [string]$ReportPath
)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/Wpce.Process.psm1') -Force
$report=[ordered]@{schema_version=1;mode=$Mode;configuration=$Configuration;status='failed';
  release_gate=$false;files=@();executed=@();skipped=@();error=''}
try {
  $SourceRoot=[IO.Path]::GetFullPath($SourceRoot)
  $cpp=Get-Command $CppcheckPath -ErrorAction SilentlyContinue
  $tidy=Get-Command $ClangTidyPath -ErrorAction SilentlyContinue
  if ($Mode -eq 'Strict') {
    if ($Files.Count) { throw 'Strict mode cannot narrow the translation-unit set.' }
    if (-not $BuildDirectory) { throw 'Strict mode requires the actual Qt/MSVC build directory.' }
    $BuildDirectory=[IO.Path]::GetFullPath($BuildDirectory)
    $cache=Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Raw
    $cacheRoot=[regex]::Match($cache,'(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$').Groups[1].Value.Trim()
    if ([IO.Path]::GetFullPath($cacheRoot) -ne $SourceRoot -or
        $cache -notmatch '(?m)^WPCE_BUILD_UI:BOOL=ON\s*$' -or
        $cache -notmatch '(?m)^WPCE_BUILD_TESTS:BOOL=ON\s*$') {
      throw 'Compilation database must come from this source tree with Qt UI and tests enabled.'
    }
    $database=Join-Path $BuildDirectory 'compile_commands.json'
    $all=Get-Content -LiteralPath $database -Raw | ConvertFrom-Json
    $selected=@($all | Where-Object {
      $line=if($_.PSObject.Properties['command']) {$_.command} else {$_.arguments -join ' '}
      $line -match ('CMAKE_INTDIR=[\\"]*'+$Configuration+'(?:[\\"]|\s)') -or
        ($cache -match ('(?m)^CMAKE_BUILD_TYPE:STRING='+$Configuration+'\s*$'))
    })
    if (-not $selected.Count) { throw 'No compile commands match the selected configuration.' }
    $units=@{}; $filtered=@()
    foreach ($entry in $selected) {
      $path=$entry.file
      if (-not [IO.Path]::IsPathRooted($path)) { $path=Join-Path $entry.directory $path }
      $path=[IO.Path]::GetFullPath($path)
      if (-not $path.StartsWith($SourceRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {continue}
      if ($path -like '*\_deps\*' -or $path -like '*\build\*' -or $path -like '*\artifacts\*') {continue}
      $line=if($entry.PSObject.Properties['command']) {$entry.command} else {$entry.arguments -join ' '}
      if ($line -notmatch 'cl(?:\.exe)?[" ]' -or $line -notmatch '[-/]std:c\+\+20' -or $line -notmatch 'WIN32') {
        throw "Non-MSVC/Win32/C++20 compile entry: $path"
      }
      if($path -match '[\\/]src[\\/](ui|management|billing)[\\/]' -and $line -notmatch 'QT_CORE_LIB') {throw "Qt compile definitions missing: $path"}
      if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing translation unit: $path" }
      $entry.file=$path
      $units[$path]=$true
      $filtered+=$entry
    }
    foreach($required in @('src/ui/main_window.cpp','src/billing/billing_service.cpp','src/filter/filter_pipeline.cpp','tests/billing_service_tests.cpp')) {
      if(-not $units.ContainsKey([IO.Path]::GetFullPath((Join-Path $SourceRoot $required)))) {throw "Required Qt/filter/test target absent: $required"}
    }
    $replyRoot=Join-Path $BuildDirectory '.cmake/api/v1/reply'
    $indexes=@(Get-ChildItem -LiteralPath $replyRoot -Filter 'index-*.json' | Sort-Object LastWriteTimeUtc -Descending)
    if(-not $indexes.Count){throw 'CMake File API target model is missing; reconfigure with compile-command export enabled.'}
    $index=Get-Content -LiteralPath $indexes[0].FullName -Raw | ConvertFrom-Json
    $modelFile=$index.reply.'codemodel-v2'.jsonFile
    $model=Get-Content -LiteralPath (Join-Path $replyRoot $modelFile) -Raw | ConvertFrom-Json
    $models=@($model.configurations | Where-Object {$_.name -eq $Configuration})
    if($models.Count -ne 1){throw 'Target model does not match the selected configuration.'}
    $expected=@{}
    foreach($target in $models[0].targets){
      $detail=Get-Content -LiteralPath (Join-Path $replyRoot $target.jsonFile) -Raw | ConvertFrom-Json
      foreach($source in $detail.sources){
        if(($source.path -notmatch '\.cpp$') -or ($source.PSObject.Properties['isGenerated'] -and $source.isGenerated)){continue}
        $path=$source.path
        if(-not [IO.Path]::IsPathRooted($path)){$path=Join-Path $model.paths.source $path}
        $path=[IO.Path]::GetFullPath($path)
        if(-not $path.StartsWith($SourceRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){continue}
        if($path -like '*\build\*' -or $path -like '*\_deps\*' -or $path -like '*\artifacts\*'){continue}
        $expected[$path]=$true
        if(-not $units.ContainsKey($path)){throw "Target translation unit absent from compile commands: $path"}
      }
    }
    if($expected.Count -ne $units.Count){throw 'Compile commands and actual target source sets differ.'}
    if (-not $filtered.Count) { throw 'No project translation units found.' }
    $report.files=@($filtered | ForEach-Object {$_.file})
    if (-not $cpp) { throw "cppcheck not found: $CppcheckPath" }
    if (-not $tidy) { throw "clang-tidy not found: $ClangTidyPath" }
    $analysisDir=Join-Path $BuildDirectory "_static-analysis/$Configuration"
    $null=New-Item -ItemType Directory -Path $analysisDir -Force
    $filtered | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $analysisDir 'compile_commands.json') -Encoding UTF8
    $report.files=@($filtered | ForEach-Object {$_.file})
    $project="--project=$(Join-Path $analysisDir 'compile_commands.json')"
    Invoke-WpceNativeStep 'cppcheck' { & $cpp.Source $project --enable=warning,performance,portability --error-exitcode=1 --inline-suppr }
    $report.executed+= 'cppcheck'
    $unitIndex=0
    foreach ($entry in $filtered) {
      # Same source may have different defines in UI, filter and test targets.
      # Give clang-tidy exactly one command so each actual variant is checked.
      $unitDirectory=Join-Path $analysisDir ('unit-'+$unitIndex);$unitIndex++
      $null=New-Item -ItemType Directory -Path $unitDirectory -Force
      ConvertTo-Json -InputObject @($entry) -Depth 20 | Set-Content -LiteralPath (Join-Path $unitDirectory 'compile_commands.json') -Encoding UTF8
      Invoke-WpceNativeStep 'clang-tidy' { & $tidy.Source $entry.file "-p=$unitDirectory" '--warnings-as-errors=*' }
    }
    $report.executed+= 'clang-tidy'
    $report.release_gate=$true
  } else {
    if (-not $cpp) { throw "cppcheck not found: $CppcheckPath" }
    if (-not $Files.Count) { throw 'Local mode requires an explicit file list.' }
    $paths=@($Files | ForEach-Object {
      $candidate=if([IO.Path]::IsPathRooted($_)) {$_} else {Join-Path $SourceRoot $_}
      if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { throw "Missing local source: $candidate" }
      [IO.Path]::GetFullPath($candidate)
    })
    $report.files=$paths
    Invoke-WpceNativeStep 'local cppcheck' {
      & $cpp.Source --enable=warning,performance,portability --error-exitcode=1 --std=c++20 --platform=win64 --inline-suppr --suppress=missingIncludeSystem '-DQ_OBJECT=' '-DQ_DECLARE_METATYPE(x)=' '-Dslots=' '-Dsignals=public' '-Demit=' "-I$SourceRoot/src" @paths
    }
    $report.executed+='cppcheck-local'; $report.skipped+='clang-tidy/full compilation database/release gate'
    Write-Warning 'LOCAL ONLY: selected files, simplified defines; this is not a release gate pass.'
  }
  $report.status='passed'
} catch { $report.error=$_.Exception.Message; Write-Error $_ -ErrorAction Continue }
finally {
  if ($ReportPath) { $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding UTF8 }
}
if ($report.status -ne 'passed') {exit 1}

