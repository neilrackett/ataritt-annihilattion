# On a Falcon (an experiment)

The same `ANNIHILA.TOS` also starts on a Falcon030 with a VGA monitor and
6 MB of free memory, in practice a 14 MB Falcon, and on any other Falcon says
what is missing. It is a base for later work rather than a target, and has
only run in Hatari's Falcon (with EmuTOS), never on a real one.

- It shows the TT's picture in the Falcon's own 640x480 16-colour mode; all
  its memory is the Falcon's one kind, as it has no TT-RAM.
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
or `TOS`.
