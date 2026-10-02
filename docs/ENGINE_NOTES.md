# Engine notes - what is known about `ed8.exe`

Facts the mods rely on, measured against the supported build (Steam, EN, 8,893,440 bytes, md5 in
[supported_exe.txt](../manager/payload/supported_exe.txt)). All addresses are image VAs (base `0x400000`, no ASLR),
so the exe on disk and the running process agree. Per-mod usage is in [MODS.md](MODS.md).

## The executable

32-bit MSVC 2010 binary on D3D11. Imports include KERNEL32, USER32, MSVCP100/MSVCR100, WINMM, libpng, steam_api,
Galaxy, d3d11, XINPUT1_3 (by ordinal), GFSDK_SSAO_D3D11 and DINPUT8. Text is drawn by the game's own bitmap font
engine (no DirectWrite/GDI), which is why capturing text means hooking the game's text functions.

**Which import may a proxy replace?** One that nothing else in the process imports, with every export forwarded.
Measured against the live process (`atmt_modules --pid <pid> --imports <dll>`): `GFSDK_SSAO_D3D11.win32.dll` is
imported by `ed8.exe` alone and has two exports (`GFSDK_SSAO_CreateContext_D3D11`, `GFSDK_SSAO_GetVersion`);
`libpng.dll` likewise but with 232 exports; `winmm.dll` by seven modules including the NVIDIA driver, Steam's overlay
and XAudio2 - a winmm proxy crashed the game ("entry point timeGetDevCaps not located in nvwgf2um.dll"). Hence the
loader is a proxy for the GFSDK library, forwarding through two 6-byte `jmp [ptr]` thunks (`loader/src/proxy_gfsdk.s`)
so no prototype or calling convention has to be guessed.

Other anchors: calls go through an ILT thunk region (`0x402000`-`0x404000`); the message-text cluster is
`0x4C0000`-`0x4C2800`; useful `.rdata` literals are `"t_name"` (`0xB43F58`), `"data/text/dat_us/%s.tbl"`
(`0xB43DFF`), `"scripts/scena/dat_us/%s.dat"` (`0xB4531C`), `"font_us.itf"` (`0xB47486`), `"save%03d.dat"` (`0xB3D9C0`).

## Dialog

* Localised dialog is stored **inline in the compiled scripts** (`data/scripts/{scena,talk,book}/dat_us/*.dat`;
  `dat/` holds unused JP leftovers), not in a message table. `data/text/dat_us/t_*.tbl` hold menus, items,
  quests and names.
* A message is a stream walked with a cursor. Text is the operand of the message opcodes `0x18`/`0x1A`; a run
  ends at the next control byte below `0x20`, `0x01` inside a run is a soft line break, `02` ends a page
  (`02 03` = another page follows, optionally headed by `11/12 <u32>`, `10 00 00 1a <u16>` or `0b`). Inline
  `#`-codes (`#K`, `#E6`, `#M0`, `#0T`, `#1P` ...) select box style, voice, expression and so on; a code is `#`,
  optional digits, one letter, so `#1P*yawn*` is the text `*yawn*`.
* `1a <id>` before a text names the speaker: an index into `t_name.tbl`, or `fe ff` = "chosen at runtime"
  (which is why scripts alone cannot name many speakers - 11,167 messages have a dynamic id). The `#E<n>`
  code is *not* a voice id (0% agreement over 584 samples).
* The same data in memory: the **message state** object (the setter's `ecx`):

  | offset | what |
  | --- | --- |
  | `+0x32C` | `char*` run the setter stored |
  | `+0x330` | how far the text has been typed out (the renderer writes it) |
  | `+0x334` | where the page on screen starts |
  | `+0x358` | name plate, an inline NUL-terminated buffer |

* **Setters.** `0x00701E50` sets the run (`push ebp; mov eax,[ebp+8]; mov [ecx+0x32C],eax; mov [ecx+0x330],0`);
  `0x00702040` sets the name (`record+0x2EC` is the display name it `strncpy`s to `+0x358`). Dispatchers hand
  handlers their object at `0x0064F270` (table `0x012EADA0`) and `0x0064F240` (table `0x012EAD08`). The box's
  renderer is `FUN_005ef810` (called from `0x703410`, measured at `0x7035c0`/`0x706fa0`); `0x703390` clears the
  state when the box resets.
* Runs are reused: of 32,354 captured lines only 21,742 distinct addresses appeared. The text is drawn once;
  watching a line already on screen never fires - capture the call that sets it.

## Input

| what | address |
| --- | --- |
| `XInputGetState` import slot / thunk | `0x0136A500` / `0x009FE836` |
| `XInputSetState` import slot / thunk | `0x0136A504` / `0x009FE830` |
| the game's pad update (only caller of both) | `FUN_00935340` (calls at `0x0093536D`, `0x00935382`, `0x0093558C`); the `XINPUT_STATE` is a stack local at `ebp-0x14` |
| pad object | 0x3B8 bytes, ctor `FUN_0092EE50`; XInput slot at `+0x3A0`, connected `+0x3A4`, packet `+0x3A8`, buttons `+0x3AC`; decoded buttons `+0x38C..+0x39B`, sticks `+0x364/+0x368/+0x370/+0x374` |
| `DirectInput8Create` import thunk | `0x00A005B0` |
| DInput init / `EnumDevices` | `FUN_00940530` (call `0x0094054A`); again after `WM_DEVICECHANGE`: `FUN_00940580` |
| `IDirectInput8W*` global | `0x01363FE4` |
| keyboard poll (`GetDeviceState`, vtable index 9, 0x100 bytes) | `FUN_00933110`; decoded keys at object `+0x218` |
| DirectInput pad poll (0x110 bytes) | `FUN_00935120` |
| engine device list | head `0x00C68230` / `0x00C68234`; per-device poll is vtable index 5; polled from `FUN_00446ED0` at `0x00446F71` |
| button query | `FUN_00446240(input, button, mode, flag)` - mode 1 consumes the press |

The game never reads the Windows message queue (keystrokes from `SendInput` are ignored; hotkeys are read with
`GetAsyncKeyState`). The overlay therefore patches the game's own **import slot** for `XInputGetState` (never the
export: the mods start before the game's entry point, and a hook on the export would sit there before Steam's
overlay hooks it for Steam Input, after which the game ignored the pad until the Guide button was pressed). The
slot is patched, not overwritten, so another patcher of the same slot (the `-al` mod's pad injector, the camera
test) chains. DirectInput is hooked through a throwaway keyboard device's vtable - which the game's devices
share - on the first `Present`, not at start-up, because the game makes its pad objects from one `EnumDevices` call
at startup and nothing of ours should touch DirectInput before that.

## Rendering

| what | address |
| --- | --- |
| render-device singleton | `0x00C77618`; `ID3D11Device*` `+0x70` (`0x00C77688`), context `+0x74`, swapchain `+0x78` (`0x00C77690`), `DXGI_SWAP_CHAIN_DESC` `+0x7C` |
| D3D setup | `FUN_0078B480`: `D3D11CreateDevice` (probe) at `0x0078B4E7`, `D3D11CreateDeviceAndSwapChain` at `0x0078B574`, `GFSDK_SSAO_CreateContext_D3D11` at `0x0078B932` |
| UI render context (enqueue `this`) | `0x00C7D734`; `+0x10/+0x14` = size of the target being drawn into |
| main camera | `FUN_005A46A0`; aspect at camera `+0xC0`, written every frame at `0x005A7368` from W/H |
| UI/3D draw submit | `FUN_005558c0` (textured quads; 9th arg = NDC transform), `FUN_00557860` (UI meshes), `FUN_005a1750` (text), `FUN_007daa00` -> `FUN_008b2b70`/`FUN_007d82e0` (constants built at enqueue) |
| vsync flag | `0x01363FCC` (`vsync=` command-line argument) |

The renderer is a **deferred command queue**: per-draw constants are built on the main thread at enqueue, drawing
happens later on a render thread, so backtracing from a `Draw` call never reaches the UI function that asked for it.
UI is authored in a 1280x720 canvas; `FUN_00483650` (object at `[0x00C7C0D0]+8`) computes `f = W/(H*16/9)`
(0.9 at 800) and forcing it to 1 breaks the glyph atlas and minimap icons. Text lines are 1024x64 `B8G8R8A8`
`USAGE_DYNAMIC` textures filled with Map/Unmap (the Phyre texture holds the `ID3D11Texture2D` at `+0x28`, its SRV
at `+0x2c`). The overlay hooks the code `IDXGISwapChain::Present` points at (the vtable slot itself is read-only
data, which the loader refuses to patch).

## Saving and loading

The save folder is `%USERPROFILE%\Saved Games\Falcom\ed8`: `save000..063.dat` (460,992 bytes each),
`autosave00..07.dat` with plain-text `autosaveNN_t.dat` headers, `thumbNNN.bmp`, `sdslot.dat` (64 x 5,760 bytes of
per-slot metadata; **the game lists saves from it** - it imports no `FindFirstFile`), `save511.dat` (52 bytes,
the settings blob, also the "system file" read at start-up) and `steam_autocloud.vdf`.

**The request/state block** (one request at a time; the reader spin-waits on the flag):

| VA | meaning |
| --- | --- |
| `0x00C3E750` | save-manager pointer |
| `0x00C3E754` | slot id of the request (`0x3F` = `save063.dat`, `0x1FF` = the system file) |
| `0x00C3E758` | file-name buffer (`strncpy(..., 0x103)`) the worker reads |
| `0x00C3E85C` / `0x00C3E860` | destination buffer / size |
| `0x00C3E864` | worker thread handle (`_beginthread` result; `-1` idle) |
| `0x00C3E868` / `0x00C3E869` | request flag / success byte (also the first argument of both callbacks) |
| `0x00D339B0` | the game's static save buffer (460,992 bytes) used by a menu load |
| `0x012EACBC` | static buffer of `save511.dat` (0x34 bytes) |

**Functions.** `0x00485000` is the reader (reached only via thunk `0x0040DEE0`); `0x00484870` is the request
(`thiscall` + 5 args, thunk `0x0040AE93`): `_beginthread` the worker, reset the block, copy the name, store
manager/slot/buffer/size, set the flag. `0x00485370`/`FUN_004855B0` is the manager's state machine, called by the
game every frame (its only xref is a data reference, so a call graph dead-ends there). Callbacks: cb1 `0x0040B073`
-> `0x0064ACA0` (phase 1 deletes the running scene's script tasks - the in-game load; phase 2, the title's, only
clears 0x33 bytes), cb2 `0x0040B1C2` -> `0x0064A7B0` ("data ready").

**Manager struct** (offsets from `[0x00C3E750]`): `+0x04` kind (0 worker `0x40218A`; 1 worker `0x40DEE0` with
`[+8]/[+0xC]` as buffer/size - the real load; 2 worker `0x40C829`), `+0x10/+0x14` cb1/cb2, `+0x18` the arg object
handed to both (`+0xC` phase), `+0x20` slot base, `+0x4C` flag: name built from the slot as an autosave name
(`"%s%sautosave%02d%s.dat"`, `0x00B3B6D8`), `+0x50` state (0 idle, 1 issue, 3 wait, 5/6 callback stages), `+0x54`
slot, `+0x58` sub-state (2 builds `save%03d.dat` and issues the request), `+0x5C` tagged pointer to the save dir.

**The menu.** `Open(phase, cb, arg)` is `FUN_0064A720` (thiscall, `ret 0xC`; sets `+4=1`, `+0xC=phase`, `+0x14=cb`,
`+0x18=arg` and initialises the list UI); update is `FUN_0064D2C0` (switch on `menu+4`: 1 timer then flow(phase);
2 -> 3; 3 list shown while the manager is busy; 4 phase 1/2 ok then re-read `save511.dat` (phase 5), apply -> 7;
7 closes and calls `cb(ok, arg)`); list confirm at `0x0064D0D3` (`mgr->SetSlot(i)` `0x00484A10`, `mgr->SetSub(2)`
`0x00484A30`). Callers of `Open`: title (phase 2, cb `0x0040896D`), camp menu `0x0054E9C9` (phase 1, cb
`0x0040D3D7`), start-up `0x0068F804` (phase 5), saves (phases 0/3/6). `FUN_0064CAD0` is one step of the menu's
state machine, not a session entry.

**The title.** `FUN_00690B30(this, dt)`: if the menu is open only the menu updates; `+0x10` state (2 = logo "press any
button", 3 = top menu, 5 = post-load prompt), `+0x1C != -1` = a load completed -> `FUN_006905F0` starts the game.
The title's "Load" is at `0x00691037`. `FUN_005E5740(ctx, map, "", 0, 0, 1)` changes the scene (`"title"` returns to
the title). The scene's tick, `FUN_005be250`, owns battle/field/title dispatch.

## Camera

`0x0053c0b0` is the field camera update (`this+0x1a0` right stick, `+0x5d0/+0x5d4` pitch/yaw written by
`asin`/`atan2` at `0x0053d37f`/`0x0053d35f`); the caller `0x005a72c0` runs it only when `[this+0xd1c]` is non-null.
R3's reset is `0x0053af80`. See [MODS.md](MODS.md#stick_rotation_speed).

## Fonts

`data/fonts/font_us.itf` / `font_us_hd.itf` are Falcom's bitmap font atlases with metrics; the layout is documented
in `mods/trails_dialog_logger/src/itf_font.h` (the mod reads the loaded font from the game's memory, so a
SenPatcher or HD-pack font is what is drawn). `sjisutf8.dat`/`utf8sjis.dat` are conversion tables.

## Data formats

**`data/text/dat_us/t_*.tbl`** (matches SenPatcher's `tbl.cpp`): `u16 entryCount`, then per entry a NUL-terminated
UTF-8 `name` (the dataset, e.g. `NameTableData`), `u16 dataLen`, `data`. For most tables `data` is `u16 subIndex`
(signed, -1 = none) and a NUL-terminated UTF-8 string; `item` and `magic` carry a fixed struct plus three
strings. `t_name.tbl` (339 entries) maps character ids to names (0 Rean, 1 Alisa, ...); `t_voice.tbl` maps voice ids.

**`data/scripts/<category>/dat_us/*.dat`**: a header (table of section offsets/counts, magic `00 EF CD AB`, the
internal name, a table of entry names such as `TK_Alan`) followed by the bytecode. Categories: `scena` (story),
`talk` (per-NPC chatter, `tk_<name>.dat`), `book`, `battle`, `ani`, `ui`. Dialog encoding: see above.
`tools/tcs_script.py` and `tools/tcs_tbl.py` read these.

### Asset formats

Used by the manager's icon pack (`manager/src/core/falcom_pkg.*`, `p3a.*`, `icon_pack.*`).

**`data/asset/D3D11[_us]/*.pkg`** (the English build prefers `D3D11_us`, which only has some packages):

```
u32 unknown, u32 count
count x { char name[0x40]; u32 size; u32 stored_size; u32 offset; u32 flags }
flags: 1 Falcom type-1 compression, 2 crc32 prefix, 4 lz4, 8 lzma, 0x10 zstd
```

Type 1: `u32 size, u32 stored_size, u32 backref_byte`, then bytes. A byte equal to `backref_byte` is followed by an
`offset` (the same byte again means a literal `backref_byte`; an `offset > backref_byte` is decremented) and a
`length` to copy from `offset` bytes back (decoder: SenPatcher `native/sen/pkg_extract.cpp`).

**`*.png.phyre` / `*.dds.phyre`**: a PhyreEngine container. UI atlases are uncompressed `RGBA8`/`ARGB8` (bytes B, G,
R, A), level 0 then any mip chain, at the end of the file, with height and width as adjacent `u32`s about 140 bytes
before the pixels. Rows are stored bottom-up (the game's `v = 0` is the first stored row). `.png.phyre` atlases have
**no mipmaps**.

**`mods/*.p3a`** (SenPatcher archives, loaded in `mods/order.txt` order, first wins):

```
char magic[8] "PH3ARCV\0"; u32 flags; u32 version (1100 | 1200); u64 count; u64 xxh64(bytes 0..23)
v1200 only: u64 xxh64(next 8 bytes); u32 ext_size (16); u32 entry_size (304)
count x { char path[256]; u64 compression (0 none, 1 lz4 block, 2 zstd, 3 zstd+dict);
          u64 stored_size; u64 size; u64 offset; u64 xxh64(stored bytes); [v1200: u64 xxh64(data)] }
```

SenPatcher checks the stored-bytes hash on load. Its load order (`native/modload/loaded_mods.cpp`): the names in
`mods/order.txt` (UTF-8 BOM skipped, CRLF or LF), then every other `*.p3a` in `mods/`; when it adds any it moves its
own `zzz_senpatcher_*` archives to the end and rewrites `order.txt`. The HD pack ("TLoH ToCS HD Pack") is a v1100
archive of whole `.pkg` files.

## Finding addresses

* **Measure before hooking.** Guessed hook addresses crash the game. An address is safe once its bytes (and ideally
  its prologue) have been read from the exe; `shared/code_check.h` makes every mod verify them at start.
  The loader's hook service also refuses addresses that are not committed executable code.
* **Static first.** `tools/pe_probe.py` (needs `pefile` and `capstone`; exe via `--exe` or `TOCS_EXE`) lists
  imports, strings, string xrefs, calls to a thunk and disassembly (`--from-func` finds the enclosing function). For
  decompiles use Ghidra headless: set `GHIDRA_ROOT`, copy your `ed8.exe` to `ghidra\ed8.exe`, run
  `ghidra\run_import.cmd` once (minutes), then `ghidra\run_script.cmd <script>.java <args>` with the scripts in
  `ghidra\scripts` (`decompile`, `callers_of`, `find_calls`, `find_xrefs`, `find_strings`, `find_ptr`,
  `find_symbol`, `dump_at`, `dump_bytes`, `dump_range`, `dump_name_table`, `list_blocks`, ...). Import the exe as
  x86 / 32-bit / MSVC.
* **Dynamic.** A hardware execute breakpoint tests whether an address runs at all without patching a byte. A line
  is drawn once, so arm a watch before it appears and advance one line. Cheat Engine's "find out what writes to
  this address" on a live buffer (the name plate, a run) is how the setters above were found.
* **Hooks must not change game state.** The mods' probes read what they were handed and tail-jump to the
  trampoline; a measured address is used, never a guessed one.

## Other people's work this builds on

* [SenPatcher](https://github.com/AdmiralCurtiss/SenPatcher) (AdmiralCurtiss): `native/sen1/*` documents stable `ed8.exe`
  addresses (kerning `0x5A0631`, textbox prompt `0x467222`, camera sensitivity `0x53c874`), the `.tbl`/`.p3a` formats and
  the load order of `mods/`.
* [SenScriptsDecompiler](https://github.com/TwnKey/SenScriptsDecompiler) (TwnKey): the script instruction set, in
  particular how dialog operands are encoded.
* [Ouroboros/Falcom](https://github.com/Ouroboros/Falcom): script disassembly and opcode names.

Checkouts, if wanted, go in `research/` (gitignored). Each project keeps its own licence.
