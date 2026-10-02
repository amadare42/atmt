# The mods

Each mod is a dll in `atmt_mods/` (contract: [ARCHITECTURE.md](ARCHITECTURE.md)). All of them check the
bytes of every address they touch against the supported `ed8.exe` before installing anything
(`shared/code_check.h`). Addresses are plain image VAs - the exe has no ASLR - and the facts behind them are in
[ENGINE_NOTES.md](ENGINE_NOTES.md). Every setting listed here also appears in the overlay's settings bar and
the manager's Settings screen; the authoritative table is the mod's own source.

| mod | what it does |
| --- | --- |
| `atmt_overlay` | the UI over the game: settings bar and a window host for other mods ([OVERLAY.md](OVERLAY.md)) |
| `trails_dialog_logger` | records every dialog line with its speaker; shows the log in the overlay |
| `deckscreen` | native 16:10 rendering on the Steam Deck with the 16:9 UI laid out 1:1; crisp small text |
| `stick_rotation_speed` | right-stick camera speed and the camera's auto-rotation |
| `load_last_save` | loads the newest save from the title (`-al`) |
| `battle_load` | the game's own load menu in battle, on the field and during events |

## trails_dialog_logger

One hook does the capture: the game's "set the message's run" call, `0x00701E50` (thiscall; `ecx` = the message
state, argument = the run pointer). The speaker is the name plate in the state (`+0x358`), the text is the run's
bytes. Both come from the game in the same frame - nothing is searched, matched or indexed, which is why speakers
the scripts cannot name (`Harison`, `Hanna`, "Girl's Voice") come out right.

* **Where a run starts.** The pointer has two shapes and the byte at it says which: `0x11` is the `11 <u32>` text
  opcode (the bytes start 5 in); anything else is the run itself. A run can never start with `0x11`, so this is
  decided from the data, not configured.
* **Pages.** A message holds several pages (`02 03` breaks) behind one pointer and the game advances through them
  itself, so the setter is not called again. The hook walks the message once, then holds pages 2+ until the box's
  renderer shows them: message state `+0x32C` is the run, `+0x330` how far the text is typed, `+0x334` where the
  page on screen starts. A page is released once either has moved past its break; whatever is still held goes out
  when the next message is set, so nothing is dropped.
* **Deduplication.** A run is set again while a box is measured and redrawn, and the game reuses buffers for
  different lines, so repeats are keyed on pointer *and* content over a one-second window.
* **Name plate timing.** The first line of a box is parsed a few milliseconds before the name is set; a line without
  a speaker waits `PendingSpeakerMs` and re-reads the plate from the same object.
* **Output.** Lines are always kept in memory (the panel's ring). `LogToFile=true` also writes
  `atmt_dialogs.jsonl` and `atmt_latest.txt` next to the game (`tools/export_log.py` turns the former into
  Markdown/HTML, `tools/tail.ps1` follows the latter). With it off the mod writes no files at all.
* **The panel** draws in the overlay with the game's own font (`.itf`, read from the game's memory so the
  heart and other symbols work) and ImGui's word wrap. The dll carries its own copy of ImGui, so it must call
  `ImTextInitClassifiers()` before wrapping - otherwise the character table is zero-filled and every character
  counts as a blank. Keys and sizing: [OVERLAY.md](OVERLAY.md).

Settings: `OverlayKey` (F3), `OverlayPadToggle`, size/position/opacity/dim, `OverlayGameFont`,
`OverlayNewestFirst`, `OverlayCapture`, `OverlayLines`, `LogToFile`, `Diagnostics`, `DevReload`. The documented
template is `mods/trails_dialog_logger/trails_dialog_logger.ini`.

## deckscreen

Runs the game at the screen's native size (the Deck's 1280x800) without the 10% black bars of the game's own
16:9 handling, and lays the UI out exactly as it is at 1280x720, centered.

* **The mechanism.** The UI is authored for a 1280x720 canvas and placed into an NDC space whose y spans the
  canvas and whose x carries a 16:9 factor the shader divides back out with the camera's aspect. At 16:10 the
  viewport stretches that uniformly by 800/720, so it overflows left and right. The fix is one *uniform* scale in
  NDC, `fit = min(W/1280, H/720) / (H/720)` (0.9 at 1280x800), applied to x and y, position and size together,
  on a private copy where each path builds its transform. Earlier attempts patched constants that changed x
  and y (or position and size) by different amounts, and each broke shapes.
* **Paths.** Textured quads (`FUN_005558c0`, called for `FUN_005571b0`/`FUN_005581f0`), UI meshes such as the
  minimap (`FUN_00557860`) and text (`FUN_005a1750`, with `FUN_00516e70` returning the fitted translation).
  The render context `0x00C7D734` (`+0x10/+0x14` = target size) says whether a draw goes to the screen; offscreen
  targets (the 266x266 minimap, the 512x64 and 1024x1024 text caches) are left alone.
* **What is deliberately not rescaled.** The 3D scene (it keeps the native size and aspect, so nothing is
  resampled). Things the engine draws through the UI mesh path with a *world* matrix the shader projects with the
  scene camera: scene sprites such as the "Talk" bubble (`FUN_00592ec0`), the running speed lines
  (`FUN_005e78f0`) and the area map's plane and markers (`FUN_006293d0`). A thread-local depth counter around
  those functions switches the fit off.
* **Full-canvas draws** (the Camp Menu backdrop, the speed-line ghost frame) are fitted in x only so they cover
  the screen; sprites parked wholly outside the canvas (battle turn-order icons at y=720) are culled.
* **Save thumbnails.** `FUN_00442660` samples 176 rows over the full height; at 16:10 that leaves every tenth row
  black. A mid-function hook at `0x00442858` samples the centre 1280x720 band instead.
* **Crisp text.** The game draws each text line from a 1024x64 texture it writes on the CPU, shrunk to the font
  size with plain bilinear filtering (one mip), so strokes come out uneven. `text_prefilter.cpp` keeps a shadow
  texture per line, filled by a shader with the Lanczos-2 downscale for the screen pixel each texel lands in
  (pre-shrunk, stretched back nearest-neighbour), so the game's bilinear sample returns the clean downscale
  exactly. It needs the line shrunk 2x or more; larger lines keep the game's texture. Mipmaps were tried and
  rejected (trilinear blends two levels; the renderer's sampler cannot be biased).
* **Icons.** Small icons are jagged for the same reason (an HD texture pack's 64 px cells drawn at 20 px with no
  mipmaps). The fix is a pre-shrunk SenPatcher archive built by the manager on the player's machine
  ([MANAGER.md](MANAGER.md#the-icon-pack)).
* **`TraceDraws`** logs every distinct UI draw (hook, target, scaled/kept/culled, call chain, matrix, sprite
  rect, sampler and texture header); this is how new draw paths and icon atlases are found.

Settings: `Enabled`, `Letterbox`, `CrispText`, `CrispTextWeight`, `ForceDeckResolution`, `AnchoredScreens`,
`TraceDraws`. The PC preset switches the mod off; the Steam Deck preset turns it on.

## stick_rotation_speed

* **Speed.** The camera update (`0x0053c0b0`) loads a shared `30.0` double through `fld qword ptr [0x00b3a198]`
  at `0x0053c874`; that scales the per-frame turn the stick's deflection produces. 51 other call sites share the
  constant, so the instruction is spliced, not the constant: a `jmp` into a small code space that runs the original
  `fld` and then `fmul`s by `RotationSpeedMultiplier`. SenPatcher's own camera-sensitivity patch detours the same
  instruction; the mod recognises it and chains after it.
* **Auto-rotation** (`AutoRotation=game|off|modern`). While the followed character moves, the game pulls the
  camera's eye direction toward "straight behind the direction of travel" by a move-towards (`0x005397b0`) with a
  chord length of at most 0.05 *per frame* - never multiplied by the frame time, so at 240 fps the swing is ~4.3
  rad/s and at 30 fps ~0.5 rad/s. It is strongest when running sideways (which with camera-relative controls
  turns "hold right" into running in circles), has no easing, ignores the right stick and spins on diagonal runs
  toward the camera.
  * `off`: the `jne` at `0x0053d0f7` that gates the swing becomes a `jmp` to the game's own skip path. The right
    stick and R3's reset do not pass through it and still work.
  * `modern`: the call at `0x0053d18d` goes to `ModernSwing` with the same inputs and output, but the yaw turns at
    an angular speed in rad/s using real time, eased in and out, proportional to how sideways (and how fast) the
    character runs, never while running toward the camera, and paused for a moment after a manual turn.
* **Testing.** `TestScript=<file>` (`camera_test.cpp`) feeds a scripted stick sequence through `XInputGetState`
  and logs yaw/pitch, so modes can be compared on the same walk without hands on the pad.

## load_last_save

Loads the newest save straight from the title. `Autoload=always|parameter|disabled`; `always` (default) loads on
every start, `parameter` only when the process was started with `-al` (Steam launch options). The save folder is
`%USERPROFILE%\Saved Games\Falcom\ed8`; the game lists saves from `sdslot.dat`, so the mod picks by file time among
`saveNNN.dat` (and optionally `autosaveNN.dat`) and never touches decoys (`save511.dat` is the settings blob,
`*_t.dat` headers, thumbnails).

* **Why a keypress is not an option.** The game reads only DirectInput and XInput and never the window message
  queue, so synthesized keystrokes are ignored. The mod drives the game's own state machines instead.
* **The title route** (`TitleLoad`, default) hooks the title's update (`FUN_00690B30`). From the logo or top-menu
  state, with nothing modal up, it calls the save menu's `Open(2, 0x0040896D, title)` (`FUN_0064A720`) - the same
  call as the title's own "Load" - and then does what the list confirm does: sets the manager's slot and
  sub-state 2. The title's load callback (`0x0040896D` -> `FUN_006900F0`) then plays the fade and starts the game.
  Everything before that was a dead end: the file was always read, but *entering* the game is the menu's
  completion callback, which only the title phase installs (the in-game phase's callback deletes the running
  scene's script tasks instead).
* **Objects.** `g_ctx` = `*0x00C7C598`; save menu = `*(*(g_ctx+0x7BC)+0x5A80)`; save manager = `*0x00C3E750`
  (vtable `0x00B3D9FC`; `+0x50` state, `+0x54` slot, `+0x58` sub-state, `+0x4C` autosave naming flag, `+0x5C` save
  dir). Details and the request/reader functions: [ENGINE_NOTES.md](ENGINE_NOTES.md#saving-and-loading).
* **Probes.** `probe_*.cpp` are opt-in observers (hooks that log and tail-jump to the trampoline) used to measure
  the load path; `pad_inject.cpp` is test-only scripted pad input (`PadScript=`). Neither runs in a default
  configuration.
* **Autosaves** are loadable (`autosaveNN.dat` sets the manager's autosave flag).
* Other mods use the title route through the published service (`shared/load_last_save_api.h`).

The offline tests (`atmt_al_test`) cover the command line, the options, which files count as saves and which
is newest, the save-name parser, and the pad-script parser.

## battle_load

Opens the game's own load menu where the game has none. The save menu is only opened from the title and the camp
menu; this mod calls the same functions from elsewhere.

* **Battle.** The per-frame scene update `FUN_005be250` runs the battle (`[holder+0x5A90]`) through
  `FUN_004DB080`. While the mod's menu is open the battle is not updated at all (no input reaches it) and the
  menu's update `FUN_0064D2C0` is called instead with the same dt, as the camp menu does it. `Route=direct`
  then does what the camp's Load does: the manager reads the save into the live state, phase 1 deletes the
  scene's script tasks, and the scene change to the save's map (`FUN_005E5740`) tears the battle down.
  `Route=title` records the pick, cancels the menu, sends the battle back to the title the way a lost fight does
  and lets `load_last_save`'s title route load the save.
* **Field.** There is no single field update to freeze (a dozen systems check "is the camp open",
  `FUN_0053FC80`), so the camp itself is opened with its own call and, once its opening has played, the save menu as
  the camp's System > Load does (`Open(1, 0x0040D3D7, camp)`). A cancel closes the camp again.
* **Events** (dialogs, cutscenes): the camp cannot open (the control handler bails on `[ctx+0x170C]` bit 1 and
  `[holder+0x1418]`). The menu opens directly and the event is kept from the menu's buttons: every button query
  goes through `FUN_00446240(input, button, mode, flag)` and answers "not pressed" to everyone but the menu while
  it is open. A load is the battle's direct route; a cancel hands the dialog back where it was.
* If the save manager's state machine (`FUN_004855B0`, driven by the game itself during a battle) ever stalls
  while the mod waits, the mod drives it once per frame.

Settings: `Key` (F9), `PadChord` (BACK+START), `Route`, `Field`, `Events`, `MenuPhase`, `Diagnostics`.

## atmt_overlay

See [OVERLAY.md](OVERLAY.md).
