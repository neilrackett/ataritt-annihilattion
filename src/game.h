/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A mission on screen: the battlefield, TA's panels around it, and the
 * player's mouse and keys.
 */
#ifndef GAME_H
#define GAME_H

#include <stdint.h>

#define VIEW_X 128
#define VIEW_Y 32
#define VIEW_W 512
#define VIEW_H 416

/// Plays the loaded mission until it is won, lost and clicked away, or quit.
///
/// @param difficulty 0 easy, 1 medium, 2 hard: the mission's unit layout
/// @return 1 won, -1 lost, 0 quit to the menu
int game_run(int difficulty);

/// Scripted input for automated checks (main.c reads OATEST.TXT).
typedef struct {
    /// When set, called once a frame before the input is read: 0 to go on,
    /// -1 to end the mission.
    int (*next)(void);
} TestHooks;

extern TestHooks test_hooks;

/// Puts the pointer somewhere for a test, in place of the mouse from then on.
///
/// @param x the pointer's screen x, pixels
/// @param y the pointer's screen y, pixels
/// @param buttons bit 0 left, bit 1 right
void test_mouse(int x, int y, int buttons);

/// Presses a key for a test: input_key() returns it next.
///
/// @param key (scan code << 8) | ASCII
void test_key(uint16_t key);

/// Centres the view on a battlefield point, as far as the map's edges allow.
///
/// @param x the point's x, pixels
/// @param z the point's z, pixels
void game_camera(int x, int z);

/// Makes the view scroll after the Commander, at the speed of scrolling at
/// the screen's edges, for test scripts' recordings.
///
/// @param on whether to follow
void game_follow(int on);

/// Writes to the test log up to 8 features in view, with their places on
/// screen and what reclaiming them gives.
void game_list_features(void);

/// Writes to the test log where the composite differs from the background
/// outside this frame's marked cells, and where the front screen differs
/// from the composite.
void game_check_layers(void);

/// Reads the pointer's x, from the mouse or a test script.
///
/// @return screen x, pixels
int input_x(void);

/// Reads the pointer's y, from the mouse or a test script.
///
/// @return screen y, pixels
int input_y(void);

/// Reads the mouse buttons, from the mouse or a test script.
///
/// @return bit 0 left, bit 1 right
int input_buttons(void);

/// Takes the next key press, a test script's first.
///
/// @return (scan code << 8) | ASCII, or 0 when none was pressed
uint16_t input_key(void);

/// Writes the front screen and its palette to NAME.RAW for a test.
///
/// @param name the file's name without its extension
void test_screenshot(const char* name);

/// Writes a line to the test log, OALOG.TXT, when a test runs.
///
/// @param fmt a printf format, without the line end
void test_log(const char* fmt, ...);

extern int32_t game_frames;
extern uint32_t game_draw_hz, game_sim_hz; /* 200 Hz ticks spent drawing and simulating */
/* 200 Hz ticks spent in each stage of drawing: background, battlefield,
 * minimap, side panel, bars, composite, units */
extern uint32_t stage_hz[7];
extern int32_t game_ticks_run;
extern int game_fast; /* tests: no frame pacing */

#endif
