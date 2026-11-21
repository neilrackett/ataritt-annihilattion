# Playing AnnihilaTTion

## What it plays

- The Arm campaign of the Total Annihilation demo: **1: A Hero Returns**,
  **2: CORE KBot Base, Destroy It!** and **3: Spider Technology**, each at
  Easy, Medium or Hard (the missions' own three unit layouts), with their
  briefings and spoken narration.
- The missions' own objectives: reach the Galactic Gate; destroy the CORE
  KBot Lab; bring a Spider home. Losing the Commander, the Gate, every Spider
  or every unit loses the mission, as the mission files say.
- The CORE units follow the missions' scripts (patrols, waits, attacks on
  named unit types, factories building on a timer); the computer player also
  builds from idle factories and sends groups of idle units to attack.
- The demo's 32 Arm and CORE units, drawn from their 3D models in 16
  directions, with their weapons (lasers, cannons, rockets, the E.M.G., the
  Spider's paralyzer, the Commander's D-gun), wrecks to reclaim, trees for
  energy, metal deposits for extractors, solar collectors and wind
  generators, labs and vehicle plants with their build queues, and
  construction by the Commander's and construction units' nanolathes.
- Total Annihilation's screen layout at 640x480: the side panel with the
  minimap, BUILD and ORDERS pages and the unit pictures, the metal and energy
  bars at the top, the unit bar at the bottom; the game's fonts, cursors,
  buttons and menu pictures; line of sight (CORE units show only where yours
  can see).

## What it leaves out

These are the compromises the TT needs, and they are deliberate:

- 16 colours: one palette chosen for the three green-planet maps and the
  units, with ordered dithering. The menus and briefings each have their own.
- Units are pictures made from their models, not models drawn in real time:
  no walking legs or turning turrets (a walking unit bobs; a unit faces what
  it fires at); buildings face one way; no shadows under units.
- The map view scrolls in 16-pixel steps across and 1-pixel steps down;
  rocks, metal deposits and the trees' shadows are part of the map's tiles.
  Similar map tiles are drawn with one tile so each map fits in memory.
- The simulation runs 15 times a second (the original runs 30); units can
  overlap; paths are found on 32-pixel cells; the original's unit scripts
  (COB) do not run.
- One sound plays at a time, mostly at 6258 Hz; no music (the demo has none).
- No saving, multiplayer, skirmish or options beyond sound and difficulty.

## Controls

| Input | Action |
| --- | --- |
| Left click | Select a unit; with units selected, move to the ground, attack an enemy, or reclaim a tree or wreck |
| Left drag | Select the units in a box |
| Shift + click or drag | Add to the selection |
| Shift + an order | Queue it after the units' orders |
| Right click | Cancel an order button, or clear the selection |
| Minimap: left click | Look there |
| Minimap: right click | Send the selected units there |
| Screen edges, arrow keys | Scroll |
| M, A, S, P, G, D, R | Move, Attack, Stop, Patrol, Guard, D-gun, Reclaim |
| B, O | The side panel's BUILD or ORDERS page |
| C | Find the Commander |
| Ctrl+A | Select every unit |
| Esc | Pause (Q then quits the mission) |
| X | Sound on or off |

On the side panel, BUILD shows what the selected builder makes: click a
picture, then a place on the map (green: it can go there). For a lab or a
plant, a left click adds one to its queue (Shift: five) and a right click
takes one away. Pointing at a build picture shows its cost.

## On the TT

- TOS 3.06 with 2 MB of ST-RAM and 4 MB of TT-RAM: the program loads into
  TT-RAM and keeps the screens, sounds and narration in ST-RAM. With
  mission 3 loaded about 1 MB of each is still free.
- It needs the TT's own video, and stops with a message on any other machine
  but a Falcon ([an experiment](falcon.md)).
- The 68030's caches are switched on while it runs, and put back on exit.
- With a GemDrive (ACSI2STM) card, a program in the card's AUTO folder has
  been seen to make later C programs hang at a blank screen: rename the AUTO
  folder if `ANNIHILA.TOS` hangs at start.

## Speed

The game keeps 15 ticks a second of game time when the machine keeps up, and
slows down rather than skipping when it does not.
