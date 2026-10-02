# build_manager.ps1 - build atmt_manager (manager/) for Windows and/or Linux, with its payload.
#
#   powershell -ExecutionPolicy Bypass -File tools\build_manager.ps1              # Windows x64
#   powershell -ExecutionPolicy Bypass -File tools\build_manager.ps1 -Linux       # + Linux / AppImage (Docker)
#   powershell -ExecutionPolicy Bypass -File tools\build_manager.ps1 -Linux -NoWindows
#
# The mods must be built first (tools\build.ps1: it also writes dist\settings_schema.json). This
# stages the payload (tools\stage_payload.ps1) into dist\payload - next to the exe, which is where
# the app looks for its bundled payload - builds the app with llvm-mingw (x86_64), runs its tests and
# puts dist\atmt_manager.exe in place. -Linux builds in Docker (manager/packaging/Dockerfile):
# dist\linux\atmt_manager (the bare binary tools\deploy_deck.ps1 runs on the deck) and the AppImage
# with the payload inside. Build trees: build\manager-win, build\manager-linux.
#
#   -Version     the app's version (default: manager/CMakeLists.txt's; the components' versions are
#                manager/payload/components.json's)
#   -Repo        owner/name of the public GitHub repository update checks ask (default: none)
#   -NoGui       core + CLI only (no SDL2 build; quicker)
#   -SkipTests
param(
    [switch]$Linux,
    [switch]$NoWindows,
    [switch]$NoGui,
    [switch]$SkipTests,
    [string]$Version,
    [string]$Repo = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# Git submodules (third_party/, pinned by commit); a checkout made without --recursive gets them here.
git -C $root submodule update --init --depth 1 third_party/imgui third_party/SDL2 third_party/monocypher third_party/zstd
if ($LASTEXITCODE -ne 0) { throw 'fetching the submodules failed' }

$payload = Join-Path $root 'dist\payload'
$stageArgs = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'stage_payload.ps1'), '-Out', $payload)
$cmakeExtra = @("-DATMT_RELEASE_REPO=$Repo")
if ($Version) { $cmakeExtra += "-DATMT_MANAGER_VERSION=$Version" }
& powershell @stageArgs
if ($LASTEXITCODE -ne 0) { throw "staging the payload failed" }

if (-not $NoWindows) {
    $mingw = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter '*LLVM-MinGW*' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($mingw) {
        $bin = Get-ChildItem $mingw.FullName -Recurse -Directory -Filter 'bin' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'llvm-mingw.*ucrt.*\\bin$' } | Select-Object -First 1
        if ($bin) { $env:Path = "$($bin.FullName);$env:Path" }
    }
    if (-not (Get-Command x86_64-w64-mingw32-gcc -ErrorAction SilentlyContinue)) {
        throw "x86_64-w64-mingw32-gcc not found. Install LLVM-MinGW (winget install MartinStorsjo.LLVM-MinGW.UCRT)"
    }
    $build = Join-Path $root 'build\manager-win'
    $gui = if ($NoGui) { 'OFF' } else { 'ON' }
    cmake -S (Join-Path $root 'manager') -B $build -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM=mingw32-make `
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ `
        -DCMAKE_BUILD_TYPE=Release "-DATMT_MANAGER_GUI=$gui" @cmakeExtra
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    mingw32-make -C $build -j8 atmt_manager atmt_manager_test
    if ($LASTEXITCODE -ne 0) { throw "the manager build failed" }
    if (-not $SkipTests) {
        & (Join-Path $build 'atmt_manager_test.exe')
        if ($LASTEXITCODE -ne 0) { throw "the manager tests failed" }
    }
    Copy-Item (Join-Path $build 'atmt_manager.exe') (Join-Path $root 'dist\atmt_manager.exe') -Force
    & (Join-Path $root 'dist\atmt_manager.exe') --cli verify-payload
    if ($LASTEXITCODE -ne 0) { throw "the built app does not accept its payload" }
    Write-Host ("built dist\atmt_manager.exe ({0} KB), payload in dist\payload" -f `
                [math]::Round((Get-Item (Join-Path $root 'dist\atmt_manager.exe')).Length / 1KB))
}

if ($Linux) {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { throw "the Linux build needs Docker" }
    docker build -q -t atmt-manager-build (Join-Path $root 'manager\packaging') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "docker build failed" }
    $cmakeArgs = (@(if ($NoGui) { '-DATMT_MANAGER_GUI=OFF' } else { '-DATMT_MANAGER_GUI=ON' }) + $cmakeExtra) -join ' '
    docker run --rm -e "ATMT_CMAKE_ARGS=$cmakeArgs" -v "${root}:/src" atmt-manager-build `
        bash manager/packaging/build_linux.sh dist/payload $Version
    if ($LASTEXITCODE -ne 0) { throw "the Linux build failed" }
    Get-ChildItem (Join-Path $root 'dist\linux') -Filter '*.AppImage' | ForEach-Object {
        Write-Host ("built dist\linux\{0} ({1} MB)" -f $_.Name, [math]::Round($_.Length / 1MB, 1))
    }
}
exit 0
