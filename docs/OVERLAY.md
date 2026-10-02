# The overlay, and the dialog panel in it

Two mods share this:

- **the overlay** (`mods/overlay`, `atmt_overlay.dll`, settings in `atmt_overlay.ini`) owns everything a UI
  over the game needs once per process: the Present hook, the ImGui context and backends, the mouse and its
  cursor, the hooks that take the pad, keyboard and mouse away from the game, and the **settings bar** (every
  mod's settings in one menu bar). Other mods draw in it through the interface it publishes with the loader
  (`shared/overlay_api.h`, service `atmt.overlay`).
- **the dialog panel** (`mods/trails_dialog_logger`, `panel_client.cpp` + `panel.cpp`) is one such window: the
  dialog log as a sheet drawn **over the game**, so a line can be checked while it is still on screen. It owns no
  D3D, no input hook and no cursor.

The addresses involved are in [ENGINE_NOTES.md](ENGINE_NOTES.md#input).

## How a window works

Every frame, on the render thread, the overlay calls each window's `update()` - open or not, so a window can watch
its own hotkey - with the raw pad and flags saying whether it may act on input, must close (the settings bar has
the input) or should draw a preview (the bar shows its mod's menu). The answer says whether to draw, whether it is
open (cursor, mouse) and whether the game's input should be held. Then, inside one ImGui frame, `draw()` runs for
those that asked. Each mod links its own copy of ImGui (same pinned version, checked), so `draw()` starts by
setting the overlay's context and allocator. Only one thing has the input at a time: opening the bar closes an open
window, and a window that wants the input while the bar is open asks for focus and the bar closes.

## What it draws

`host.cpp` hooks the *code* `IDXGISwapChain::Present` points at (not the vtable slot: that is read-only data and
the loader refuses to patch anything that is not code). Two ways in:

- **the game creates its device after the mod is loaded** - the mod hooks
  `d3d11!D3D11CreateDeviceAndSwapChain`, which hands the game's swapchain to the Present hook;
- **the game already had its device** (a mod loaded into a running game, and the dev reload) - the mod creates a
  throwaway 64x64 swapchain on a window of its own only to learn where Present lives, hooks that, and adopts the
  first foreign swapchain it sees presented.

Nothing touches D3D or ImGui while everything is closed: the Present detour polls the toggle keys, decides nothing
has to be drawn and calls the real Present.

`panel.cpp` draws the sheet in the game's palette (dark navy, a thin gold rule, amber name plate, warm white text):
speaker, timestamp, line, and a footer with the keys.

## The dialog panel

| input | what it does |
|---|---|
| `OverlayKey` (F3; a chord such as Ctrl+F3 also works) | opens and closes the panel |
| up / down, d-pad, left stick | scroll a line (holding repeats and speeds up the longer it is held) |
| PgUp/PgDn, LB/RB | scroll a screen |
| Home / End, A | jump to the first line / follow the newest again |
| Esc, `OverlayPadClose` (B) | close |
| Ctrl+up / Ctrl+down, Y / X | the text size, live |
| right stick | resize the sheet live: up/down = height, left/right = width |
| `OverlayPadToggle` | a pad button (or chord) that also opens the panel (empty = keyboard only) |
| mouse wheel | scrolls (with `Mouse=true` in `atmt_overlay.ini`) |

* Opening always starts at the **newest** line, and "following" is a *position*: it follows while the list sits at
  the newest end. A panel left scrolled back cannot hide the line you just heard. `OverlayNewestFirst=true` flips the
  order for a tail view.
* The panel's own keys are read with `GetAsyncKeyState` (`shared/atmt_input.h`) - the game never reads the message
  queue, so there is nothing to take those keys from. The pad comes raw from the overlay every frame.
* **Text size** (`OverlayFontSize`, 8..128 px of the game's client area): the size must reach ImGui *before* a font
  is added - since ImGui 1.92 `AddFontDefault()` picks the 13 px bitmap face below 15 px and the scalable face above,
  so setting `FontSizeBase` afterwards would draw a 13 px bitmap scaled up. `LoadOverlayFont()` sets it first. With
  `OverlayGameFont=true` (default) the log uses the game's own font; a `ttf`/`otf` in the overlay's `FontPath` is
  needed for a Japanese or Chinese log. The footer hint is a fixed small size.
* **Size and position** (`OverlayWidthPct`/`HeightPct` 5..100, `OverlayAnchor`, opacity, dim). A live change of width,
  height or text size is written back into the ini when the panel **closes** - one save per session, as a targeted
  in-place update (`atmt_ini::WriteValues`: the key's own inline comment and every other line are untouched).
* Held directions repeat with a shorter tick and a bigger step after 900 ms and 2500 ms, so a long log can be read by
  holding DOWN.

## What it takes away from the game

While the settings bar is open, or the panel is open **and** `OverlayCapture=true`, the game is handed input that
does nothing. All of it is the overlay's (`mods/overlay/src/input.cpp`), told once per frame:

- **the pad**: the game's own import slot for `XInputGetState` is patched (not the export, see ENGINE_NOTES). The
  overlay polls through the slot's previous pointer, so it always reads the player's real pad, while the game's
  calls through the detour get an idle `XINPUT_STATE`. The slot is patched, not overwritten, so others sharing it
  chain.
- **the keyboard and the mouse as the game reads them**: `IDirectInputDevice8::GetDeviceState`/`GetDeviceData`,
  hooked through a throwaway keyboard device's vtable (shared with the game's devices) on the first `Present`; the
  game's copy is zeroed (the keyboard's 256 bytes and the mouse's 20 alike).
- **the cursor**: the game hides the system cursor and recentres it every frame for its mouse camera. With
  `Mouse=true` the overlay patches the game's own `SetCursorPos`/`GetCursorPos` import slots (ImGui, through its
  own imports, still sees the real cursor): while anything is open the recentring is skipped, so the pointer moves
  freely and the camera does not. ImGui draws the pointer (`ShowCursor`).

`SetPadCapture()` and `SetKeyboardCapture()` are called **every frame**, also when nothing is open. A flag that is
only set by a poll that runs while the panel is open stays set after the last frame of an open panel, and the game
then stays dead to the controller until restart.

## The settings bar

`SettingsKey` (F2) or the `SettingsPadToggle` chord (L3+R3), both in `atmt_overlay.ini`, open a menu bar across the
top with one menu per mod that registered settings with the loader (`AtmtModApi::settings_register`,
[ARCHITECTURE.md](ARCHITECTURE.md)). Nothing in `settings_bar.cpp` knows any mod: menus, rows, types and ranges
come from the registry.

| input | what it does |
|---|---|
| LB / RB, Tab / Shift+Tab | previous / next menu |
| up / down (d-pad, left stick, arrows) | pick a row |
| left / right | change the value (held: repeats and accelerates) |
| A, Enter, Space | toggle / cycle / run an action / start picking a key or pad button |
| B, Esc | close the bar |
| the mouse (`Mouse=true`) | click a menu, a row or its `<` `>`; wheel, drag and the end arrows scroll the menus sideways |

* Each menu starts with the mod's one-sentence description (`AtmtModDescription`).
* Each change goes through `settings_set`, so the owning mod's `on_change` sees it at once; rows marked *(restart)*
  are saved but apply at the next start. The selected row's help is shown under the menu (the pad has no hover).
* A key or pad-button row picks by pressing: A, let go, then press the key - or a button, or a chord (L3+R3). Esc
  cancels; so does waiting 8 seconds.
* While the *Dialog log* menu shows, the panel is drawn behind it as a live preview.
* The bar always holds the game's input, whatever `OverlayCapture` says. Everything changed is saved in place into
  each mod's own ini when the bar and the panel are both closed (`settings_commit`).

## Settings and files

The panel: `<game>\atmt_mods\trails_dialog_logger.ini` (the documented template is
`mods/trails_dialog_logger/trails_dialog_logger.ini`). The overlay: `<game>\atmt_mods\atmt_overlay.ini`
(`SettingsKey`, `SettingsPadToggle`, `FontSize`, `Mouse`, `ShowCursor`, `FontPath`), written by the settings registry on
first start. The registry adds any registered key the file lacks (with its help as a comment) and writes live changes
back in place. A key a mod does not know is reported in `atmt_loader.log` (`config: ignored unknown keys: ...`).

For iterating on the panel without reloading a save, see `tools\dev_cycle.ps1` in [BUILD.md](BUILD.md).
