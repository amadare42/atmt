# build.ps1 - build the loader and the mods (32-bit), write the settings schema, run the tests.
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -SkipTests
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Clean
#
# Output (see docs/BUILD.md): everything usable lands in dist\, the build tree (build\game) only
# holds intermediates.
#
#   dist\game\GFSDK_SSAO_D3D11.win32.dll    the loader (the proxy flavour that ships)
#   dist\game\atmt_mods\*.dll               the mods
#   dist\tools\atmt_loader.dll, atmt_inject.exe   the injectable loader flavour, for development
#   dist\settings_schema.json               every mod's settings, for the manager (tools\stage_payload.ps1)
#
# Compiler: llvm-mingw (winget install MartinStorsjo.LLVM-MinGW.UCRT), 32-bit i686 target.
param(
    [switch]$SkipTests,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build\game'
$dist = Join-Path $root 'dist'

# ---------------------------------------------------------------- third-party sources
# Git submodules (third_party/, pinned by commit); a checkout made without --recursive gets them here.
git -C $root submodule update --init --depth 1 third_party/minhook third_party/imgui
if ($LASTEXITCODE -ne 0) { throw 'fetching the submodules failed' }

# ---------------------------------------------------------------- compiler
$mingw = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter '*LLVM-MinGW*' -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($mingw) {
    $bin = Get-ChildItem $mingw.FullName -Recurse -Directory -Filter 'bin' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'llvm-mingw.*ucrt.*\\bin$' } | Select-Object -First 1
    if ($bin) { $env:Path = "$($bin.FullName);$env:Path" }
}
if (-not (Get-Command i686-w64-mingw32-gcc -ErrorAction SilentlyContinue)) {
    throw "i686-w64-mingw32-gcc not found. Install LLVM-MinGW: winget install MartinStorsjo.LLVM-MinGW.UCRT"
}

# ---------------------------------------------------------------- build
if ($Clean) {
    Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $dist 'game'), (Join-Path $dist 'tools') -Recurse -Force -ErrorAction SilentlyContinue
}
$targets = 'atmt_loader', 'atmt_loader_gfsdk', 'atmt_overlay', 'trails_dialog_logger', 'load_last_save',
           'deckscreen', 'stick_rotation_speed', 'battle_load',
           'atmt_selftest', 'atmt_al_test', 'atmt_settings_test', 'atmt_loader_test', 'atmt_dummy_orig',
           'atmt_inject', 'atmt_modules', 'atmt_schema_dump'
cmake -S $root -B $build -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM=mingw32-make `
    -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++ `
    -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
mingw32-make -C $build -j8 @targets
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$outputs = 'game\GFSDK_SSAO_D3D11.win32.dll', 'tools\atmt_loader.dll',
           'game\atmt_mods\atmt_overlay.dll', 'game\atmt_mods\trails_dialog_logger.dll',
           'game\atmt_mods\load_last_save.dll', 'game\atmt_mods\deckscreen.dll',
           'game\atmt_mods\stick_rotation_speed.dll', 'game\atmt_mods\battle_load.dll'
foreach ($rel in $outputs) {
    $path = Join-Path $dist $rel
    if (-not (Test-Path $path)) { throw "build failed: dist\$rel not produced" }
    Write-Host ("built dist\{0} ({1} KB)" -f $rel, [math]::Round((Get-Item $path).Length / 1KB))
}

# ---------------------------------------------------------------- settings schema
# Every mod's settings table as data (tests\schema_dump.cpp), for the manager's generated settings
# pages. Only the mods listed above: a renamed target leaves its old dll behind in dist\game\atmt_mods.
$schemaMods = @($outputs | Where-Object { $_ -like 'game\atmt_mods\*' } | ForEach-Object { Join-Path $dist $_ })
& (Join-Path $build 'tests\atmt_schema_dump.exe') (Join-Path $dist 'settings_schema.json') @schemaMods
if ($LASTEXITCODE -ne 0) { throw "the settings schema could not be written" }

if ($SkipTests) { return }

# ---------------------------------------------------------------- tests (no game needed)
$tests = Join-Path $build 'tests'
$tmp = Join-Path $env:TEMP 'atmt_tests'
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $tmp | Out-Null

# the logger's pure logic: ini reader, text helpers, log sink, the panel's ring of lines
& (Join-Path $tests 'atmt_selftest.exe') $tmp
if ($LASTEXITCODE -ne 0) { throw "self test failed" }

# everything that decides what the load_last_save mod does (docs/MODS.md)
& (Join-Path $tests 'atmt_al_test.exe') $tmp
if ($LASTEXITCODE -ne 0) { throw "the load_last_save offline tests failed" }

# the loader's settings registry and the in-place ini writer (AtmtModApi::settings_*)
& (Join-Path $tests 'atmt_settings_test.exe') (Join-Path $tmp 'settings')
if ($LASTEXITCODE -ne 0) { throw "the settings registry tests failed" }

# The loader + mod pair, exactly as the game would see it: the sandbox gets the loader, a stand-in
# for the library it impersonates, and the mod dll.
$sandbox = Join-Path $tmp 'loader_test'
New-Item -ItemType Directory -Force (Join-Path $sandbox 'atmt_mods') | Out-Null
Copy-Item (Join-Path $dist 'game\GFSDK_SSAO_D3D11.win32.dll') $sandbox
Copy-Item (Join-Path $dist 'game\atmt_mods\trails_dialog_logger.dll') (Join-Path $sandbox 'atmt_mods')
Copy-Item (Join-Path $tests 'atmt_loader_test.exe') $sandbox
# the dummy has the same two exports and answers recognisably, so the forwarding check is a real
# check even without the game installed
Copy-Item (Join-Path $tests 'atmt_dummy_orig.dll') (Join-Path $sandbox 'GFSDK_SSAO_D3D11.win32.orig.dll')
# The test reads what the loader and the mod did from atmt_loader.log and the dialog log; by default
# they are errors only (LogLevel=errors) and not written at all (LogToFile=false).
[IO.File]::WriteAllText((Join-Path $sandbox 'atmt_loader.ini'), "[Loader]`r`nLogLevel=all`r`n")
[IO.File]::WriteAllText((Join-Path $sandbox 'atmt_mods\trails_dialog_logger.ini'), "[General]`r`nLogToFile=true`r`n")
Push-Location $sandbox
try {
    # Heuristic antivirus scanners sometimes refuse to start this harness (it loads a dll by name and
    # writes logs - see README, "Security software"). That is a scanner decision, not a test failure.
    $started = $true
    try { & '.\atmt_loader_test.exe' } catch { $started = $false }
    if (-not $started) {
        Write-Warning "the loader test could not be started (blocked by antivirus?). Add an exclusion for $sandbox and $build, or run tools\build.ps1 -SkipTests."
    } elseif ($LASTEXITCODE -ne 0) {
        throw "loader test failed"
    }
} finally { Pop-Location }

# A blocked harness leaves a nonzero $LASTEXITCODE behind, which powershell -File would report as
# this script's exit code. Reaching this point means every test that could run passed.
exit 0
