# ARCHITECTURE.md - the loader / mod split

The project is two kinds of dll:

```
loader/          the hook host. Knows how to get into ed8.exe and how to load mods.
mods/<name>/     one feature each. Knows about the game.
shared/mod_api.h the contract between them. shared/ini.h is a small settings reader.
```

Nothing in `loader/` mentions Trails of Cold Steel; nothing in a mod has to figure
out where the game is or how to hook something.

## 1. The loader

**Job 1: get into the process.** Two built flavours of the same sources:

| file | how it starts | when to use it |
| --- | --- | --- |
| `GFSDK_SSAO_D3D11.win32.dll` | the game imports it from its own folder, so Windows loads it at startup (the original library is renamed next to it) | normal play: nothing to run, nothing modified |
| `atmt_loader.dll` | injected by path with `atmt_inject` (both in `dist/tools/`) | development against a game that has no proxy installed |

Which import is safe to impersonate is a measurable question, not a preference: a
proxy dll must export everything every module in the process imports from that name.
`atmt_modules --imports <dll>` answers it against the live process. Measured here:
`GFSDK_SSAO_D3D11.win32.dll` → `ed8.exe` alone; `winmm.dll` → seven modules
(Steam's overlay, Steam, XAudio2, the NVIDIA driver), which is why the winmm proxy
crashed the game and is not offered any more.

**Job 2: provide services.** On `DllMain` it spawns one thread, reads
`atmt_loader.ini`, finds the mods and loads them. A mod gets:

```
version, size            api version negotiation (see below)
game_module, game_dir    ed8.exe and its folder - where user-visible files go
mod_dir, mod_name        the mod's own folder and base name - where assets go
config_path              <mod_dir>\<mod_name>.ini, created on first run
log_path, log()          one shared line-based log: <game>\atmt_loader.log
hook_create/enable/      the shared hook engine (MinHook, one instance per process);
  disable/remove         it refuses addresses that are not executable code
settings_register/...   the settings registry: a mod declares its settings as a
                         table, the loader fills them from the mod's ini, a UI lists
                         and changes them, the loader writes changes back in place
service_publish/find     one mod's interface for the others, by name (the overlay
                         mod publishes "atmt.overlay": shared/overlay_api.h)
```

**Services.** A mod that offers something to other mods publishes an
interface (a struct of function pointers) under a name; others look it up with
`service_find`, and ask again later if it is not there yet (mods load one after the
other, in no promised order). The loader drops a mod's services when the mod goes.
The one service today is the overlay (`mods/overlay`): the Present hook, ImGui, the
mouse and the input hooks exist once per process, and a mod that wants a window on
screen - the dialog log's panel - adds one through it (docs/OVERLAY.md).

**Settings.** A mod hands `settings_register` a table of `AtmtSetting`
(type, ini section/key, label, help, range, pointer to the mod's own storage,
optional `on_change`). On registration the loader reads the ini into that storage,
clamped, and adds any key the ini lacks (with `help` as a comment above it), so
the file always lists every setting. A UI - the overlay's top bar - uses
`settings_acquire`/`settings_release` to enumerate every table and `settings_set`
to change a value; `settings_commit` writes the changed values back with
`atmt_ini::WriteValues`, which replaces a line's value and leaves its comment and
every other line alone. The registry lives in the loader so load order does not
matter, and so a mod's table is dropped (after saving what is unsaved) when the
mod shuts down or refuses to start - a UI never points into a gone dll. The
details, threads included, are in `shared/mod_api.h`; the tests are
`tests/test_settings.cpp`.

**Job 3: stay honest.** A mod that throws, refuses the api or asks for a newer
version is logged and unloaded - one broken mod must not take the game down. On
shutdown, mods get `AtmtModShutdown`, except while the process is exiting (calling
into a mod then can deadlock on the loader lock).

## 2. A mod

A dll in `atmt_mods` exporting these functions:

```c
uint32_t       AtmtModInit(const AtmtModApi* api);  // ATMT_MOD_API_VERSION, or 0 to opt out
void           AtmtModShutdown(void);              // optional
const char*    AtmtModDescription(void);           // what it does, one UTF-8 sentence
```

`AtmtModDescription` is the one place a mod says what it is for. It is an export, not an api
call, so it can be read without starting the mod and from a mod with no settings: the loader
hands it out with the mod's settings (`AtmtSettingsGroup::description`, shown at the top of the
mod's menu in the overlay's bar), and `tests/schema_dump.cpp` copies it into
`settings_schema.json` (`"mods"`), where the manager's Mods screen reads it.

Rules that matter in practice:

* **Do not block `AtmtModInit`.** It runs on the loader's thread; do the real work on
  a thread of your own.
* **Never let an exception cross the boundary.** Plain C types, `__cdecl`, 32-bit.
* **Hooking goes through `api->hook_*`**, not your own MinHook: one instance per
  process, one safety check (an address must be committed executable code - a stale
  address used to be a crash), one log line when it fails.
* **Check the game's code before touching it.** Every fixed address a mod hooks, calls or
  patches is compared with the supported exe's bytes first (`shared/code_check.h`); on a
  mismatch the mod logs an error and installs nothing. That covers another build (ed8jp.exe, a
  game patch, an exe an old SenPatcher patched on disk) and other patchers.
* **SenPatcher (v1.x)** coexists: its `DINPUT8.dll` patches ed8.exe in memory only (the md5 on
  disk stays the supported one) inside its DllMain, i.e. before the game's entry point, while
  the mods load after it (the loader's thread cannot run before process initialisation ends).
  None of its CS1 patch sites (v1.3.1 and its current sources) overlaps a site a mod uses,
  except the camera sensitivity patch in its sources after v1.3.1, which detours the same
  `fld` as `stick_rotation_speed` at 0x0053c874; that mod recognises it and chains after it.
  A SenPatcher older than v1.0 patched ed8.exe on disk: the manager reports an unverified
  build and says how to restore it.
* **Paths:** write where the user expects it (`api->game_dir`), keep your own files
  in `api->mod_dir`.
* **Never ship game content.** A mod reads what it needs from the game at run time (the dialog
  mod takes the text from the game's own message setter; the manager builds the icon pack from the
  player's own textures), so nothing of the game is redistributed and a game update cannot leave
  stale copies behind.
* **Version negotiation:** return the version you need. If the loader implements
  less, it unloads you; that is better than a mod that half-works.

`mods/trails_dialog_logger` is a full reference implementation, and `mods/_template` is a
skeleton to copy.

## 3. Adding a mod (checklist)

1. `cp -r mods/_template mods/<your_mod>`, rename the source and the target name.
2. `add_subdirectory(mods/<your_mod>)` in the top-level `CMakeLists.txt` and add the target to
   the list in `tools\build.ps1`.
3. `tools\build.ps1` - the dll lands in `dist/game/atmt_mods/`.
4. List it in `manager/payload/components.json` (title, version) so it ships; a dll that is not
   listed stays out of every payload. Bump its version there whenever it changes: each mod is
   released and updated on its own ([MANAGER.md](MANAGER.md#components)).
5. `tools\deploy.ps1 -GameDir "<game>"` installs everything the payload lists.
6. Start the game; with `LogLevel=all` in `atmt_loader.ini`, `atmt_loader.log` says whether your mod started.
7. Disable it without touching the game: the manager's Mods screen, or move the dll to `atmt_mods\disabled`.

## 4. Loader settings (`atmt_loader.ini`, game folder)

```ini
[Loader]
Enabled=true              ; master switch for the whole loader
ModDir=atmt_mods           ; relative to the loader dll, or an absolute path
Mods=                     ; empty = every *.dll in ModDir, alphabetical
LoadDelayMs=0             ; wait before loading (some games prefer late attach)
LogLevel=errors           ; atmt_loader.log: errors (only failures; no file when none) | all | off
```

Each mod owns its settings in its own ini (`<mod_dir>\<mod_name>.ini`).

Logging: `api->log` is informational and reaches `atmt_loader.log` only with
`LogLevel=all`; `api->log_error` is for failures and is written unless `LogLevel=off`. The loader
opens the file with the first line it writes, so with the default (`errors`) a session where nothing
fails leaves no log at all. Each mod's own diagnostics settings are off by default.

## 5. Why the split is worth it

* The dangerous part (being loaded early, forwarding a game's import, keeping the
  process alive) is small, boring and written once - 758 KB instead of 6 MB, and it
  contains no game knowledge.
* A new mod is a dll plus one CMake line: no proxy code, no path guessing, no hook
  engine, no version of itself to maintain.
* Failures are isolated and visible: the loader log says which mod started, and a
  mod that misbehaves is unloaded rather than half-running.
* A mod can be disabled by deleting its dll - there is no registration step.
