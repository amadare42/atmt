# Building, testing, deploying

`ed8.exe` is a 32-bit binary, so the loader and every mod are 32-bit dlls. The manager (`manager/`) is
a normal 64-bit program and a project of its own.

## Where things go

```
build/                  intermediates only (gitignored)
  game/                 the 32-bit tree: loader, mods, tests
  manager-win/          the manager for Windows
  manager-linux/        the manager for Linux (Docker)
dist/                   everything usable that came out of a build (gitignored)
  game/                 drop-in layout for the game folder
    GFSDK_SSAO_D3D11.win32.dll      the loader (proxy flavour; this is what ships)
    atmt_mods/*.dll                 the mods
  tools/                atmt_loader.dll (injectable flavour), atmt_inject.exe - development only
  settings_schema.json  every mod's settings table (written by tools\build.ps1)
  payload/              the staged manager payload (what an install is made of)
  atmt_manager.exe      the Windows manager; finds payload\ next to it
  linux/                atmt_manager (bare binary, what deploy_deck runs) and the AppImage
  release/<tag>/        tools\release.ps1's output
third_party/            git submodules: MinHook, Dear ImGui, SDL2, Monocypher, zstd (pinned by commit)
```

Nothing built is ever written anywhere else; `git status` stays clean after a build.

## The mods and the loader

Needs CMake, git and llvm-mingw (`winget install MartinStorsjo.LLVM-MinGW.UCRT`), 32-bit target.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1             # build, write the schema, run the tests
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -SkipTests
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Clean      # start from an empty build tree
```

The third-party sources are git submodules (`third_party/`, shallow, pinned by commit): clone with
`git clone --recursive`, or let the build scripts run `git submodule update --init --depth 1` for what they need.
To update one, check out the new tag inside its folder and commit the changed submodule pointer. `-static` links keep every dll free
of libc++/libunwind dependencies, so they run on a plain Windows install.

### Tests (no game needed)

| test | covers |
| --- | --- |
| `atmt_selftest` | the dialog logger's pure logic: the ini reader, text helpers, the log sink, the panel's ring of lines, key names |
| `atmt_al_test` | everything that decides what `load_last_save` does: the `-al` switch, which files are saves, which is newest, the options |
| `atmt_settings_test` | the loader's settings registry and the in-place ini writer |
| `atmt_loader_test` | the loader and a mod in a sandbox folder, exactly as the game sees them: exports, forwarding to the renamed original, mod discovery and start |

Heuristic antivirus scanners sometimes refuse to start `atmt_loader_test.exe` (it loads a dll by name and
writes logs). `build.ps1` reports that as a warning, not a failure; add an exclusion for `build\` or use
`-SkipTests`.

The other harnesses (`atmt_modules`, which lists which modules of a live process import a given dll - the census
that decides which dll a proxy may safely replace; `atmt_schema_dump`) are built into `build/game/tests`.

## The manager

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_manager.ps1                 # Windows: dist\atmt_manager.exe + dist\payload
powershell -ExecutionPolicy Bypass -File tools\build_manager.ps1 -Linux          # + dist\linux (Docker)
powershell -ExecutionPolicy Bypass -File tools\build_manager.ps1 -NoGui          # core + CLI only, no SDL2
```

Run `tools\build.ps1` first: the payload is made of its output. More in [MANAGER.md](MANAGER.md). Its install
tests refuse to run while `ed8.exe` is running.

## Installing a build into a game

```powershell
powershell -ExecutionPolicy Bypass -File tools\deploy.ps1 -GameDir "<game folder>"            # install
powershell -ExecutionPolicy Bypass -File tools\deploy.ps1 -GameDir "<game folder>" -Undeploy   # revert
powershell -ExecutionPolicy Bypass -File tools\deploy_deck.ps1 -Target deck@<address>          # a Steam Deck, over ssh
```

Both stage a payload and run `atmt_manager --cli install` on it: the same backup, verification and rollback a
player's install has. The game must be closed (a loaded dll is locked).

## Iterating on a running game

`tools\dev_cycle.ps1 -GameDir "<game folder>" [-Test]` builds the dialog-log mod, puts it into
`<game>\atmt_mods\dev` under a fresh name and writes the path into `<game>\atmt_reload.txt`; with `DevReload=true`
the running build hands the process over to it (the panel and the message hook move, the overlay keeps
running). Handover copies live in `atmt_mods\dev` on purpose: the loader loads every `*.dll` directly inside
`atmt_mods`, and a stray copy would run as a second instance of the mod.

For a game that has no proxy installed, `dist\tools\atmt_inject.exe --pid <pid> --dll dist\tools\atmt_loader.dll`
loads the loader by injection (the injector must be 32-bit for the same reason the dlls are).

## Releases

`tools\release.ps1` builds a release, packs the components whose version is new and writes the signed
`manifest.json`; see [MANAGER.md](MANAGER.md#releases).

## Continuous integration

`.github/workflows/manager.yml` builds and tests the manager on Linux (the Docker image of
`manager/packaging`) and on Windows (llvm-mingw x86_64). The 32-bit mods are not built in CI.
