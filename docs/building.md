# Building AnnihilaTTion

AnnihilaTTion plays Total Annihilation's own missions, units and maps,
converted for the TT on a modern computer: the free demo's, or the full
game's from your own CDs, with its two expansions' if you have those too.
None of them is part of this repository: you download the demo, or make
images of your CDs, and `build.sh` does the rest.

## What you need

- [atarist-toolkit-docker](https://github.com/neilrackett/atarist-toolkit-docker),
  for `stcmd`: this fork carries the gcc 13 `m68k-atari-mintelf` cross
  compiler the game is built with. `CROSS=direct ./build.sh` calls
  `m68k-atari-mintelf-gcc` from the PATH instead.
- `python3` with numpy and Pillow, for the converter.
- [Hatari](https://www.hatari-emu.org), only to play or test it on the
  computer you build on.
- For the full game's films, [ffmpeg](https://ffmpeg.org), which decodes
  them. Without it the full game plays without its films.

## Get the demo

Download **Total Annihilation.exe** from the Internet Archive:
[archive.org/details/TotalAnnihilation_201405](https://archive.org/details/TotalAnnihilation_201405).
The file is 21,540,864 bytes, and its SHA-256 is:

```text
5e41cf05226c274b4ac9e4398f74f6b321506bd7a4317ee1744ff7aceba34c49
```

Put it in `tmp/ta-demo/` in this folder, or pass its path to `./build.sh`.
`build.sh` checks its size and SHA-256 itself, and nothing in it runs: the
game data is read out of the installer.

## Build

```
./build.sh
```

1. The game data is taken out of the installer and unpacked into
   `tmp/ta-demo/data` (`tools/pe_resource.py`, `tools/hpi_extract.py`),
   the first time only.
2. The demo's data is converted for the TT (`tools/convert.py`, about a
   minute) into `dist/DATA`, with pictures of the converted art in
   `build/previews`. It is converted again whenever the converter changes.
3. `ANNIHILA.TOS` is compiled (`Makefile`, `src/`) through `stcmd` into
   `dist`, beside `DATA`, its objects in `build`. `dist` then holds only
   what the TT needs.

`dist` and `tmp/` are ignored by Git: they hold the game's data, which is
Cavedog's.

Copy `ANNIHILA.TOS` and the `DATA` folder in `dist` (about 7 MB) to a
folder on the TT's hard disk, for example `C:\ANNIHILA`, and run
`ANNIHILA.TOS` from it. It reads its data from `DATA`, and keeps your
progress there.

## The full game

The full game comes from images of its two CDs (Total Annihilation, 1997):
a CUE sheet with its BIN or IMG, or an ISO, for each disc. Put them in
`tmp/disks`, in folders whose names include `DISK1` and `DISK2` (for
example `tmp/disks/Base Game/DISK1/Total.cue`), or pass their paths:

```
./build.sh --full [DISC1 DISC2]
```

1. The CDs are unpacked into `tmp/ta-full` (`tools/cd_extract.py`), the
   first time only: the game data the installer would copy, then the
   discs' own archives over it, the first disc's audio tracks (the music,
   which needs the CUE sheet) and its films. Then the expansions' discs,
   if they are there (see below).
2. The full game's data is converted (`tools/convert.py`, about half an
   hour) into `dist/full/DATA`, with pictures of the converted art in
   `build/previews-full`: both campaigns' 50 missions, the 24 multiplayer
   maps that fit the TT for skirmishes, a palette and the units' sprites for
   each of its 9 worlds, the music and the films.
3. `ANNIHILA.TOS` is compiled and copied into `dist/full`, beside `DATA`.

Copy `ANNIHILA.TOS` and the `DATA` folder in `dist/full` (about 300 MB) to
a folder on the TT's hard disk and run `ANNIHILA.TOS` from it. `DATA=dist/full tools/play_hatari.sh`
plays it in Hatari.

### The expansions

The Core Contingency and Battle Tactics (1998) come from images of their
CDs, ISOs or CUE sheets, in folders of `tmp/disks` whose names say
`Contingency` and `Tactics` (for example `tmp/disks/Core Contingency
Expansion/TACC.ISO`). Either or both will do. With them there,
`./build.sh --full` unpacks each into a folder of its own beside the full
game's data, `tmp/ta-full/cc` and `tmp/ta-full/bt`, the first time only,
and converts it with the full game into `dist/full`: The Core
Contingency's two campaigns and Battle Tactics' eight, their units, worlds
and maps, and more skirmishes. That takes about an hour and a half, and `DATA`
grows to about 600 MB. `OA_EXPANSIONS=cc`, `bt` or `none` converts only
those, or the full game alone.

Each push to `main` also builds `ANNIHILA.TOS` on GitHub and puts it on the
[latest release](https://github.com/neilrackett/ataritt-annihilattion/releases/tag/latest)
(`.github/workflows/release.yml`). That is the program alone: the data it
plays is still converted on your own computer.

## Or in a browser

The [data converter](https://labs.neilrackett.com/ataritt-annihilattion/)
does `build.sh`'s first two steps in a web page: drop the installer on it and
it gives back the converted data, the same files byte for byte. It runs the
same `tools/*.py` in [Pyodide](https://pyodide.org), Python built for the
browser, in about two minutes, and nothing is uploaded.

It lives in `web/` (Vite and TypeScript, with pnpm). `pnpm install` then
`pnpm run dev` serves it locally; `pnpm test` converts the installer in
`tmp/ta-demo` through Pyodide in Node and checks the result against `dist/DATA`,
skipping that when either is missing. Each push to `main` that changes
`web/` or the converter publishes it to the `gh-pages` branch
(`.github/workflows/pages.yml`), which GitHub Pages serves.

## Make

| Command | What it does |
| --- | --- |
| `stcmd make` | compiles `dist/ANNIHILA.TOS` only |
| `stcmd make clean` | removes `build` and `dist/ANNIHILA.TOS` |
| `make play` | builds with `build.sh`, then plays it in a Hatari window as the TT |
| `make test TEST=tests/win2.txt` | builds, then runs one test script headless ([testing](testing.md)) |

`make play` and `make test` run on the computer itself, not through `stcmd`,
as Hatari does; `MACHINE=falcon` runs either as a [Falcon](falcon.md), and
`MONITOR=rgb` or `MONITOR=tv` puts that Falcon on an RGB monitor or a TV.
Hatari needs a TOS image in `tmp/tos`, which Git ignores:

| Machine | Image | Where from |
| --- | --- | --- |
| TT | `TOS 3.06 - UK.img` | Atari's TOS 3.06, read from your own TT's ROMs |
| Falcon | `EmuTOS 1.4 - UK - 512K.img` | `etos512uk.img` from [EmuTOS](https://emutos.sourceforge.io)'s 512K release, renamed |

`TOS` names any other image instead. Without TOS 3.06, the EmuTOS image runs
the TT too: `TOS="$PWD/tmp/tos/EmuTOS 1.4 - UK - 512K.img" make test`.
