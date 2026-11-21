/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Sound effects on the TT's DMA sound: one voice, so a new effect takes it
 * over unless the one playing matters more (prio), and the briefings'
 * narration plays through the same voice.
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

/// Plays samples that no effect may cut off, such as a briefing's narration.
///
/// @param data signed 8-bit samples in ST-RAM
/// @param len the number of samples
/// @param rate 0 for 6258 Hz, 1 for 12517 Hz
void snd_raw(const int8_t* data, int32_t len, int rate);

/// Stops whatever plays.
void snd_stop(void);

extern int snd_enabled; ///< 0 when the player turned sound off

#endif
