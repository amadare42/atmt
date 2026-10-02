# stage_payload.ps1 - put the built loader + mods together as a manager payload: one folder per component.
#
#   powershell -ExecutionPolicy Bypass -File tools\stage_payload.ps1 -Out dist\payload
#
# A payload is what atmt_manager installs (manager/src/core/payload.h has the layout): a folder of
# components, each with its own version from manager\payload\components.json:
#
#   loader\                the loader (files\GFSDK_SSAO_D3D11.win32.dll) and its settings' schema
#   data\                  manager\payload\data\ (presets, supported_exe.txt, install_rules.json,
#                          icon_pack.json) and the schema's input names
#   <mod>\                 files\atmt_mods\<mod>.dll, its documented ini template (mods\<folder>\<mod>.ini,
#                          when there is one) and its part of dist\settings_schema.json
#
# Each has component.json and a manifest.md5 over all of it; nothing in them changes unless the
# component does (no build time), so tools\release.ps1 can tell a changed component by its hashes.
# Only the mods components.json lists ship. tools\deploy.ps1, tools\deploy_deck.ps1,
# tools\build_manager.ps1 and tools\release.ps1 all install or ship one of these, so there is one
# install layout.
#
#   -Out       the folder to write (emptied first)
#
# No icon pack ships: it is made of the player's own textures, so the manager builds it on their
# machine from icon_pack.json (docs/MANAGER.md, "The icon pack").
param(
    [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$utf8 = New-Object System.Text.UTF8Encoding($false)

# A built file by its path under dist\game (the install layout).
function Find-Built([string]$rel) {
    $hit = Get-Item (Join-Path $root "dist\game\$rel") -ErrorAction SilentlyContinue
    if ($hit) { return $hit.FullName }
    return $null
}

function Get-Md5([string]$path) {
    $md5 = New-Object System.Security.Cryptography.MD5CryptoServiceProvider
    $stream = [IO.File]::OpenRead($path)
    try { $hash = $md5.ComputeHash($stream) } finally { $stream.Close() }
    return (($hash | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Write-Json([string]$path, $value) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 32) + "`n", $utf8)
}

# component.json + manifest.md5 (LF and '/' paths: it is read on Linux too).
function Complete-Component([string]$dir, [string]$name, [string]$kind, $entry) {
    $meta = [ordered]@{
        name = $name
        kind = $kind
        version = $entry.version
        title = $entry.title
        min_manager_version = $(if ($entry.min_manager_version) { $entry.min_manager_version } else { $catalog.min_manager_version })
    }
    if ($null -ne $entry.default_enabled) { $meta.default_enabled = [bool]$entry.default_enabled }
    if (-not $meta.version) { throw "manager\payload\components.json: $name has no version" }
    Write-Json (Join-Path $dir 'component.json') $meta
    $lines = @()
    foreach ($file in (Get-ChildItem $dir -Recurse -File | Where-Object { $_.Name -ne 'manifest.md5' } | Sort-Object FullName)) {
        $rel = $file.FullName.Substring($dir.Length + 1) -replace '\\', '/'
        $lines += ('{0}  {1}' -f (Get-Md5 $file.FullName), $rel)
    }
    [IO.File]::WriteAllText((Join-Path $dir 'manifest.md5'), (($lines -join "`n") + "`n"), $utf8)
}

if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location) $Out }
$Out = [IO.Path]::GetFullPath($Out)
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force $Out | Out-Null

$catalog = Get-Content (Join-Path $root 'manager\payload\components.json') -Raw | ConvertFrom-Json
$shipped = @($catalog.mods | ForEach-Object { $_.name })
if ($shipped.Count -eq 0) { throw "manager\payload\components.json lists no mods" }

$schemaPath = Join-Path $root 'dist\settings_schema.json'
if (-not (Test-Path $schemaPath)) { throw "dist\settings_schema.json is missing - run: powershell -File tools\build.ps1" }
$schema = Get-Content $schemaPath -Raw | ConvertFrom-Json
function Get-SchemaPart([string]$mod) {
    $part = [ordered]@{ schema_version = $schema.schema_version }
    $part.mods = @($schema.mods | Where-Object { $_.mod -eq $mod })
    $part.groups = @($schema.groups | Where-Object { $_.mod -eq $mod })
    return $part
}

# ---------------------------------------------------------------- the loader
$dir = Join-Path $Out 'loader'
New-Item -ItemType Directory -Force (Join-Path $dir 'files') | Out-Null
$proxy = Find-Built 'GFSDK_SSAO_D3D11.win32.dll'
if (-not $proxy) { throw "the loader is not built - run: powershell -File tools\build.ps1" }
Copy-Item $proxy (Join-Path $dir 'files\GFSDK_SSAO_D3D11.win32.dll')
Write-Json (Join-Path $dir 'schema.json') (Get-SchemaPart 'atmt_loader')
Complete-Component $dir 'loader' 'loader' $catalog.loader

# ---------------------------------------------------------------- the data
$dir = Join-Path $Out 'data'
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item (Join-Path $root 'manager\payload\data\*') $dir -Recurse
Write-Json (Join-Path $dir 'schema.json') ([ordered]@{ schema_version = $schema.schema_version; input = $schema.input })
Complete-Component $dir 'data' 'data' $catalog.data

# ---------------------------------------------------------------- the mods
# What ships is exactly what components.json lists: a dll that is built but not listed there (a
# development build of a mod) stays out of every payload - developers copy it into <game>\atmt_mods
# by hand.
foreach ($extra in @(Get-ChildItem (Join-Path $root 'dist\game\atmt_mods') -Filter '*.dll' -ErrorAction SilentlyContinue |
                     Where-Object { $shipped -notcontains $_.BaseName })) {
    Write-Host "note: $($extra.Name) is not in manager\payload\components.json - not shipped"
}
foreach ($mod in $catalog.mods) {
    $name = $mod.name
    $src = Find-Built "atmt_mods\$name.dll"
    if (-not $src) { throw "$name.dll is not built - run: powershell -File tools\build.ps1" }
    $dir = Join-Path $Out $name
    New-Item -ItemType Directory -Force (Join-Path $dir 'files\atmt_mods') | Out-Null
    Copy-Item $src (Join-Path $dir "files\atmt_mods\$name.dll")
    # A documented config (it becomes the mod's ini; installing only writes one when the game folder
    # has none). Mods without one get theirs from the loader's settings registry.
    # mods\<folder>\<name>.ini, at the top of a mod's folder only (a folder is not always named like
    # its dll: mods\overlay builds atmt_overlay)
    $inis = @(Get-ChildItem (Join-Path $root 'mods') -Directory | ForEach-Object { Join-Path $_.FullName "$name.ini" } |
              Where-Object { Test-Path $_ -PathType Leaf })
    if ($inis.Count -gt 1) { throw "more than one template for $name`: $($inis -join ', ')" }
    if ($inis.Count -eq 1) { Copy-Item $inis[0] (Join-Path $dir "files\atmt_mods\$name.ini") }
    Write-Json (Join-Path $dir 'schema.json') (Get-SchemaPart $name)
    Complete-Component $dir $name 'mod' $mod
}

Write-Host ("payload -> {0}" -f $Out)
foreach ($c in (Get-ChildItem $Out -Directory | Sort-Object Name)) {
    $meta = Get-Content (Join-Path $c.FullName 'component.json') -Raw | ConvertFrom-Json
    Write-Host ("  {0,-24} {1}" -f $meta.name, $meta.version)
}
