# How AnnihilaTTion works

AnnihilaTTion is a small engine written in C for the 68030, with a converter
in Python that turns the demo's data into what that engine reads.

## The converter

`tools/convert.py` reads the demo's own formats (`tools/ta.py`: HPI archives,
TDF/FBI/OTA text, GAF pictures, 3DO models, TNT maps) and writes, all
big-endian for the 68030:

| File | What it holds |
| --- | --- |
| `UNITS.DAT` | the weapon and unit tables, and every unit's sprites |
| `COMMON.DAT` | the battle palette, the panels, features, effects, cursors, unit pictures, buttons and the font |
| `MISS1.DAT` to `MISS3.DAT` | each mission's map tiles, heights, metal, objectives, scripts and placed units |
| `SOUNDS.DAT` | the sound effects, signed 8-bit |
| `BRIEF1.SND`, `BRIEF1.TXT` ... | each briefing's narration at 6258 Hz and its text |
| `TITLE.IMG`, `SINGLE.IMG`, `BRIEF.IMG`, `GLAM1.IMG` ... | full-screen pictures with their own palettes |

Units are drawn offline from their 3D models in 16 directions
(`tools/render3do.py`, the game's own projection and light), then mapped to a
16-colour palette chosen from the maps and units with ordered dithering
(`tools/quant.py`). Each map is cut into 32x32 tiles, with near-identical
tiles merged so that each mission fits in memory. Pictures of the converted
art go to `build/previews` for checking.

## The engine

| Path | What it holds |
| --- | --- |
| `src/tt.c`, `src/tt.h` | the machine: video, the triple-buffered screens, the IKBD mouse, keys, DMA sound, memory, and the Falcon's differences |
| `src/gfx.c`, `src/gfx.h` | drawing on 4-plane screens: terrain tiles, masked sprites, the nanolathe frame, lines, text |
| `src/data.c`, `src/data.h` | loading the converted data; its layouts match `tools/convert.py` |
| `src/sim.c`, `src/sim.h` | the battle: units, orders, paths, weapons, building, metal and energy, scripts, the computer player, objectives |
| `src/game.c`, `src/game.h` | a mission on screen: the background and composite buffers, the panels, the mouse and keys |
| `src/main.c` | the menus, briefings and loading screens, and the scripted test driver |
| `src/snd.c`, `src/snd.h` | sound effects and narration |

The battlefield is drawn from a background in TT-RAM, the terrain with the
trees and wrecks over it, and a composite, the background with each frame's
units, shots and effects. Both are rings the size of the view: a place on
the battlefield always sits at the same spot in them (its position modulo
their size), so when the view scrolls only the strips that come into it are
drawn, and nothing already there is moved; the area round a feature that
comes or goes is drawn again the same way. Drawing on the composite splits
whatever crosses the rings' seams. Each of the three screens in ST-RAM
copies only the 16x16 cells that changed since it was last shown, or the
whole view after a scroll. The view moves across in 16-pixel steps, as the
screen's planes are copied a group of 16 pixels at a time, and down by the
pixel. The interface round the battlefield is drawn into a buffer of its own
when it changes.

The simulation runs 15 ticks a second in 24.8 fixed point: A\* paths on a grid
of 32-pixel cells that movement keeps to as well, 64-pixel buckets for finding
units, line of sight, the weapons' kinds, the economy, the missions' scripts
and objectives, and a computer player that builds from idle factories and
attacks in waves.

## Tools

| Path | What it holds |
| --- | --- |
| `build.sh` | unpacks, converts and compiles ([building](building.md)) |
| `Makefile` | compiles `ANNIHILA.TOS` from `src/`; `make play` and `make test` |
| `tools/pe_resource.py`, `tools/hpi_extract.py` | taking the game data archive out of the installer and unpacking it |
| `tools/test_hatari.sh`, `tools/raw2png.py`, `tools/profsum.py` | headless test runs in Hatari, their screenshots, and profiles |
| `tools/play_hatari.sh` | playing the build in a Hatari window |
| `tools/hatari_tt.sh` | the Hatari settings for the TT, and the Falcon, that both use |
| `tests/` | test scripts for `tools/test_hatari.sh` ([testing](testing.md)) |
| `web/` | the data converter in a web page, running `tools/*.py` in Pyodide ([building](building.md#or-in-a-browser)) |
