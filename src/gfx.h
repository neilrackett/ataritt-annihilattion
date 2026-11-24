/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Drawing on a TT medium screen: 4 interleaved planes, 16-pixel groups of
 * four words. Sprites are the converter's masked format (data.h); every
 * routine clips to the current clip rectangle, whose left and right edges
 * fall on 16-pixel group boundaries.
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

/* Palette entries the engine names (the converter fixes them) */
enum {
    C_BLACK = 0,
    C_BLUE = 1,
    C_RED = 2,
    C_YELLOW = 3,
    C_GREEN = 4,
    C_LGREY = 12,
    C_GREY = 13,
    C_SILVER = 14,
    C_WHITE = 15
};

extern int gfx_stride; ///< bytes a row of what is drawn on (default: the screen's)

/// Copies n groups (n > 0) from s to d: a group a pass, in two moves, which
/// GCC's own loops do not manage, and no call, which memcpy() costs.
///
/// @return where d got to
static inline uint32_t* copy_run(uint32_t* d, const uint32_t* s, int n) {
    __asm__ volatile("subq.w #1,%2\n"
                     "1:\tmove.l (%1)+,(%0)+\n\t"
                     "move.l (%1)+,(%0)+\n\t"
                     "dbra %2,1b"
                     : "+a"(d), "+a"(s), "+d"(n)
                     :
                     : "memory");
    return d;
}

/// Sets the rectangle drawing keeps to.
///
/// @param x0 the left edge, a multiple of 16
/// @param y0 the top edge
/// @param x1 the right edge, inclusive; x1 + 1 a multiple of 16
/// @param y1 the bottom edge, inclusive
void gfx_clip(int x0, int y0, int x1, int y1);

/// Sets the clip rectangle to the whole screen.
void gfx_clip_full(void);

/// Draws a masked sprite.
///
/// @param scr what is drawn on, gfx_stride bytes a row
/// @param spr the sprite, in the converter's format (data.h)
/// @param x where the sprite's origin goes, pixels
/// @param y where the sprite's origin goes, pixels
void gfx_sprite(uint8_t* scr, const int16_t* spr, int x, int y);

/// Draws a unit being built: the part not yet built as a green nanolathe
/// frame through a checker, the rest as the sprite.
///
/// @param scr what is drawn on
/// @param spr the sprite
/// @param x where its origin goes, pixels
/// @param y where its origin goes, pixels
/// @param built256 how much is built, 0 to 256, from the bottom up
void gfx_sprite_nano(uint8_t* scr, const int16_t* spr, int x, int y, int built256);

/// Draws a sprite's shape in one colour through a checker, for placing buildings
/// and the flash of a paralyzed unit.
///
/// @param scr what is drawn on
/// @param spr the sprite
/// @param x where its origin goes, pixels
/// @param y where its origin goes, pixels
/// @param colour a palette entry
void gfx_sprite_tint(uint8_t* scr, const int16_t* spr, int x, int y, int colour);

/// Copies an unmasked picture onto the screen, rows off the screen left out.
///
/// @param scr the screen
/// @param src the picture, interleaved planes
/// @param x its left edge, a multiple of 16
/// @param y its top edge
/// @param groups its width in 16-pixel groups
/// @param rows its height
void gfx_image(uint8_t* scr, const uint16_t* src, int x, int y, int groups, int rows);

/// Fills a rectangle, clipped.
///
/// @param scr what is drawn on
/// @param x0 the left edge
/// @param y0 the top edge
/// @param x1 the right edge, inclusive
/// @param y1 the bottom edge, inclusive
/// @param colour a palette entry
void gfx_fill(uint8_t* scr, int x0, int y0, int x1, int y1, int colour);

/// Sets one pixel, when it is inside the clip rectangle.
///
/// @param scr what is drawn on
/// @param x the pixel's x
/// @param y the pixel's y
/// @param colour a palette entry
void gfx_pixel(uint8_t* scr, int x, int y, int colour);

/// Draws a line between two points, both ends included, clipped; a line
/// longer than 1200 pixels across or down is not drawn.
///
/// @param scr what is drawn on
/// @param x0 one end's x
/// @param y0 one end's y
/// @param x1 the other end's x
/// @param y1 the other end's y
/// @param colour a palette entry
void gfx_line(uint8_t* scr, int x0, int y0, int x1, int y1, int colour);

/// Draws a rectangle's edges, one pixel wide, clipped.
///
/// @param scr what is drawn on
/// @param x0 the left edge
/// @param y0 the top edge
/// @param x1 the right edge, inclusive
/// @param y1 the bottom edge, inclusive
/// @param colour a palette entry
void gfx_rect(uint8_t* scr, int x0, int y0, int x1, int y1, int colour);

/// Copies a rectangle between two screen-sized buffers, widened to whole
/// 16-pixel groups and kept to the screen.
///
/// @param dst the buffer copied to
/// @param src the buffer copied from
/// @param x0 the left edge
/// @param y0 the top edge
/// @param x1 the right edge, inclusive
/// @param y1 the bottom edge, inclusive
void gfx_copy_rect(uint8_t* dst, const uint8_t* src, int x0, int y0, int x1, int y1);

/// Sets the font text is drawn in.
///
/// @param font the converter's font: line height, glyph offsets, glyphs
void gfx_set_font(const uint8_t* font);

/// Draws a line of text with the font's dark outline, clipped.
///
/// @param scr what is drawn on
/// @param x the left edge of the first letter
/// @param y the text's top
/// @param s the text; characters below 32 are left out
/// @param colour the letters' palette entry
/// @return the text's width, pixels
int gfx_text(uint8_t* scr, int x, int y, const char* s, int colour);

/// Measures a line of text.
///
/// @param s the text
/// @return its width, pixels
int gfx_text_width(const char* s);

/// Gives the height of a line of text.
///
/// @return pixels from one line's top to the next's
int gfx_line_height(void);

/// Draws text wrapped at word breaks into a box; & and a letter (Y, R, G, B,
/// W) change the colour as TA's briefings do, until the next line.
///
/// @param scr what is drawn on
/// @param x the box's left edge
/// @param y the box's top
/// @param w the box's width, pixels
/// @param s the text, lines broken by \n
/// @param colour the text's palette entry where no code sets another
/// @return the height the text took, pixels
int gfx_text_box(uint8_t* scr, int x, int y, int w, const char* s, int colour);

/// Draws terrain from 32x32 tiles of 512 bytes, unclipped.
///
/// @param scr what is drawn on
/// @param tiles the tiles
/// @param tilemap each map tile's tile, tw a row
/// @param tw the tile map's width, tiles
/// @param camx the battlefield x at the area's left edge, a multiple of 16
/// @param camy the battlefield y at the area's top
/// @param vx the area's left edge on scr, a multiple of 16
/// @param vy the area's top on scr
/// @param vw the area's width, a multiple of 16
/// @param vh the area's height
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
);

#endif
