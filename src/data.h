/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The converted game data (tools/convert.py writes it; the layouts here
 * must match). Every multi-byte value is big-endian, the 68030's own order.
 * The demo and the full game have the same files: GAME.DAT says which
 * campaigns and missions there are.
 */
#ifndef DATA_H
#define DATA_H

#include <stdint.h>

#define TICKS 15          /* simulation ticks a second */
#define MAX_UNITTYPES 255 /* a unit type is a byte; 255 is none */
#define NO_TYPE 255
#define MAX_BUILD 24 /* build options a unit type holds */
#define MAX_CLASSES 6 /* move classes: a map cell's flags have six bits for them */

/* ---------------------------------------------------------------- the game */

typedef struct {
    char name[24];
    uint8_t side, pad; /* 0 Arm, 1 Core, 2 a skirmish's maps */
    uint16_t first, count; /* its missions in missions[] */
} Campaign;                /* 30 bytes */

typedef struct {
    char name[40];
    char file[10];       /* the files' name: FILE.DAT, .TXT, .SND, .IMG */
    uint8_t world, side; /* its world (0 on); the player's side */
    uint16_t pad;
} MissionInfo; /* 54 bytes */

typedef struct {
    uint8_t num, kind; /* the CD's track number; 0 calm, 1 loud */
    uint32_t samples;
} Track; /* 6 bytes */

extern int ncampaigns, nmissions, ntracks;
extern Campaign* campaigns;
extern MissionInfo* missions;
extern Track* tracks;

/* ---------------------------------------------------------------- weapons */

/* How a weapon reaches its target: a beam hits at once; the D-gun's shot
 * goes on through whatever it meets; a bomb falls from an aircraft; a
 * torpedo runs under water */
enum { WK_BEAM, WK_CANNON, WK_ROCKET, WK_SHOT, WK_DGUN, WK_BOMB, WK_TORPEDO };

#define WF_PARALYZE 1   /* it stops what it hits instead of harming it */
#define WF_MANUAL 2     /* it fires only when ordered to, paying its energy */
#define WF_BALLISTIC 4  /* its shots arc */
#define WF_TRACKS 8     /* its shots follow their target */
#define WF_AIR 16       /* it can hit aircraft */
#define WF_WATER 32     /* it hits only what is in the water */
#define WF_STOCKPILE 64 /* its missiles are built before they fire */
#define WF_INTERCEPT 128 /* it shoots down nuclear missiles */
#define WF2_NUKE 1       /* an anti-missile system can shoot it down */
#define WF2_FLAME 2      /* a flamethrower's */

typedef struct {
    uint16_t range, reload, damage, damage_air, speed, aoe; /* px, ticks, hp, hp, 8.8 px/tick, px */
    uint8_t kind, colour, burst, gap; /* colour: the game palette's, through world.beam[] */
    int16_t snd_start, snd_hit, anim;
    uint8_t flags, flags2;
    uint16_t energy, metal; /* a shot's cost */
    char name[12];
} WeaponType; /* 40 bytes */

/* ---------------------------------------------------------------- units */

enum { UK_MOBILE, UK_BUILDING, UK_FACTORY };

#define UF_BUILDER 1
#define UF_COMMANDER 2
#define UF_ARMED 4
#define UF_WIND 8
#define UF_PATROL 16
#define UF_MEX 32
#define UF_FLY 64            /* an aircraft */
#define UF_SHIP 128          /* floats: a ship, or a building on the water */
#define UF_SUB 256           /* goes under water */
#define UF_CAPTURE 512       /* can capture */
#define UF_TIDAL 1024        /* makes energy from the tide */
#define UF_MAKER 2048        /* turns energy into metal */
#define UF_GEO 4096          /* stands on a geothermal vent */
#define UF_KAMIKAZE 8192     /* blows itself up on its target */
#define UF_TRANSPORT 16384   /* carries units */
#define UF_AIRPREF 32768UL   /* its weapons look for aircraft first */
#define UF_NOAIR 65536UL     /* it leaves aircraft alone */
#define UF_BOMBER (1UL << 17)
#define UF_ANTINUKE (1UL << 18)
#define UF_STOCKPILE (1UL << 19)
#define UF_PHIB (1UL << 20)     /* amphibious */
#define UF_ONOFF (1UL << 21)    /* can be switched on and off */
#define UF_WATERBLD (1UL << 22) /* a building that stands in water */
#define UF_RADAR (1UL << 23)
#define UF_SONAR (1UL << 24)
#define UF_JAMMER (1UL << 25)
#define UF_REPAIR (1UL << 26) /* repairs */
#define UF_ACTIVE (1UL << 27) /* switched on when built: pays its upkeep */

typedef struct {
    char name[10];
    char title[20];
    char desc[32];
    uint8_t side, kind, footx, footz, moveclass, nbuild;
    int8_t weapon[3], explode;
    uint32_t flags;
    uint16_t maxhp, speed, accel, turn, sight, builddist, workertime, cruisealt, radar;
    uint32_t metalcost, energycost, buildtime;
    int32_t emake, mmake, euse, extract, wind, mstore, estore; /* make/use: 1/256 a tick */
    uint8_t ndirs, radius;
    int16_t top, snd_select, snd_ok;
    int16_t corpse; /* its wreck, a feature type (wreck_feat()), or -1 */
    int8_t selfd;   /* the weapon it blows up with when it destroys itself */
    uint8_t minwater, maxwater, maxslope; /* where a building may stand */
    uint8_t build[MAX_BUILD]; /* NO_TYPE past nbuild */
    uint8_t yard[8];
} UnitType; /* 180 bytes */

/* ---------------------------------------------------------------- features */

#define FF_RECLAIM 1
#define FF_INDESTRUCT 4
#define FF_BAKED 8
#define FF_GEO 16 /* a geothermal vent */

#define FEAT_NONE 0xFFFFFFFFUL  /* FeatureType.sprite: none */
#define FEAT_WRECK 0xFFFFFFFEUL /* FeatureType.sprite: a unit's wreck, in its sprites */

typedef struct {
    char name[16];
    uint16_t flags;
    uint32_t metal, energy, damage;
    uint8_t footx, footz, height, pad;
    uint32_t sprite; /* in the mission's bank, or FEAT_NONE or FEAT_WRECK */
} FeatureType;       /* 38 bytes */

/* ---------------------------------------------------------------- a world */

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
    CUR_CAPTURE,
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
    BTN_CAPTURE,
    BTN_ONOFF,
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

/* WORLDn.DAT: what a world's colours change */
typedef struct {
    int num; /* 1 on, or 0 for none loaded */
    uint16_t palette[16];
    int nanim;
    Anim* anim;
    uint32_t cursor[CUR_COUNT];
    uint32_t title[TITLE_COUNT];
    uint8_t beam[256]; /* the game palette's colours as the world's */
    const uint8_t* bank;
    void* block;
} World;

/* PANELnA.DAT or PANELnC.DAT: the panels round the battlefield, kept where
 * they show (the side panel, the bars above and below the battlefield),
 * and a side's order buttons */
typedef struct {
    uint16_t button_frames[BTN_COUNT];
    uint32_t button[BTN_COUNT][6];
    const uint8_t* bank;
    void* block;
} Panel;

/* COMMON.DAT: what every world shares */
typedef struct {
    const uint8_t* font;
    /* the fog's edges: for each of 4 sets of tiles, each pattern of
     * unexplored quarters (1 top left, 2 top right, 4 bottom left, 8 bottom
     * right), each quarter (top left, top right, bottom left, bottom right)
     * and each of its 16 rows, the pixels left drawn */
    const uint16_t* fog;
} Common;

/* ---------------------------------------------------------------- missions */

enum {
    /* what wins */
    COND_MOVETO = 1,
    COND_KILLTYPE = 2,
    COND_DESTROYALL = 3,
    COND_CAPTURE = 4,
    COND_BUILD = 5,
    COND_KILLCOMMANDER = 6,
    COND_KILLALLOFTYPE = 7,
    COND_TYPEPASSESZ = 8,
    /* what loses */
    COND_COMMANDER = 10,
    COND_ALLKILLED = 11,
    COND_ALLOFTYPE = 12,
    COND_TYPEKILLED = 13,
    COND_ANYPASSESZ = 14,
    COND_TIMER = 15
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
    OP_ATTACKPOS,
    OP_SELFDESTRUCT
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
    int w, h, tw, th, ntiles, sealevel, mw, mh, nftypes, nfeat, ncond;
    int minwind, maxwind, nschema, tidal, world, side, nneed;
    int skirmish; /* set by the game for a skirmish: the computer builds its base */
    Condition cond[8];
    uint16_t* need; /* the unit types its layouts place */
    const uint8_t* allowed; /* a bit each unit type the player may build, the first type's bit 7 */
    FeatureType* ftypes; /* its own, then every unit type's wreck (wreck_feat()) */
    uint16_t* featpos; /* type, x, z */
    ScriptOp* ops;
    Schema schema[3];
    uint16_t* tilemap;
    uint8_t *height, *flags, *metal;
    uint16_t* minimap;
    uint8_t* tiles;
    const uint8_t* bank; /* the features' sprites */
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
extern FeatureType* wrecks; /* each unit type's wreck */
extern Common common;
extern World world;
extern Panel panel;
extern Sounds sounds;
extern Mission mission;

/* The files are read from the DATA folder beside the program, from
 * load_game() on; before it, from the program's own folder. A load that
 * fails leaves its reason for data_error(). */

/// Opens a data file to read.
///
/// @param name the file, such as "FILM1.DAT"
/// @return its handle, or a negative GEMDOS error
long data_open(const char* name);

/// Creates or empties a file in the data's folder, to write.
///
/// @param name the file, such as "PROGRESS.DAT"
/// @return its handle, or a negative GEMDOS error
long data_create(const char* name);

/// Reads from a file, as Fread() does. The disk's DMA reaches only ST-RAM,
/// so a driver copies what is bound for TT-RAM itself: older GemDrives more
/// slowly than reading into ST-RAM and copying, newer ones faster. So the
/// first megabyte read into TT-RAM goes half each way, through a buffer in
/// ST-RAM and straight, and the rest the way that was faster.
///
/// @param fh the file's handle
/// @param n the bytes to read
/// @param dst where they go, in either memory
/// @return the bytes read, or a negative GEMDOS error
long data_read(long fh, long n, void* dst);

/// How data_read() reads into TT-RAM, for the test log.
///
/// @param ms gets the milliseconds its trial took each way, through ST-RAM
/// and straight, over 512 KB each
/// @return 1 straight, 0 through ST-RAM, -1 while it is still trying both
int data_read_way(long ms[2]);

/// Loads GAME.DAT, moving on to the data's folder: the campaigns, their
/// missions and the music.
///
/// @return 1 when loaded, 0 when not
int load_game(void);

/// Loads UNITS.DAT: the weapons, the unit types and their wrecks.
///
/// @return 1 when loaded, 0 when not
int load_units(void);

/// Loads COMMON.DAT: the font and the fog's edges.
///
/// @return 1 when loaded, 0 when not
int load_common(void);

/// Loads SOUNDS.DAT into ST-RAM.
///
/// @return 1 when loaded, 0 when not
int load_sounds(void);

/// Loads a world's palette, effects, cursors and titles, freeing the one
/// loaded before (nothing when it is loaded already).
///
/// @param n the world, 1 on
/// @return 1 when loaded, 0 when not
int load_world(int n);

/// Loads a world's panels for a side into a screen-sized buffer, and keeps
/// the side's order buttons.
///
/// @param n the world, 1 on
/// @param side 0 Arm, 1 Core
/// @param[out] screen 640x480 planar: the panels, the battlefield's part black
/// @return 1 when loaded, 0 when not
int load_panel(int n, int side, uint8_t* screen);

/// Loads a mission into mission, freeing the one loaded before.
///
/// @param n the mission, an index into missions[]
/// @return 1 when loaded, 0 when not
int load_mission(int n);

/// Frees the loaded mission, if any.
void free_mission(void);

/// Loads a full-screen picture and its palette.
///
/// @param name the file
/// @param[out] pal the picture's 16 colours
/// @return the file, its planar picture 32 bytes in; free it with tt_free()
uint8_t* load_picture(const char* name, uint16_t* pal);

/// Loads a text file.
///
/// @param name the file
/// @return the text, ending in a zero; free it with tt_free()
char* load_text(const char* name);

/// Says why the last load failed, as words to follow "Sorry, AnnihilaTTion ".
///
/// @return the reason, for the player, such as "can't find UNITS.DAT"
const char* data_error(void);

/// Finds a mission by its file's name.
///
/// @param file such as "AC05", any case
/// @return its index in missions[], or -1
int find_mission(const char* file);

/// Reads a big-endian 16-bit value, at any address.
///
/// @param p its first byte
/// @return the value
static inline uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

/// Reads a big-endian 32-bit value, at any address.
///
/// @param p its first byte
/// @return the value
static inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/// Finds a sprite in the world's bank.
///
/// @param ofs the sprite's offset in the bank
/// @return the sprite
static inline const int16_t* world_sprite(uint32_t ofs) {
    return (const int16_t*)(world.bank + ofs);
}

/// Finds a sprite in the panel's bank (the order buttons).
///
/// @param ofs the sprite's offset in the bank
/// @return the sprite
static inline const int16_t* panel_sprite(uint32_t ofs) {
    return (const int16_t*)(panel.bank + ofs);
}

/// Finds a frame of an effect.
///
/// @param anim the effect
/// @param f the frame, below the effect's nframes
/// @return the frame's sprite
static inline const int16_t* anim_frame(int anim, int f) {
    const uint32_t* t = (const uint32_t*)(world.bank + world.anim[anim].table);
    return (const int16_t*)(world.bank + t[f]);
}

/// Gives the feature type of a unit type's wreck.
///
/// @param type the unit type
/// @return the feature type, past the mission's own
static inline int wreck_feat(int type) {
    return mission.nftypes + type;
}

/// Tells whether the player may build a unit type in the mission.
///
/// @param type the unit type
/// @return nonzero when allowed
static inline int build_allowed(int type) {
    return mission.allowed[type >> 3] & (0x80 >> (type & 7));
}

/// Gives a feature type.
///
/// @param f the feature type, the mission's own or a wreck
/// @return its record
static inline const FeatureType* feat_type(int f) {
    return f < mission.nftypes ? &mission.ftypes[f] : &wrecks[f - mission.nftypes];
}

#endif
