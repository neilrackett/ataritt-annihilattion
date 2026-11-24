# Working on AnnihilaTTion

AnnihilaTTion is a Total Annihilation engine for the Atari TT, playing the
free demo's three Arm missions, or the full game's two campaigns from its
CDs, in TT medium (640x480, 16 colours). It is a small engine of its own,
written in C for the 68030, plus a Python converter that turns the game's
data into what the engine reads; the demo and the full game convert to the
same files, and one program plays either. This file holds the
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
  (`ANNIHILA.TOS`, and the demo's data in `dist/DATA`) and `dist/full` (the
  full game's, its data in `dist/full/DATA`);
  the demo, the CDs' images and what they unpack to, test runs and TOS
  images in `tmp/` (`tmp/ta-demo`, `tmp/disks`, `tmp/ta-full`, `tmp/runs`,
  `tmp/tos`). Nothing points at other checkouts on Neil's computer.
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
./build.sh --full                # the full game, from the CDs in tmp/disks, into dist/full
STCMD_NO_TTY=1 stcmd make        # compile dist/ANNIHILA.TOS only (agents, CI)
```

gcc 13 `m68k-atari-mintelf` with mintlib, through `stcmd`
([atarist-toolkit-docker](https://github.com/neilrackett/atarist-toolkit-docker)).
Keep the build free of warnings. The converter needs `python3` with numpy and
Pillow, and ffmpeg for the full game's films (they are left out without it).
`build.sh` converts again whenever a `tools/*.py` is newer than
`GAME.DAT`; the demo takes about a minute, the full game about half an hour.
`OA_ONLY=ac05,cc12` converts only those missions and their worlds, to try
things out quickly (into another folder: the result is incomplete).

## Test

`tools/test_hatari.sh NAME SCRIPT [SECONDS]` runs a script headless; the log is
`tmp/runs/NAME/OALOG.TXT`, which ends in `result N` for a mission.
`MACHINE=falcon` runs the Falcon (`MONITOR=rgb` or `tv` for its RGB
mode), `PRG=` another build, `PROF=1` writes a profile for
`tools/profsum.py`, `CLOCK=16` runs the CPU at 16 MHz.

**Before committing a change to the simulation that should not change what
happens**, run scripts that are `fast` from their first line with both
builds and compare their `units` lines, which give every unit's place and
hit points. A script that starts at 15 ticks a second runs differently
whenever a frame takes longer.

What each script should end with, on the TT and the Falcon alike:
`m1_gate`, `win2` and `win3` win (`result 1`); `long2` and `m2_attack` lose
(`result -1`); every other mission script ends at `result 0`; `menu` has no
result.

For the full game, `tools/test_missions.sh tests/full/look.txt` runs a
script on each of its 50 missions (`DATA=dist/full`), or those named; each
must load and play to its end without failing.

**Before committing a change to drawing**, check that it still draws the
same:

- Keep a build of the last commit to compare against:
  `git archive HEAD src Makefile | tar -x -C build/old`, then
  `stcmd make` in `build/old`.
- Battle screenshots must stay pixel-identical: run the same script with
  both builds (`PRG=build/old/dist/ANNIHILA.TOS`) and compare the PNGs.
- The `check` command logs where the composite differs from the background
  with the units drawn on it afresh, and the screen from the composite.
  Outside what the frame drew over the units (shots, effects, boxes), the
  first must be 0; the screen may differ only under the pointer. Run it at
  awkward camera positions, so the rings' seams cross the view.

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
- **Marks.** A unit that draws the same as last frame stays on the
  composite (`drawn[]`: its sprite, box, look and place in the order, as it
  can change places without moving). The cells to draw again, a bit each
  in `dirty`, are where the background changed, where last frame's shots,
  effects and boxes went (`mark()`, a `comp_blit()` marks its own) and the
  old and new boxes of units that changed. Every unit that meets them adds
  its whole box, until none does, so that what lies under and over is drawn
  again in order. `comp_restore()` takes those cells from the background,
  their units are drawn, and the rest goes over them. Each screen copies
  the cells marked since it was last drawn (`comp_marks()` keeps three
  frames'), or the whole view after the camera moves. That holds because
  `tt_flip()` waits for a blank when a frame took less than one. Without
  the wait, the next frame replaced the last screen before it was seen,
  the screen on show stayed there, and when it came round again its marks
  were gone, so it copied the whole view: a faster simulation made the
  drawing slower. Anything new drawn on the composite that is not a unit
  is drawn after the units, and marked.
- **The interface draws only what changed**, into `ui`, and each screen
  copies the parts it lacks. The bars are three parts (metal, energy and
  the line below the battlefield), each with its own key and box. The
  minimap remembers each unit's dot and the view's box, and draws and
  copies only the rows where one of them, or what is explored, changed.
  Comparing each drawing with the last instead cost as much as the copies
  it saved, as it read 16 KB of TT-RAM every time.
- **Copy loops.** GCC turns simple copy loops into `memcpy` calls, which cost
  more than they save on short runs. Inlined into a big function, its loops
  spill pointers to the stack. `copy_run()` is a small assembly loop (two
  `move.l` and a `dbra`) for that reason. The mixer's inner loops
  (`put_samples()` and those after it) are assembly too: GCC's took more
  than twice the instructions, and the mixer twice the time. Look at the
  object code (`m68k-atari-mintelf-objdump -d build/game.o`) before
  trusting a loop.
- **DMA sound reads only ST-RAM.** The narration and the music are too long
  to keep there at 12517 Hz, so `snd_stream_file()` reads them from the disk
  as they play into a 64 KB ring in any RAM, and the mixer takes them from
  there into the 8 KB ring the DMA plays.
- **The explored map.** It is kept in 32x32 sight cells, and the fog tiles
  lie 16 pixels off them, so each 16x16 group cell is one quarter of a tile
  and the masks stay on whole groups. Explore after each `sim_tick()`, not
  each frame, so that the fog doesn't depend on the frame rate. Whether an
  enemy shows is `unit_shown()`, for drawing and picking alike. The `check`
  command compares the background with a fresh drawing, which catches a
  missed redraw.
- **Per-call costs add up.** A function call, a modulo or a division per
  row or per group cost more than the work. Step pointers instead, and wrap
  ring columns with `& (RING_G - 1)`. Hatari counts a `muls.l` as about 4
  cycles where the 68030 takes up to 44, so its profiles hide multiplies:
  `build_buckets()` takes its rows from tables, and empties only the
  buckets it filled. Work done each frame for the units under the pointer,
  or each tick for the player's, is done once and kept: `hover_unit()` for
  both the hover and the pointer's shape, `explore()`'s list of the units
  that explore and `selected()`'s counts, made again when `sim_roster`
  says a unit came, went, was finished or changed sides.
- **Memory is the full game's limit.** A mission's file is sized by the
  converter to fit `MISSION_BUDGET`: its cells, the sim's and the explored
  map's arrays (`map_alloc()`, `ex_alloc()`, sized per map when it starts)
  and what is left for tiles; keep the converter's sums in step with the
  engine's. Overrunning one of those blocks corrupts the next one silently:
  the explored map broke so once.
- **The units' sprites are a cache** (`sprites.c`): one block from TOS,
  shared out by its own first-fit list, since TOS has only so many memory
  descriptors. It gives up what was drawn longest ago, off screen too, and
  reads it back when needed; a type held only while alive left aircraft
  invisible.
- **Paths never look for what is out of reach, and no tick waits on a
  search.** Each move class's parts of the map are labelled when a mission
  starts (`label_regions()`); a goal in another part becomes the nearest
  cell in reach, and the computer's waves pick targets they can reach.
  Searches run one at a time, `PATH_BUDGET` cells a tick (a cell costs
  about 2000 cycles), up to `PATH_NODES`; a unit heads straight for its goal
  until its path comes. A low cap left units in the dead ends of winding
  maps. Searches that ran to their limit for land units across water took
  200 ms a tick on the archipelago.
- **A search for the nearest enemy looks only where one could be.** Each
  side's units are also kept in cells of 4 x 4 buckets, filled with the
  buckets each tick, and `nearest_enemy()` looks through those within its
  radius and `reach`, the furthest a unit moves in a tick. It keeps the
  lowest-numbered of the nearest, as going down the enemy's whole list did.
  That list, for searches wider than three buckets (most of them, by weapon
  range or sight), took 30% of AC25's time. Most cells are empty, and most
  searches meet no unit at all, so the cells are stepped through along a
  row, and a search is set up only when it meets a unit (`search_cell()`):
  a multiply for each cell, and setting up every search, cost as much
  again as the units themselves.
- **Most units, most ticks, do nothing.** In AC25 nine units in ten are
  armed, and nearly all of them have no target, don't move and aren't
  reloading; the computer's wait on their scripts. `unit_step()` keeps
  their step short: the other orders (`order_step()`), the look for a
  target and the firing are out of line, and what it asks of a unit's type
  is a byte (`type_step[]`), not two of the cache's lines. GCC inlines a
  function called once, and with the whole order switch inlined every
  unit's step saved 11 registers. The fields a tick reads come first in
  `Unit`, whose size stays a multiple of 4.
- **The economy and sight are worked out again only when they change.**
  `economy_step()` sums what the units make and use only when `eco_stale`
  is set: by a unit finished, destroyed or captured, or by the wind. Set it
  from anything new that changes those. Metal makers, which stop while
  energy runs short, are added each tick. `update_sight()` draws the discs
  again only when one of the player's units came into another bucket, came
  or went.
- **A building touching a path cell closes it**: buildings 16 pixels apart
  wall units in. The skirmish computer leaves 32 pixels round its own, and
  the lane in front of a factory clear.
- **TA's solar collectors make their energy as negative upkeep**
  (`EnergyUse=-20`, `EnergyMake=0`), and a radar makes what it uses:
  energy is `EnergyMake` less the upkeep of buildings switched on. Until
  the upkeep counted, solar collectors made nothing.
- **The computer builds bases in skirmishes only** (`computer_base()`), so
  the campaigns, the demo's included, play as their scripts say.
- **Read into TT-RAM with `data_read()`.** The disk's DMA reaches only
  ST-RAM, so the driver copies what is bound for TT-RAM itself, as fast as
  that driver does. On Neil's TT a read into ST-RAM takes 1.2 s a megabyte.
  Reading 8 KB at a time into a buffer in ST-RAM and copying it adds 0.4 s.
  GemDrive (Neil's driver is in `../acsi2stm`) adds about 0.35 s with
  `GEMDRIVE.PRG` from his release of 23 November 2026, but more than the
  buffer from its boot sector or with older firmware. So `data_read()`
  reads its first megabyte half each way, in runs of 256 KB, and then
  keeps to the faster one: use it for every read into TT-RAM. Time runs,
  not single chunks: 8 KB each way in turn took 25 ms a pair, five ticks of
  the 200 Hz clock, so the rounding fell on the same side every time and
  the faster way looked slower. GemDrive before that release also read more
  slowly the further into a file it was, so compare figures from the same
  firmware. Hatari reads in no time at all, so only the TT shows any of
  this; the `disk` test command measures it there, and logs which way
  `data_read()` chose.
- **The films' budget.** At those speeds a TT reads and draws about 30 KB of
  film in a frame's time. The converter gives each frame 26 KB, lets quiet
  frames save up to 2 MB for busy ones, and leaves out a frame that would
  overdraw it (`FILM_FRAME_BYTES`, `FILM_BANK`). The player reads
  `FILM_LEAD`, 1 MB, before the first frame: keep it in step with
  `tools/convert.py`. Leaving out groups instead of frames tore the picture.
- **Sound is mixed**, not played: DMA sound loops a short ring that
  `snd_update()` fills a little ahead of it, every frame, menus included.
  It silences what has played, so a short wait without it (a disk read) is
  quiet; a longer one (a loading screen) calls `snd_silence()` first, or
  the last frame's sound comes round again with the ring.
- **TOS loads the program on any even address.** In Hatari it lands 2
  bytes past a long, so every long in its data, on its stack and in its
  blocks from `Mxalloc` straddled two of the bus's. `main()` runs the game
  on a stack rounded down to 16 bytes (`tt_call_aligned()`), and
  `tt_alloc_st()` and `tt_alloc_any()` give blocks on 16-byte boundaries:
  drawing and the simulation each took 4 to 7% less. Putting `units[]` on
  a boundary as well made no difference.
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
  `dist/DATA`. It skips itself without the installer, as on GitHub's runners.
  Run it after any change to `tools/*.py`. The web converter does the demo
  only; the full game's CDs and films are `build.sh --full`'s.
- TA's data has its typos: numbers such as `3.6.`, a script moving a unit
  to x 81200. Read it forgivingly, and keep what it says within range.

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

The demo plays on Neil's TT, and so does the full game's first mission; the
rest of the full game has been tried in Hatari only. If `ANNIHILA.TOS` hangs
at a blank screen, ask about the AUTO folder and GemDrive first: that
combination, not the toolchain, hung earlier builds on Neil's TT.
