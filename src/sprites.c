/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The units' sprites, read as they are needed (see sprites.h).
 *
 * WORLDn.SPR holds, for each unit type, its build picture (64x64 planar)
 * and a blob: a table of the offsets of its sprites, one a direction and
 * then its wreck's (0 for none), and the sprites. They are read into one
 * block of memory, shared out by a first-fit list of free runs: TOS keeps
 * only so many blocks of its own, so the game does not ask it for one a
 * sprite.
 */
#include <string.h>
#include <stdio.h>
#include <mint/osbind.h>
#include "sprites.h"
#include "sim.h"
#include "tt.h"

typedef struct Free {
    int32_t size; /* the run's bytes, this header's 4 too */
    struct Free* next;
} Free;

static uint8_t* arena;
static int32_t arena_size, arena_used;
static Free* free_list;
static long fh = -1;
static int ntypes;
static uint32_t* spr_index; /* each type's picture offset, blob offset and blob length */
static uint8_t* blob[MAX_UNITTYPES];
static uint16_t* pic[MAX_UNITTYPES];
static uint32_t seen_blob[MAX_UNITTYPES], seen_pic[MAX_UNITTYPES];
static uint32_t frame_no = 1;
static const int16_t empty[4] = {0, 0, 0, 0};

static void* arena_alloc(int32_t n) {
    Free **pp, *f;
    n = (n + 4 + 7) & ~7L; /* a 4-byte header, and runs a multiple of 8 */
    for (pp = &free_list; (f = *pp) != 0; pp = &f->next) {
        if (f->size < n)
            continue;
        if (f->size - n >= 16) {
            Free* r = (Free*)((uint8_t*)f + n);
            r->size = f->size - n;
            r->next = f->next;
            *pp = r;
            f->size = n;
        } else {
            *pp = f->next;
        }
        arena_used += f->size;
        return (int32_t*)f + 1;
    }
    return 0;
}

static void arena_free(void* p) {
    Free *b = (Free*)((int32_t*)p - 1), *prev = 0, *f = free_list;
    arena_used -= b->size;
    while (f && f < b) {
        prev = f;
        f = f->next;
    }
    b->next = f;
    if (prev)
        prev->next = b;
    else
        free_list = b;
    if (f && (uint8_t*)b + b->size == (uint8_t*)f) {
        b->size += f->size;
        b->next = f->next;
    }
    if (prev && (uint8_t*)prev + prev->size == (uint8_t*)b) {
        prev->size += b->size;
        prev->next = b->next;
    }
}

void spr_close(void) {
    if (fh >= 0)
        (void)Fclose((short)fh);
    fh = -1;
    tt_free(arena);
    tt_free(spr_index);
    arena = 0;
    spr_index = 0;
    free_list = 0;
    arena_used = 0;
    memset(blob, 0, sizeof blob);
    memset(pic, 0, sizeof pic);
}

int spr_open(int n, int32_t bytes) {
    char name[16];
    uint8_t head[8];
    spr_close();
    sprintf(name, "WORLD%d.SPR", n);
    fh = data_open(name);
    if (fh < 0 || Fread((short)fh, 8, head) != 8 || memcmp(head, "OAS3", 4)) {
        spr_close();
        return 0;
    }
    ntypes = rd16(head + 4);
    spr_index = tt_alloc_any(ntypes * 12L);
    arena = tt_alloc_any(bytes);
    if (!spr_index || !arena || Fread((short)fh, ntypes * 12L, spr_index) != ntypes * 12L) {
        spr_close();
        return 0;
    }
    arena_size = bytes & ~7L;
    free_list = (Free*)arena;
    free_list->size = arena_size;
    free_list->next = 0;
    arena_used = 0;
    return 1;
}

void spr_frame(void) {
    frame_no++;
}

/* The unit types the map needs: a unit of the type alive, or a wreck of it */
static uint8_t used[MAX_UNITTYPES];

static void count_used(void) {
    int i;
    memset(used, 0, sizeof used);
    for (i = 0; i < unit_hi; i++)
        if (units[i].alive)
            used[units[i].type] = 1;
    for (i = 0; i < nfeats; i++)
        if (feats[i].alive && feats[i].type >= mission.nftypes)
            used[feats[i].type - mission.nftypes] = 1;
}

/* Gives back the memory of what was seen longest ago, not in this frame:
 * first what nothing on the map needs (count_used()), then what is only
 * off the screen, to be read again when it comes into view */
static int evict(void) {
    uint32_t oldest = 0xFFFFFFFFUL;
    int i, pass, best = -1, is_pic = 0;
    for (pass = 0; pass < 2 && best < 0; pass++)
        for (i = 0; i < ntypes; i++) {
            if (blob[i] && used[i] == pass && seen_blob[i] < oldest && seen_blob[i] != frame_no) {
                oldest = seen_blob[i];
                best = i;
                is_pic = 0;
            }
            if (pic[i] && seen_pic[i] < oldest && seen_pic[i] != frame_no) {
                oldest = seen_pic[i];
                best = i;
                is_pic = 1;
            }
        }
    if (best < 0)
        return 0;
    if (is_pic) {
        arena_free(pic[best]);
        pic[best] = 0;
    } else {
        arena_free(blob[best]);
        blob[best] = 0;
    }
    return 1;
}

/* n bytes of the block, making room when needed; null when it cannot */
static void* take(int32_t n) {
    void* p = arena_alloc(n);
    if (p)
        return p;
    count_used();
    while (!(p = arena_alloc(n)))
        if (!evict())
            return 0;
    return p;
}

static int read_at(uint32_t ofs, int32_t n, void* dst) {
    return Fseek((long)ofs, (short)fh, 0) == (long)ofs && data_read(fh, n, dst) == n;
}

int spr_load(int type) {
    const uint32_t* ix;
    uint8_t* b;
    if (blob[type])
        return 1;
    if (fh < 0 || type >= ntypes)
        return 0;
    ix = spr_index + type * 3;
    if (!ix[2] || !(b = take((int32_t)ix[2])))
        return 0;
    if (!read_at(ix[1], (int32_t)ix[2], b)) {
        arena_free(b);
        return 0;
    }
    blob[type] = b;
    seen_blob[type] = frame_no;
    return 1;
}

const int16_t* unit_frame(int type, int dir) {
    const uint32_t* t;
    if (!blob[type] && !spr_load(type))
        return empty;
    seen_blob[type] = frame_no;
    t = (const uint32_t*)blob[type];
    return (const int16_t*)(blob[type] + t[dir]);
}

const int16_t* unit_wreck(int type) {
    const uint32_t* t;
    uint32_t o;
    if (!blob[type] && !spr_load(type))
        return empty;
    seen_blob[type] = frame_no;
    t = (const uint32_t*)blob[type];
    o = t[unittypes[type].ndirs];
    return o ? (const int16_t*)(blob[type] + o) : empty;
}

const uint16_t* unit_pic(int type) {
    const uint32_t* ix;
    uint16_t* p;
    if (!pic[type]) {
        if (fh < 0 || type >= ntypes)
            return 0;
        ix = spr_index + type * 3;
        if (!ix[0] || !(p = take(2048)))
            return 0;
        if (!read_at(ix[0], 2048, p)) {
            arena_free(p);
            return 0;
        }
        pic[type] = p;
    }
    seen_pic[type] = frame_no;
    return pic[type];
}

int spr_stats(int32_t* used, int32_t* size) {
    int i, n = 0;
    for (i = 0; i < ntypes; i++)
        n += blob[i] != 0;
    *used = arena_used;
    *size = arena_size;
    return n;
}
