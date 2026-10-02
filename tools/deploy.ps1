# deploy.ps1 - install the built loader + mods into a game folder (the developer flow on Windows).
#
#   powershell -ExecutionPolicy Bypass -File tools\deploy.ps1 -GameDir "<game folder>"
#
# The install itself is the manager's (manager/, docs/MANAGER.md): this stages the built files as a
# payload (tools\stage_payload.ps1) and runs `atmt_manager --cli install` on it - the same code a
# player's install runs: backup into atmt_backup_<tag>\, the game's own library kept as
# GFSDK_SSAO_D3D11.win32.orig.dll, existing inis kept, leftovers of earlier deployments removed,
# every written file verified against the manifest and rolled back on a mismatch.
#
#   <game>\GFSDK_SSAO_D3D11.win32.dll          the loader, impersonating that dll
#   <game>\GFSDK_SSAO_D3D11.win32.orig.dll     the game's own library, only renamed
#   <game>\atmt_mods\<mod>.dll                 the mods (disabled ones in atmt_mods\disabled)
#   <game>\atmt_install.json                   what the manager installed
#
# Needs tools\build.ps1 and tools\build_manager.ps1 (dist\atmt_manager.exe) first.
#
#   -Disable    mods (dll base names) to leave out; they are parked in atmt_mods\disabled
#   -Undeploy   `atmt_manager --cli uninstall` (everything is backed up first)
#   -PurgeLogs  with -Undeploy: also remove the dialog logs
#   -Manager    the atmt_manager.exe to use (default dist\atmt_manager.exe)
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [switch]$Undeploy,
    [switch]$PurgeLogs,
    [string[]]$Disable = @(),
    [string]$Manager
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path (Join-Path $GameDir 'ed8.exe'))) {
    throw "ed8.exe not found in $GameDir - is that the right folder?"
}
if (-not $Manager) { $Manager = Join-Path $root 'dist\atmt_manager.exe' }
if (-not (Test-Path $Manager)) {
    throw "atmt_manager.exe is not built - run: powershell -File tools\build_manager.ps1 -NoGui"
}

if ($Undeploy) {
    & $Manager --cli uninstall --game-dir $GameDir --tag "undeploy_$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    if ($LASTEXITCODE -ne 0) { throw "uninstall failed (exit $LASTEXITCODE)" }
    if ($PurgeLogs) {
        foreach ($f in 'atmt_dialogs.jsonl', 'atmt_latest.txt', 'atmt_diagnostics.log') {
            Remove-Item (Join-Path $GameDir $f) -ErrorAction SilentlyContinue
        }
    }
    return
}

$payload = Join-Path $root 'build\deploy_payload'
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'stage_payload.ps1') -Out $payload
if ($LASTEXITCODE -ne 0) { throw "staging the payload failed" }

$mods = @(Get-ChildItem $payload -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'files\atmt_mods\*.dll') } |
          ForEach-Object { $_.Name } | Where-Object { $Disable -notcontains $_ })
# --yes: a developer deploying to an unverified exe build knows it is one (it is still logged)
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
& $Manager --cli install --game-dir $GameDir --payload $payload --mods ($mods -join ',') --tag "deploy_$stamp" --yes
if ($LASTEXITCODE -ne 0) { throw "the install failed (exit $LASTEXITCODE) - the game folder was rolled back" }

Write-Host ""
Write-Host "loader log: $(Join-Path $GameDir 'atmt_loader.log')   (errors only unless LogLevel=all in atmt_loader.ini)"
Write-Host "revert everything with:"
Write-Host "  powershell -File tools\deploy.ps1 -GameDir `"$GameDir`" -Undeploy"
