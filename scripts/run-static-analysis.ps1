param(
  [string]$SourceRoot = "."
)

$ErrorActionPreference = "Stop"

$cppcheck = Get-Command cppcheck -ErrorAction SilentlyContinue
if ($cppcheck) {
  cppcheck `
    --enable=warning,style,performance,portability `
    --std=c++20 `
    --inline-suppr `
    --suppress=missingIncludeSystem `
    "$SourceRoot\src" "$SourceRoot\tests"
} else {
  Write-Warning "cppcheck not found; skipping cppcheck."
}

$clangTidy = Get-Command clang-tidy -ErrorAction SilentlyContinue
if ($clangTidy) {
  $compileCommands = Join-Path $SourceRoot "build\ninja-msvc\compile_commands.json"
  if (Test-Path $compileCommands) {
    Get-ChildItem "$SourceRoot\src" -Recurse -Include *.cpp | ForEach-Object {
      clang-tidy $_.FullName -p (Split-Path $compileCommands)
    }
  } else {
    Write-Warning "compile_commands.json not found; configure with the ninja-msvc preset before clang-tidy."
  }
} else {
  Write-Warning "clang-tidy not found; skipping clang-tidy."
}

