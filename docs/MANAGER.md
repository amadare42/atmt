# ATMT Manager - the installer and configurator

`manager/` is one app that installs, updates and configures the loader and the mods, on the Steam
Deck (Desktop Mode and Game Mode, gamepad only) and on Windows: this is how it works, and how to build, test
and release it.

```
atmt_manager (SDL2 + Dear ImGui)          atmt_manager --cli ...
        |                                         |
        +---------- manager_core (no UI) ---------+
  locator  install  settings  payload  updater  sign  archive  http  icon_pack (falcom_pkg, p3a)  senpatcher
```

| file | what |
|---|---|
| `manager/src/core/locator.*` | finding the game: Steam (`libraryfolders.vdf`, Flatpak Steam, SD cards, `appmanifest_538680.acf`), GOG / Galaxy (registry), Heroic, Lutris, Bottles and other Wine prefixes; the Wine/Proton prefix (see "Where the game is found") |
| `manager/src/core/install.*` | install_core: backup, install, md5 verification, **rollback**, uninstall, mod toggles, backups/restore, `atmt_install.json` |
| `manager/src/core/settings.*` | settings_model: the schema, the inis through `shared/ini.h` (`WriteValues`: comments survive), raw keys, presets, conflict check, the queue for a running game |
| `manager/src/core/payload.*` | the components (`component.json`, `files/`, `schema.json`, presets, `supported_exe.txt`, `icon_pack.json`, `manifest.md5`) and the payload made of the newest of each |
| `manager/src/core/icon_pack.*` | the icon pack builder (see "The icon pack"): sources resolved like SenPatcher, the Pillow-exact Lanczos pre-shrink, `mods/order.txt`, staleness |
| `manager/src/core/falcom_pkg.*` | the game's `.pkg` packages and Falcom's type-1 compression |
| `manager/src/core/p3a.*` | SenPatcher's `.p3a` archives: read (raw, lz4, zstd) and write; zstd and xxh64 from `third_party/zstd` |
| `manager/src/core/updater.*` | the release manifest (`manifest.json` + signature), component downloads, app self-replace |
| `manager/src/core/sign.*` | Ed25519 in minisign's formats (Monocypher) |
| `manager/src/core/archive.*` | gzip/inflate + tar, so `<component>-<ver>.tar.gz` needs no zlib; zip (stored, deflated) for SenPatcher's release |
| `manager/src/core/senpatcher.*` | SenPatcher (see "SenPatcher"): its latest release from GitHub and running `SenPatcher.exe`; on Linux the game's Proton, the `dinput8` dll override, running it in the game's prefix |
| `manager/src/core/http.*` | WinHTTP; on Linux the system's libcurl through `dlopen` |
| `manager/src/cli.cpp` | the command line |
| `manager/src/gui/*` | the window: Status, Mods, Settings, Presets, System |
| `manager/payload/` | `components.json` (every component's version and title), `data/` (`presets/*.json`, `supported_exe.txt`, `install_rules.json`, `icon_pack.json`: the data component) |
| `tests/schema_dump.cpp` | `atmt_schema_dump`: every mod's `AtmtModDescription` and `AtmtSetting` table -> `dist/settings_schema.json` |

## Building

The mods first (they are the payload), then the app:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1           # the mods, + dist\settings_schema.json
powershell -ExecutionPolicy Bypass -File tools\build_manager.ps1   # dist\atmt_manager.exe + dist\payload\
powershell -ExecutionPolicy Bypass -File tools\build_manager.ps1 -Linux   # + dist\linux\ (Docker)
```

Build trees are `build\manager-win` and `build\manager-linux`; the layout of `dist\` is in [BUILD.md](BUILD.md).

* Windows: llvm-mingw's x86_64 compilers (the same package as the 32-bit mod build). One static
  exe; it is a console program so the CLI prints, and the window frees the console when it was
  started from Explorer.
* Linux: `manager/packaging/Dockerfile` (Debian bookworm, glibc 2.36) runs
  `manager/packaging/build_linux.sh`: `dist/linux/atmt_manager` (needs only glibc) and
  `atmt-manager-<ver>-x86_64.AppImage` with the payload inside (`usr/share/atmt_manager/payload`).
* SDL2 (`release-2.32.10`), Monocypher (`4.0.2`) and zstd (`v1.5.7`; only its common, compress and
  decompress sources are compiled, without the assembly decoder) are git submodules in `third_party/`,
  initialised by `tools\build_manager.ps1` (the Docker build needs them checked out on the host).
* `-NoGui` builds the core and the CLI only (no SDL2).
* The tests (`atmt_manager_test`) run in both builds: hashes, signatures, the archive reader, VDF,
  the settings model, the whole install cycle on a sandbox game folder (install, repair after
  Steam's file check, update keeping a disabled mod disabled, a damaged file rolled back,
  restore, uninstall, the unverified-build acknowledgement, the hard stops), and the icon pack
  (Falcom type 1, `.pkg` and `.p3a` round trips, the Lanczos against Pillow's bytes, a synthetic
  atlas end to end with a texture-mod archive and `original_art`, the ratio < 2 skip, `order.txt`,
  staleness, install / uninstall), and SenPatcher (the zip reader, GitHub's release answer,
  the copy here, Proton's choice and command line on a sandbox Steam, `user.reg` and launch
  options, the whole run with stand-in `proton` / runtime scripts on Linux; `RunAndWait` on Windows). Tests that install fail with "close the game first" while
  `ed8.exe` runs.

## Components

The manager, the loader, the data and every mod each have a version of their own and are released
and updated on their own: a release that fixes one mod carries that mod and nothing else, and the
app downloads only what is newer than what it has.

`manager/payload/components.json` is the list of what ships and the one place versions are set:

```
{"min_manager_version": "0.1.0",                 // the default for every component
 "loader": {"title", "version", "changelog"},
 "data":   {"title", "version", "changelog"},
 "mods":   [{"name", "title", "version", "changelog", "default_enabled"?, "min_manager_version"?}, ...]}
```

Bump a component's version when its files change (the release refuses a changed component with the
published version). A dll that is built but not listed there (a development build of a mod) is left
out of every payload, also the ones `tools\deploy.ps1`, `tools\deploy_deck.ps1` and
`tools\release.ps1` make. A developer who wants it copies the dll from `dist\game\atmt_mods` into
`<game>\atmt_mods` by hand; the manager then shows it like any other installed mod.

## The payload

`tools\stage_payload.ps1 -Out <dir>` puts the built files together, one folder per component:

```
loader/                    the loader
  component.json           {"name", "kind": "loader", "version", "title", "min_manager_version"}
  files/GFSDK_SSAO_D3D11.win32.dll
  schema.json              its part of the settings schema (the atmt_loader group)
  manifest.md5             every file of the component
data/                      kind "data": presets/, supported_exe.txt, install_rules.json,
                           icon_pack.json (manager/payload/data/), schema.json (the input names)
<mod>/                     kind "mod": files/atmt_mods/<mod>.dll, files/atmt_mods/<mod>.ini (its
                           documented template, when mods/*/<mod>.ini exists), schema.json (its
                           description and settings, cut from dist/settings_schema.json)
```

`files/` is what goes into the game folder, by its path there. Nothing in a component records when it
was built, and the mods link without a time stamp, so an unchanged component stages to the same
bytes: that is how a release tells what changed. Nothing made of game files ships: the icon pack
is built on the player's machine.

The app uses one component of each name: the newest one its own version may use
(`min_manager_version`) out of the bundled payload folders - `$ATMT_PAYLOAD`, `<exe>/payload`,
`<exe>/../share/atmt_manager/payload` (the AppImage) - and the downloaded components in
`%LOCALAPPDATA%\atmt_manager\components\<name>\<version>\` /
`~/.local/share/atmt_manager/components/<name>/<version>/`. `--payload <dir>` uses that folder's
components only. `atmt_install.json` records the version of every component installed, so Status
says which ones the app has newer ("Dialog log 0.1.0 -> 0.1.1") and offers Update.

### Presets

`manager/payload/data/presets/<id>.json`:

```
{
  "name": "PC",
  "description": "...",
  "recommended_on": ["windows"],          // part of "Install everything" there ("linux" / "windows")
  "mods": {"deckscreen": false},          // mods switched on / off (atmt_mods/ vs atmt_mods/disabled/)
  "icon_pack": false,                     // true: a small screen - applying it builds the icon pack
  "changes": [{"mod": "atmt_overlay", "section": "General", "key": "Mouse", "value": true}]
}
```

`changes` are ini values (`mod` is a settings_schema group, `atmt_loader` for the loader's ini).
`mods` switches whole mods - cleaner than a mod's own `Enabled=false`, since a switched-off mod is
not loaded at all; a mod that is not installed is left alone. Applying a preset (Presets screen,
`preset <id>`, `install --preset <id>`) writes both, and builds the icon pack when `icon_pack` is
true and it is not built or out of date (not one the player removed); "Install everything" leaves out the mods its
recommended presets switch off. Shipped: **PC** (Windows: deckscreen off, the overlay's mouse and
pointer on), **Steam Deck 800p** (Linux: deckscreen on, 1280x800, crisp text, anchored screens, the
mouse on for the touchscreen / trackpads, the icon pack), **No files written**.

## The settings schema

`atmt_schema_dump` loads each mod dll in a child process of its own with a stand-in `AtmtModApi`;
the table handed to `settings_register` is written out and the child exits right there, so nothing
the mod does after registering (hooking fixed addresses of `ed8.exe`) ever runs. The loader's own
`atmt_loader.ini` keys are described in the tool itself. Adding a setting to a mod makes it appear
in the app's Settings screen with no UI work; a key an ini has that the schema does not know shows
under "Advanced (raw)".

Before starting it, the child also reads the dll's `AtmtModDescription` export, so every mod - one
without settings too - has an entry in `"mods"`; that sentence is what the Mods screen shows under
the mod's switch (`components.json` only has the title, the version, `default_enabled`). The
staged payload splits it: each mod's `schema.json` has its own entries, the loader's its group, the
data's `input`; the app puts them back together:

```
{"schema_version": 3,
 "input":  {"keys": [...], "pad_buttons": [...], "pad_chord_separator": "+"},
 "mods":   [{"mod": "deckscreen", "description": "Runs the game at ..."}, ...],
 "groups": [{"mod": "deckscreen", "title": "Deckscreen", "ini": "atmt_mods/deckscreen.ini",
             "settings": [{"type": "bool", "flags": [...], "section", "key", "label", "help",
                           "default", "min", "max", "step", "choices", "max_length"}, ...]}, ...]}
```

## The command line

```
atmt_manager --cli status | find | install | uninstall | enable <mod> | disable <mod>
                   settings [<mod>] | set <mod> <section> <key> <value> | presets | preset <id> | reset [<mod>]
                   backups | restore <backup> | check-update | update | verify-payload
                   keygen <dir> | sign <key> <file> | verify-sig <pub> <file> | verify-manifest <pub> <file>
                   steam add|remove|status [--restart-steam]
                   icon-pack status|build|remove [--out <file>] [--config <json>]
                   senpatcher status|download|run|override
  --game-dir <dir>   --payload <dir>   --all   --mods a,b   --preset <id>   --tag <t>   --yes   --icon-pack   --no-icon-pack
  uninstall --purge  everything of the toolkit's (atmt_*: logs, output files, backups) as well
```

`--game-dir` defaults to the last folder used, else the one found (`find` lists every install and
where it came from); it may also name `ed8.exe` itself. `install` exits with 2 when
the exe is an unverified build and `--yes` was not given. The long form works too:
`atmt_manager --install --game-dir <dir> --mods a,b --preset steam_deck_800p --yes`.

## The developer flow

* `tools\deploy.ps1 -GameDir <game>` stages a payload and runs `atmt_manager --cli install` on it
  (`--mods` = everything but `-Disable`, `--yes`); `-Undeploy` runs `uninstall`.
* `tools\deploy_deck.ps1` stages a payload, copies it and `dist/linux/atmt_manager` to
  `/tmp` on the deck and runs the same install there over ssh (which builds the icon pack on the
  deck from its own textures, where SenPatcher is set up). There is one install code path; the window
  has no ssh deploy - that stays a developer tool.

## The icon pack

The game draws its small UI icons far below their texel size from atlases without mipmaps (the
battle status icons: 20x20 on the 720p canvas, from 64x64 cells of an HD pack's 1024x1024
`icons.png`), so they look jagged. The icon pack pre-shrinks those cells to their draw size and
stretches them back with nearest-neighbour, so the game's own bilinear sampling lands exactly on a
clean downscale (the same trick as the deckscreen mod's crisp text, [MODS.md](MODS.md#deckscreen)). It is made of
the player's own textures - often a third-party HD texture pack - so it cannot ship; the manager builds it on
their machine. Cause: the installed HD pack replaces the game's 512x512 `icons.png` atlas (32 px cells, 1.6x
minification) with a 1024x1024 one (3.2x, no mipmaps), so letters lose strokes - a 720p screen shows the same.
The `deckscreen` trace (`TraceDraws`) logs each quad's sampler, texture size and UV extent, which is how targets
are found: the UV size gives the grid, `m00 x 360` the draw size.

* **What**: `manager/payload/icon_pack.json` (in every payload; its `_doc` explains the fields)
  lists the targets: packages, the texture in them, the cell grid, the draw size, which rows /
  columns, `original_art`. The code (`manager/src/core/icon_pack.*`) is a generic "pre-shrink cells
  of uncompressed RGBA PhyreEngine atlases inside `.pkg` packages into a SenPatcher `.p3a`" builder;
  it knows no texture.
* **From what**: each package from where SenPatcher would load it - the first archive in
  `mods/order.txt` (then the unlisted `mods/*.p3a`, SenPatcher's own `zzz_senpatcher_*` last) that
  has it, the pack itself left out, else the game's data folder. Archive entries stored raw, lz4 or
  zstd are read; an archive that has the package but cannot be read skips that package (a lower
  source would cover the texture mod's art).
* **How**: Pillow's `LANCZOS` exactly (premultiplied alpha, separable, support 3, 22-bit fixed-point
  coefficients, the horizontal pass first), nearest stretch-back (numpy's half-to-even rounding);
  a texture whose cells are drawn at less than half their size is left alone. The Windows and Linux builds
  write byte-identical packages (checked against Pillow's resampling on a real game with SenPatcher and an HD
  pack). A target with `enlarge` first doubles an atlas whose cells are under 2x the draw size (the game's own
  512px `icons.png`: 32px cells at 20px) - each texel repeated, the header's pixel byte count, log2 size and
  height/width patched, the only fields that differ from an HD pack's 1024px one - and takes the pictures from
  the cells as they were, so the game's own textures get the same status icons as an HD pack. Written as `mods/atmt_icon_pack.p3a` (version 1100, zstd level
  6: about 34 MB, ~1 s), first in `mods/order.txt` (`mods/` and `order.txt` are created when
  SenPatcher's DLL is there without them).
* **When**: only where SenPatcher's `mods/` loader is set up (its `DINPUT8.dll`, or a
  `mods/order.txt`) - without it nothing reads the pack. a new pack only comes from a preset with
  `"icon_pack": true` (the Steam Deck one: applied, or recommended under Install everything),
  `install --icon-pack` or Status; after that Install / Update / Repair rebuild the pack that is there
  (a log line; a failure is a note, never a rollback; `--no-icon-pack` leaves it); Status shows **Icon pack: built / out of date (texture mods changed) / not built / not
  needed / not available (needs SenPatcher)** with **Rebuild icon pack**; `icon-pack build|remove|status`
  on the command line (`--out <file>` builds elsewhere and only reads the game folder). Never while
  the game runs. "Nothing to pack" (every target below ratio 2 and without `enlarge`) removes
  an older pack and is not an error.
* **Who needs it**: small screens only (the Deck's 1280x800, 720p) - with an HD texture pack the icons
  are shrunk far enough to lose strokes; with the game's own textures (enlarged) the gain is smaller. On 1080p and bigger they are drawn larger and the
  pack only softens them, so the Status row says so and offers **Remove icon pack**. Removing it
  (GUI, or `icon-pack remove`) is remembered in `atmt_install.json` (`"icon_pack_removed": true`,
  kept across installs): installs leave it out and Status shows **removed** until it is built by
  hand again (**Build icon pack** / `icon-pack build`), which forgets the removal.
* **Staleness**: `mods/atmt_icon_pack.json` records what it was built from - per package the
  archive that provided it (name, size, modification time) or the data folder's file, and the hash
  of the payload's target list. A texture mod added, removed, reordered or replaced over a targeted
  package, or a payload with other targets, shows it as out of date; an unrelated archive does not.
* **Removal**: the pack and its record are `mods/atmt_*`, which backups, restores, Uninstall and
  Remove everything treat as the toolkit's (its `order.txt` line goes too).

## SenPatcher

[SenPatcher](https://github.com/AdmiralCurtiss/SenPatcher) (AdmiralCurtiss's; not part of the toolkit)
fixes the game and loads `.p3a` mods - the icon pack needs it. It has no Linux release and needs none:
its window (`SenPatcher.exe`) runs under Proton like the game, and installs its `DINPUT8.dll` next to
`ed8.exe` as on Windows. On Linux, for the Steam version, Status has **Run SenPatcher**
(`senpatcher run` on the command line; `manager/src/core/senpatcher.*`):

1. **The latest release, never a pinned one**: `api.github.com/repos/AdmiralCurtiss/SenPatcher/releases/latest`,
   its `SenPatcher*.zip` asset, checked against its size and the sha256 GitHub records for it
   (`digest`; without one only HTTPS vouches for it, and the log says so). Downloaded only when the tag
   is newer than the copy here; offline, the copy here is used. Kept in
   `<DataDir>/senpatcher/<tag>/` (`~/.local/share/atmt_manager/senpatcher/`): `SenPatcher.exe`, its
   license and readme, and `Trails of Cold Steel/` (the CS1 hook dll SenPatcher installs from there);
   the other games' folders stay in the zip. `atmt_senpatcher.json` is written last (a copy without it
   is unfinished); older copies are removed. `senpatcher download` does only this.
2. **The game's Proton**: Steam's `config/config.vdf` `CompatToolMapping` - the game's (538680), else
   Steam's default (`0`) - matched against Valve's Protons in the libraries' `steamapps/common`
   (`Proton 9.0 (Beta)` is `proton_9`, `Proton - Experimental` is `proton_experimental`) and the
   custom ones in `compatibilitytools.d` (`compatibilitytool.vdf`); else the newest of Valve's numbered
   ones. The command line comes from the tools' `toolmanifest.vdf`, the way Steam starts a game: the
   Steam Linux Runtime the Proton asks for (`require_tool_appid`, found by its appmanifest) around
   `proton waitforexitandrun`, with Steam's `STEAM_COMPAT_*` environment (data path
   `compatdata/538680`, client, tool, library and install paths). `SteamGameId` is left as it is:
   in Game Mode the manager's own lets gamescope show SenPatcher's window. `senpatcher status`
   prints the choice and the command.
3. **The dll override**: Wine loads its own `dinput8` unless told otherwise, so
   `reg.exe add HKCU\Software\Wine\DllOverrides /v dinput8 /d native,builtin` runs through that
   Proton first (creating the prefix when the game never ran). `senpatcher override` (and Status's
   **Set dll override**, shown when SenPatcher is installed and nothing sets it) does only this. Set
   is: the prefix's `user.reg`, or `WINEDLLOVERRIDES=...dinput8=n,b...` in the game's launch options
   (`userdata/*/config/localconfig.vdf`, read only).
4. **SenPatcher's window**: its game folder setting (`SenPatcherGui/gui.ini` in the prefix's
   AppData) is pointed at the game as a Wine path (`Z:\...`) when it has none, then `SenPatcher.exe`
   runs in the game's prefix; the manager waits (its busy screen) until it is closed, then shows whether
   the game is patched. Proton's output: `<DataDir>/senpatcher/proton.log`.

Other Linux installs (Heroic, Lutris, Bottles) say what to do instead: run `SenPatcher.exe` in the
game's Wine prefix and set the override there.

On Windows, for any store's game, **Run SenPatcher** does steps 1 and 4 only: the same download, its
`gui.ini` (`%LOCALAPPDATA%\SenPatcherGui\gui.ini`) pointed at the game folder when it has none, and
`SenPatcher.exe` started as it is, waited for. No override is needed: Windows loads the `DINPUT8.dll`
next to `ed8.exe` by itself, and `senpatcher override` says so.

## Releases

Once:

1. A **public** GitHub repository for the releases (a private repo's assets need a token).
2. A signing key, kept off the repository and off CI:
   ```powershell
   dist\atmt_manager.exe --cli keygen $env:USERPROFILE\.atmt
   copy $env:USERPROFILE\.atmt\atmt_release.pub manager\release_pubkey.txt   # commit this one
   ```
   The public half is compiled into the app (`manager/CMakeLists.txt` reads
   `manager/release_pubkey.txt`); without it, or without `-Repo`, the app's update checks are off.

Each release: bump the versions of what changed (`manager/payload/components.json`; the app's in
`manager/CMakeLists.txt`, `ATMT_MANAGER_VERSION`), then

```powershell
powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Repo owner/name            # build, pack, sign
powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Repo owner/name -Upload    # + the GitHub release
```

`-BumpChanged` gives a component that changed under its published version the next patch version
in `components.json` (and builds the app again with it) instead of stopping. `-Upload` creates the
release as a draft, checks that every file arrived whole and that every earlier release's download
the manifest keeps is still there, and only then publishes it as the latest: the apps never see a
manifest whose downloads are not all in place.

It reads the published `manifest.json` (the latest release's, or `-Previous <file>`; `-Previous
none` for the first release) and writes the next one into `dist\release\<tag>\` (`-Tag`, default
`r<yyyy.MM.dd.HHmm>`):

* a component whose version is new: `<name>-<version>.tar.gz` (the staged folder) in this release,
  and its entry points there;
* a component whose version is the published one: the published entry, unchanged - its download
  stays in the release it came with. If its files differ from the published ones (`content`, the
  sha256 of its `manifest.md5` without the `component.json` line, so a new title or
  `min_manager_version` is not a change) the release stops: bump its version (or `-BumpChanged`);
* a version lower than the published one stops it too;
* always: the AppImage, the Windows zip and bare exe (they carry the current payload), the
  `Install ATMT Manager.desktop` launcher (browsers drop the AppImage's exec bit; the launcher sets
  it and starts it), `notes.md` (the release's text: what is new and every version);
* `manifest.json` and `manifest.json.sig` (minisign's format, so
  `minisign -Vm manifest.json -p manager/release_pubkey.txt` checks it too; the script checks it with
  `atmt_manager --cli verify-manifest`).

```
{"format": 1, "published": "...", "page": "https://github.com/<repo>/releases/tag/<tag>",
 "manager": {"version", "changelog",
             "downloads": {"windows": {"url", "sha256", "size"}, "linux": {...}}},
 "components": [{"name", "kind", "title", "version", "min_manager_version", "changelog",
                 "url", "sha256", "size", "content"}, ...]}
```

What the app does with it: it reads `https://github.com/<repo>/releases/latest/download/manifest.json`
(the URL is compiled in: `-DATMT_RELEASE_REPO`, or `-DATMT_MANIFEST_URL` for another one;
`state.json`'s `update.manifest_url` overrides it, the key cannot be) and the same URL + `.sig`, at
most once a day (`If-None-Match`, "Check now" in System), quiet when offline. The signature must
verify with the compiled-in key; every download must match its sha256 in the manifest. A manifest
`published` before the newest one already seen from the same URL is refused (an old, validly signed
manifest served again would hide every update). Each
component is compared with the one the app uses: a newer one is offered (Status lists them with
their changelogs), a new one too, one whose `min_manager_version` is above the app is shown as
needing the app update first. "Download and install the updates" fetches only those components
into the components folder (a version not newer than one already there is refused; older
downloads of it are removed) and installs (backup, verify, rollback as always); installing asks
unless "Install updates automatically" is on. A newer app version is offered on its own: the
AppImage replaces itself in place (the old one stays as `.prev` for one run); the Windows exe
renames itself to `.old` and the next start deletes it.

## On the Deck (player instructions)

1. Desktop Mode: download the AppImage and `Install ATMT Manager.desktop` into the same folder and
   open the launcher (or: Properties > Permissions > "Is executable" on the AppImage).
2. On its first start the app offers **Add to Steam** (also System > Steam library): Steam closes
   for a moment and starts again with "ATMT Manager" under Non-Steam, its Deck controller layout set
   to "Gamepad with Mouse Trackpad" (`configset_controller_neptune.vdf` in Steam's `Steam Controller
   Configs/<account>/config`, keyed by the lower-case name, as Steam ROM Manager does it; a layout the
   player chose is kept). Remove from Steam undoes both.
3. Press **Install everything (recommended)**.
4. In Game Mode: LB/RB switch screens, A selects, B backs out. Text fields open Steam's keyboard.
   The right trackpad moves a pointer; pressing it clicks (the layout sends it as R3, and the app
   clicks with R3 while the pointer moved last - a pad button, a key or the left stick give control
   back to the pad's focus). The right stick scrolls what is under the pointer. The touchscreen taps.

The app never touches the game's launch options. It changes the game's Proton prefix only when
asked to run SenPatcher (see "SenPatcher"): the `dinput8` dll override and SenPatcher's
own `gui.ini`.

## What the screens do

* **Status** - the game (and the store it came from), the exe check, the icon pack (with **Rebuild
  icon pack** where SenPatcher is set up), SenPatcher (installed, on Linux the dll override; **Run
  SenPatcher** and, on Linux when needed, **Set dll override**), and **Install everything
  (recommended)**: every mod on but what the presets the payload marks `recommended_on` this platform
  switch off, plus those presets' settings (the Deck preset on Linux, the PC preset on Windows). Once
  installed, the version's Install/Update/Repair also offers to keep the current choices.
* **Mods**, **Settings**, **Presets**: key and pad settings are pickers built from
  the schema's `input` names (the mods' own parser), never from a list in the app.
* **System** - Steam library (add / remove / update the non-Steam entry; Steam is closed for the
  change and started again, which needs Desktop Mode), updates ("Check for updates automatically" is
  on by default, "Install updates automatically" off), backups, the two uninstall levels (mods and
  settings, backed up / everything, nothing kept), and the logs.

## Where the game is found

Every store installs the same folder layout (`ed8.exe` next to `GFSDK_SSAO_D3D11.win32.dll`), so an
install is recognised by `ed8.exe`, in the folder a store or launcher names or one folder below it:

| | Windows | Linux |
|---|---|---|
| Steam | `libraryfolders.vdf` + `appmanifest_538680.acf` | the same, native / Flatpak, SD cards; prefix `compatdata/538680/pfx` |
| GOG, GOG Galaxy | `HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\<id>` `path`; `C:\GOG Games\*`, `GOG Galaxy\Games\*` | through Heroic, Lutris or a Wine prefix |
| Heroic | `%APPDATA%\heroic` | `~/.config/heroic` (and Flatpak): `gog_store/installed.json`, `sideload_apps/library.json`; the prefix from `GamesConfig/<app>.json` `winePrefix` |
| Lutris | - | `games/*.yml` (`exe:`, `prefix:`) in `~/.config/lutris`, `~/.local/share/lutris`, Flatpak |
| Wine prefixes | - | Bottles' bottles, `~/Games/*`, Heroic's prefixes, `~/.wine`: `drive_c`'s GOG / Program Files / Steam folders |

The game is sold on Steam and GOG only. A folder picked by hand always works
(Browse... in the window outside Game Mode, a typed path, or `--game-dir`); it only needs
`ed8.exe`. GOG builds of `ed8.exe` are not in `supported_exe.txt`, so installing on them
asks for the one confirmation every unverified build needs. Only "Add to Steam" needs Steam: without
a Steam account it is disabled (the CLI says so), and the rest works the same.

## No mod in the app

The app knows the loader's layout (the proxy dll and its `.orig`, `atmt_mods/`, `atmt_loader.ini`), the
toolkit prefix `atmt_` and the game (`ed8.exe`; Steam app 538680, GOG product 2029703882). Everything about a mod comes from
the payload: titles and versions (`component.json`), descriptions and settings (`schema.json`, from
the mods themselves), presets, the
leftovers of earlier versions (`install_rules.json`: only the toolkit's own files - a rule whose first
path part does not start with `atmt_`, or that is rooted or has `..`, is refused, so no rule can touch
the game's or another mod's files), the icon pack's targets (`icon_pack.json`).

Every install, uninstall and restore backs up first (`atmt_backup_<tag>/`) and, once it has
succeeded, keeps only the newest two backups (`kKeepBackups`). An uninstall that cannot put the
game's own `GFSDK_SSAO_D3D11.win32.dll` back keeps `.orig`, so the loader still has something to
forward to.

## Logging

The loader writes `atmt_loader.log` with `LogLevel=errors` by default: only failures, and the file is
only created when there is one. Mods report failures with `api->log_error` and everything
else with `api->log` (`LogLevel=all`). Each mod's own diagnostics settings are off by default. The
"No files written" preset sets `LogLevel=off` as well.

## Not done / open

* Hardware: the Game Mode layout, Steam's on-screen keyboard (`steam://open/keyboard`) and the
  Gamepad with Mouse Trackpad layout (set on Add to Steam) are untested on a real Deck.
* No release repository and no signing key exist yet, so update checks are off in every build
  (see Releases). `tools\release.ps1` has only been dry-run (a throwaway key, no upload).
* The CI workflow (`.github/workflows/manager.yml`) has not run yet.
* **Run SenPatcher** is tested against a sandbox Steam with stand-in `proton` and runtime scripts and
  the real release zip, not yet with a real Proton on a Deck. It covers the Steam version only;
  Heroic, Lutris and Bottles installs get the manual steps.
