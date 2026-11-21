/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The converted game data (tools/convert.py writes it; the layouts here
 * must match). Every multi-byte value is big-endian, the 68030's own order.
 */
#ifndef DATA_H
#define DATA_H

#include <stdint.h>

#define TICKS 15         /* simulation ticks a second */
#define NMISSIONS 3      /* MISS1.DAT .. MISS3.DAT */
#define MAX_UNITTYPES 64 /* the converter keeps to it */

/* ---------------------------------------------------------------- weapons */

/* How a weapon reaches its target: a beam hits at once; the D-gun's shot
 * goes on through whatever it meets */
enum { WK_BEAM, WK_CANNON, WK_ROCKET, WK_SHOT, WK_DGUN };

#define WF_PARALYZE 1  /* it stops what it hits instead of harming it */
#define WF_MANUAL 2    /* it fires only when ordered to, paying its energy */
#define WF_BALLISTIC 4 /* its shots arc */
#define WF_TRACKS 8    /* its shots follow their target */

typedef struct {
    uint16_t range, reload, damage, speed, aoe; /* px, ticks, hp, 8.8 px/tick, px */
    uint8_t kind, colour, burst, gap;
    int16_t snd_start, snd_hit, anim;
    uint8_t flags, pad;
    uint16_t energy; /* a shot's cost */
    char name[12];
} WeaponType; /* 36 bytes */

/* ---------------------------------------------------------------- units */

enum { UK_MOBILE, UK_BUILDING, UK_FACTORY };

#define UF_BUILDER 1
#define UF_COMMANDER 2
#define UF_ARMED 4
#define UF_WIND 8
#define UF_MEX 32

typedef struct {
    char name[10];
    char title[20];
    char desc[32];
    uint8_t side, kind;
    uint16_t flags;
    uint8_t footx, footz, moveclass, nbuild;
    int8_t weapon[3], explode;
    uint16_t maxhp, speed, accel, turn, sight, builddist, workertime;
    uint32_t metalcost, energycost, buildtime;
    int32_t emake, mmake, euse, extract, wind, mstore, estore; /* make/use: 1/256 a tick */
    uint32_t sprite_ofs;
    uint8_t ndirs, radius;
    int16_t top, snd_select, snd_ok;
    int16_t corpse; /* the feature it leaves, or -1 */
    int8_t build[24];
    uint8_t yard[8];
} UnitType; /* 174 bytes */

/* ---------------------------------------------------------------- common */

#define FF_RECLAIM 1
#define FF_INDESTRUCT 4
#define FF_BAKED 8

typedef struct {
    char name[16];
    uint16_t flags;
    uint32_t metal, energy, damage;
    uint8_t footx, footz, height, pad;
    uint32_t sprite; /* 0xFFFFFFFF: none */
} FeatureType;       /* 38 bytes */

typedef struct {
    uint16_t nframes, pad;
    uint32_t table;
} Anim;

enum {
    CUR_NORMAL,
    CUR_SELECT,
    CUR_MOVE,
    CUR_ATTACK,
    CUR_BUILD,
    CUR_PATROL,
    CUR_CANT,
    CUR_RECLAIM,
    CUR_GUARD,
    CUR_REPAIR,
    CUR_COUNT
};

enum {
    BTN_MOVE,
    BTN_ATTACK,
    BTN_STOP,
    BTN_PATROL,
    BTN_GUARD,
    BTN_DGUN,
    BTN_RECLAIM,
    BTN_REPAIR,
    BTN_ORDERS,
    BTN_BUILD,
    BTN_PREV,
    BTN_NEXT,
    BTN_FIREORD,
    BTN_MOVEORD,
    BTN_COUNT
};

enum { TITLE_VICTORY, TITLE_DEFEAT, TITLE_PAUSED, TITLE_COUNT };

/* Effects the engine shows by number, in the converter's order */
enum { ANIM_EXPLOSION }; /* a unit's death, a tree catching fire */

/* Sounds the engine plays by number, in the converter's order */
enum {
    SND_BEEP2,
    SND_BEEP4,
    SND_BUTTON,
    SND_CANTDO,
    SND_XPLOMED,
    SND_XPLOSML,
    SND_XPLOLRG,
    SND_ACTIVATE,
    SND_UNITDONE,
    SND_ADDBUILD,
    SND_NOMETAL,
    SND_WARNING,
    SND_NANO,
    SND_VICTORY,
    SND_XPLOBLD
};

typedef struct {
    uint16_t palette[16];
    const uint16_t* panel; /* 640x480 planar */
    int nfeat, nanim;
    FeatureType* feat;
    Anim* anim;
    uint32_t cursor[CUR_COUNT];
    uint32_t* pic; /* per unit type: 64x64 planar */
    uint16_t button_frames[BTN_COUNT];
    uint32_t button[BTN_COUNT][6];
    uint32_t title[TITLE_COUNT];
    const uint8_t* font;
    const uint8_t* bank;
} Common;

/* ---------------------------------------------------------------- missions */

enum {
    COND_MOVETO = 1,
    COND_KILLTYPE = 2,
    COND_COMMANDER = 10,
    COND_ALLKILLED = 11,
    COND_ALLOFTYPE = 12
};

enum {
    OP_MOVE = 1,
    OP_PATROL,
    OP_WAIT,
    OP_WAITATTACK,
    OP_ATTACKTYPE,
    OP_ORDERS,
    OP_BUILD,
    OP_GUARD,
    OP_ATTACKPOS
};

typedef struct {
    int16_t kind, type, a, b, c;
} Condition;

typedef struct {
    int16_t op, a, b, c;
} ScriptOp;

typedef struct {
    uint16_t type;
    uint8_t player, hp;
    int16_t x, z;
    uint16_t angle, script, nops;
} PlacedUnit; /* 14 bytes */

typedef struct {
    int32_t human_metal, human_energy, computer_metal, computer_energy;
    int nunits;
    PlacedUnit* units;
} Schema;

/* cell flags: bits 0-5 blocked for a move class, 6 water, 7 a baked feature */
#define CELL_WATER 0x40
#define CELL_ROCK 0x80

typedef struct {
    char name[40];
    int w, h, tw, th, ntiles, mw, mh, nfeat, ncond;
    int minwind, maxwind, nschema;
    Condition cond[8];
    uint16_t* featpos; /* type, x, z */
    ScriptOp* ops;
    Schema schema[3];
    uint16_t* tilemap;
    uint8_t *height, *flags, *metal;
    uint16_t* minimap;
    uint8_t* tiles;
    void* block;
} Mission;

/* ---------------------------------------------------------------- sounds */

typedef struct {
    int n;
    int8_t* data; /* ST-RAM */
    uint32_t *ofs, *len;
    uint8_t* rate; /* 0 6258 Hz, 1 12517 Hz */
} Sounds;

extern int nweapons, nunittypes, nclasses;
extern WeaponType* weapons;
extern UnitType* unittypes;
extern const uint8_t* unit_bank;
extern Common common;
extern Sounds sounds;
extern Mission mission;

/* The files are read from the current folder. A load that fails leaves its
 * reason for data_error(). */

/// Loads UNITS.DAT: the weapons, the unit types and their sprites.
///
/// @return 1 when loaded, 0 when not
int load_units(void);

/// Loads COMMON.DAT: the battle palette, the panels, features, effects,
/// cursors, unit pictures, buttons, titles and font.
///
/// @return 1 when loaded, 0 when not
int load_common(void);

/// Loads SOUNDS.DAT into ST-RAM.
///
/// @return 1 when loaded, 0 when not
int load_sounds(void);

/// Loads a mission into mission, freeing the one loaded before.
///
/// @param n the mission, 1 to NMISSIONS
/// @return 1 when loaded, 0 when not
int load_mission(int n);

/// Frees the loaded mission, if any.
void free_mission(void);

/// Reads a mission's name from its file.
///
/// @param n the mission, 1 to NMISSIONS
/// @param[out] name at least 40 characters: the name, or "" when the file cannot be read
void mission_name(int n, char* name);

/// Loads a full-screen picture and its palette.
///
/// @param name the file
/// @param[out] pal the picture's 16 colours
/// @return the file, its planar picture 32 bytes in; free it with tt_free()
uint8_t* load_picture(const char* name, uint16_t* pal);

/// Loads a briefing's narration (BRIEFn.SND, signed 8-bit at 12517 Hz),
/// into TT-RAM where there is some, for snd_stream().
///
/// @param name the file
/// @param[out] len the number of samples
/// @return the samples, to free with tt_free(), or null
int8_t* load_voice(const char* name, int32_t* len);

/// Loads a text file.
///
/// @param name the file
/// @return the text, ending in a zero; free it with tt_free()
char* load_text(const char* name);

/// Says why the last load failed, as words to follow "Sorry, AnnihilaTTion ".
///
/// @return the reason, for the player, such as "can't find UNITS.DAT"
const char* data_error(void);

/// Finds a unit type's sprite for a direction.
///
/// @param u the unit type
/// @param dir 0 to ndirs - 1, 0 facing down the screen
/// @return the sprite
static inline const int16_t* unit_frame(const UnitType* u, int dir) {
    const uint32_t* t = (const uint32_t*)(unit_bank + u->sprite_ofs);
    return (const int16_t*)(unit_bank + t[dir]);
}

/// Finds a sprite in COMMON.DAT's bank.
///
/// @param ofs the sprite's offset in the bank
/// @return the sprite
static inline const int16_t* common_sprite(uint32_t ofs) {
    return (const int16_t*)(common.bank + ofs);
}

/// Finds a frame of an effect.
///
/// @param anim the effect
/// @param f the frame, below the effect's nframes
/// @return the frame's sprite
static inline const int16_t* anim_frame(int anim, int f) {
    const uint32_t* t = (const uint32_t*)(common.bank + common.anim[anim].table);
    return (const int16_t*)(common.bank + t[f]);
}

#endif
