# Building AnnihilaTTion

AnnihilaTTion plays the free Total Annihilation demo's own missions, units and
maps, converted for the TT on a modern computer. The demo is not part of this
repository: you download it, and `build.sh` does the rest.

## What you need

- [atarist-toolkit-docker](https://github.com/neilrackett/atarist-toolkit-docker),
  for `stcmd`: this fork carries the gcc 13 `m68k-atari-mintelf` cross
  compiler the game is built with. `CROSS=direct ./build.sh` calls
  `m68k-atari-mintelf-gcc` from the PATH instead.
- `python3` with numpy and Pillow, for the converter.
- [Hatari](https://www.hatari-emu.org), only to play or test it on the
  computer you build on.

## Get the demo

Download **Total Annihilation.exe** from the Internet Archive:
[archive.org/details/TotalAnnihilation_201405](https://archive.org/details/TotalAnnihilation_201405).
The file is 21,540,864 bytes, and its SHA-256 is:

```text
5e41cf05226c274b4ac9e4398f74f6b321506bd7a4317ee1744ff7aceba34c49
```

Put it in `local/ta-demo/` in this folder, or pass its path to `./build.sh`.
`build.sh` checks its size and SHA-256 itself, and nothing in it runs: the
game data is read out of the installer.

## Build

```
./build.sh
```

1. The game data is taken out of the installer and unpacked into
   `local/ta-demo/data` (`tools/pe_resource.py`, `tools/hpi_extract.py`),
   the first time only.
2. The demo's data is converted for the TT (`tools/convert.py`, about a
   minute) into `local/ANNIHILA`, with pictures of the converted art in
   `local/previews`. It is converted again whenever the converter changes.
3. `ANNIHILA.TOS` is compiled (`Makefile`, `src/`) through `stcmd` and copied
   into the same folder.

`local/` is ignored by Git: it holds the demo's data, which is Cavedog's.

Copy the whole `local/ANNIHILA` folder (about 7 MB) to the TT's hard disk,
for example as `C:\ANNIHILA`, and run `ANNIHILA.TOS` from it.

## Make

| Command | What it does |
| --- | --- |
| `stcmd make` | compiles `build/ANNIHILA.TOS` only |
| `stcmd make clean` | removes `build/` |
| `make play` | builds with `build.sh`, then plays it in a Hatari window as the TT |
| `make test TEST=tests/win2.txt` | builds, then runs one test script headless ([testing](testing.md)) |

`make play` and `make test` run on the computer itself, not through `stcmd`,
as Hatari does; `MACHINE=falcon` runs either as a [Falcon](falcon.md).
Hatari needs a TOS image: by default TOS 3.06 from an `ataritt-doom` checkout
beside this one, and EmuTOS from an `atarist-stdl` checkout for the Falcon;
`TOS` names another.
