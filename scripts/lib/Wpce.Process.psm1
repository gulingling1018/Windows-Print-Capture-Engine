#requires -Version 5.1
Set-StrictMode -Version Latest

function Invoke-WpceNativeStep {
  param([Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][scriptblock]$Command)
  $global:LASTEXITCODE = 0
  & $Command
  $code = $LASTEXITCODE
  if ($code -ne 0) { throw "$Name failed with exit code $code." }
}

function Reset-WpceProcessLanguage {
  # Only the child process environment is normalized; no user/machine changes.
  $keys = @([Environment]::GetEnvironmentVariables('Process').Keys |
    ForEach-Object { [string]$_ } | Where-Object { $_ -ieq 'LANG' })
  foreach ($key in $keys) { [Environment]::SetEnvironmentVariable($key, $null, 'Process') }
  Remove-Item Env:LANG -ErrorAction SilentlyContinue
  Remove-Item Env:lang -ErrorAction SilentlyContinue
}

Export-ModuleMember -Function Invoke-WpceNativeStep,Reset-WpceProcessLanguage
