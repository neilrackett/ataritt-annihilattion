/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The units' sprites, wrecks and build pictures, read from the world's
 * WORLDn.SPR as they are needed. The full game has too many unit types to
 * hold all of them, so they share one block of memory: when it is full,
 * the types seen longest ago that no unit or wreck on the battlefield needs
 * make room.
 */
#ifndef SPRITES_H
#define SPRITES_H

#include <stdint.h>
#include "data.h"

/// Opens a world's sprites and takes a block of memory for them, closing
/// what was open before.
///
/// @param n the world, 1 on
/// @param bytes the memory they may take
/// @return 1 when open, 0 when its file cannot be read or the memory taken
int spr_open(int n, int32_t bytes);

/// Closes the world's sprites and gives their memory back.
void spr_close(void);

/// Reads a unit type's sprites, when they are not held already.
///
/// @param type the unit type
/// @return 1 when they are held, 0 when there is no room
int spr_load(int type);

/// Starts a frame: what is drawn from here on counts as seen now.
void spr_frame(void);

/// Finds a unit type's sprite for a direction, reading it when needed.
///
/// @param type the unit type
/// @param dir 0 to ndirs - 1, 0 facing down the screen
/// @return the sprite, or an empty one when it cannot be read
const int16_t* unit_frame(int type, int dir);

/// Finds a unit type's wreck sprite, reading it when needed.
///
/// @param type the unit type
/// @return the sprite, or an empty one
const int16_t* unit_wreck(int type);

/// Finds a unit type's build picture, reading it when needed.
///
/// @param type the unit type
/// @return 64x64 planar, or null when it cannot be read
const uint16_t* unit_pic(int type);

/// Tells how much of the block is in use, for tests.
///
/// @param[out] used bytes held
/// @param[out] size the block's size
/// @return how many unit types are held
int spr_stats(int32_t* used, int32_t* size);

#endif
