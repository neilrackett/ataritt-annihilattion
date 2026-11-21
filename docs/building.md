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
   minute) into `dist`, with pictures of the converted art in
   `build/previews`. It is converted again whenever the converter changes.
3. `ANNIHILA.TOS` is compiled (`Makefile`, `src/`) through `stcmd` into the
   same folder, its objects in `build`. `dist` then holds only what the TT
   needs.

`dist` and `tmp/` are ignored by Git: they hold the demo's data, which is
Cavedog's.

Copy the files in `dist` (about 7 MB) to a folder on the TT's hard disk,
for example `C:\ANNIHILA`, and run `ANNIHILA.TOS` from it.

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
`tmp/ta-demo` through Pyodide in Node and checks the result against `dist`,
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
as Hatari does; `MACHINE=falcon` runs either as a [Falcon](falcon.md).
Hatari needs a TOS image in `tmp/tos`, which Git ignores:

| Machine | Image | Where from |
| --- | --- | --- |
| TT | `TOS 3.06 - UK.img` | Atari's TOS 3.06, read from your own TT's ROMs |
| Falcon | `EmuTOS 1.4 - UK - 512K.img` | `etos512uk.img` from [EmuTOS](https://emutos.sourceforge.io)'s 512K release, renamed |

`TOS` names any other image instead. Without TOS 3.06, the EmuTOS image runs
the TT too: `TOS="$PWD/tmp/tos/EmuTOS 1.4 - UK - 512K.img" make test`.
