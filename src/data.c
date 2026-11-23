/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Loading the converted game data (see data.h).
 */
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <mint/osbind.h>
#include "data.h"
#include "tt.h"

int ncampaigns, nmissions, ntracks;
Campaign* campaigns;
MissionInfo* missions;
Track* tracks;
int nweapons, nunittypes, nclasses;
WeaponType* weapons;
UnitType* unittypes;
FeatureType* wrecks;
Common common;
World world;
Panel panel;
Sounds sounds;
Mission mission;

_Static_assert(sizeof(Campaign) == 30, "Campaign must match GAME.DAT");
_Static_assert(sizeof(MissionInfo) == 54, "MissionInfo must match GAME.DAT");
_Static_assert(sizeof(Track) == 6, "Track must match GAME.DAT");
_Static_assert(sizeof(WeaponType) == 40, "WeaponType must match UNITS.DAT");
_Static_assert(sizeof(UnitType) == 180, "UnitType must match UNITS.DAT");
_Static_assert(sizeof(FeatureType) == 38, "FeatureType must match UNITS.DAT and the missions'");
_Static_assert(sizeof(PlacedUnit) == 14, "PlacedUnit must match the missions'");

static char err[160];

/* The data's folder, DATA beside the program, from load_game() on: the test
 * script, read before it, lies beside the program */
static char dir[6];

/* A data file's name with its folder */
static const char* path(const char* name) {
    static char buf[32];
    sprintf(buf, "%s%s", dir, name);
    return buf;
}

const char* data_error(void) {
    return err;
}

long data_open(const char* name) {
    return Fopen(path(name), 0);
}

long data_create(const char* name) {
    return Fcreate(path(name), 0);
}

#define BOUNCE 8192L                 /* read through ST-RAM at a time */
#define TT_RAM 0x01000000UL          /* where TT-RAM starts */
#define TRIAL (512 * 1024L)          /* read each way into TT-RAM before choosing */
#define TRIAL_RUN (256 * 1024L)      /* read one way before the other */

static int straight = -1;            /* into TT-RAM: 1 straight, 0 through ST-RAM, -1 trying both */
static long tried[2], took[2];       /* the trial's bytes and 200 Hz ticks, through ST-RAM and straight */

long data_read(long fh, long n, void* dst) {
    static uint8_t* bounce;
    uint8_t* d = dst;
    long got = 0;
    if ((uint32_t)d < TT_RAM || straight == 1 || (!bounce && !(bounce = tt_alloc_st(BOUNCE))))
        return Fread((short)fh, n, d);
    while (got < n) {
        long want = n - got < BOUNCE ? n - got : BOUNCE, r;
        /* while trying, runs through ST-RAM, straight, straight and through
         * ST-RAM, long enough for the 200 Hz clock: a chunk each way in turn
         * kept step with it, and timed the faster way the slower */
        long run = (tried[0] + tried[1]) / TRIAL_RUN;
        int way = straight < 0 && ((run ^ (run >> 1)) & 1);
        uint32_t t = HZ200;
        r = Fread((short)fh, want, way ? d + got : bounce);
        if (r <= 0)
            return got ? got : r;
        if (!way)
            memcpy(d + got, bounce, r);
        if (straight < 0) {
            tried[way] += r;
            took[way] += (long)(HZ200 - t);
            if (tried[0] >= TRIAL && tried[1] >= TRIAL)
                straight = took[1] * tried[0] < took[0] * tried[1];
        }
        got += r;
        if (r < want)
            break;
    }
    return got;
}

int data_read_way(long ms[2]) {
    ms[0] = took[0] * 5;
    ms[1] = took[1] * 5;
    return straight;
}

/* Reads a whole file into memory from tt_alloc_any (or ST-RAM); with twice,
 * into the top half of a block twice its length, for double_samples() */
static uint8_t* load_file(const char* name, int32_t* len, int st, int twice) {
    long fh, size, room;
    uint8_t* buf;
    fh = data_open(name);
    if (fh < 0) {
        sprintf(err, "can't find %s", path(name));
        return 0;
    }
    size = Fseek(0, (short)fh, 2);
    (void)Fseek(0, (short)fh, 0);
    room = twice ? size * 2 + 4 : size + 4;
    buf = st ? tt_alloc_st(room) : tt_alloc_any(room);
    if (!buf) {
        (void)Fclose((short)fh);
        sprintf(err, "needs more free RAM to load %s", name);
        return 0;
    }
    if (data_read(fh, size, twice ? buf + size : buf) != size) {
        (void)Fclose((short)fh);
        tt_free(buf);
        sprintf(err, "can't read %s", name);
        return 0;
    }
    (void)Fclose((short)fh);
    if (len)
        *len = size;
    return buf;
}

static const uint8_t* align4(const uint8_t* base, const uint8_t* p) {
    while ((p - base) & 3)
        p++;
    return p;
}

static int wrong_version(const char* name) {
    sprintf(err, "can't use %s from another version", name);
    return 0;
}

int load_game(void) {
    uint8_t* f;
    const uint8_t* p;
    strcpy(dir, "DATA\\");
    f = load_file("GAME.DAT", 0, 0, 0);
    if (!f)
        return 0;
    if (memcmp(f, "OAG3", 4))
        return wrong_version("GAME.DAT");
    ncampaigns = rd16(f + 4);
    nmissions = rd16(f + 6);
    /* f + 8: the worlds, which each mission names */
    ntracks = rd16(f + 10);
    p = f + 16;
    /* the records are used where they lie: they are all an even length */
    campaigns = (Campaign*)p;
    p += ncampaigns * sizeof(Campaign);
    missions = (MissionInfo*)p;
    p += nmissions * sizeof(MissionInfo);
    tracks = (Track*)p;
    return 1;
}

int find_mission(const char* file) {
    int i, j;
    for (i = 0; i < nmissions; i++) {
        for (j = 0; file[j] && missions[i].file[j]; j++)
            if (toupper((unsigned char)file[j]) != missions[i].file[j])
                break;
        if (!file[j] && !missions[i].file[j])
            return i;
    }
    return -1;
}

int load_units(void) {
    uint8_t* f = load_file("UNITS.DAT", 0, 0, 0);
    const uint8_t* p;
    if (!f)
        return 0;
    nweapons = rd16(f + 4);
    nunittypes = rd16(f + 6);
    nclasses = rd16(f + 8);
    if (memcmp(f, "OAU3", 4) || nunittypes > MAX_UNITTYPES || nclasses > MAX_CLASSES)
        return wrong_version("UNITS.DAT");
    /* each class's slope and depths: the converter's alone */
    p = f + 10 + nclasses * 3;
    if ((p - f) & 1)
        p++;
    weapons = (WeaponType*)p;
    p += nweapons * sizeof(WeaponType);
    unittypes = (UnitType*)p;
    p += nunittypes * sizeof(UnitType);
    wrecks = (FeatureType*)p;
    return 1;
}

int load_common(void) {
    uint8_t* f = load_file("COMMON.DAT", 0, 0, 0);
    const uint8_t* bank;
    if (!f)
        return 0;
    if (memcmp(f, "OAC3", 4))
        return wrong_version("COMMON.DAT");
    bank = f + 16;
    common.font = bank + rd32(f + 4);
    common.fog = (const uint16_t*)(bank + rd32(f + 8));
    return 1;
}

int load_world(int n) {
    char name[16];
    uint8_t* f;
    const uint8_t* p;
    int i, ncur, ntit;
    if (world.num == n)
        return 1;
    if (world.block) {
        tt_free(world.block);
        tt_free(world.anim);
        world.block = 0;
        world.anim = 0;
        world.num = 0;
    }
    sprintf(name, "WORLD%d.DAT", n);
    f = load_file(name, 0, 0, 0);
    if (!f)
        return 0;
    world.block = f;
    if (memcmp(f, "OAW3", 4))
        return wrong_version(name);
    for (i = 0; i < 16; i++)
        world.palette[i] = rd16(f + 4 + i * 2);
    p = f + 36;
    world.nanim = rd16(p);
    ncur = rd16(p + 2);
    ntit = rd16(p + 4);
    p += 6;
    if (ncur != CUR_COUNT || ntit != TITLE_COUNT)
        return wrong_version(name);
    memcpy(world.beam, p, 256);
    p += 256;
    world.anim = tt_alloc_any(world.nanim * (int32_t)sizeof(Anim) + 4);
    for (i = 0; i < world.nanim; i++, p += 8) {
        world.anim[i].nframes = rd16(p);
        world.anim[i].table = rd32(p + 4);
    }
    for (i = 0; i < CUR_COUNT; i++, p += 4)
        world.cursor[i] = rd32(p);
    for (i = 0; i < TITLE_COUNT; i++, p += 4)
        world.title[i] = rd32(p);
    p = align4(f, p);
    world.bank = p + 4;
    world.num = n;
    return 1;
}

int load_panel(int n, int side, uint8_t* screen) {
    char name[16];
    long fh, size, rest;
    uint8_t* f;
    const uint8_t* p;
    int i, j, nbtn;
    if (panel.block) {
        tt_free(panel.block);
        panel.block = 0;
    }
    sprintf(name, "PANEL%d%c.DAT", n, side ? 'C' : 'A');
    fh = data_open(name);
    if (fh < 0) {
        sprintf(err, "can't find %s", path(name));
        return 0;
    }
    size = Fseek(0, (short)fh, 2);
    (void)Fseek(0, (short)fh, 0);
    rest = size - SCREEN_BYTES;
    f = rest > 0 ? tt_alloc_any(rest + 4) : 0;
    if (!f || data_read(fh, SCREEN_BYTES, screen) != SCREEN_BYTES || data_read(fh, rest, f) != rest) {
        (void)Fclose((short)fh);
        tt_free(f);
        sprintf(err, f ? "can't read %s" : "needs more free RAM to load %s", name);
        return 0;
    }
    (void)Fclose((short)fh);
    panel.block = f;
    nbtn = rd16(f);
    if (nbtn != BTN_COUNT)
        return wrong_version(name);
    p = f + 2;
    for (i = 0; i < BTN_COUNT; i++, p += 26) {
        panel.button_frames[i] = rd16(p);
        for (j = 0; j < 6; j++)
            panel.button[i][j] = rd32(p + 2 + j * 4);
    }
    /* the block starts where the picture ended, on a 4-byte boundary */
    p = align4(f, p);
    panel.bank = p + 4;
    return 1;
}

/* 6258 Hz samples as 12517 Hz ones, each twice, for a machine with no 6258
 * Hz (a Falcon): n samples from s written as 2n from d on, an even address.
 * d may lie below s in one block: each write stays below what is still to
 * be read as long as s - d is at least n. */
static void double_samples(int8_t* d, const int8_t* s, int32_t n) {
    uint16_t* w = (uint16_t*)d;
    while (n-- > 0) {
        uint8_t v = (uint8_t)*s++;
        *w++ = (uint16_t)(v << 8 | v);
    }
}

/* Every sound at 12517 Hz: SOUNDS.DAT sits in the top half of the block f,
 * and its samples go from f's start on, the 6258 Hz ones doubled, the rest
 * as they are; then the block is cut to them */
static void sounds_at_12517(int8_t* f) {
    int32_t at = 0;
    int i;
    for (i = 0; i < sounds.n; i++) {
        const int8_t* s = sounds.data + sounds.ofs[i];
        int32_t n = sounds.len[i];
        if (sounds.rate[i]) {
            memmove(f + at, s, n);
        } else {
            double_samples(f + at, s, n);
            n *= 2;
        }
        sounds.ofs[i] = at;
        sounds.len[i] = n;
        sounds.rate[i] = 1;
        at += n;
    }
    sounds.data = f;
    (void)Mshrink(f, at);
}

int load_sounds(void) {
    int32_t size;
    int twice = !tt_plays_6258;
    uint8_t* f = load_file("SOUNDS.DAT", &size, 1, twice);
    const uint8_t *head, *p;
    int i;
    if (!f)
        return 0;
    head = twice ? f + size : f;
    if (memcmp(head, "OAS2", 4))
        return wrong_version("SOUNDS.DAT");
    sounds.n = rd16(head + 4);
    sounds.ofs = tt_alloc_any(sounds.n * 4L);
    sounds.len = tt_alloc_any(sounds.n * 4L);
    sounds.rate = tt_alloc_any(sounds.n + 2L);
    p = head + 6;
    for (i = 0; i < sounds.n; i++, p += 12) {
        sounds.ofs[i] = rd32(p);
        sounds.len[i] = rd32(p + 4);
        sounds.rate[i] = (uint8_t)rd16(p + 8);
    }
    sounds.data = (int8_t*)p;
    if (twice)
        sounds_at_12517((int8_t*)f);
    return 1;
}

void free_mission(void) {
    int i;
    if (mission.block) {
        tt_free(mission.block);
        mission.block = 0;
    }
    for (i = 0; i < 3; i++)
        if (mission.schema[i].units) {
            tt_free(mission.schema[i].units);
            mission.schema[i].units = 0;
        }
}

int load_mission(int n) {
    char name[16];
    uint8_t* f;
    const uint8_t* p;
    int i, j, cells;
    uint32_t nops;
    free_mission();
    sprintf(name, "%s.DAT", missions[n].file);
    f = load_file(name, 0, 0, 0);
    if (!f)
        return 0;
    mission.block = f;
    if (memcmp(f, "OAM3", 4) || rd16(f + 44 + 20) > 8 || rd16(f + 44 + 28) > 3)
        return wrong_version(name);
    memcpy(mission.name, f + 4, 40);
    p = f + 44;
    mission.w = rd16(p);
    mission.h = rd16(p + 2);
    mission.tw = rd16(p + 4);
    mission.th = rd16(p + 6);
    mission.ntiles = rd16(p + 8);
    mission.sealevel = rd16(p + 10);
    mission.mw = rd16(p + 12);
    mission.mh = rd16(p + 14);
    mission.nftypes = rd16(p + 16);
    mission.nfeat = rd16(p + 18);
    mission.ncond = rd16(p + 20);
    /* p + 22: the sun's strength, which solar collectors' output already holds */
    mission.minwind = rd16(p + 24);
    mission.maxwind = rd16(p + 26);
    mission.nschema = rd16(p + 28);
    mission.tidal = rd16(p + 30);
    mission.world = rd16(p + 32) + 1;
    mission.side = rd16(p + 34);
    /* p + 36: whether it is a lava world, which its tiles show */
    mission.nneed = rd16(p + 38);
    p += 40;
    for (i = 0; i < mission.ncond; i++, p += 10)
        memcpy(&mission.cond[i], p, 10);
    mission.skirmish = 0;
    mission.need = (uint16_t*)p;
    p += mission.nneed * 2;
    mission.allowed = p;
    p += 32;
    mission.ftypes = (FeatureType*)p;
    p += mission.nftypes * sizeof(FeatureType);
    mission.featpos = (uint16_t*)p;
    p += mission.nfeat * 6;
    nops = rd32(p);
    mission.ops = (ScriptOp*)(p + 4);
    p += 4 + nops * 8;
    for (i = 0; i < mission.nschema; i++) {
        Schema* s = &mission.schema[i];
        s->human_metal = (int32_t)rd32(p);
        s->human_energy = (int32_t)rd32(p + 4);
        s->computer_metal = (int32_t)rd32(p + 8);
        s->computer_energy = (int32_t)rd32(p + 12);
        /* p + 16: the metal of ground with no deposit, which the cells' metal holds */
        s->nunits = rd16(p + 18);
        p += 20;
        s->units = tt_alloc_any(s->nunits * (int32_t)sizeof(PlacedUnit) + 2);
        for (j = 0; j < s->nunits; j++, p += 14)
            memcpy(&s->units[j], p, 14);
    }
    mission.tilemap = (uint16_t*)p;
    p += mission.tw * mission.th * 2;
    cells = mission.w * mission.h;
    mission.height = (uint8_t*)p;
    mission.flags = (uint8_t*)p + cells;
    mission.metal = (uint8_t*)p + cells * 2;
    p += cells * 3;
    mission.minimap = (uint16_t*)p;
    p += mission.mh * (mission.mw / 16) * 8;
    p = align4(f, p);
    mission.tiles = (uint8_t*)p;
    p += (int32_t)mission.ntiles * 512;
    mission.bank = p + 4;
    return 1;
}

uint8_t* load_picture(const char* name, uint16_t* pal) {
    uint8_t* f = load_file(name, 0, 0, 0);
    int i;
    if (!f)
        return 0;
    for (i = 0; i < 16; i++)
        pal[i] = rd16(f + i * 2);
    return f; /* planar data at f + 32 */
}

char* load_text(const char* name) {
    int32_t len;
    char* t = (char*)load_file(name, &len, 0, 0);
    if (t)
        t[len] = 0;
    return t;
}
