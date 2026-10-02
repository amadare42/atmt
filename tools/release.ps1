# release.ps1 - build a release of ATMT: build, pack the changed components, write and sign the
# manifest, (optionally) upload.
#
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Repo owner/atmt
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Repo owner/atmt -Upload
#
# docs/MANAGER.md "Releases". The manager and every component (manager/payload/components.json) have
# versions of their own. The apps read the latest release's manifest.json; this writes the next one
# from the one published now (https://github.com/<Repo>/releases/latest/download/manifest.json, or
# -Previous):
#
#   * a component whose version is new gets its archive <name>-<version>.tar.gz in this release;
#   * a component whose version is the published one keeps the published entry (its download stays
#     in the earlier release) - and when its files differ from what was published ("content": the
#     hash of its manifest.md5 without component.json, so a new title is not a change), this stops:
#     bump its version in components.json, or pass -BumpChanged;
#   * a version lower than the published one stops too.
#
# Writes dist\release\<Tag>\:
#
#   atmt-manager-<v>-x86_64.AppImage   the Linux / Steam Deck app, the payload inside
#   atmt-manager-<v>-win64.zip         the Windows app (atmt_manager.exe + payload\)
#   atmt-manager-<v>-win64.exe         the Windows exe alone (what the app's self-update downloads)
#   Install ATMT Manager.desktop       Linux: makes the downloaded AppImage executable and starts it
#   <component>-<version>.tar.gz       each component new in this release
#   manifest.json, manifest.json.sig   every version and download, signed with the release key
#   notes.md                           the release's text (what is new)
#
#   -Repo        owner/name of the PUBLIC GitHub repository the releases live in (compiled into the app:
#                its update checks read that repository's latest manifest.json)
#   -Tag         the release's tag (default: r<yyyy.MM.dd.HHmm> UTC)
#   -Key         the release signing key's secret half (default %USERPROFILE%\.atmt\atmt_release.key;
#                make one with: atmt_manager --cli keygen %USERPROFILE%\.atmt). Its public half must be
#                manager\release_pubkey.txt, or the apps built here cannot verify their own updates.
#   -AppNotes    a text file with the app's changelog (when its version is new)
#   -Previous    the published manifest.json to build on (default: downloaded; 'none': the first release)
#   -SkipBuild   do not run tools\build.ps1 first (the mods are already built)
#   -BumpChanged a component that changed under its published version gets the next patch version
#                (1.2.3 -> 1.2.4) written into manager\payload\components.json, and the app is built
#                again with it, instead of stopping. Its changelog stays as it is: fill it in first.
#   -Upload      create the GitHub release with gh (asks nothing: only pass it when the files are right).
#                It is made as a draft, checked (every file uploaded whole, every earlier release's
#                download the manifest points at still there) and only then published as the latest,
#                so the apps never see a manifest whose downloads are not all in place.
param(
    [Parameter(Mandatory = $true)][string]$Repo,
    [string]$Tag,
    [string]$Key = "$env:USERPROFILE\.atmt\atmt_release.key",
    [string]$AppNotes,
    [string]$Previous,
    [switch]$SkipBuild,
    [switch]$BumpChanged,
    [switch]$Upload
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$utf8 = New-Object System.Text.UTF8Encoding($false)
if (-not $Tag) { $Tag = 'r' + (Get-Date).ToUniversalTime().ToString('yyyy.MM.dd.HHmm') }
$out = Join-Path $root "dist\release\$Tag"
$download = "https://github.com/$Repo/releases/download/$Tag"

if (-not (Test-Path $Key)) { throw "no signing key at $Key - make one with: atmt_manager --cli keygen $env:USERPROFILE\.atmt" }
$pub = Join-Path $root 'manager\release_pubkey.txt'
if (-not (Test-Path $pub)) { throw "manager\release_pubkey.txt is missing: copy the key's .pub there (and commit it) before building" }

# -1 / 0 / 1, like the app's CompareVersions: "1.2.3", a "-suffix" sorts before the plain version.
function Compare-Version([string]$a, [string]$b) {
    function Split-Version([string]$v) {
        $v = $v -replace '^[vV]', ''
        $pre = ''
        if ($v -match '^([^-]*)-(.*)$') { $v = $Matches[1]; $pre = $Matches[2] }
        $parts = @($v.Split('.') | ForEach-Object { [long]($_ -replace '[^0-9].*$', '0') })
        while ($parts.Count -lt 3) { $parts += 0 }
        return @{ parts = $parts; pre = $pre }
    }
    $x = Split-Version $a; $y = Split-Version $b
    for ($i = 0; $i -lt [Math]::Max($x.parts.Count, $y.parts.Count); $i++) {
        $p = if ($i -lt $x.parts.Count) { $x.parts[$i] } else { 0 }
        $q = if ($i -lt $y.parts.Count) { $y.parts[$i] } else { 0 }
        if ($p -ne $q) { return $(if ($p -lt $q) { -1 } else { 1 }) }
    }
    if ($x.pre -eq $y.pre) { return 0 }
    if (-not $x.pre) { return 1 }
    if (-not $y.pre) { return -1 }
    if ([string]::CompareOrdinal($x.pre, $y.pre) -lt 0) { return -1 }
    return 1
}

function Get-Sha256([string]$path) { return (Get-FileHash -Algorithm SHA256 $path).Hash.ToLower() }

# What a component is made of: the sha256 of its manifest.md5 without the component.json line. The
# version, title and min_manager_version live there and are not the component's content; every file
# that is (files/, schema.json, presets, ...) has its line.
function Get-ComponentContent([string]$dir) {
    $lines = @([IO.File]::ReadAllText((Join-Path $dir 'manifest.md5')) -split "`n" |
               Where-Object { $_ -and ($_ -notmatch '^[0-9a-f]{32}  \*?component\.json$') })
    $bytes = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n") + "`n")
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return (($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '') } finally { $sha.Dispose() }
}

# The next patch version: 1.2.3 -> 1.2.4; a pre-release 1.2.3-rc1 -> 1.2.3.
function Get-NextVersion([string]$v) {
    $v = $v -replace '^[vV]', ''
    if ($v -match '^([^-]*)-') { return $Matches[1] }
    $parts = @($v.Split('.') | ForEach-Object { [long]($_ -replace '[^0-9].*$', '0') })
    while ($parts.Count -lt 3) { $parts += 0 }
    $parts[2] = $parts[2] + 1
    return ($parts -join '.')
}

# Sets a component's version in manager\payload\components.json, in place (the file's layout and
# its comments stay as they are).
function Set-CatalogVersion([string]$name, [string]$version) {
    $path = Join-Path $root 'manager\payload\components.json'
    $text = [IO.File]::ReadAllText($path)
    $n = [regex]::Escape($name)
    $pattern = if ($name -eq 'loader' -or $name -eq 'data') { "(""$n""\s*:\s*\{[^{}]*?""version""\s*:\s*"")[^""]*("")" }
               else { "(""name""\s*:\s*""$n""[^{}]*?""version""\s*:\s*"")[^""]*("")" }
    $hits = [regex]::Matches($text, $pattern)
    if ($hits.Count -ne 1) { throw "manager\payload\components.json: cannot find the version of $name to bump it" }
    $text = [regex]::Replace($text, $pattern, "`${1}$version`${2}")
    [IO.File]::WriteAllText($path, $text, $utf8)
}

function Build-Manager {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build_manager.ps1') -Linux -Repo $Repo
    if ($LASTEXITCODE -ne 0) { throw "tools\build_manager.ps1 failed" }
}

# ---------------------------------------------------------------- build
if (-not $SkipBuild) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1')
    if ($LASTEXITCODE -ne 0) { throw "tools\build.ps1 failed" }
}
Build-Manager
$manager = Join-Path $root 'dist\atmt_manager.exe'
$payload = Join-Path $root 'dist\payload'
$version = (Select-String -Path (Join-Path $root 'manager\CMakeLists.txt') -Pattern 'set\(ATMT_MANAGER_VERSION "([^"]+)"').Matches[0].Groups[1].Value

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null

# ---------------------------------------------------------------- the published manifest
$prev = $null
if ($Previous -eq 'none') {
    Write-Host "-Previous none: this is the first release"
} elseif ($Previous) {
    $prev = Get-Content $Previous -Raw | ConvertFrom-Json
} else {
    $prevFile = Join-Path $root 'build\release_previous_manifest.json'
    try {
        Invoke-WebRequest "https://github.com/$Repo/releases/latest/download/manifest.json" -OutFile $prevFile -UseBasicParsing
        Invoke-WebRequest "https://github.com/$Repo/releases/latest/download/manifest.json.sig" -OutFile "$prevFile.sig" -UseBasicParsing
    } catch {
        if ($_.Exception.Response -and [int]$_.Exception.Response.StatusCode -eq 404) {
            Write-Host "no published manifest: this is the first release"
            $prevFile = $null
        } else {
            throw "cannot read the published manifest: $($_.Exception.Message) (or pass -Previous)"
        }
    }
    if ($prevFile) {
        & $manager --cli verify-sig $pub $prevFile
        if ($LASTEXITCODE -ne 0) { throw "the published manifest does not verify with manager\release_pubkey.txt" }
        $prev = Get-Content $prevFile -Raw | ConvertFrom-Json
    }
}
$prevComponents = @{}
if ($prev) { foreach ($c in $prev.components) { $prevComponents[$c.name] = $c } }

# ---------------------------------------------------------------- the components
# Compares the staged components with the published ones: the components that changed under their
# published version (to bump), and every component's entry or, for a new version, its folder.
function Get-ComponentPlan {
    $plan = @{ stale = @(); kept = @(); fresh = @() }
    foreach ($dir in (Get-ChildItem $payload -Directory | Sort-Object Name)) {
        $meta = Get-Content (Join-Path $dir.FullName 'component.json') -Raw | ConvertFrom-Json
        $content = Get-ComponentContent $dir.FullName
        $old = $prevComponents[$meta.name]
        if ($old) {
            $cmp = Compare-Version $meta.version $old.version
            if ($cmp -lt 0) { throw "$($meta.name) $($meta.version) is older than the published $($old.version)" }
            if ($cmp -eq 0) {
                if ($old.content -ne $content) { $plan.stale += $old } else { $plan.kept += $old }
                continue
            }
        }
        $plan.fresh += [pscustomobject]@{ dir = $dir.FullName; meta = $meta; content = $content }
    }
    return $plan
}

$plan = Get-ComponentPlan
if ($plan.stale.Count -gt 0) {
    $names = ($plan.stale | ForEach-Object { "$($_.name) $($_.version)" }) -join ', '
    if (-not $BumpChanged) {
        throw "changed under their published version: $names - bump them in manager\payload\components.json (or pass -BumpChanged)"
    }
    foreach ($c in $plan.stale) {
        $next = Get-NextVersion $c.version
        Set-CatalogVersion $c.name $next
        Write-Host "bumped $($c.name) $($c.version) -> $next (manager\payload\components.json)"
    }
    Build-Manager   # the payload in the apps carries the versions
    $plan = Get-ComponentPlan
    if ($plan.stale.Count -gt 0) { throw "still changed under the published version after the bump: $(($plan.stale | ForEach-Object { $_.name }) -join ', ')" }
}

$catalog = Get-Content (Join-Path $root 'manager\payload\components.json') -Raw | ConvertFrom-Json
$changelogs = @{ loader = $catalog.loader.changelog; data = $catalog.data.changelog }
foreach ($m in $catalog.mods) { $changelogs[$m.name] = $m.changelog }
$entries = @($plan.kept)
$new = @()
foreach ($f in $plan.fresh) {
    $meta = $f.meta
    $content = $f.content
    $dir = Get-Item $f.dir
    if (-not $changelogs[$meta.name]) { Write-Host "note: $($meta.name) $($meta.version) has no changelog (manager\payload\components.json)" }
    $file = "$($meta.name)-$($meta.version).tar.gz"
    tar -czf (Join-Path $out $file) -C $dir.FullName .
    if ($LASTEXITCODE -ne 0) { throw "tar failed for $($meta.name)" }
    $entry = [ordered]@{
        name = $meta.name
        kind = $meta.kind
        title = $meta.title
        version = $meta.version
        min_manager_version = $meta.min_manager_version
        changelog = [string]$changelogs[$meta.name]
        url = "$download/$file"
        sha256 = Get-Sha256 (Join-Path $out $file)
        size = (Get-Item (Join-Path $out $file)).Length
        content = $content
    }
    $entries += [pscustomobject]$entry
    $new += [pscustomobject]$entry
}

# ---------------------------------------------------------------- the app
if ($prev -and (Compare-Version $version $prev.manager.version) -lt 0) {
    throw "the app $version is older than the published $($prev.manager.version)"
}
$appIsNew = -not $prev -or (Compare-Version $version $prev.manager.version) -gt 0
$appChangelog = if ($AppNotes) { [IO.File]::ReadAllText((Resolve-Path $AppNotes)) } elseif (-not $appIsNew) { [string]$prev.manager.changelog } else { '' }

$appImage = "atmt-manager-$version-x86_64.AppImage"
$zip = "atmt-manager-$version-win64.zip"
$exe = "atmt-manager-$version-win64.exe"
Copy-Item (Join-Path $root "dist\linux\$appImage") (Join-Path $out $appImage)
Copy-Item $manager (Join-Path $out $exe)
$zipStage = Join-Path $root "build\release_zip_$Tag"
if (Test-Path $zipStage) { Remove-Item $zipStage -Recurse -Force }
New-Item -ItemType Directory -Force $zipStage | Out-Null
Copy-Item $manager (Join-Path $zipStage 'atmt_manager.exe')
Copy-Item $payload (Join-Path $zipStage 'payload') -Recurse
Compress-Archive -Path (Join-Path $zipStage '*') -DestinationPath (Join-Path $out $zip)
Remove-Item $zipStage -Recurse -Force

# Browsers drop the exec bit on download: this launcher (Dolphin asks once whether to trust it) sets
# it on the AppImage next to it and starts it.
# The Exec line follows the desktop entry spec's two escaping levels (string escapes, then argument
# quoting: only double quotes, with \" and \$ inside - written \\" and \\$) and passes %k (this
# file's own path) outside the quotes, as $1.
$desktop = @'
[Desktop Entry]
Type=Application
Name=Install ATMT Manager
Comment=Makes the ATMT Manager AppImage next to this file executable and starts it
Icon=system-software-install
Exec=sh -c "d=\\$(dirname \\"\\$1\\"); for f in \\"\\$d\\"/atmt-manager-*-x86_64.AppImage; do chmod +x \\"\\$f\\" && exec \\"\\$f\\"; done" sh %k
Terminal=false
'@
[IO.File]::WriteAllText((Join-Path $out 'Install ATMT Manager.desktop'), ($desktop -replace "`r`n", "`n") + "`n", $utf8)

# ---------------------------------------------------------------- the manifest
# The app's downloads are always this release's (they carry the current payload); an app whose
# version is unchanged is simply not offered as an update.
$manifest = [ordered]@{
    format = 1
    published = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    page = "https://github.com/$Repo/releases/tag/$Tag"
    manager = [ordered]@{
        version = $version
        changelog = $appChangelog
        downloads = [ordered]@{
            windows = [ordered]@{ url = "$download/$exe"; sha256 = Get-Sha256 (Join-Path $out $exe); size = (Get-Item (Join-Path $out $exe)).Length }
            linux = [ordered]@{ url = "$download/$appImage"; sha256 = Get-Sha256 (Join-Path $out $appImage); size = (Get-Item (Join-Path $out $appImage)).Length }
        }
    }
    components = @($entries | Sort-Object name)
}
$manifestPath = Join-Path $out 'manifest.json'
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8) + "`n", $utf8)
& $manager --cli sign $Key $manifestPath --comment "atmt release $Tag"
if ($LASTEXITCODE -ne 0) { throw "signing failed" }
& $manager --cli verify-manifest $pub $manifestPath
if ($LASTEXITCODE -ne 0) { throw "the app does not accept the manifest (a wrong key? manager\release_pubkey.txt)" }

# ---------------------------------------------------------------- the notes
$notes = @()
if ($appIsNew) { $notes += "## ATMT Manager $version"; if ($appChangelog) { $notes += $appChangelog }; $notes += '' }
foreach ($c in $new) {
    $notes += "## $($c.title) $($c.version)"
    if ($c.changelog) { $notes += $c.changelog }
    $notes += ''
}
if ($notes.Count -eq 0) { $notes += "ATMT Manager $version, nothing else changed." }
$notes += '| | version |'
$notes += '|---|---|'
$notes += "| ATMT Manager | $version |"
foreach ($c in $entries) { $notes += "| $($c.title) | $($c.version) |" }
$notesFile = Join-Path $out 'notes.md'
[IO.File]::WriteAllText($notesFile, ($notes -join "`n") + "`n", $utf8)

Write-Host ""
Write-Host "release $Tag in $out"
Write-Host ("  app {0}{1}" -f $version, $(if ($appIsNew) { ' (new)' } else { '' }))
foreach ($c in $entries) {
    Write-Host ("  {0,-24} {1,-10} {2}" -f $c.name, $c.version, $(if ($new | Where-Object { $_.name -eq $c.name }) { 'new' } else { 'published' }))
}
Get-ChildItem $out -File | ForEach-Object { Write-Host ("  {0,-44} {1,12:N0} bytes" -f $_.Name, $_.Length) }

if ($Upload) {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw "-Upload needs the GitHub CLI (gh)" }
    # The downloads this manifest keeps from earlier releases must still be there.
    foreach ($c in $entries) {
        if ($new | Where-Object { $_.name -eq $c.name }) { continue }
        try {
            Invoke-WebRequest $c.url -Method Head -UseBasicParsing | Out-Null
        } catch {
            throw "$($c.name) $($c.version) points at $($c.url), which is not there any more: $($_.Exception.Message)"
        }
    }
    $files = @(Get-ChildItem $out -File | Where-Object { $_.Name -ne 'notes.md' })
    # A draft first: the latest release (and so the apps' manifest.json) only changes once every
    # file is uploaded.
    & gh release create $Tag --repo $Repo --title "ATMT $Tag" --notes-file $notesFile --draft @($files | ForEach-Object { $_.FullName })
    if ($LASTEXITCODE -ne 0) { throw "gh release create failed (a draft $Tag may be left: gh release delete $Tag --repo $Repo)" }
    $uploaded = (& gh release view $Tag --repo $Repo --json assets | ConvertFrom-Json).assets
    foreach ($f in $files) {
        # GitHub turns the spaces of an asset's name into dots
        $name = $f.Name -replace ' ', '.'
        $asset = $uploaded | Where-Object { $_.name -eq $name }
        if (-not $asset -or [long]$asset.size -ne $f.Length) {
            throw "$($f.Name) is not uploaded whole - the draft $Tag stays unpublished (gh release delete $Tag --repo $Repo)"
        }
    }
    # Through the API rather than `gh release edit`, which older gh versions (2.6) do not have.
    # Filtered here, not with --jq: Windows PowerShell drops the quotes a jq string needs.
    $id = (& gh api "repos/$Repo/releases" | ConvertFrom-Json | Where-Object { $_.tag_name -eq $Tag }).id
    if (-not $id) { throw "the draft $Tag is not found - publish it on the release page" }
    & gh api -X PATCH "repos/$Repo/releases/$id" -F draft=false -f make_latest=true --silent
    if ($LASTEXITCODE -ne 0) { throw "publishing the draft $Tag failed: gh api -X PATCH repos/$Repo/releases/$id -F draft=false -f make_latest=true" }
    Write-Host "published $Tag to https://github.com/$Repo/releases"
}
