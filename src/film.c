/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The films (see film.h). A frame of FILMn.DAT is its length (4 bytes) and
 * flags (2), then the palette when bit 0 is set (256 words), then a bit for
 * each 16-pixel group of the 320x240 picture, row by row, set for the
 * groups that change, then those groups, 16 bytes each (TT low's 8 plane
 * words). The frames are read as their time comes, against the 200 Hz
 * clock, while the sound plays through the mixer's stream.
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

int film_play(int n, int (*skip)(int shown)) {
    char name[16];
    uint8_t head[10], fh6[6];
    uint8_t* buf;
    long fh;
    int frames, k = 0;
    uint32_t t0;
    sprintf(name, "FILM%d.DAT", n);
    fh = Fopen(name, 0);
    if (fh < 0)
        return -1;
    buf = tt_alloc_any(FRAME_MAX);
    if (!buf || Fread((short)fh, 10, head) != 10 || memcmp(head, "OAF3", 4) || !tt_film_mode()) {
        tt_free(buf);
        (void)Fclose((short)fh);
        return -1;
    }
    frames = rd16(head + 4);
    sprintf(name, "FILM%d.SND", n);
    snd_stream_file(name);
    t0 = HZ200;
    for (k = 0; k < frames; k++) {
        uint32_t size;
        const uint8_t* p = buf;
        /* this frame's time: 15 a second */
        while ((HZ200 - t0) * 15 < (uint32_t)k * 200) {
            snd_update();
            if (skip(k))
                goto done;
        }
        if (Fread((short)fh, 6, fh6) != 6)
            break;
        size = rd32(fh6);
        if (size > FRAME_MAX || Fread((short)fh, (long)size, buf) != (long)size)
            break;
        if (fh6[5] & 1) {
            tt_film_palette((const uint16_t*)p);
            p += 512;
        }
        apply(tt_front, p, p + FILM_LINES * FILM_GROUPS / 8);
        snd_update();
        if (skip(k + 1)) {
            k++;
            break;
        }
    }
done:
    snd_silence();
    tt_film_end();
    tt_free(buf);
    (void)Fclose((short)fh);
    return k;
}
