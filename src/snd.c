/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Sound effects (see snd.h).
 */
#include "snd.h"
#include "data.h"
#include "tt.h"

int snd_enabled = 1;
static int cur_prio;
static uint32_t last_start;

void snd_play(int id, int prio) {
    if (!snd_enabled || id < 0 || id >= sounds.n || !sounds.data)
        return;
    if (tt_sound_busy()) {
        /* let a sound play a moment before one of its own rank cuts it off */
        if (prio < cur_prio || (prio == cur_prio && HZ200 - last_start < 16))
            return;
    }
    cur_prio = prio;
    last_start = HZ200;
    tt_sound_play(sounds.data + sounds.ofs[id], (int32_t)sounds.len[id], sounds.rate[id]);
}

void snd_raw(const int8_t* data, int32_t len, int rate) {
    cur_prio = 9;
    tt_sound_play(data, len, rate);
}

void snd_stop(void) {
    tt_sound_stop();
    cur_prio = 0;
}
