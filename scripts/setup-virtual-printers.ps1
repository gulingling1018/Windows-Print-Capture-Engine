param(
  [string]$SimPrinterName = "WPCE Sim Printer",
  [string]$FaultPrinterName = "WPCE Fault Printer",
  [string]$SimPortName = "nul:",
  [string]$FaultPortName = "WPCE_FAULT_192.0.2.1",
  [string]$FaultHostAddress = "192.0.2.1",
  [int]$FaultPortNumber = 9100,
  [string]$DriverName = "Generic / Text Only"
)

$ErrorActionPreference = "Stop"

function Ensure-Spooler {
  $service = Get-Service -Name Spooler -ErrorAction Stop
  if ($service.Status -ne "Running") {
    Start-Service -Name Spooler
    $service.WaitForStatus("Running", "00:00:20")
  }
}

function Ensure-PrinterDriver {
  param([string]$Name)

  if (-not (Get-PrinterDriver -Name $Name -ErrorAction SilentlyContinue)) {
    Add-PrinterDriver -Name $Name
  }
}

function Ensure-LocalPort {
  param([string]$Name)

  if (-not (Get-PrinterPort -Name $Name -ErrorAction SilentlyContinue)) {
    Add-PrinterPort -Name $Name
  }
}

function Ensure-TcpPort {
  param(
    [string]$Name,
    [string]$HostAddress,
    [int]$PortNumber
  )

  if (-not (Get-PrinterPort -Name $Name -ErrorAction SilentlyContinue)) {
    Add-PrinterPort -Name $Name -PrinterHostAddress $HostAddress -PortNumber $PortNumber
  }
}

function Ensure-Printer {
  param(
    [string]$Name,
    [string]$Driver,
    [string]$Port
  )

  if (-not (Get-Printer -Name $Name -ErrorAction SilentlyContinue)) {
    Add-Printer -Name $Name -DriverName $Driver -PortName $Port
  }
}

Ensure-Spooler
Ensure-PrinterDriver -Name $DriverName
Ensure-LocalPort -Name $SimPortName
Ensure-Printer -Name $SimPrinterName -Driver $DriverName -Port $SimPortName
$simPrinter = Get-Printer -Name $SimPrinterName
if ($simPrinter.PortName -ne $SimPortName) {
  Set-Printer -Name $SimPrinterName -PortName $SimPortName
}
Ensure-TcpPort -Name $FaultPortName -HostAddress $FaultHostAddress -PortNumber $FaultPortNumber
Ensure-Printer -Name $FaultPrinterName -Driver $DriverName -Port $FaultPortName
$faultPrinter = Get-Printer -Name $FaultPrinterName
if ($faultPrinter.PortName -ne $FaultPortName) {
  Set-Printer -Name $FaultPrinterName -PortName $FaultPortName
}

Get-Printer -Name $SimPrinterName,$FaultPrinterName |
  Select-Object Name, DriverName, PortName, PrinterStatus
