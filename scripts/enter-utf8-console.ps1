$ErrorActionPreference = "Stop"

chcp 65001 | Out-Null

$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8NoBom
[Console]::OutputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

Write-Host "PowerShell console encoding is now UTF-8."
Write-Host "Tip: read WPCE JSON with: Get-Content -Raw -Encoding UTF8 <path>"
