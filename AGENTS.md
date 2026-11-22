# Working on AnnihilaTTion

AnnihilaTTion is a Total Annihilation engine for the Atari TT, playing the
free demo's three Arm missions in TT medium (640x480, 16 colours). It is a
small engine of its own, written in C for the 68030, plus a Python converter
that turns the demo's data into what the engine reads. This file holds the
rules and the lessons that the docs don't: read the docs for what things are.

| For | Read |
| --- | --- |
| what it plays, controls, running it on a TT | [docs/playing.md](docs/playing.md) |
| getting the demo, building, `make`, the web converter | [docs/building.md](docs/building.md) |
| the converter and the engine, file by file | [docs/how-it-works.md](docs/how-it-works.md) |
| test scripts and their commands | [docs/testing.md](docs/testing.md) |
| the Falcon experiment | [docs/falcon.md](docs/falcon.md) |

## Rules

- **Commit or push only when Neil asks** ("Commit", "Y"). No `Co-Authored-By`
  trailer and no attribution lines, in commits or pull requests.
- **No game data in Git**: not the installer, not the unpacked or converted
  data, not a test fixture. `dist/`, `build/` and `tmp/` are ignored for
  this reason.
- **Folders**: build artifacts in `build/`; only what the TT needs in `dist/`
  (`ANNIHILA.TOS` and the data); the demo, test runs and TOS images in
  `tmp/` (`tmp/ta-demo`, `tmp/runs`, `tmp/tos`). Nothing points at other
  checkouts on Neil's computer.
- **The TT comes first**, in code and docs. The Falcon is an experiment and a
  base for later. There is **one binary** for both: `src/tt.c` handles their
  differences at run time.
- **Headers**: code, scripts and workflows start with
  `Copyright (C) 2026 Neil Rackett` and
  `SPDX-License-Identifier: GPL-3.0-or-later`. Markdown files have no header.
- **Docs wording**:
  - Open Annihilation is credited as the inspiration, and only that. Never
    write that this began or "started life" inside it: no code came from it.
  - Leave Hatari's speed against a real TT out of the docs, and any figures
    estimated from it. Real-TT numbers go in only once measured on Neil's TT.
  - The tagline is "Total Annihilation engine for the Atari TT"; elsewhere
    make it clear that it plays the demo, for now.
  - Write as the rest of the docs do: plain British English, short
    sentences, no marketing.

## Build

```
./build.sh                       # unpack and convert the demo, then compile
STCMD_NO_TTY=1 stcmd make        # compile dist/ANNIHILA.TOS only (agents, CI)
```

gcc 13 `m68k-atari-mintelf` with mintlib, through `stcmd`
([atarist-toolkit-docker](https://github.com/neilrackett/atarist-toolkit-docker)).
Keep the build free of warnings. The converter needs `python3` with numpy and
Pillow. `build.sh` converts again whenever a `tools/*.py` is newer than
`dist/MISS3.DAT`; conversion takes about a minute.

## Test

`tools/test_hatari.sh NAME SCRIPT [SECONDS]` runs a script headless; the log is
`tmp/runs/NAME/OALOG.TXT`, which ends in `result N` for a mission.
`MACHINE=falcon` runs the Falcon (`MONITOR=rgb` or `tv` for its RGB
mode), `PRG=` another build, `PROF=1` writes a profile for
`tools/profsum.py`.

What each script should end with, on the TT and the Falcon alike:
`m1_gate`, `win2` and `win3` win (`result 1`); `long2` and `m2_attack` lose
(`result -1`); every other mission script ends at `result 0`; `menu` has no
result.

**Before committing a change to drawing**, check that it still draws the
same:

- Keep a build of the last commit to compare against:
  `git archive HEAD src Makefile | tar -x -C build/old`, then
  `stcmd make` in `build/old`.
- Battle screenshots must stay pixel-identical: run the same script with
  both builds (`PRG=build/old/dist/ANNIHILA.TOS`) and compare the PNGs.
- The `check` command logs where the composite differs from the background
  and the screen from the composite. Outside what a frame drew, both must be
  0; the screen may differ only under the pointer. Run it at awkward camera
  positions, so the rings' seams cross the view.

**Timing**: `stats` logs the draw and simulation milliseconds and each
stage's share; `fps1`, `fps3`, `scroll` and `battle` are the ones to watch.

- Run timed scripts **one at a time**. Runs in parallel are not
  repeatable; runs one after another give the same figures every time.
- Profile with `noinline` on the functions you are comparing, in both
  builds. Otherwise GCC folds them into `game_run`, which the profile never
  lists, and their cost disappears from it.
- A whole script's average includes a mission's first frames, which draw
  everything. To measure steady frames, take the difference between two
  `stats` lines.

## The engine: lessons

- **The planes decide the scrolling.** A screen group is 16 pixels in four
  interleaved plane words, so the view moves across in whole groups
  (`clamp_camera()` rounds `camx`) and down by the pixel. Shifting by less
  than a group cost about four times a plain copy, even in assembly, so the
  step stays 16 pixels.
- **The rings.** The background and the composite are rings the size of the
  view: battlefield group `g` and row `y` sit at `(g mod 32, y mod 416)`. A
  scroll draws only the strips that come into view. What the rings hold is
  split into up to four pieces that don't wrap (`set_pieces()`), and drawing
  on the composite goes through `FOR_PIECES`. That draws a thing once when it
  lies in one piece, or clipped into each piece it meets, and it remembers
  the piece the clip is set to. Add new composite drawing through the
  `comp_*` helpers, not straight to `gfx_*`.
- **Marks.** Changed 16x16 ring cells are marked each frame (`mark()`; a
  `comp_blit()` marks its own sprite). `comp_begin()` restores last frame's
  cells from the background, and each screen copies the union of the last
  frames' marks, or the whole view after the camera moves.
- **Copy loops.** GCC turns simple copy loops into `memcpy` calls, which cost
  more than they save on short runs. Inlined into a big function, its loops
  spill pointers to the stack. `copy_run()` is a small assembly loop (two
  `move.l` and a `dbra`) for that reason. Look at the object code
  (`m68k-atari-mintelf-objdump -d build/game.o`) before trusting a loop.
- **DMA sound reads only ST-RAM.** The narration is too long to keep there
  at 12517 Hz, so it loads into TT-RAM and `snd_stream()` plays it through a
  64 KB ring in ST-RAM. The briefing's loop calls `snd_stream_update()`,
  which refills the half that isn't playing.
- **The explored map.** It is kept in 32x32 sight cells, and the fog tiles
  lie 16 pixels off them, so each 16x16 group cell is one quarter of a tile
  and the masks stay on whole groups. Explore after each `sim_tick()`, not
  each frame, so that the fog doesn't depend on the frame rate. Whether an
  enemy shows is `unit_shown()`, for drawing and picking alike. The `check`
  command compares the background with a fresh drawing, which catches a
  missed redraw.
- **Per-call costs add up.** A function call, a modulo or a division per
  row or per group cost more than the work. Step pointers instead, and wrap
  ring columns with `& (RING_G - 1)`.
- **Back to user mode with `SuperToUser()`, not `Super()`.** `tt_init()`
  calls `Super(0L)` and `tt_exit()` returns at another stack depth. TOS's
  and EmuTOS's `Super(ssp)` puts the user stack back where `Super(0L)` left
  it, so the return address was lost: EmuTOS's Falcon panicked on every
  quit. mintlib's `SuperToUser()` keeps the stack pointer across the trap.

## The converter: rules

- **Its output is the contract.** `build.sh` and the web converter run the
  same `tools/*.py` and must make byte-identical files. `src/data.h` mirrors
  the layouts: change both together.
- **It must not depend on the platform:**
  - Sort every directory listing; Pyodide's filesystem lists files in a
    different order from the Mac's.
  - In the browser numpy's integers are 32-bit: `np.arange(n) * 22050`
    overflowed there and garbled the briefings. Use explicit `float64` or
    `int64` wherever a product can pass 2^31.
  - Sounds are filtered before resampling with `np.convolve` on `int64`, so
    that no platform's summation order can change a byte.
- `cd web && pnpm test` converts `tmp/ta-demo/Total Annihilation.exe`
  through Pyodide in Node, about two minutes, and checks every file against
  `dist/`. It skips itself without the installer, as on GitHub's runners.
  Run it after any change to `tools/*.py`.

## The web converter

`web/` is a Vite and TypeScript page in the style of Neil's RExtract
(web-rextract): the same layout, theme switch, worker and stored zip. It loads
Pyodide (pinned in `web/src/core/convert.ts` and `web/package.json`; change
both) from jsDelivr, with numpy and Pillow, and bundles `tools/*.py` into its
worker. `pnpm run dev` serves it locally. Only the data is converted:
`ANNIHILA.TOS` is a download from
[neilrackett.com/atarist/ataritt-annihilattion](https://neilrackett.com/atarist/ataritt-annihilattion).

## Workflows

| Workflow | Runs on | Does |
| --- | --- | --- |
| `release.yml` | push to `main` | builds `ANNIHILA.TOS` onto the `latest` release and moves the `latest` tag |
| `pr.yml` | pull requests | builds, nothing more |
| `pages.yml` | push to `main` touching `web/`, `tools/*.py` or itself | tests and builds `web/` and publishes it to `gh-pages`, served at [labs.neilrackett.com/ataritt-annihilattion](https://labs.neilrackett.com/ataritt-annihilattion/) |

`release.yml` and `pr.yml` ignore pushes that touch only the docs or
`web/`.

## The video and GIF

`docs/images/annihilattion.mp4` and `.gif` were recorded in Hatari with its
AVI output (`--avi-file`, started and stopped through `--cmd-fifo`) while
two scripts ran in real time, with `slow`: the menus and briefing, then
mission 2. The battle script moves the pointer a step a frame with `mouse`:
it drags a box round the army, sends it at the CORE base from the minimap
and scrolls down into the land no unit has seen. Then `follow` moves the
pointer to the screen's edges to scroll after the Commander, and the script
goes `fast` to the river between two recordings, from one `shot RECA` to the
next `shot RECZ`. The MP4 joins the three with fades, using ffmpeg (x264,
`-tune animation`). The GIF is the two battle recordings, cut together
without a fade, at 12 frames a second through a 16-colour palette made from
`docs/images/annihilattion.png`: a fade turns muddy in 16 colours. End the
march before anything you don't want shown, such as a DEFEAT screen, and
cut the briefing in a pause in the narration.

## On the real TT

Untested there so far. If `ANNIHILA.TOS` hangs at a blank screen, ask about
the AUTO folder and GemDrive first: that combination, not the toolchain,
hung earlier builds on Neil's TT.
