# tail.ps1 - live view of the last dialog lines while you play.
#
#   powershell -File tools\tail.ps1 -Path "...\Trails of Cold Steel\atmt_latest.txt"
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$LastLines = 12,
    [int]$IntervalMs = 700
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Path)) {
    Write-Host "waiting for $Path to appear..."
    while (-not (Test-Path $Path)) { Start-Sleep -Milliseconds 500 }
}

$last = ''
while ($true) {
    $lines = Get-Content $Path -Tail $LastLines -ErrorAction SilentlyContinue
    $joined = ($lines -join "`n")
    if ($joined -ne $last) {
        Clear-Host
        Write-Host "== Trails of Cold Steel dialog log == ($Path)`n" -ForegroundColor Cyan
        $lines | ForEach-Object { Write-Host $_ }
        $last = $joined
    }
    Start-Sleep -Milliseconds $IntervalMs
}
