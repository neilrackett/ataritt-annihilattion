/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The films (see film.h). A frame of FILMn.DAT is its length (4 bytes) and
 * flags (2), then the palette when bit 0 is set (256 words), then a bit for
 * each 16-pixel group of the 320x240 picture, row by row, set for the
 * groups that change, then those groups, 16 bytes each (TT low's 8 plane
 * words). The file is read ahead into a ring, 1 MB before the first frame
 * and then a chunk at a time while the film waits for a frame's time, so
 * that its quieter stretches pay for the busier ones; the frames are shown
 * against the 200 Hz clock while the sound plays through the mixer's
 * stream.
 */
#include <string.h>
#include <stdio.h>
#include <mint/osbind.h>
#include "film.h"
#include "data.h"
#include "snd.h"
#include "tt.h"

#define FILM_LINES 240
#define FILM_GROUPS 20
#define FRAME_MAX (512 + FILM_LINES * FILM_GROUPS / 8 + FILM_LINES * FILM_GROUPS * 16 + 16)
#define CHUNK 8192L                  /* read at a time */
#define RING_MAX (3072 * 1024L)      /* the most read ahead */
#define RING_MIN (FRAME_MAX + 3 * CHUNK)
#define RING_SPARE (128 * 1024L)     /* memory left beside the ring */
#define FILM_LEAD (1024 * 1024L)     /* read before the first frame, as tools/convert.py counts on */

/* Puts a frame's changed groups on the screen, each film line on an even
 * line: the bits run along the rows, a group's 16 bytes after another */
static void apply(uint8_t* scr, const uint8_t* bits, const uint8_t* data) {
    uint32_t* row = (uint32_t*)scr;
    const uint32_t* s = (const uint32_t*)data;
    int i, col = 0;
    for (i = 0; i < FILM_LINES * FILM_GROUPS / 8; i++) {
        uint8_t b = bits[i];
        int g;
        for (g = 0; g < 8; g++, b <<= 1) {
            if (b & 0x80) {
                uint32_t* d = row + col * 4;
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = s[3];
                s += 4;
            }
            if (++col == FILM_GROUPS) {
                col = 0;
                row += 2 * LINE_BYTES / 4;
            }
        }
    }
}

FilmStats film_stats;

static long ffh;           /* the film's file */
static uint8_t* ring;      /* what is read of it ahead of the frames */
static uint32_t ring_size; /* a multiple of CHUNK, so that no read wraps */
static uint32_t r_in;      /* bytes read into the ring, from the first frame on */
static uint32_t r_out;     /* bytes of them shown */
static long left;          /* bytes not yet read */
static uint8_t* whole;     /* a frame that wraps round the ring's end, in one piece */

/* Reads a chunk into the ring when it has room for one; 0 when it has none
 * or the file is read */
static int read_ahead(void) {
    long n, got;
    uint32_t t = HZ200;
    if (left <= 0 || ring_size - (r_in - r_out) < (uint32_t)CHUNK)
        return 0;
    n = left < CHUNK ? left : CHUNK;
    got = data_read(ffh, n, ring + r_in % ring_size);
    film_stats.read_ms += (long)(HZ200 - t) * 5;
    if (got <= 0) {
        left = 0;
        return 0;
    }
    r_in += (uint32_t)got;
    left -= got;
    return 1;
}

/* The next n bytes in one piece, read first if they are not yet in the
 * ring; 0 when the file ends before them. They stay until drop() */
static const uint8_t* peek(uint32_t n) {
    uint32_t at = r_out % ring_size, first = ring_size - at;
    while (r_in - r_out < n) {
        if (!read_ahead())
            return 0;
        snd_update();
    }
    if (n <= first)
        return ring + at;
    memcpy(whole, ring + at, first);
    memcpy(whole + first, ring, n - first);
    return whole;
}

static void drop(uint32_t n) {
    r_out += n;
}

int film_play(int n, int (*skip)(int shown)) {
    char name[16];
    uint8_t head[10];
    long room, tt, st;
    int frames, k = 0;
    uint32_t t0;
    sprintf(name, "FILM%d.DAT", n);
    ffh = data_open(name);
    if (ffh < 0)
        return -1;
    left = Fseek(0, (short)ffh, 2) - 10;
    (void)Fseek(0, (short)ffh, 0);
    /* the ring in the larger memory, TT-RAM on a TT, with room for the
     * frame in one piece beside it */
    tt = (long)Mxalloc(-1, 1);
    st = (long)Mxalloc(-1, 0);
    room = (tt > st ? tt : st) - RING_SPARE - FRAME_MAX;
    if (room > RING_MAX)
        room = RING_MAX;
    ring_size = (uint32_t)(room / CHUNK * CHUNK);
    ring = room >= RING_MIN ? tt_alloc_any((int32_t)ring_size) : 0;
    whole = tt_alloc_any(FRAME_MAX);
    if (!ring || !whole || Fread((short)ffh, 10, head) != 10 || memcmp(head, "OAF3", 4) ||
        !tt_film_mode()) {
        tt_free(whole);
        tt_free(ring);
        (void)Fclose((short)ffh);
        return -1;
    }
    frames = rd16(head + 4);
    r_in = r_out = 0;
    memset(&film_stats, 0, sizeof film_stats);
    film_stats.ring_kb = (long)(ring_size / 1024);
    /* a start, in silence: the converter leaves out the frames that would
     * need more than this and a TT's reading as the film plays */
    snd_silence();
    while (r_in < (uint32_t)FILM_LEAD && read_ahead())
        if (skip(0))
            goto done;
    sprintf(name, "FILM%d.SND", n);
    snd_stream_file(name);
    t0 = HZ200;
    for (k = 0; k < frames; k++) {
        const uint8_t* p;
        uint32_t size, t;
        int32_t late;
        int flags;
        /* the frame, read ahead or read now */
        if (!(p = peek(6)))
            break;
        size = rd32(p);
        flags = p[5];
        if (size > FRAME_MAX - 6 || !(p = peek(6 + size)))
            break;
        /* its time: 15 a second, reading ahead meanwhile */
        while ((HZ200 - t0) * 15 < (uint32_t)k * 200) {
            snd_update();
            if (skip(k))
                goto done;
            read_ahead();
        }
        t = HZ200;
        late = (int32_t)((t - t0) * 15 - (uint32_t)k * 200);
        if (late >= 200)
            film_stats.late++;
        if (late / 15 * 5 > film_stats.late_ms)
            film_stats.late_ms = late / 15 * 5;
        p += 6;
        if (flags & 1) {
            tt_film_palette((const uint16_t*)p);
            p += 512;
        }
        apply(tt_front, p, p + FILM_LINES * FILM_GROUPS / 8);
        film_stats.draw_ms += (long)(HZ200 - t) * 5;
        drop(6 + size);
        snd_update();
        if (skip(k + 1)) {
            k++;
            break;
        }
    }
done:
    snd_silence();
    tt_film_end();
    tt_free(whole);
    tt_free(ring);
    (void)Fclose((short)ffh);
    return k;
}
