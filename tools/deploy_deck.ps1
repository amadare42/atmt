# deploy_deck.ps1 - install the built loader + mods into the game folder on a Steam Deck, over ssh.
#
#   powershell -ExecutionPolicy Bypass -File tools\deploy_deck.ps1
#   powershell -ExecutionPolicy Bypass -File tools\deploy_deck.ps1 -Target deck@192.168.1.50
#
# A developer tool: the player flow is the manager app itself. The install is the manager's too -
# this stages the built files as a payload (tools\stage_payload.ps1), copies it to /tmp on the deck
# together with the Linux build of the manager (dist/linux/atmt_manager, from
# tools\build_manager.ps1 -Linux) and runs `atmt_manager --cli install` there: the same backup,
# install, md5 verification and rollback a player's install runs (docs/MANAGER.md) - and, where
# SenPatcher is set up, the manager builds the icon pack there from the deck's own textures.
#
#   -Target        ssh destination ([user@]host). Default deck@steamdeck - give it a ~/.ssh/config
#                  alias, or pass the deck's address.
#   -GameDir       the game folder on the deck (the Steam library path by default).
#   -IdentityFile  the public key it authorises on the deck when ssh asks for a password
#                  (default ~/.ssh/id_ed25519.pub).
#   -Tag           name of the backup folder made on the deck (atmt_backup_<Tag>; default a
#                  timestamp), so several deploys keep separate backups.
#   -NoPause       do not wait for Enter at the end.
#
# If the deck does not know this machine's key yet, ssh asks for the deck's **password once**; this
# script then installs the key and every later call (and every later run) needs none. Run it in a
# visible window - that prompt is an ordinary ssh one and typing in it needs the console.
param(
    [string]$Target = 'deck@steamdeck',
    [string]$GameDir = '/home/deck/.local/share/Steam/steamapps/common/Trails of Cold Steel',
    [string]$IdentityFile = "$env:USERPROFILE\.ssh\id_ed25519.pub",
    [string]$Tag,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$stamp = if ($Tag) { $Tag } else { Get-Date -Format 'yyyyMMdd-HHmmss' }
# The tag becomes a folder name and a script argument on the deck: keep it to characters that are
# safe in both (a space in -Tag would need quoting on the way, which is exactly what this avoids).
$stamp = $stamp -replace '[^A-Za-z0-9._-]', '_'

# Every step asks ssh to fail instead of waiting on a missing prompt: after the key is installed
# there is nothing left to type, and a hang would look like a broken deck.
$sshCommon = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15',
               '-o', 'StrictHostKeyChecking=accept-new')

$remoteTarball = '/tmp/atmt_deck_payload.tar.gz'
$remoteRunner = '/tmp/atmt_deck_run.sh'
$remoteStage = '/tmp/atmt_stage'

$logs = Join-Path $root 'build\deck_deploy'
if (-not (Test-Path $logs)) { New-Item -ItemType Directory -Force $logs | Out-Null }
$logFile = Join-Path $logs "deck_deploy_$stamp.log"
Start-Transcript -Path $logFile -Force | Out-Null

function Say([string]$text) { Write-Host $text }

# Native tools (ssh/scp/tar) report through their exit code, and PowerShell 5.1 turns redirected
# stderr into a terminating error while $ErrorActionPreference is 'Stop'. These two wrappers relax
# that for exactly one call and leave the exit code in $script:ExitCode, so a failed step can be
# reported instead of killing the run with a stack trace.
$script:ExitCode = 0
function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments, [switch]$Quiet)
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($Quiet) { & $Exe @Arguments | Out-Null } else { & $Exe @Arguments }
        $script:ExitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
}

function Get-RemoteOutput {
    param([string]$Command)
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & ssh @sshCommon $Target $Command
        $script:ExitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    return ($out -join "`n")
}

Say "deck deploy  $stamp"
Say "  target:   $Target"
Say "  game:     $GameDir"
Say "  log:      $logFile"
Say ""

# ---------------------------------------------------------------- the payload and the manager
$manager = Join-Path $root 'dist\linux\atmt_manager'
if (-not (Test-Path $manager)) {
    throw "the Linux manager is not built - run: powershell -File tools\build_manager.ps1 -Linux -NoWindows"
}
$stage = Join-Path $logs "deck_payload_$stamp"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$stageArgs = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'stage_payload.ps1'),
               '-Out', (Join-Path $stage 'payload'))
& powershell @stageArgs
if ($LASTEXITCODE -ne 0) { throw "staging the payload failed" }
Copy-Item $manager (Join-Path $stage 'atmt_manager')
Say ("  manager  {0} KB  <- dist\linux\atmt_manager" -f [math]::Round((Get-Item $manager).Length / 1KB, 0))

$tarball = Join-Path $logs "deck_payload_$stamp.tar.gz"
if (Test-Path $tarball) { Remove-Item $tarball -Force }
Invoke-Native 'tar' @('-czf', $tarball, '-C', $stage, '.')
if ($script:ExitCode -ne 0) { throw "tar failed - no payload" }
Say ("  tarball  {0} MB  {1}" -f [math]::Round((Get-Item $tarball).Length / 1MB, 1), $tarball)

# ---------------------------------------------------------------- get in (key first, password once)
Say ""
Say "ssh $Target ..."
$probe = Get-RemoteOutput 'echo KEY_OK'
$haveKey = ($script:ExitCode -eq 0) -and ($probe -match 'KEY_OK')
if ($haveKey) {
    Say "  key login works, no password needed"
} else {
    $pub = $IdentityFile
    if (-not (Test-Path $pub)) {
        foreach ($alt in @("$env:USERPROFILE\.ssh\id_rsa.pub")) {
            if (Test-Path $alt) { $pub = $alt; break }
        }
    }
    if (-not (Test-Path $pub)) {
        throw "no public key found ($IdentityFile) - make one: ssh-keygen -t ed25519 -f `"$env:USERPROFILE\.ssh\id_ed25519`""
    }
    $keyLine = (([IO.File]::ReadAllText($pub)) -split "`n")[0].Trim()
    Say ""
    Say "  the deck does not know this machine's key yet."
    Say "  ssh is about to ask for the DECK's password - type it here, once."
    Say "  (it is installed as an authorised key for $($Target.Split('@')[0]), so nothing after this"
    Say "   prompt, and no later run of this script, asks again)"
    Say ""
    # The key is appended through a remote shell; grep -q keeps a repeated run from piling it up.
    $remote = "umask 077; mkdir -p ~/.ssh; touch ~/.ssh/authorized_keys; " +
              "grep -qxF '$keyLine' ~/.ssh/authorized_keys || printf '%s\n' '$keyLine' >> ~/.ssh/authorized_keys; " +
              "chmod 700 ~/.ssh; chmod 600 ~/.ssh/authorized_keys; echo KEY_INSTALLED"
    Invoke-Native 'ssh' @('-o', 'StrictHostKeyChecking=accept-new', '-o', 'ConnectTimeout=15', $Target, $remote)
    if ($script:ExitCode -ne 0) {
        throw "the key could not be installed on $Target - wrong password, or ssh is not reachable"
    }
    $probe = Get-RemoteOutput 'echo KEY_OK'
    if (($script:ExitCode -ne 0) -or ($probe -notmatch 'KEY_OK')) {
        throw "key login still does not work on $Target after installing the key"
    }
    Say "  key login installed and verified ($pub)"
}

# ---------------------------------------------------------------- upload and install
Say ""
Say "uploading the payload ..."
$scpArgs = $sshCommon + @($tarball, "$Target`:$remoteTarball")
Invoke-Native 'scp' $scpArgs -Quiet
if ($script:ExitCode -ne 0) {
    # OpenSSH 9 scp speaks sftp; if the deck's sshd has no sftp-server, the old protocol still works.
    Say "  scp failed, retrying with the legacy protocol (-O) ..."
    Invoke-Native 'scp' ($sshCommon + @('-O', $tarball, "$Target`:$remoteTarball")) -Quiet
}
if ($script:ExitCode -ne 0) { throw "the payload could not be copied to $Target" }

Say "unpacking on the deck ..."
Invoke-Native 'ssh' ($sshCommon + @($Target,
    "rm -rf $remoteStage && mkdir -p $remoteStage && tar -xzf $remoteTarball -C $remoteStage && chmod +x $remoteStage/atmt_manager"))
if ($script:ExitCode -ne 0) { throw "the payload could not be unpacked on the deck" }

# An ssh call is the one place where quoting is fragile: PowerShell 5.1 escapes embedded double
# quotes for a native command line, so the game path ("Trails of Cold Steel", with spaces) only
# survives if it is quoted on the deck's side. The arguments therefore go into a small runner
# script that ssh starts by name - a command line with no quotes in it cannot be mangled on the way.
$gameQuoted = "'" + ($GameDir -replace "'", "'\''") + "'"
$runnerText = "#!/usr/bin/env bash`n" +
              "# written by tools\deploy_deck.ps1 - this deploy's arguments, so ssh has to pass none`n" +
              "exec $remoteStage/atmt_manager --cli install --game-dir $gameQuoted --payload $remoteStage/payload " +
              "--tag '$stamp' --yes`n"
$localRunner = Join-Path $logs "deck_run_$stamp.sh"
[IO.File]::WriteAllText($localRunner, $runnerText, (New-Object System.Text.UTF8Encoding($false)))
Invoke-Native 'scp' ($sshCommon + @($localRunner, "$Target`:$remoteRunner")) -Quiet
if ($script:ExitCode -ne 0) { throw "the runner script could not be copied to $Target" }

Say ""
Say "installing (atmt_manager backs up, installs, verifies, rolls back on a failure) ..."
Say "----------------------------------------------------------------------"
Invoke-Native 'ssh' ($sshCommon + @($Target, "bash $remoteRunner"))
$installExit = $script:ExitCode
Say "----------------------------------------------------------------------"
if ($installExit -ne 0) { throw "the install on the deck failed (exit $installExit)" }

Say ""
Say "DONE  loader + mods installed on $Target"
Say "      game folder:  $GameDir"
Say "      backup:       $GameDir/atmt_backup_$stamp"
Say "      start the game from Steam; what the loader did is in <game>/atmt_loader.log"
Stop-Transcript | Out-Null
if (-not $NoPause) { Read-Host 'press Enter to close' | Out-Null }
exit 0
