/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The battle: units, their orders and movement, weapons, construction, the
 * two players' metal and energy, the missions' scripts and objectives, and
 * the computer player. It runs at TICKS (15) ticks a second; positions are
 * pixels in 24.8 fixed point, headings 65536 to the turn with 0 facing down
 * the screen.
 */
#ifndef SIM_H
#define SIM_H

#include <stdint.h>
#include "data.h"

#define MAX_UNITS 400
#define MAX_ORDERS 8
#define MAX_PATH 24
#define MAX_PROJ 160
#define MAX_FX 96
#define MAX_BEAMS 48
#define MAX_FEATS 1200
#define FIX 8

enum { PLAYER = 0, ENEMY = 1 };

enum {
    O_NONE,
    O_MOVE,
    O_FIGHT,
    O_ATTACK,
    O_PATROL,
    O_GUARD,
    O_BUILD,
    O_RECLAIM,
    O_DGUN,
    O_WAIT,
    O_WAITATTACK,
    O_ATTACKTYPE
};

typedef struct {
    uint8_t kind, a; /* a: a unit type for builds */
    int16_t x, z;    /* a place, in pixels */
    int16_t target;  /* a unit or a feature, or -1 */
    int16_t n;       /* ticks to wait, or a count */
} Order;

/* Unit.flags */
#define UF_SELECTED 1
#define UF_UNBUILT 2
#define UF_SEEN 8
#define UF_WAS_SEEN 16

typedef struct {
    uint8_t alive, type, owner, flags;
    int32_t x, z;
    uint16_t heading;
    int16_t speed; /* 8.8 pixels a tick */
    int32_t hp;
    uint32_t progress; /* while unbuilt: work done, of buildtime * TICKS */
    int16_t target;    /* the unit its weapons are on, or -1 */
    uint8_t reload[3], burst[3];
    uint8_t norders, npath, pathi, stuck;
    Order order[MAX_ORDERS];
    uint16_t path[MAX_PATH];
    int16_t goalx, goalz; /* where the path goes */
    uint8_t has_goal, want_path, movestate, firestate;
    uint16_t paralyzed;
    int16_t nano; /* what it builds or reclaims: a unit, or ~feature */
    uint8_t fq_type[6], fq_count[6];
    int16_t script, nscript; /* the mission script still to run */
    int32_t attacked;        /* tick it was last hit */
    int16_t attacker;
    int16_t homex, homez; /* patrol origin, and where a maneuvering unit returns */
    int16_t next;         /* the next unit in its bucket */
    int32_t mex;          /* metal it extracts, 1/256 a tick */
    int16_t built_by;
    uint16_t reclaim;      /* ticks of reclaiming done */
    uint8_t repaths, pad2; /* searches since it last got anywhere */
} Unit;

typedef struct {
    int32_t metal, energy;    /* 1/256 */
    int32_t mstore, estore;   /* whole units */
    int32_t mincome, eincome; /* the last tick's, 1/256 */
    int32_t base_mstore, base_estore;
    int units, kills, losses;
} Player;

typedef struct {
    uint8_t alive, weapon, owner, ticks;
    int32_t x, z, vx, vz;
    int16_t y, vy; /* height for ballistic arcs, 8.8 */
    int16_t target;
} Projectile;

typedef struct {
    uint8_t alive, anim, frame, pad;
    int16_t x, y; /* screen-space world pixels (z - height / 2) */
} Effect;

typedef struct {
    uint8_t alive, colour, ticks, pad;
    int16_t x0, y0, x1, y1;
} Beam;

typedef struct {
    uint8_t alive, type;
    int16_t x, z; /* centre */
    int32_t metal, energy;
} Feature;

extern Unit units[MAX_UNITS];
extern Player players[2];
extern Projectile projs[MAX_PROJ];
extern Effect fx[MAX_FX];
extern Beam beams[MAX_BEAMS];
extern Feature feats[MAX_FEATS];
extern int nfeats;
extern int32_t feat_version; /* goes up whenever a feature appears or goes */
extern int32_t sim_tick_count;
extern int sim_result; /* 0 playing, 1 won, -1 lost */
extern int wind_now;   /* energy a second a wind generator gives, before its cap */
extern int unit_hi;    /* every living unit's index is below this */
/* Where features came or went since the screen last took them: battlefield
 * pixels, y as on screen (z - height / 2); x0 > x1 when none did */
extern int feat_dirty_x0, feat_dirty_y0, feat_dirty_x1, feat_dirty_y1;

/// Starts the loaded mission: its features and one of its unit layouts, the
/// players' metal and energy, the map's paths and line of sight.
///
/// @param schema the layout: 0 easy, 1 medium, 2 hard
void sim_start(int schema);

/// Runs one tick: orders and scripts, paths, weapons and shots, metal and
/// energy, the computer player, line of sight and the objectives.
void sim_tick(void);

/// Gives the ground's height at a battlefield point, the nearest map cell's.
///
/// @param x the point's x, pixels
/// @param z the point's z, pixels
/// @return the height, pixels
int ground_height(int x, int z);

/// Gives a battlefield point's y on screen.
///
/// @param x the point's x, pixels
/// @param z the point's z, pixels
/// @return z - height / 2, pixels
static inline int ground_screen_y(int x, int z) {
    return z - ground_height(x, z) / 2;
}

/// Gives where a unit stands on screen.
///
/// @param u the unit
/// @return its ground_screen_y(), pixels
int unit_screen_y(const Unit* u);

/// Gives the heading that faces along a direction.
///
/// @param dx the direction's x
/// @param dz the direction's z
/// @return 65536 to the turn, 0 facing down the screen
uint16_t heading_to(int dx, int dz);

/// Drops a unit's orders and the way it was going.
///
/// @param u the unit
void order_clear(Unit* u);

/// Gives a unit an order; a ninth queued order is dropped.
///
/// @param u the unit
/// @param o the order
/// @param queue nonzero to add it after the unit's orders, 0 to replace them
void order_add(Unit* u, const Order* o, int queue);

/// Gives a unit an order to a place: move, fight, patrol or D-gun.
///
/// @param u the unit
/// @param kind O_*
/// @param x the place's x, pixels
/// @param z the place's z, pixels
/// @param queue nonzero to add it after the unit's orders, 0 to replace them
void order_at(Unit* u, int kind, int x, int z, int queue);

/// Stops a unit: no orders, no target, standing still.
///
/// @param u the unit
void order_stop(Unit* u);

/// Adds to a factory's queue or takes from it, at the slot holding the type
/// or the first empty one; a slot holds at most 99.
///
/// @param u the factory
/// @param type the unit type
/// @param n how many to add; less than 0 takes that many away
/// @return 1, or 0 when every slot holds another type
int factory_queue(Unit* u, int type, int n);

/// Counts a type in a factory's queue.
///
/// @param u the factory
/// @param type the unit type
/// @return how many are queued
int factory_queued(const Unit* u, int type);

/// Tells whether a building fits: on the map, on ground the first movement
/// class may cross, off water, rocks and other buildings.
///
/// @param type the building's type
/// @param x its footprint's centre, pixels, as snap_build_x() gives
/// @param z its footprint's centre, pixels, as snap_build_z() gives
/// @return 1 when it fits
int can_build_at(int type, int x, int z);

/// Finds a unit type's weapon that fires only when ordered to (the D-gun).
///
/// @param t the unit type
/// @return its weapon slot, 0 to 2, or -1
int manual_weapon(const UnitType* t);

/// Puts a building's x where its footprint falls on whole map cells.
///
/// @param type the building's type
/// @param x a battlefield x, pixels
/// @return its footprint's centre, pixels
int snap_build_x(int type, int x);

/// Puts a building's z where its footprint falls on whole map cells.
///
/// @param type the building's type
/// @param z a battlefield z, pixels
/// @return its footprint's centre, pixels
int snap_build_z(int type, int z);

/// Finds the unit drawn under a point, nearest first; a mobile unit before
/// a building, of the units the screen shows.
///
/// @param x the point's battlefield x, pixels
/// @param sy the point's battlefield y on screen, pixels
/// @param owner_mask bit 0 the player's units, bit 1 the computer's
/// @param shown whether the screen shows a unit
/// @return the unit, or -1
int unit_at(int x, int sy, int owner_mask, int (*shown)(const Unit*));

/// Gives the half-widths of a disc of cells, row by row: the disc that sight
/// covers, in the sim's buckets and the explored map's cells alike.
///
/// @param r the disc's radius in cells, at most 31
/// @return for each row d from the centre, 0 to r, the cells either side
const uint8_t* disc_widths(int r);

/// Finds the reclaimable feature drawn nearest a point, within 24 pixels.
///
/// @param x the point's battlefield x, pixels
/// @param sy the point's battlefield y on screen, pixels
/// @return the feature, or -1
int feature_at(int x, int sy);

/// Plays a sound when its place is on screen or near it.
///
/// @param sound the sound
/// @param x the place's x, pixels
/// @param z the place's z, pixels
void sim_sound_at(int sound, int x, int z);

/// Destroys a unit for a test, as if it were killed.
///
/// @param idx the unit
void sim_kill(int idx);

/* Set by the UI: the visible part of the battlefield, for sounds */
extern int view_x0, view_y0, view_x1, view_y1;
/* Messages for the bottom bar */
extern char sim_message[64];
extern int32_t sim_message_tick;

#endif
