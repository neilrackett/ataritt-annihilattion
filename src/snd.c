/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Sound effects (see snd.h).
 */
#include "snd.h"
#include <string.h>
#include "data.h"
#include "tt.h"

/* A stream plays at 12517 Hz through a ring in ST-RAM of two halves: while
 * DMA sound plays one, the other is filled with what comes next. Sample
 * stream_next goes into half (stream_next / HALF) & 1. */
#define HALF 32768L

int snd_enabled = 1;
static int cur_prio;
static uint32_t last_start;
static int8_t* ring;
static const int8_t* stream;
static int32_t stream_len, stream_next;

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

/* Fills the next half of the ring with what comes next, or silence */
static void fill_half(void) {
    int8_t* d = ring + ((stream_next / HALF) & 1) * HALF;
    int32_t n = stream_len - stream_next;
    if (n < 0)
        n = 0;
    if (n > HALF)
        n = HALF;
    memcpy(d, stream + stream_next, (size_t)n);
    memset(d + n, 0, (size_t)(HALF - n));
    stream_next += HALF;
}

void snd_stream(const int8_t* data, int32_t len) {
    snd_stop();
    if (!snd_enabled || !(ring = tt_alloc_st(2 * HALF)))
        return;
    stream = data;
    stream_len = len;
    stream_next = 0;
    fill_half();
    fill_half();
    cur_prio = 9;
    tt_sound_loop(ring, 2 * HALF, 1);
}

void snd_stream_update(void) {
    int playing;
    if (!ring)
        return;
    playing = tt_sound_pos() >= ring + HALF;
    if (playing == ((stream_next / HALF) & 1))
        return;
    /* the half playing holds what comes before stream_next: silence once
     * that is past the end */
    if (stream_next - HALF >= stream_len) {
        snd_stop();
        return;
    }
    fill_half();
}

void snd_stop(void) {
    tt_sound_stop();
    cur_prio = 0;
    if (ring) {
        tt_free(ring);
        ring = 0;
    }
}
