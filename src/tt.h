/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Atari TT's hardware as the demake uses it: TT medium resolution
 * (640x480, 16 colours, 4 interleaved planes), the 200 Hz system clock,
 * the IKBD mouse and keyboard, and DMA sound. The whole program runs in
 * supervisor mode, entered by tt_init(). A Falcon with a VGA monitor shows
 * the same screen in its own 640x480 mode, as an experiment: its mode,
 * colours, caches and sound routing are tt.c's alone, and the rest of the
 * game sees only that it plays no 6258 Hz sound (tt_plays_6258).
 */
#ifndef TT_H
#define TT_H

#include <stdint.h>

#define SCREEN_W 640
#define SCREEN_H 480
#define LINE_BYTES 320
#define SCREEN_BYTES (LINE_BYTES * SCREEN_H)

/* The 200 Hz clock TOS keeps at 0x4BA */
#define HZ200 (*(volatile uint32_t*)0x4BAUL)

/* Mouse state the IKBD handler keeps: the pointer is clamped to the screen */
extern volatile int16_t mouse_x, mouse_y;
extern volatile uint8_t mouse_buttons; /* bit 0 left, bit 1 right */

extern int tt_plays_6258; ///< 1 when DMA sound plays 6258 Hz: a TT's does, a Falcon's not

/// Takes the machine over: checks it is a TT (or a Falcon with a VGA monitor
/// and the memory), makes the three screens, enters supervisor mode,
/// switches the caches on, sets 640x480 in 16 colours and takes the mouse;
/// tells the player why and waits for a key when the machine cannot run it.
///
/// @return 1 when the machine is ready, 0 when the program should end
int tt_init(void);

/// Gives the machine back as tt_init() found it: resolution, palette,
/// screen, caches, mouse and supervisor mode.
void tt_exit(void);

/// Takes the next key press from the keyboard, without waiting.
///
/// @return (scan code << 8) | ASCII, or 0 when no key was pressed
uint16_t tt_key(void);

/// Reads the keys held down that change others.
///
/// @return bit 0 either Shift, bit 1 Control, bit 2 Alternate
int tt_shift(void);

/* Three screens in ST-RAM: draw into tt_back, then tt_flip() shows it from
 * the next vertical blank and hands back a screen that is neither shown nor
 * waiting to be, so drawing never waits for the blank */
#define NSCREENS 3
extern uint8_t *tt_back, *tt_front;
extern uint8_t* tt_screens[NSCREENS];

/// Shows tt_back from the next vertical blank, makes it tt_front, and sets
/// tt_back to a screen neither shown nor waiting to be.
void tt_flip(void);

/// Sets the 16 colours, from the next tt_flip(): they change with the
/// screen drawn for them, a black frame between the two.
///
/// @param pal 16 colours, 4 bits a gun: 0x0RGB
void tt_set_palette(const uint16_t* pal);

/// Reads the 16 colours shown.
///
/// @param[out] pal 16 colours, 4 bits a gun: 0x0RGB
void tt_get_palette(uint16_t* pal);

/// Waits for the next vertical blank.
void tt_wait_vbl(void);

/// Allocates ST-RAM, the only memory the video and DMA sound can read.
///
/// @param bytes the size wanted
/// @return the block, or null when there is not enough
void* tt_alloc_st(int32_t bytes);

/// Allocates memory, TT-RAM when there is enough of it, else ST-RAM.
///
/// @param bytes the size wanted
/// @return the block, or null when there is not enough
void* tt_alloc_any(int32_t bytes);

/// Frees a block from tt_alloc_st() or tt_alloc_any().
///
/// @param p the block; null does nothing
void tt_free(void* p);

/// Plays signed 8-bit mono samples through DMA sound over and over, until
/// stopped, stopping whatever played.
///
/// @param start the first sample, in ST-RAM
/// @param len the number of samples, even
/// @param rate 0 for 6258 Hz, only where tt_plays_6258; 1 for 12517 Hz
void tt_sound_loop(const int8_t* start, int32_t len, int rate);

/// Tells where DMA sound plays.
///
/// @return the address it reads samples from
const int8_t* tt_sound_pos(void);

/// Stops DMA sound.
void tt_sound_stop(void);

/// Shows the films' mode: TT low resolution (320x480 in 256 colours, 8
/// interleaved planes), or the same on a Falcon's VGA monitor, on the
/// front screen, cleared to black.
///
/// @return 1 when set, 0 when the machine has no such mode (a Falcon's RGB
/// monitor or TV)
int tt_film_mode(void);

/// Sets the films' 256 colours.
///
/// @param pal 256 colours, 4 bits a gun: 0x0RGB
void tt_film_palette(const uint16_t* pal);

/// Goes back from the films' mode to 640x480 in 16 colours, black.
void tt_film_end(void);

/// Tells the player on the console why the game stops, before or after its
/// screens, and waits for a key.
///
/// @param why words to follow "Sorry, AnnihilaTTion ", such as "needs a 68030"
void tt_sorry(const char* why);

#endif
