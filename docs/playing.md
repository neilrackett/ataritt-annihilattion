# Playing AnnihilaTTion

AnnihilaTTion plays whichever data you give it: the free demo's, or the full
game's, converted from your own CDs, with its two expansions' if you have
them too ([building](building.md)).

## What it plays

The demo:

- The Arm campaign of the Total Annihilation demo: **1: A Hero Returns**,
  **2: CORE KBot Base, Destroy It!** and **3: Spider Technology**, each at
  Easy, Medium or Hard (the missions' own three unit layouts), with their
  briefings and spoken narration.
- The demo's 32 Arm and CORE units.

The full game:

- Both campaigns, Arm's and Core's, 25 missions each, at Easy, Medium or
  Hard, with their briefings and narration. A campaign opens its missions
  as you win them, and remembers how far you got (`PROGRESS.DAT`).
- All 157 of the game's units: KBots and vehicles, aircraft, ships and
  submarines, amphibious tanks, and the buildings that make and use them,
  from shipyards and air plants to fusion plants, metal makers, radar,
  geothermal plants on their vents, tidal generators on the water, nuclear
  missile silos and the systems that shoot their missiles down.
- Skirmishes on 24 of the multiplayer maps, as Arm or Core: your Commander
  against the computer's, each from nothing, the computer building its base
  as a player would (extractors, power, factories, defences) and attacking.
- The CD's music, calm while you build and loud while you fight, and the
  films: the Cavedog logo and the introduction at the start, and each side's
  victory and the credits at the end of its campaign.

With the expansions, The Core Contingency and Battle Tactics (1998), as
well:

- The Core Contingency's two campaigns, Arm's and Core's, 12 missions each,
  on its new worlds: Acid, Crystal, Lusch, Slate, Urban and Water.
- Battle Tactics' eight campaigns, each side's very short, short, medium and
  long: 92 of its 100 missions, any of them at any time, as there.
- Their 115 units, 272 in all: hovercraft and their platforms, seaplanes,
  mines, flak, underwater buildings and the rest, on the builders' menus
  where the expansions put them.
- Skirmishes on 40 maps, the expansions' among them.
- The single player screen lists the campaigns by game.

In both:

- The missions' own objectives: destroying the enemy, capturing a Gate or a
  unit, building a shipyard, reaching a place, killing a Commander, getting
  a ship past a line. Winning any of a mission's objectives wins it; losing
  the Commander, a unit you must keep, or every unit loses it, as the
  mission files say. A timed mission counts down at the bottom of the screen.
- The enemy follows the missions' scripts (patrols, waits, attacks on named
  unit types, factories building on a timer, crawling bombs blowing up); the
  computer player also builds from idle factories, sends groups of idle
  units at whatever of yours they can reach, and fires its missiles.
- Units drawn from their 3D models in 16 directions, with their weapons
  (lasers, cannons, rockets and missiles, the E.M.G., the Spider's
  paralyzer, the Commander's D-gun, bombs, torpedoes, nuclear missiles),
  wrecks to reclaim, trees for energy, metal deposits for extractors, and
  construction by nanolathe. Commanders capture enemy units; construction
  units repair yours.
- Total Annihilation's screen layout at 640x480, with Arm's or Core's panels
  as you play either side: the side panel with the minimap, BUILD and ORDERS
  pages and the unit pictures, the metal and energy bars at the top, the
  unit bar at the bottom; the game's fonts, cursors, buttons and menu
  pictures; line of sight (enemy units show only where yours can see, and on
  the minimap where your radar does), and the map black, on the battlefield
  and the minimap, until your units have explored it.

## What it leaves out

These are the compromises the TT needs, and they are deliberate:

- 16 colours: a palette for each world, chosen from its maps and the units,
  with ordered dithering. The menus and briefings each have their own.
- Units are pictures made from their models, not models drawn in real time:
  no walking legs or turning turrets (a walking unit bobs; a unit faces what
  it fires at); buildings face one way; no shadows under units or aircraft.
- The map view scrolls in 16-pixel steps across and 1-pixel steps down;
  rocks, metal deposits and the trees' shadows are part of the map's tiles.
  Similar map tiles are drawn with one tile so each map fits in memory: the
  largest maps lose the most detail.
- The simulation runs 15 times a second (the original runs 30); units can
  overlap; paths are found on 32-pixel cells; the original's unit scripts
  (COB) do not run.
- Aircraft hover where they stop instead of landing, and transports carry
  nothing; there is no cloaking, and jammers hide nothing. With the
  expansions, the targeting facility finds nothing for your units, and acid
  seas harm nothing.
- Sound is mono at 12517 Hz: four effects at a time over the narration or
  the music. The films play at 15 frames a second, 320x240 in 256 colours,
  each line on every other line of the screen.
- The other 13 multiplayer maps are too large for the TT's memory (56 with
  the expansions'), and so are eight of Battle Tactics' missions and The
  Core Contingency's Krogoth Encounter. A skirmish has one computer player.
- No saved games, multiplayer or options beyond sound and difficulty.

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
| M, A, S, P, G, D, R | Move, Attack, Stop, Patrol, Guard, D-gun (or a silo's missile), Reclaim |
| T, E | Capture, Repair |
| B, O | The side panel's BUILD or ORDERS page |
| C | Find the Commander |
| Ctrl+A | Select every unit |
| Esc | Pause (Q then quits the mission) |
| X | Sound on or off |
| On the title: I, C | The introduction, or the credits (the full game) |

On the side panel, BUILD shows what the selected builder makes, as far as
the mission allows: click a picture, then a place on the map (green: it can
go there). For a factory, a left click adds one to its queue (Shift: five)
and a right click takes one away. Pointing at a build picture shows its
cost. A key or a click skips a film.

## On the TT

- TOS 3.06 with 2 MB of ST-RAM and 4 MB of TT-RAM: the program loads into
  TT-RAM and keeps the screens and sounds in ST-RAM. A mission's map takes
  up to about 1.9 MB, and the units' sprites the TT-RAM left over (about
  1 MB); they are read from the disk as units come into view.
- The narration, the music and the films are read from the disk as they
  play: the full game needs a hard disk, not floppies. Its data takes about
  300 MB, a third of it the films.
- It needs the TT's own video, and stops with a message on any other machine
  but a Falcon ([an experiment](falcon.md)).
- The 68030's caches are switched on while it runs, and put back on exit.
- With a GemDrive (ACSI2STM) card, a program in the card's AUTO folder has
  been seen to make later C programs hang at a blank screen: rename the AUTO
  folder if `ANNIHILA.TOS` hangs at start.

## Speed

The game keeps 15 ticks a second of game time when the machine keeps up, and
slows down rather than skipping when it does not.
