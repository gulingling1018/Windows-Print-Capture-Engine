param(
  [string]$VisualStudioRoot = "D:\download\Microsoft Visual Studio18_Community"
)

$ErrorActionPreference = "Stop"

function Show-Command {
  param([string]$Name)

  $command = Get-Command $Name -ErrorAction SilentlyContinue
  if ($command) {
    "{0,-16} {1}" -f $Name, $command.Source
  } else {
    "{0,-16} <missing>" -f $Name
  }
}

function Show-PathStatus {
  param(
    [string]$Name,
    [string]$Path
  )

  if (Test-Path $Path) {
    "{0,-16} {1}" -f $Name, $Path
  } else {
    "{0,-16} <missing: {1}>" -f $Name, $Path
  }
}

"WPCE toolchain check"
"===================="
""
"Commands:"
Show-Command "git"
Show-Command "cmake"
Show-Command "ninja"
Show-Command "cl"
Show-Command "msbuild"
Show-Command "clang-format"
Show-Command "clang-tidy"
Show-Command "cppcheck"
""

"Visual Studio:"
$vsDevCmd = Join-Path $VisualStudioRoot "Common7\Tools\VsDevCmd.bat"
Show-PathStatus "VsDevCmd" $vsDevCmd

$msBuild = Join-Path $VisualStudioRoot "MSBuild\Current\Bin\MSBuild.exe"
Show-PathStatus "MSBuild" $msBuild

$msvcRoot = Join-Path $VisualStudioRoot "VC\Tools\MSVC"
if (Test-Path $msvcRoot) {
  Get-ChildItem $msvcRoot -Directory | ForEach-Object {
    "{0,-16} {1}" -f "MSVC", $_.FullName
  }
} else {
  "{0,-16} <missing: {1}>" -f "MSVC", $msvcRoot
}
""

"Windows SDK / WDK:"
$kitRoot = "C:\Program Files (x86)\Windows Kits\10"
Show-PathStatus "WindowsKits" $kitRoot

$latestKitBin = Join-Path $kitRoot "bin\10.0.26100.0\x64"
Show-PathStatus "signtool" (Join-Path $latestKitBin "signtool.exe")
Show-PathStatus "inf2cat" (Join-Path $latestKitBin "inf2cat.exe")
Show-PathStatus "stampinf" (Join-Path $latestKitBin "stampinf.exe")
Show-PathStatus "tracefmt" (Join-Path $latestKitBin "tracefmt.exe")
""

"Printers:"
if (Get-Command Get-Printer -ErrorAction SilentlyContinue) {
  Get-Printer | Select-Object Name, DriverName, PortName | Format-Table -AutoSize
} else {
  "Get-Printer is not available in this shell."
}

