# On a Falcon (an experiment)

The same `ANNIHILA.TOS` also starts on a Falcon030 with a VGA monitor, an
RGB monitor or a TV, and 6 MB of free memory, in practice a 14 MB Falcon,
and on any other Falcon says what is missing. It is a base for later work
rather than a target, and has only run in Hatari's Falcon (with EmuTOS),
never on a real one.

- It shows the TT's picture in 640x480 in 16 colours; all its memory is the
  Falcon's one kind, as it has no TT-RAM.
- On VGA that is the Falcon's own mode. On an RGB monitor or a TV it is
  640x400 interlaced made 480 lines tall, with 40 more lines at each end as
  the Falcon's overscan modes have, and the same 640 pixels a line. PAL or
  NTSC is the desktop's. NTSC has too few lines below the picture for 40
  more, so there the picture moves up 3 lines, and a TV may crop its top
  and bottom rows. Interlaced, each line shows 25 times a second in PAL, so
  text and thin lines flicker on a CRT: a flicker fixer or an upscaler
  takes that away.
- A Falcon plays no sound at 6258 Hz, so the sound effects made at that
  rate are doubled to 12517 Hz as they load: about 420 KB more.
- A stock Falcon has no FPU, without which mintlib's start-up stops the
  program; `src/tt.c` puts its own CPU check in place of mintlib's, asking
  only for a 68030 (the game uses no floating point).
- In Hatari's Falcon it draws 20 to 27 ms a frame with the view still and
  60 ms while it scrolls every frame, 2 to 3 times as long as in Hatari's TT.

The Falcon's mode, colours, caches and sound routing are set and put back in
one part of `src/tt.c`; the rest of the game only knows that it plays no
6258 Hz sound.

`MACHINE=falcon make play` and `MACHINE=falcon make test` run it in a 14 MB
Falcon on VGA in Hatari, with EmuTOS (`tmp/tos/EmuTOS 1.4 - UK - 512K.img`),
or `TOS`. `MONITOR=rgb` or `MONITOR=tv` runs it on an RGB monitor or a TV
instead. Hatari's Falcon keeps its PAL or NTSC setting in `hatari.nvram`, in
Hatari's own folder; without that file, an RGB monitor or a TV starts in
PAL. Hatari shows the whole picture, but not the flicker or what a real
monitor or TV would crop.
