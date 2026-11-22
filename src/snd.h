/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Sound on the TT's DMA sound: a mixer of four channels of effects and a
 * stream read from the disk as it plays (a briefing's narration, or the
 * CD's music), signed 8-bit mono at 12517 Hz. DMA sound plays a short ring
 * in ST-RAM over and over; snd_update(), once a frame, mixes what comes
 * next into the part it has played.
 */
#ifndef SND_H
#define SND_H

#include <stdint.h>

/// Takes the ring in ST-RAM and starts DMA sound on it, silent.
///
/// @return 1, or 0 when ST-RAM is short (the game is then silent)
int snd_init(void);

/// Stops DMA sound and gives the ring back.
void snd_exit(void);

/// Plays a sound effect on a free channel, or one playing something that
/// matters less (prio); the same sound started again within 60 ms is left
/// out, so that a volley sounds once.
///
/// @param id the sound, an index into SOUNDS.DAT (SND_* for the interface's)
/// @param prio how much it matters: higher cuts off lower
void snd_play(int id, int prio);

/// Plays a file of signed 8-bit samples at 12517 Hz on the stream channel,
/// in place of what it played; it is read as it plays.
///
/// @param name the file
/// @return 1 when it plays, 0 when it cannot be opened
int snd_stream_file(const char* name);

/// Tells whether the stream channel plays.
///
/// @return nonzero while it does
int snd_stream_busy(void);

/// Mixes what comes next, reading the stream's file when it needs more.
/// Call it once a frame; the ring holds about 0.65 seconds.
void snd_update(void);

/// Plays the CD's music in a mission: calm tracks, or loud ones while
/// there is fighting, each to its end and then another of the kind wanted.
/// Does nothing when the data has no music.
///
/// @param battle nonzero while the player's units fight
void snd_music(int battle);

/// Stops the effects and the stream.
void snd_stop(void);

/// Stops the effects and the stream, and clears the ring: the end of a
/// mission or a film.
void snd_silence(void);

extern int snd_enabled; ///< 0 when the player turned sound off
extern int snd_music_on; ///< 0 when the player turned the music off

#endif
