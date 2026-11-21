/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Drawing on a TT medium screen (see gfx.h).
 */
#include <string.h>
#include "gfx.h"
#include "tt.h"

int gfx_stride = LINE_BYTES;   /* bytes a row of the target */
static int cx0, cy0, cx1, cy1; /* clip rectangle, inclusive */
static int cg0, cg1;           /* clip groups, inclusive */

void gfx_clip(int x0, int y0, int x1, int y1) {
    cx0 = x0;
    cy0 = y0;
    cx1 = x1;
    cy1 = y1;
    cg0 = x0 >> 4;
    cg1 = x1 >> 4;
}

void gfx_clip_full(void) {
    gfx_clip(0, 0, SCREEN_W - 1, SCREEN_H - 1);
}

/* ------------------------------------------------------------ sprites */

/* Writes one group: m is the mask, p the planes (zero where m is clear) */
#define PUT(d, m, a, b, c, e)                                                                      \
    do {                                                                                           \
        uint16_t nm_ = (uint16_t)~(m);                                                             \
        (d)[0] = (uint16_t)(((d)[0] & nm_) | (a));                                                 \
        (d)[1] = (uint16_t)(((d)[1] & nm_) | (b));                                                 \
        (d)[2] = (uint16_t)(((d)[2] & nm_) | (c));                                                 \
        (d)[3] = (uint16_t)(((d)[3] & nm_) | (e));                                                 \
    } while (0)

/* Writes the pixels of mask m in one colour */
#define PUT_COLOUR(d, m, colour)                                                                   \
    PUT(d,                                                                                         \
        m,                                                                                         \
        ((colour) & 1) ? (m) : 0,                                                                  \
        ((colour) & 2) ? (m) : 0,                                                                  \
        ((colour) & 4) ? (m) : 0,                                                                  \
        ((colour) & 8) ? (m) : 0)

/* Sprite rows hold five runs (mask, planes 0-3) of G + 2 words with a zero at
 * each end; a long read at word g holds source groups g-1 and g, and shifted
 * right by x & 15 its low word is screen group gx + g. split: rows above it
 * are drawn as a tint of colour through a checker, for nanolathe frames. */
static void blit(uint8_t* scr, const int16_t* spr, int x, int y, int split, int colour) {
    int G = spr[0], H = spr[1], r0 = 0, r1, s, gx, ga, gb, r, run;
    const uint16_t* src = (const uint16_t*)(spr + 4);
    if (G <= 0)
        return;
    x -= spr[2];
    y -= spr[3];
    r1 = H;
    if (y + r1 - 1 > cy1)
        r1 = cy1 - y + 1;
    if (y < cy0)
        r0 = cy0 - y;
    if (r0 >= r1 || x + G * 16 <= cx0 || x > cx1)
        return;
    s = x & 15;
    gx = x >> 4;
    ga = gx < cg0 ? cg0 - gx : 0;
    gb = s ? G : G - 1;
    if (gx + gb > cg1)
        gb = cg1 - gx;
    run = G + 2;
    src += r0 * run * 5;
    for (r = r0; r < r1; r++, src += run * 5) {
        uint16_t* d = (uint16_t*)(scr + (y + r) * gfx_stride) + (gx + ga) * 4;
        const uint16_t* m = src + ga;
        int g;
        if (r >= split) {
            for (g = ga; g <= gb; g++, m++, d += 4) {
                uint16_t mm = (uint16_t)(*(const uint32_t*)m >> s), nm;
                if (!mm)
                    continue;
                nm = (uint16_t)~mm;
                d[0] = (uint16_t)((d[0] & nm) | (uint16_t)(*(const uint32_t*)(m + run) >> s));
                d[1] = (uint16_t)((d[1] & nm) | (uint16_t)(*(const uint32_t*)(m + run * 2) >> s));
                d[2] = (uint16_t)((d[2] & nm) | (uint16_t)(*(const uint32_t*)(m + run * 3) >> s));
                d[3] = (uint16_t)((d[3] & nm) | (uint16_t)(*(const uint32_t*)(m + run * 4) >> s));
            }
        } else {
            uint16_t pat = ((y + r) & 1) ? 0xAAAA : 0x5555;
            for (g = ga; g <= gb; g++, m++, d += 4) {
                uint16_t mm = (uint16_t)((*(const uint32_t*)m >> s) & pat);
                if (!mm)
                    continue;
                PUT_COLOUR(d, mm, colour);
            }
        }
    }
}

void gfx_sprite(uint8_t* scr, const int16_t* spr, int x, int y) {
    blit(scr, spr, x, y, 0, 0);
}

void gfx_sprite_nano(uint8_t* scr, const int16_t* spr, int x, int y, int built256) {
    int H = spr[1];
    int split = H - (H * built256 >> 8);
    blit(scr, spr, x, y, split, C_GREEN);
}

void gfx_sprite_tint(uint8_t* scr, const int16_t* spr, int x, int y, int colour) {
    blit(scr, spr, x, y, 0x7FFF, colour);
}

void gfx_image(uint8_t* scr, const uint16_t* src, int x, int y, int groups, int rows) {
    int r;
    for (r = 0; r < rows; r++) {
        if (y + r >= 0 && y + r < SCREEN_H)
            memcpy(scr + (y + r) * LINE_BYTES + (x >> 4) * 8, src, groups * 8);
        src += groups * 4;
    }
}

/* ------------------------------------------------------------ fills and lines */

void gfx_fill(uint8_t* scr, int x0, int y0, int x1, int y1, int colour) {
    int y, g;
    if (x0 < cx0)
        x0 = cx0;
    if (y0 < cy0)
        y0 = cy0;
    if (x1 > cx1)
        x1 = cx1;
    if (y1 > cy1)
        y1 = cy1;
    if (x0 > x1 || y0 > y1)
        return;
    for (y = y0; y <= y1; y++) {
        uint16_t* row = (uint16_t*)(scr + y * gfx_stride);
        for (g = x0 >> 4; g <= x1 >> 4; g++) {
            uint16_t m = 0xFFFF, *d = row + g * 4;
            if (g == x0 >> 4)
                m &= (uint16_t)(0xFFFF >> (x0 & 15));
            if (g == x1 >> 4)
                m &= (uint16_t)(0xFFFF << (15 - (x1 & 15)));
            PUT_COLOUR(d, m, colour);
        }
    }
}

void gfx_pixel(uint8_t* scr, int x, int y, int colour) {
    uint16_t *d, m;
    if (x < cx0 || x > cx1 || y < cy0 || y > cy1)
        return;
    d = (uint16_t*)(scr + y * gfx_stride) + (x >> 4) * 4;
    m = (uint16_t)(0x8000 >> (x & 15));
    PUT_COLOUR(d, m, colour);
}

/* Bresenham's line, stepping a row pointer rather than working out each pixel's address */
void gfx_line(uint8_t* scr, int x0, int y0, int x1, int y1, int colour) {
    int dx = x1 - x0, dy = y1 - y0, sx = 1, sy = 1, err, n, step = gfx_stride;
    uint8_t* row;
    if (dx < 0) {
        dx = -dx;
        sx = -1;
    }
    if (dy < 0) {
        dy = -dy;
        sy = -1;
        step = -step;
    }
    if (dx > 1200 || dy > 1200)
        return;
    err = dx - dy;
    row = scr + y0 * gfx_stride;
    for (n = dx + dy + 1; n > 0; n--) {
        int e2;
        if (x0 >= cx0 && x0 <= cx1 && y0 >= cy0 && y0 <= cy1) {
            uint16_t* d = (uint16_t*)row + (x0 >> 4) * 4;
            uint16_t m = (uint16_t)(0x8000 >> (x0 & 15));
            PUT_COLOUR(d, m, colour);
        }
        if (x0 == x1 && y0 == y1)
            break;
        e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
            row += step;
        }
    }
}

void gfx_rect(uint8_t* scr, int x0, int y0, int x1, int y1, int colour) {
    gfx_fill(scr, x0, y0, x1, y0, colour);
    gfx_fill(scr, x0, y1, x1, y1, colour);
    gfx_fill(scr, x0, y0, x0, y1, colour);
    gfx_fill(scr, x1, y0, x1, y1, colour);
}

void gfx_copy_rect(uint8_t* dst, const uint8_t* src, int x0, int y0, int x1, int y1) {
    int y, o, n;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > SCREEN_W - 1)
        x1 = SCREEN_W - 1;
    if (y1 > SCREEN_H - 1)
        y1 = SCREEN_H - 1;
    if (x0 > x1 || y0 > y1)
        return;
    o = (x0 >> 4) * 8;
    n = ((x1 >> 4) - (x0 >> 4) + 1) * 8;
    for (y = y0; y <= y1; y++)
        memcpy(dst + y * LINE_BYTES + o, src + y * LINE_BYTES + o, n);
}

/* ------------------------------------------------------------ text */

static const uint8_t* font;

void gfx_set_font(const uint8_t* f) {
    font = f;
}

int gfx_line_height(void) {
    return font ? (font[0] << 8 | font[1]) : 12;
}

static const int8_t* glyph(unsigned char c) {
    const uint8_t* t = font + 4;
    int o = t[c * 2] << 8 | t[c * 2 + 1];
    return (const int8_t*)(font + 4 + 512 + o);
}

int gfx_text_width(const char* s) {
    int w = 0;
    for (; *s; s++)
        if ((unsigned char)*s >= 32)
            w += glyph((unsigned char)*s)[0];
    return w;
}

static void glyph_row(uint8_t* scr, uint16_t bits, int left, int yy, int colour) {
    int s = left & 15, gx = left >> 4, k;
    for (k = 0; k < 2; k++) {
        uint16_t m = k ? (uint16_t)(s ? bits << (16 - s) : 0) : (uint16_t)(bits >> s);
        int dg = gx + k;
        uint16_t* d;
        if (!m || dg < cg0 || dg > cg1)
            continue;
        if (dg == cg0 && (cx0 & 15))
            m &= (uint16_t)(0xFFFF >> (cx0 & 15));
        d = (uint16_t*)(scr + yy * gfx_stride) + dg * 4;
        PUT_COLOUR(d, m, colour);
    }
}

/* Glyph rows are two words: the letter, then its dark outline */
static void draw_glyph(uint8_t* scr, const int8_t* g, int x, int y, int colour) {
    int h = g[1], r;
    const uint8_t* rows = (const uint8_t*)(g + 4);
    int top = y - g[3], left = x - g[2];
    for (r = 0; r < h; r++, rows += 4) {
        int yy = top + r;
        if (yy < cy0 || yy > cy1)
            continue;
        glyph_row(scr, (uint16_t)(rows[2] << 8 | rows[3]), left, yy, 0);
        glyph_row(scr, (uint16_t)(rows[0] << 8 | rows[1]), left, yy, colour);
    }
}

int gfx_text(uint8_t* scr, int x, int y, const char* s, int colour) {
    int x0 = x;
    if (!font)
        return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        const int8_t* g;
        if (c < 32)
            continue;
        g = glyph(c);
        if (c != 32)
            draw_glyph(scr, g, x, y, colour);
        x += g[0];
    }
    return x - x0;
}

static int code_colour(char c, int dflt) {
    switch (c) {
    case 'Y':
    case 'y':
        return C_YELLOW;
    case 'R':
    case 'r':
        return C_RED;
    case 'G':
    case 'g':
        return C_GREEN;
    case 'B':
    case 'b':
        return C_BLUE;
    case 'W':
    case 'w':
        return C_WHITE;
    default:
        return dflt;
    }
}

int gfx_text_box(uint8_t* scr, int x, int y, int w, const char* s, int colour) {
    char word[64];
    int px = x, py = y, lh = gfx_line_height(), col = colour;
    while (*s) {
        int n = 0, ww;
        if (*s == '\r') {
            s++;
            continue;
        }
        if (*s == '\n') {
            px = x;
            py += lh;
            s++;
            continue;
        }
        if (*s == '&' && s[1]) {
            col = code_colour(s[1], colour);
            s += 2;
            continue;
        }
        if (*s == ' ') {
            px += glyph(' ')[0];
            s++;
            continue;
        }
        while (s[n] && s[n] != ' ' && s[n] != '\n' && s[n] != '\r' && s[n] != '&' && n < 63) {
            word[n] = s[n];
            n++;
        }
        word[n] = 0;
        ww = gfx_text_width(word);
        if (px + ww > x + w && px > x) {
            px = x;
            py += lh;
        }
        gfx_text(scr, px, py, word, col);
        px += ww;
        s += n;
        if (*s == '\n' || *s == '\r')
            col = colour;
    }
    return py + lh - y;
}

/* ------------------------------------------------------------ terrain */

void gfx_terrain(
    uint8_t* scr,
    const uint8_t* tiles,
    const uint16_t* tilemap,
    int tw,
    int camx,
    int camy,
    int vx,
    int vy,
    int vw,
    int vh
) {
    int r, groups = vw >> 4;
    int gx0 = camx >> 4;
    for (r = 0; r < vh; r++) {
        int wy = camy + r;
        const uint16_t* tm = tilemap + (wy >> 5) * tw;
        int trow = (wy & 31) * 16;
        uint32_t* d = (uint32_t*)(scr + (vy + r) * gfx_stride + (vx >> 4) * 8);
        int g = 0, wg = gx0;
        if (wg & 1) {
            const uint32_t* s = (const uint32_t*)(tiles + (uint32_t)tm[wg >> 1] * 512 + trow + 8);
            d[0] = s[0];
            d[1] = s[1];
            d += 2;
            g++;
            wg++;
        }
        for (; g + 1 < groups; g += 2, wg += 2) {
            const uint32_t* s = (const uint32_t*)(tiles + (uint32_t)tm[wg >> 1] * 512 + trow);
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
            d += 4;
        }
        if (g < groups) {
            const uint32_t* s = (const uint32_t*)(tiles + (uint32_t)tm[wg >> 1] * 512 + trow);
            d[0] = s[0];
            d[1] = s[1];
        }
    }
}
