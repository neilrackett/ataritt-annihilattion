# Testing AnnihilaTTion

`make test TEST=tests/SCRIPT.txt` builds, then runs
`tools/test_hatari.sh SCRIPT tests/SCRIPT.txt`. That runs `ANNIHILA.TOS`
headless in Hatari as the target TT (with the TOS image in `tmp/tos`, or
`TOS`; see [Building](building.md#make)), fast-forwarded, with the script as
`OATEST.TXT`. The run's folder is `tmp/runs/SCRIPT` (or `NAME`): the
program's log is `OALOG.TXT` there, and every shot the script took is a PNG.

- `MACHINE=falcon` runs it as a [Falcon](falcon.md).
- `PRG` names another program to run, to compare two builds.
- `PROF=1` also writes Hatari's profile, which `tools/profsum.py` sums per
  function.

## Scripts

A script starts with `mission N DIFFICULTY` (or `menu`, for the menus) and
then gives one command a line:

| Command | What it does |
| --- | --- |
| `wait FRAMES` | lets frames pass |
| `fast`, `slow` | a tick every frame from now on, however long a frame takes; or back to 15 ticks a second |
| `mouse X Y BUTTONS`, `key SCAN ASCII` | the pointer and buttons, or a key (hex) |
| `camera X Z` | looks at a place on the battlefield |
| `follow`, `follow 0` | scrolls the view after the Commander from now on, as scrolling at the screen's edges does; or stops |
| `explore` | uncovers the whole map, as if every part of it had been seen |
| `shot NAME` | a screenshot |
| `stats`, `units`, `res`, `proj`, `feat` | logs the timings, the units, both players' resources, the shots in flight, or the features in view |
| `check` | logs where the screen differs from the buffers it is drawn from, and where the background differs from the same drawn afresh |
| `debug UNIT` | logs a unit's movement and path |
| `kill TYPE`, `teleport UNIT X Z` | destroys every CORE unit of a type, or moves a unit |
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

The web converter has its own test: `pnpm test` in `web/` converts the
installer through Pyodide and checks every file against `dist`
([building](building.md#or-in-a-browser)).
