/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Sound effects on the TT's DMA sound: one voice, so a new effect takes it
 * over unless the one playing matters more (prio), and the briefings'
 * narration streams through the same voice.
 */
#ifndef SND_H
#define SND_H

#include <stdint.h>

/// Plays a sound effect, unless sound is off or a sound that matters more
/// plays; one of the same rank is cut off only after it has played 80 ms.
///
/// @param id the sound, an index into SOUNDS.DAT (SND_* for the interface's)
/// @param prio how much it matters: higher cuts off lower
void snd_play(int id, int prio);

/// Plays a long run of samples that no effect may cut off, such as a
/// briefing's narration, through a ring of 64 KB in ST-RAM; keep calling
/// snd_stream_update() while it plays. Plays nothing when sound is off or
/// ST-RAM is short.
///
/// @param data signed 8-bit samples, in any RAM, kept until it stops
/// @param len the number of samples
/// @param rate 0 for 6258 Hz (only where tt_plays_6258), 1 for 12517 Hz
void snd_stream(const int8_t* data, int32_t len, int rate);

/// Refills the stream's ring, and stops it after its last sample. Call it
/// at least every 2.6 seconds at 12517 Hz.
void snd_stream_update(void);

/// Stops whatever plays.
void snd_stop(void);

extern int snd_enabled; ///< 0 when the player turned sound off

#endif
