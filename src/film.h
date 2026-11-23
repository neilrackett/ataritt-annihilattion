/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The full game's films, from FILMn.DAT and FILMn.SND (see
 * tools/convert.py): 320x240 at 15 frames a second in TT low resolution,
 * each line shown on every other line of the screen, as TA showed them.
 */
#ifndef FILM_H
#define FILM_H

enum { FILM_LOGO = 1, FILM_INTRO, FILM_ARM_WINS, FILM_CORE_WINS, FILM_CREDITS };

/// Plays a film with its sound, until it ends or a key or click skips it.
///
/// @param n the film, FILM_*
/// @param skip called while it plays, with the frames shown so far: nonzero to stop
/// @return the frames shown, or -1 when there is no such film or the
/// machine cannot show it
int film_play(int n, int (*skip)(int shown));

/// How the last film played, for the test log: the size of the ring it was
/// read ahead into, the milliseconds spent reading it and putting its frames
/// on the screen, the frames shown more than a frame's time after theirs,
/// and the latest one's lateness
typedef struct {
    long ring_kb, read_ms, draw_ms, late_ms;
    int late;
} FilmStats;

extern FilmStats film_stats;

#endif
