#requires -Version 5.1 -RunAsAdministrator
<#
.SYNOPSIS
  Install WPCE XPS Capture Filter for print parameter interception.

.DESCRIPTION
  Windows 10/11 uses XPS-based print pipelines.  This script:
    1. Copies wpce_xps_filter.dll to a permanent location
    2. Registers the COM server (CLSID registration)
    3. Sets environment variable WPCE_OUTPUT_DIR for the spooler
    4. Discovers all local printers and identifies v4 (XPSDrv) drivers
    5. Injects the WPCE pass-through filter into each v4 printer's
       FilterPipelineConfig.xml as the FIRST filter (before any
       rendering filter)

  After installation and spooler restart, every XPS print job
  processed by v4 printers on this machine will have its PrintTicket
  XML captured to the output directory — providing real-time per-job
  settings: colour, duplex, paper size, N-up, copies, collation,
  orientation, media type, and print quality.

  -- Compatibility --------------------------------------------------
  Works with: HP, Canon, Epson, Brother, Kyocera, Ricoh, Sharp,
              Xerox, Konica Minolta, Toshiba, Dell, Lexmark, and
              any other XPSDrv (v4) printer driver.
  v3 (GDI) drivers: XPS pipeline not used; spooler watcher provides
              lifecycle events; PrintTicket metadata shows "NotReported".

.PARAMETER OutputDir
  Directory where captured metadata is written.
  Default: ./data relative to script directory.

.PARAMETER DryRun
  Report what would be changed without making changes.
#>

param(
  [string]$OutputDir = "$PSScriptRoot/../data",
  [switch]$DryRun = $false
)

$ErrorActionPreference = 'Stop'
$repoRoot = Resolve-Path "$PSScriptRoot/.."
$dllSource = "$repoRoot/build/vs2026-x64-qt/src/Release/wpce_xps_filter.dll"
$installDir = "${env:ProgramFiles}\WPCE"
$filterDll = "$installDir\wpce_xps_filter.dll"
$filterClsid = "{B1C8E3F0-2D4A-5B6C-7D8E-9F0A1B2C3D4E}"
$pipelineNs = "http://schemas.microsoft.com/windows/2005/06/printing/FilterPipeline"

Write-Host "=== WPCE XPS Filter Installation ===" -ForegroundColor Cyan

# =====================================================================
# 1. Copy DLL
# =====================================================================
Write-Host "`n[1/5] Installing filter DLL ..." -ForegroundColor Yellow

if (-not (Test-Path $dllSource)) {
  Write-Host "ERROR: $dllSource not found. Build Release first." -ForegroundColor Red
  Write-Host "  cmake --build build/vs2026-x64-qt --config Release"
  exit 1
}

if (-not $DryRun) {
  $null = New-Item -ItemType Directory -Force -Path $installDir
  Copy-Item $dllSource $filterDll -Force
}
Write-Host "  DLL: $filterDll"

# =====================================================================
# 2. Register COM server
# =====================================================================
Write-Host "`n[2/5] Registering COM server ..." -ForegroundColor Yellow

if (-not $DryRun) {
  try {
    $regResult = Start-Process regsvr32.exe -ArgumentList "/s", "`"$filterDll`"" `
      -Wait -NoNewWindow -PassThru
    if ($regResult.ExitCode -eq 0) {
      Write-Host "  COM registration OK"
    } else {
      Write-Host "  WARNING: regsvr32 exit code $($regResult.ExitCode)" -ForegroundColor Yellow
      Write-Host "  Attempting manual DllRegisterServer..."
      Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class NativeMethods {
    [DllImport("$filterDll", EntryPoint="DllRegisterServer", CallingConvention=CallingConvention.StdCall)]
    public static extern int DllRegisterServer();
}
"@
      $hr = [NativeMethods]::DllRegisterServer()
      if ($hr -eq 0) { Write-Host "  Manual registration OK" }
      else { Write-Host "  WARNING: returned 0x$($hr.ToString('X8'))" -ForegroundColor Yellow }
    }
  } catch {
    Write-Host "  WARNING: COM registration failed: $_" -ForegroundColor Yellow
  }
}

# =====================================================================
# 3. Set environment variable
# =====================================================================
Write-Host "`n[3/5] Setting spooler environment ..." -ForegroundColor Yellow

$outAbs = (Resolve-Path $OutputDir -ErrorAction SilentlyContinue)?.Path
if (-not $outAbs) {
  $null = New-Item -ItemType Directory -Force -Path $OutputDir
  $outAbs = (Resolve-Path $OutputDir).Path
}

if (-not $DryRun) {
  [Environment]::SetEnvironmentVariable("WPCE_OUTPUT_DIR", $outAbs, "Machine")
}
Write-Host "  WPCE_OUTPUT_DIR = $outAbs (Machine-level)"

# =====================================================================
# 4. Discover v4 (XPSDrv) drivers
# =====================================================================
Write-Host "`n[4/5] Discovering v4 XPS drivers ..." -ForegroundColor Yellow

$driverStore = "$env:SystemRoot\System32\DriverStore\FileRepository"
$v4Printers = @{}

$allPrinters = Get-Printer | Where-Object { $_.Name -ne $null }
Write-Host "  Total printers on system: $($allPrinters.Count)"

foreach ($printer in $allPrinters) {
  $driverName = $printer.DriverName
  if (-not $driverName) { continue }

  # Normalize driver name for filesystem search.
  $safeName = ($driverName -replace '[\\/:*?"<>|\s]', '_')

  # Find matching INF files in the driver store.
  $infFiles = Get-ChildItem -Path $driverStore -Recurse -Filter "*.inf" `
    -ErrorAction SilentlyContinue | Where-Object {
      $_.Name -like "*$driverName*" -or $_.Name -like "*$safeName*" -or
      $_.DirectoryName -like "*$safeName*"
    }

  $isV4 = $false
  $driverDir = $null
  $configPath = $null

  foreach ($inf in $infFiles) {
    $infText = Get-Content $inf.FullName -Raw -ErrorAction SilentlyContinue
    if (-not $infText) { continue }

    # v4 driver markers in INF:
    #  - "XPSDrvFilter" section: declares XPS pipeline filters
    #  - "FilterPipelineConfig": references external pipeline config
    #  - "DriverConfig" without "CoreDriverSections": v4 pattern
    #  - "v4" in the Manufacturer section
    if ($infText -match 'XPSDrvFilter|FilterPipelineConfig|^\s*DriverConfig\s*=') {
      $isV4 = $true
      $driverDir = $inf.DirectoryName
      break
    }
    # Secondary: v4 INF uses [DriverConfig] instead of [CoreDriverSections]
    if ($infText -match '\[DriverConfig\]' -and $infText -notmatch '\[CoreDriverSections\]') {
      $isV4 = $true
      $driverDir = $inf.DirectoryName
      break
    }
  }

  # Look for FilterPipelineConfig.xml.
  if ($driverDir) {
    $xmlPath = Join-Path $driverDir "FilterPipelineConfig.xml"
    if (Test-Path $xmlPath) { $configPath = $xmlPath }
  }

  $v4Printers[$printer.Name] = @{
    Printer     = $printer
    IsV4        = $isV4
    DriverDir   = $driverDir
    ConfigPath  = $configPath
  }

  $tag = if ($isV4) { "  [v4 XPSDrv]" } else { "  [v3 GDI — XPS not used]" }
  $configTag = if ($configPath) { " config.xml found" } else { "" }
  Write-Host "  $($printer.Name) — $($printer.DriverName)$tag$configTag"
}

# =====================================================================
# 5. Inject filter into v4 pipeline configs
# =====================================================================
Write-Host "`n[5/5] Injecting WPCE filter into v4 pipelines ..." -ForegroundColor Yellow

# Pass-through capture filter XML fragment.
# Placed as the FIRST child of <Filters> so the raw (unprocessed)
# PrintTicket is captured before any downstream rendering filter
# can alter or strip it.
$filterXml = @"
  <Filter xmlns="$pipelineNs"
          dll="$filterDll" clsid="$filterClsid" name="WPCE Capture">
    <Input>
      <InputPart type="IFixedDocumentSequence" />
      <InputPart type="IFixedDocument" />
      <InputPart type="IFixedPage" />
    </Input>
    <Output>
      <OutputPart type="IFixedDocumentSequence" />
      <OutputPart type="IFixedDocument" />
      <OutputPart type="IFixedPage" />
    </Output>
  </Filter>
"@

$configured = 0
$skipped = 0

foreach ($name in $v4Printers.Keys) {
  $info = $v4Printers[$name]

  if (-not $info.IsV4) {
    Write-Host "  SKIP  $name — v3 GDI driver (no XPS pipeline)"
    ++$skipped
    continue
  }

  if (-not $info.ConfigPath) {
    Write-Host "  SKIP  $name — v4 driver but no FilterPipelineConfig.xml`n" +
               "         (inbox/IPP class driver; may use compiled pipeline)"
    ++$skipped
    continue
  }

  Write-Host "  CONF  $name -> $($info.ConfigPath)"

  if ($DryRun) {
    Write-Host "         (dry-run) would inject WPCE capture filter"
    ++$configured
    continue
  }

  try {
    $configXml = [xml](Get-Content $info.ConfigPath -Raw -Encoding UTF8)

    # Namespace-aware check for our CLSID already present.
    $nsmgr = New-Object System.Xml.XmlNamespaceManager($configXml.NameTable)
    $nsmgr.AddNamespace("fp", $pipelineNs)
    $alreadyThere = $configXml.SelectSingleNode(
      "//fp:Filter[@clsid='$filterClsid']", $nsmgr)
    if ($alreadyThere) {
      Write-Host "         Already configured — skipped"
      ++$configured
      continue
    }

    $filtersNode = $configXml.SelectSingleNode("//fp:Filters", $nsmgr)
    if (-not $filtersNode) {
      Write-Host "         WARNING: <Filters> element not found in config"
      ++$skipped
      continue
    }

    # Backup before modifying.
    $backupPath = $info.ConfigPath + ".wpce-backup"
    if (-not (Test-Path $backupPath)) {
      Copy-Item $info.ConfigPath $backupPath
      Write-Host "         Backup: $backupPath"
    }

    # Inject as first child of <Filters>.
    $frag = $configXml.CreateDocumentFragment()
    $frag.InnerXml = $filterXml
    $filtersNode.PrependChild($frag) | Out-Null
    $configXml.Save($info.ConfigPath)

    Write-Host "         OK — filter injected as first pipeline stage"
    ++$configured
  }
  catch {
    Write-Host "         ERROR: $_" -ForegroundColor Red
    ++$skipped
  }
}

# =====================================================================
# Summary
# =====================================================================
Write-Host @"

==========================================================
  WPCE XPS Filter Installation Complete
  --------------------------------------------------------
  DLL:       $filterDll
  CLSID:     $filterClsid
  Output:    $outAbs
  v4 drivers configured: $configured
  Skipped / v3:          $skipped

  NEXT STEPS:
    1. Restart the Print Spooler service:
         net stop spooler && net start spooler
    2. Launch the WPCE GUI and print a test document
    3. Verify PrintTicket metadata appears in the
       detail panel (参数来源: XPS PrintTicket)

  ROLLBACK:
    Copy .wpce-backup files back to their original
    names under:
      $driverStore
    Then restart the spooler.

  TROUBLESHOOTING:
    - Open regedit -> HKLM\CLSID\$filterClsid
      Verify InProcServer32 points to $filterDll
    - Use DebugView (Sysinternals) to watch for
      [WPCE] messages from the filter DLL
    - Run: fltmc instances   (check filter attach)
==========================================================

"@ -ForegroundColor Green
