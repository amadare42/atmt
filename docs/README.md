# Documentation

Player-facing documentation is the top-level [README](../README.md). This folder is for people building or
extending the toolkit.

| document | read it for |
| --- | --- |
| [BUILD.md](BUILD.md) | building, the `dist/` layout, tests, deploying to a game or a Steam Deck, the live-reload loop |
| [ARCHITECTURE.md](ARCHITECTURE.md) | the loader / mod split, the mod contract, adding a mod |
| [MODS.md](MODS.md) | how each mod works: the mechanism, the key addresses, the settings |
| [OVERLAY.md](OVERLAY.md) | the overlay, the settings bar and the dialog panel |
| [MANAGER.md](MANAGER.md) | ATMT Manager: payload, presets, settings schema, CLI, icon pack, releases |
| [ENGINE_NOTES.md](ENGINE_NOTES.md) | what is known about `ed8.exe`: structures, addresses, data formats, how to find more |

Conventions: addresses are `ed8.exe` image VAs (no ASLR); a mod never hooks an address whose bytes it has not
checked (`shared/code_check.h`); nothing of the game's content is shipped.
