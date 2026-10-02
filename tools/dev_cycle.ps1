# dev_cycle.ps1 - put a freshly built dialog-log mod into the *running* game, without loading a save.
#
#   powershell -ExecutionPolicy Bypass -File tools\dev_cycle.ps1 -GameDir "<game folder>"
#   powershell -ExecutionPolicy Bypass -File tools\dev_cycle.ps1 -GameDir "<game folder>" -Test
#   powershell -ExecutionPolicy Bypass -File tools\dev_cycle.ps1 -GameDir "<game folder>" -BuildOnly
#
# The panel can only be judged in the game, and every restart means loading a save by hand. So a build
# is replaced inside the running game (needs the loader installed - tools\deploy.ps1 - and
# DevReload=true in atmt_mods\trails_dialog_logger.ini):
#
#   1. build trails_dialog_logger (tools\build.ps1's compiler)
#   2. copy the dll into <game>\atmt_mods\dev under a fresh name - a loaded dll's file is locked, so
#      each build needs a name of its own; atmt_mods\dev because the loader loads every *.dll directly
#      in atmt_mods, and a copy left there would run as a second instance of the mod
#   3. write the fresh path into <game>\atmt_reload.txt
#   4. report what the running build did with it (it hands the process over: dev_reload.cpp)
#
# -Test also builds and runs the offline self test (seconds; run it every iteration).
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [switch]$BuildOnly,
    [switch]$Test,
    [int]$WaitSeconds = 20
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build\game'
$mod = 'trails_dialog_logger'

if (-not (Test-Path (Join-Path $GameDir 'ed8.exe'))) {
    throw "ed8.exe not found in $GameDir - is that the game folder?"
}

# ---------------------------------------------------------------- 1. build
$mingw = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter '*LLVM-MinGW*' -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($mingw) {
    $bin = Get-ChildItem $mingw.FullName -Recurse -Directory -Filter 'bin' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'llvm-mingw.*ucrt.*\\bin$' } | Select-Object -First 1
    if ($bin) { $env:Path = "$($bin.FullName);$env:Path" }
}
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    throw 'no build tree - run tools\build.ps1 once first'
}
cmake --build $build --target $mod
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
$built = Join-Path $root "dist\game\atmt_mods\$mod.dll"
if (-not (Test-Path $built)) { throw "the build produced no dll at $built" }
Write-Host ("built {0} KB" -f [math]::Round((Get-Item $built).Length / 1KB))

if ($Test) {
    cmake --build $build --target atmt_selftest
    if ($LASTEXITCODE -ne 0) { throw 'the self test did not build' }
    $tmp = Join-Path $env:TEMP 'atmt_selftest'
    New-Item -ItemType Directory -Force $tmp | Out-Null
    & (Join-Path $build 'tests\atmt_selftest.exe') $tmp
    if ($LASTEXITCODE -ne 0) { throw 'the self test failed' }
}
if ($BuildOnly) { return }

# ---------------------------------------------------------------- 2. a name of our own
$modsDir = Join-Path $GameDir 'atmt_mods'
$devDir = Join-Path $modsDir 'dev'
New-Item -ItemType Directory -Force $devDir | Out-Null
$freshName = "${mod}_dev_$(Get-Date -Format 'yyyyMMdd_HHmmss').dll"
$fresh = Join-Path $devDir $freshName
Copy-Item $built $fresh -Force
Write-Host "  + atmt_mods\dev\$freshName"

# The loader hands a mod <dir>\<name>.ini, so the handover build reads the running mod's settings
# only if they sit next to it as well.
$modIni = Join-Path $modsDir "$mod.ini"
if (Test-Path $modIni) {
    Copy-Item $modIni ([System.IO.Path]::ChangeExtension($fresh, '.ini')) -Force
} else {
    Write-Host "  (no atmt_mods\$mod.ini yet: the handover build will use the defaults)"
}

try {
    Copy-Item $built (Join-Path $modsDir "$mod.dll") -Force
    Write-Host "  + atmt_mods\$mod.dll (the next cold start)"
} catch {
    Write-Host "  ($mod.dll is locked by the running game: deploy again before the next cold start)"
}

# ---------------------------------------------------------------- 3. tell the running build
if (-not (Get-Process -Name ed8 -ErrorAction SilentlyContinue)) {
    Write-Host 'the game is not running: the next start loads the new build (no handover needed)'
    return
}
$flag = Join-Path $GameDir 'atmt_reload.txt'
Set-Content -Path $flag -Value $fresh -Encoding ascii
Write-Host "  + atmt_reload.txt -> $freshName"

$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ((Get-Date) -lt $deadline -and (Test-Path $flag)) { Start-Sleep -Milliseconds 500 }

# ---------------------------------------------------------------- 4. what happened
if (Test-Path $flag) {
    Write-Warning ("atmt_reload.txt is still there after $WaitSeconds s - nothing was listening. Is $mod loaded " +
                   "in that process, and does its ini say DevReload=true?")
}
$log = Join-Path $GameDir 'atmt_loader.log'
if (Test-Path $log) {
    Write-Host ''
    Write-Host "  $log (LogLevel=all in atmt_loader.ini shows the handover):"
    Get-Content $log -Tail 80 | Where-Object { $_ -match "dev reload|$mod|overlay" } | ForEach-Object { Write-Host "    $_" }
}
