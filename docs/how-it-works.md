# How AnnihilaTTion works

AnnihilaTTion is a small engine written in C for the 68030, with a converter
in Python that turns Total Annihilation's data into what that engine reads:
the free demo's, or the full game's from its CDs. The demo and the full game
convert to the same files, so one program plays both.

## The converter

`tools/convert.py` reads the game's own formats (`tools/ta.py`: TDF, FBI,
OTA and GUI text, GAF pictures, 3DO models, TNT maps) and writes, all
big-endian for the 68030:

| File | What it holds |
| --- | --- |
| `GAME.DAT` | the campaigns, their missions in order (a skirmish's maps as a third), and the music's tracks |
| `UNITS.DAT` | the weapon and unit tables, and each unit's wreck |
| `COMMON.DAT` | the font and the fog's edges |
| `SOUNDS.DAT` | the sound effects, signed 8-bit |
| `WORLD1.DAT` ... | a world's palette, explosions, cursors and titles, and how the game palette's colours look in it |
| `WORLD1.SPR` ... | every unit's sprites, wreck and build picture in a world's colours |
| `PANEL1A.DAT`, `PANEL1C.DAT` ... | the panels round the battlefield and the order buttons, Arm's and Core's, in a world's colours |
| `AC01.DAT` ... `SK01.DAT` ... | a mission's or a skirmish's map tiles, heights, metal, features, objectives, scripts, placed units and the units the player may build |
| `AC01.TXT`, `AC01.SND`, `AC01.IMG` ... | its briefing's text, its narration at 12517 Hz and its loading picture |
| `TITLE.IMG`, `SINGLE.IMG`, `BRIEFARM.IMG`, `BRIEFCOR.IMG` | full-screen pictures with their own palettes |
| `MUSIC02.SND` ... | the full game's music, from the CD's audio tracks, at 12517 Hz |
| `FILM1.DAT`, `FILM1.SND` ... | the full game's films, and their sound |

The missions are those the campaigns in `camps/` list whose maps are there:
the demo's three, or the full game's 25 for each side. The full game's other
maps, its multiplayer ones, become skirmishes where they fit the TT's memory:
a Commander at each of the first two start positions, and the enemy's to
kill. Each world (an OTA's
planet: green, archipelago, desert, lava and so on) has a palette of its own,
chosen from its maps and the units, so a mission's map, units and panels are
in that world's 16 colours.

Units are drawn offline from their 3D models in 16 directions
(`tools/render3do.py`, the game's own projection and light), once for all
the worlds, then mapped to each world's palette with ordered dithering
(`tools/quant.py`). Each map is drawn and dithered a band at a time and cut
into 32x32 tiles; near-identical tiles are merged until the map, its cells
and the engine's arrays for it fit a fixed memory budget, so a larger map has
fewer tiles. Pictures of the converted art go to `build/previews` for
checking.

The full game's data comes off its two CDs first (`tools/cd_extract.py`): the
archives the installer would copy, the CD's audio tracks as WAV, and the
films. The films are Smacker; ffmpeg decodes them, where there is one, and
the converter keeps every other frame at 320x240, in TT low resolution's 256
colours, writing only the 16-pixel groups whose colours change noticeably.
Without ffmpeg the films are left out.

## The engine

| Path | What it holds |
| --- | --- |
| `src/tt.c`, `src/tt.h` | the machine: video, the triple-buffered screens, the films' mode, the IKBD mouse, keys, DMA sound, memory, and the Falcon's differences |
| `src/gfx.c`, `src/gfx.h` | drawing on 4-plane screens: terrain tiles, masked sprites, the nanolathe frame, lines, text |
| `src/data.c`, `src/data.h` | loading the converted data; its layouts match `tools/convert.py` |
| `src/sprites.c`, `src/sprites.h` | the units' sprites and build pictures, read from the world's file as they are needed |
| `src/sim.c`, `src/sim.h` | the battle: units, orders, paths, weapons, building, capture, metal and energy, scripts, the computer player, objectives |
| `src/game.c`, `src/game.h` | a mission on screen: the background and composite buffers, the explored map, the panels, the mouse and keys |
| `src/main.c` | the menus, campaigns, briefings and loading screens, and the scripted test driver |
| `src/snd.c`, `src/snd.h` | the mixer: sound effects, and the narration or music streamed from the disk |
| `src/film.c`, `src/film.h` | the full game's films |

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

The full game has too many unit types to hold all their sprites: a late
mission may build 150 of them, at 15 to 40 KB each. They are read from the
world's `.SPR` file when a unit of the type is first drawn, into one block of
the memory a mission leaves over; when it is full, the types drawn longest
ago make room, those no unit or wreck needs first.

The simulation runs 15 ticks a second in 24.8 fixed point: A\* paths on a grid
of 32-pixel cells that movement keeps to as well, with each movement class's
reachable parts of the map labelled when a mission starts so that no search
looks for a place out of reach; 64-pixel buckets for finding units; line of
sight and radar; aircraft that fly straight at their height, ships on the
water and submarines under it, each weapon hitting only what it can reach;
the economy; captures and repairs; silos' missiles and the systems that
shoot them down; the missions' scripts and objectives; and a computer player
that builds from idle factories, attacks in waves and fires its missiles,
and in a skirmish builds its base from its Commander up.

Sound goes through a mixer: DMA sound plays a short ring in ST-RAM over and
over, and each frame the mixer fills the part it has played with four
channels of effects and a stream, the narration or music, read from the
disk in 8 KB pieces as it plays.

## Tools

| Path | What it holds |
| --- | --- |
| `build.sh` | unpacks, converts and compiles, the demo or (`--full`) the full game ([building](building.md)) |
| `Makefile` | compiles `ANNIHILA.TOS` from `src/`; `make play` and `make test` |
| `tools/pe_resource.py`, `tools/hpi_extract.py` | taking the game data archive out of the demo's installer and unpacking it |
| `tools/cd_extract.py` | unpacking the full game's CDs |
| `tools/test_hatari.sh`, `tools/test_missions.sh`, `tools/raw2png.py`, `tools/profsum.py` | headless test runs in Hatari, of one script or a script on each of the full game's missions, their screenshots, and profiles |
| `tools/play_hatari.sh` | playing the build in a Hatari window |
| `tools/hatari_tt.sh` | the Hatari settings for the TT, and the Falcon, that both use |
| `tests/` | test scripts for `tools/test_hatari.sh`, and in `tests/full/` for `tools/test_missions.sh` ([testing](testing.md)) |
| `web/` | the demo's data converter in a web page, running `tools/*.py` in Pyodide ([building](building.md#or-in-a-browser)) |
