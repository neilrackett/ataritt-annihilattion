/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Loading the converted game data (see data.h).
 */
#include <string.h>
#include <stdio.h>
#include <mint/osbind.h>
#include "data.h"
#include "tt.h"

int nweapons, nunittypes, nclasses;
WeaponType* weapons;
UnitType* unittypes;
const uint8_t* unit_bank;
Common common;
Sounds sounds;
Mission mission;

_Static_assert(sizeof(WeaponType) == 36, "WeaponType must match UNITS.DAT");
_Static_assert(sizeof(UnitType) == 174, "UnitType must match UNITS.DAT");
_Static_assert(sizeof(FeatureType) == 38, "FeatureType must match COMMON.DAT");
_Static_assert(sizeof(PlacedUnit) == 14, "PlacedUnit must match MISSn.DAT");

static char err[160];

const char* data_error(void) {
    return err;
}

/* Reads a whole file into memory from tt_alloc_any (or ST-RAM); with twice,
 * into the top half of a block twice its length, for double_samples() */
static uint8_t* load_file(const char* name, int32_t* len, int st, int twice) {
    long fh, size, room;
    uint8_t* buf;
    fh = Fopen(name, 0);
    if (fh < 0) {
        sprintf(err, "can't find %s", name);
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
    if (Fread((short)fh, size, twice ? buf + size : buf) != size) {
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

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static const uint8_t* align4(const uint8_t* base, const uint8_t* p) {
    while ((p - base) & 3)
        p++;
    return p;
}

int load_units(void) {
    uint8_t* f = load_file("UNITS.DAT", 0, 0, 0);
    const uint8_t* p;
    if (!f)
        return 0;
    nweapons = rd16(f + 4);
    nunittypes = rd16(f + 6);
    nclasses = rd16(f + 8);
    if (memcmp(f, "OAU2", 4) || nunittypes > MAX_UNITTYPES) {
        strcpy(err, "can't use UNITS.DAT from another version");
        return 0;
    }
    p = f + 10 + nclasses * 3; /* each class's slope and depths: the converter's alone */
    weapons = tt_alloc_any(nweapons * (int32_t)sizeof(WeaponType));
    unittypes = tt_alloc_any(nunittypes * (int32_t)sizeof(UnitType));
    memcpy(weapons, p, nweapons * sizeof(WeaponType));
    p += nweapons * sizeof(WeaponType);
    memcpy(unittypes, p, nunittypes * sizeof(UnitType));
    p += nunittypes * sizeof(UnitType);
    p = align4(f, p);
    unit_bank = p + 4;
    return 1;
}

int load_common(void) {
    uint8_t* f = load_file("COMMON.DAT", 0, 0, 0);
    const uint8_t* p;
    int i, j, ncur, npic, nbtn, ntit;
    uint32_t panel, font, fog;
    if (!f)
        return 0;
    if (memcmp(f, "OAC2", 4)) {
        strcpy(err, "can't use COMMON.DAT from another version");
        return 0;
    }
    for (i = 0; i < 16; i++)
        common.palette[i] = rd16(f + 4 + i * 2);
    p = f + 36;
    panel = rd32(p);
    common.nfeat = rd16(p + 4);
    common.nanim = rd16(p + 6);
    ncur = rd16(p + 8);
    npic = rd16(p + 10);
    nbtn = rd16(p + 12);
    ntit = rd16(p + 14);
    font = rd32(p + 16);
    fog = rd32(p + 20);
    p += 24;
    if (ncur != CUR_COUNT || nbtn != BTN_COUNT || ntit != TITLE_COUNT) {
        strcpy(err, "can't use COMMON.DAT from another version");
        return 0;
    }
    common.feat = tt_alloc_any(common.nfeat * (int32_t)sizeof(FeatureType));
    memcpy(common.feat, p, common.nfeat * sizeof(FeatureType));
    p += common.nfeat * sizeof(FeatureType);
    common.anim = tt_alloc_any(common.nanim * (int32_t)sizeof(Anim));
    for (i = 0; i < common.nanim; i++, p += 8) {
        common.anim[i].nframes = rd16(p);
        common.anim[i].table = rd32(p + 4);
    }
    for (i = 0; i < CUR_COUNT; i++, p += 4)
        common.cursor[i] = rd32(p);
    common.pic = tt_alloc_any(npic * 4L);
    for (i = 0; i < npic; i++, p += 4)
        common.pic[i] = rd32(p);
    for (i = 0; i < BTN_COUNT; i++, p += 26) {
        common.button_frames[i] = rd16(p);
        for (j = 0; j < 6; j++)
            common.button[i][j] = rd32(p + 2 + j * 4);
    }
    for (i = 0; i < TITLE_COUNT; i++, p += 4)
        common.title[i] = rd32(p);
    p = align4(f, p);
    common.bank = p + 4;
    common.panel = (const uint16_t*)(common.bank + panel);
    common.font = common.bank + font;
    common.fog = (const uint16_t*)(common.bank + fog);
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
    if (memcmp(head, "OAS2", 4)) {
        strcpy(err, "can't use SOUNDS.DAT from another version");
        return 0;
    }
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
    sprintf(name, "MISS%d.DAT", n);
    f = load_file(name, 0, 0, 0);
    if (!f)
        return 0;
    mission.block = f;
    if (memcmp(f, "OAM2", 4) || rd16(f + 44 + 18) > 8 || rd16(f + 44 + 26) > 3) {
        sprintf(err, "can't use %s from another version", name);
        return 0;
    }
    memcpy(mission.name, f + 4, 40);
    p = f + 44;
    mission.w = rd16(p);
    mission.h = rd16(p + 2);
    mission.tw = rd16(p + 4);
    mission.th = rd16(p + 6);
    mission.ntiles = rd16(p + 8);
    /* p + 10: the sea level, which the cell flags already hold */
    mission.mw = rd16(p + 12);
    mission.mh = rd16(p + 14);
    mission.nfeat = rd16(p + 16);
    mission.ncond = rd16(p + 18);
    /* p + 20: the sun's strength, which solar collectors' output already holds */
    mission.minwind = rd16(p + 22);
    mission.maxwind = rd16(p + 24);
    mission.nschema = rd16(p + 26);
    p += 28;
    for (i = 0; i < mission.ncond; i++, p += 10)
        memcpy(&mission.cond[i], p, 10);
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
    return 1;
}

void mission_name(int n, char* name) {
    char file[16];
    long fh;
    sprintf(file, "MISS%d.DAT", n);
    name[0] = 0;
    fh = Fopen(file, 0);
    if (fh < 0)
        return;
    if (Fseek(4, (short)fh, 0) == 4 && Fread((short)fh, 40, name) == 40)
        name[39] = 0;
    else
        name[0] = 0;
    (void)Fclose((short)fh);
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

int8_t* load_voice(const char* name, int32_t* len) {
    return (int8_t*)load_file(name, len, 0, 0);
}
