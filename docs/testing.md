# Testing AnnihilaTTion

`make test TEST=tests/SCRIPT.txt` builds, then runs
`tools/test_hatari.sh SCRIPT tests/SCRIPT.txt`. That runs `ANNIHILA.TOS`
headless in Hatari as the target TT (with the TOS image in `tmp/tos`, or
`TOS`; see [Building](building.md#make)), fast-forwarded, with the script as
`OATEST.TXT`. The run's folder is `tmp/runs/SCRIPT` (or `NAME`): the
program's log is `OALOG.TXT` there, and every shot the script took is a PNG.

- `MACHINE=falcon` runs it as a [Falcon](falcon.md), and with it
  `MONITOR=rgb` or `MONITOR=tv` on an RGB monitor or a TV instead of VGA.
- `PRG` names another program to run, to compare two builds.
- `PROF=1` also writes Hatari's profile, which `tools/profsum.py` sums per
  function.
- `DATA` names another game's folder, its data in `DATA` there, such as
  `dist/full` for the full game.
- `CLOCK` sets the CPU's speed in MHz, such as `CLOCK=16`.

`tools/test_missions.sh SCRIPT [MISSION...]` runs a script on each of the
full game's missions and skirmish maps in turn, or those named
(`ac05 cc12 sk01`), with the game in
`dist/full`: the script's word `MISSION` becomes the mission's name, and a
line a mission says how it ended and what the units' sprites took.

## Scripts

A script starts with `mission N DIFFICULTY` (or `menu`, for the menus) and
then gives one command a line. N is the mission's number, 1 on, or its
file's name, such as `ac05`; for a skirmish (`sk01` on), `core` after the
difficulty plays Core.

| Command | What it does |
| --- | --- |
| `wait FRAMES` | lets frames pass |
| `fast`, `slow` | a tick every frame from now on, however long a frame takes; or back to 15 ticks a second |
| `mouse X Y BUTTONS`, `key SCAN ASCII` | the pointer and buttons, or a key (hex) |
| `camera X Z` | looks at a place on the battlefield |
| `follow`, `follow 0` | from now on, moves the pointer to the screen's edges whenever the Commander strays from the middle of the view, so that the view scrolls after it as a player's would; or stops |
| `explore` | uncovers the whole map, as if every part of it had been seen |
| `shot NAME` | a screenshot |
| `stats`, `units`, `res`, `proj`, `feat` | logs the timings, the units, both players' resources, the shots in flight, or the features in view |
| `check` | logs where the composite differs from the background with the units drawn afresh, the screen from the composite, and the background from the same drawn afresh |
| `debug UNIT` | logs a unit's movement and path |
| `kill TYPE`, `killall`, `teleport UNIT X Z` | destroys every computer unit of a type, or all of them, or moves a unit |
| `spawn TYPE X Z [1]`, `give TYPE` | puts down a unit of the player's (or with 1 the computer's), or hands the player the computer's first unit of a type, as a capture does |
| `move UNIT X Z`, `attack UNIT TARGET`, `capture UNIT TARGET` | orders a unit to a place, or to attack or capture another |
| `explored`, `sprites` | logs the explored map, or what the units' sprites hold |
| `film N [FRAMES]` | plays a film, or its first frames, and logs how long it took, how much of that went on reading and on drawing, and how many frames came late |
| `disk FILE` | logs how fast a data file reads, into ST-RAM, into TT-RAM and into TT-RAM as the game reads (`data_read()`), in reads of 8, 32 and 64 KB, each from the next megabyte of the file; then into ST-RAM again, to show whether the place in the file matters, how long copying a megabyte from ST-RAM to TT-RAM takes, and which way `data_read()` chose |
| `result` | logs whether the mission is won or lost |
| `end`, `quit` | ends the mission, or the program |

The scripts in `tests/` cover:

- winning mission 1 by reaching the Gate (`m1_gate.txt`), and the mission 2
  and 3 objectives (`win2.txt`, `win3.txt`)
- building with the Commander, factory queues, reclaiming and the D-gun
  (`m2_build.txt`, `m2_factory.txt`, `m2_reclaim.txt`)
- 15 minutes of CORE attacks (`long2.txt`), a fight on the way to the Gate
  (`battle.txt`), every unit sent against the CORE base and the Commander's
  walk there (`m2_attack.txt`, `m2_com.txt`)
- the menus and briefing (`menu.txt`)
- a first look at each mission (`smoke1.txt`, `m2_look.txt`, `m3_look.txt`)
  and the memory left with the largest mission loaded (`mem.txt`)
- speed (`fps1.txt`, `fps3.txt`, `scroll.txt`)

and, for the full game with `tools/test_missions.sh`, in `tests/full/`:

- a look at a mission's start and whole map, and a few minutes of play
  (`look.txt`)
- the mission once every enemy unit is destroyed, won where that is one of
  its objectives (`win.txt`)
- ten minutes of a skirmish, the computer building its base and attacking
  (`skirmish.txt`, on the `sk` maps)

The web converter has its own test: `pnpm test` in `web/` converts the
installer through Pyodide and checks every file against `dist/DATA`
([building](building.md#or-in-a-browser)).
