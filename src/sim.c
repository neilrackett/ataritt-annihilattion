/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The battle (see sim.h).
 */
#include <string.h>
#include <stdio.h>
#include "sim.h"
#include "snd.h"
#include "trig.h"

Unit units[MAX_UNITS];
Player players[2];
Projectile projs[MAX_PROJ];
Effect fx[MAX_FX];
Beam beams[MAX_BEAMS];
Feature feats[MAX_FEATS];
int nfeats;
int32_t feat_version;
int32_t sim_tick_count;
int sim_result;
int wind_now;
int unit_hi;
int feat_dirty_x0, feat_dirty_y0, feat_dirty_x1 = -1, feat_dirty_y1 = -1;
int view_x0, view_y0, view_x1, view_y1;
char sim_message[64];
int32_t sim_message_tick = -1000;

#define BUCKET 64
#define MAXB (48 * 48)
#define MAXP (96 * 96)
#define PATH_NODES 2200

static int bw, bh; /* buckets across and down */
static int16_t bucket[MAXB];
static uint8_t seen[MAXB];
static int pw, ph; /* path cells (32 pixels) */
static uint8_t pblock[MAXP];
static uint8_t cell_x[MAXP], cell_z[MAXP]; /* each path cell's column and row */
static int16_t occ[200 * 200];             /* per 16-pixel cell: the building on it + 1, or 0 */
static int path_rr;
static int kills_of_type[MAX_UNITTYPES];
static int had_commander;
static int32_t next_wave;
static int32_t last_warning = -1000;
static uint32_t rnd = 12345;

static int rand16(void) {
    rnd = rnd * 1103515245UL + 12345UL;
    return (int)((rnd >> 16) & 0x7FFF);
}

static uint32_t isqrt(uint32_t v) {
    uint32_t r = 0, b = 1UL << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

static int dist(int dx, int dz) {
    if (dx < 0)
        dx = -dx;
    if (dz < 0)
        dz = -dz;
    if (dx > 4000 || dz > 4000)
        return 8000;
    return (int)isqrt((uint32_t)(dx * dx + dz * dz));
}

/* dist(dx, dz) < r, without the square root */
static int within(int dx, int dz, int r) {
    if (r <= 0)
        return 0;
    if (dx < -4000 || dx > 4000 || dz < -4000 || dz > 4000)
        return 8000 < r;
    return (int32_t)dx * dx + (int32_t)dz * dz < (int32_t)r * r;
}

/* The angle whose sine and cosine go as s and c */
static uint16_t angle_of(int s, int c) {
    int as = s < 0 ? -s : s, ac = c < 0 ? -c : c;
    int a;
    if (!as && !ac)
        return 0;
    if (as <= ac)
        a = atantab[(int32_t)as * 256 / ac];
    else
        a = 16384 - atantab[(int32_t)ac * 256 / as];
    if (s >= 0 && c >= 0)
        return (uint16_t)a;
    if (s >= 0)
        return (uint16_t)(32768 - a);
    if (c < 0)
        return (uint16_t)(32768 + a);
    return (uint16_t)(65536 - a);
}

uint16_t heading_to(int dx, int dz) {
    return angle_of(-dx, dz);
}

int ground_height(int x, int z) {
    int cx = x >> 4, cz = z >> 4;
    if (cx < 0)
        cx = 0;
    if (cz < 0)
        cz = 0;
    if (cx >= mission.w)
        cx = mission.w - 1;
    if (cz >= mission.h)
        cz = mission.h - 1;
    return mission.height[cz * mission.w + cx];
}

int unit_screen_y(const Unit* u) {
    return ground_screen_y(u->x >> FIX, u->z >> FIX);
}

void sim_sound_at(int sound, int x, int z) {
    if (x >= view_x0 - 64 && x <= view_x1 + 64 && z >= view_y0 - 64 && z <= view_y1 + 96)
        snd_play(sound, 1);
}

static void message(const char* s) {
    strncpy(sim_message, s, sizeof sim_message - 1);
    sim_message_tick = sim_tick_count;
}

/* ------------------------------------------------------------ the map */

/* A path cell (32 pixels) is closed to a movement class when two of its four
 * map cells are, or a building stands on any of them; units move by these
 * cells too, so they never meet a wall the path search did not know of. */
static void update_pcell(int px, int pz) {
    int i, j, cls, b = 0, built = 0;
    if (px < 0 || pz < 0 || px >= pw || pz >= ph)
        return;
    for (j = 0; j < 2; j++)
        for (i = 0; i < 2; i++) {
            int cx = px * 2 + i, cz = pz * 2 + j;
            if (cx < mission.w && cz < mission.h && occ[cz * mission.w + cx])
                built = 1;
        }
    for (cls = 0; cls < nclasses; cls++) {
        int n = 0;
        for (j = 0; j < 2; j++)
            for (i = 0; i < 2; i++) {
                int cx = px * 2 + i, cz = pz * 2 + j;
                if (cx >= mission.w || cz >= mission.h ||
                    (mission.flags[cz * mission.w + cx] >> cls & 1))
                    n++;
            }
        if (n >= 2 || built)
            b |= 1 << cls;
    }
    pblock[pz * pw + px] = (uint8_t)b;
}

static int pcell_blocked(int mask, int x, int z) {
    int px = x >> 5, pz = z >> 5;
    if (px < 0 || pz < 0 || px >= pw || pz >= ph)
        return 1;
    return pblock[pz * pw + px] & mask;
}

/* The first map cell across and down that a building centred at x, z covers */
static int foot_x0(const UnitType* t, int x) {
    return (x - t->footx * 8 + 8) >> 4;
}

static int foot_z0(const UnitType* t, int z) {
    return (z - t->footz * 8 + 8) >> 4;
}

static void set_occupancy(int idx, int on) {
    Unit* u = &units[idx];
    const UnitType* t = &unittypes[u->type];
    int x0 = foot_x0(t, u->x >> FIX), z0 = foot_z0(t, u->z >> FIX);
    int i, j;
    for (j = 0; j < t->footz; j++)
        for (i = 0; i < t->footx; i++) {
            int cx = x0 + i, cz = z0 + j;
            if (cx < 0 || cz < 0 || cx >= mission.w || cz >= mission.h)
                continue;
            if (j < 8 && i < 8 && (t->yard[j] & (0x80 >> i)))
                continue;
            occ[cz * mission.w + cx] = (int16_t)(on ? idx + 1 : 0);
        }
    for (j = (z0 >> 1) - 1; j <= ((z0 + t->footz) >> 1) + 1; j++)
        for (i = (x0 >> 1) - 1; i <= ((x0 + t->footx) >> 1) + 1; i++)
            update_pcell(i, j);
}

int snap_build_x(int type, int x) {
    const UnitType* t = &unittypes[type];
    return (foot_x0(t, x) << 4) + t->footx * 8;
}

int snap_build_z(int type, int z) {
    const UnitType* t = &unittypes[type];
    return (foot_z0(t, z) << 4) + t->footz * 8;
}

/* Buildings stand where the first movement class may go (bit 0 of a cell's flags) */
int can_build_at(int type, int x, int z) {
    const UnitType* t = &unittypes[type];
    int x0 = (x - t->footx * 8) >> 4, z0 = (z - t->footz * 8) >> 4;
    int i, j;
    for (j = 0; j < t->footz; j++)
        for (i = 0; i < t->footx; i++) {
            int cx = x0 + i, cz = z0 + j, c;
            if (cx < 0 || cz < 0 || cx >= mission.w || cz >= mission.h)
                return 0;
            c = cz * mission.w + cx;
            if (occ[c] || (mission.flags[c] & (CELL_WATER | CELL_ROCK | 1)))
                return 0;
        }
    return 1;
}

/* ------------------------------------------------------------ path search */

static uint16_t gcost[MAXP], stampv[MAXP], closedv[MAXP];
static uint8_t from[MAXP];
static uint32_t heap[PATH_NODES * 8 + 16];
static int heapn;
static uint16_t cur_stamp;

static void heap_push(uint32_t v) {
    int i = heapn++;
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (heap[p] <= v)
            break;
        heap[i] = heap[p];
        i = p;
    }
    heap[i] = v;
}

static uint32_t heap_pop(void) {
    uint32_t top = heap[0], v = heap[--heapn];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= heapn)
            break;
        if (c + 1 < heapn && heap[c + 1] < heap[c])
            c++;
        if (heap[c] >= v)
            break;
        heap[i] = heap[c];
        i = c;
    }
    heap[i] = v;
    return top;
}

static const int8_t ddx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
static const int8_t ddz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
static const uint8_t dcost[8] = {10, 10, 10, 10, 14, 14, 14, 14};

static int heur(int x, int z, int gx, int gz) {
    int dx = x > gx ? x - gx : gx - x, dz = z > gz ? z - gz : gz - z;
    return dx > dz ? 10 * dx + 4 * dz : 10 * dz + 4 * dx;
}

static void find_path(Unit* u) {
    const UnitType* t = &unittypes[u->type];
    int cls = t->moveclass, mask = 1 << cls;
    int sx = (u->x >> FIX) >> 5, sz = (u->z >> FIX) >> 5;
    int gx = u->goalx >> 5, gz = u->goalz >> 5;
    int start, goal, best, besth, n = 0, i, d;
    int doff[8]; /* a step's change of cell index */
    uint16_t tmp[256];
    int nt = 0;
    u->want_path = 0;
    u->npath = u->pathi = 0;
    if (sx < 0 || sz < 0 || sx >= pw || sz >= ph)
        return;
    if (gx < 0)
        gx = 0;
    if (gz < 0)
        gz = 0;
    if (gx >= pw)
        gx = pw - 1;
    if (gz >= ph)
        gz = ph - 1;
    start = sz * pw + sx;
    goal = gz * pw + gx;
    for (d = 0; d < 8; d++)
        doff[d] = ddz[d] * pw + ddx[d];
    if (start == goal)
        return;
    if (++cur_stamp == 0) {
        memset(stampv, 0, sizeof stampv);
        memset(closedv, 0, sizeof closedv);
        cur_stamp = 1;
    }
    heapn = 0;
    gcost[start] = 0;
    stampv[start] = cur_stamp;
    from[start] = 255;
    best = start;
    besth = heur(sx, sz, gx, gz);
    heap_push((uint32_t)besth << 16 | (uint32_t)start);
    while (heapn && n < PATH_NODES) {
        uint32_t v = heap_pop();
        int c = (int)(v & 0xFFFF), cx = cell_x[c], cz = cell_z[c];
        if (closedv[c] == cur_stamp)
            continue;
        closedv[c] = cur_stamp;
        n++;
        if (c == goal) {
            best = c;
            break;
        }
        {
            int h = heur(cx, cz, gx, gz);
            if (h < besth) {
                besth = h;
                best = c;
            }
        }
        for (d = 0; d < 8; d++) {
            int nx = cx + ddx[d], nz = cz + ddz[d], nc, g;
            if (nx < 0 || nz < 0 || nx >= pw || nz >= ph)
                continue;
            nc = c + doff[d];
            if ((pblock[nc] & mask) && nc != goal)
                continue;
            if (d >= 4 && ((pblock[c + ddx[d]] & mask) || (pblock[c + doff[d] - ddx[d]] & mask)))
                continue;
            if (closedv[nc] == cur_stamp)
                continue;
            g = gcost[c] + dcost[d];
            if (stampv[nc] == cur_stamp && gcost[nc] <= g)
                continue;
            stampv[nc] = cur_stamp;
            gcost[nc] = (uint16_t)g;
            from[nc] = (uint8_t)d;
            if (heapn < (int)(sizeof heap / sizeof heap[0]) - 1)
                heap_push((uint32_t)(g + heur(nx, nz, gx, gz)) << 16 | (uint32_t)nc);
        }
    }
    /* walk back from the best cell, keeping the cells where the way turns */
    {
        int c = best, lastd = -1;
        while (c != start && nt < 256) {
            int d = from[c];
            if (d == 255)
                break;
            if (d != lastd) {
                tmp[nt++] = (uint16_t)c;
                lastd = d;
            }
            c -= doff[d];
        }
    }
    /* tmp holds the turns from the end back; the unit wants them from its end */
    for (i = 0; i < nt && i < MAX_PATH; i++)
        u->path[i] = tmp[nt - 1 - i];
    u->npath = (uint8_t)(nt < MAX_PATH ? nt : MAX_PATH);
    if (best != goal) {
        /* the goal cannot be reached: end at the nearest place */
        if (u->npath) {
            int c = u->path[u->npath - 1];
            u->goalx = (int16_t)(cell_x[c] * 32 + 16);
            u->goalz = (int16_t)(cell_z[c] * 32 + 16);
        } else {
            u->has_goal = 0;
        }
    }
}

/* ------------------------------------------------------------ buckets and sight */

static void build_buckets(void) {
    int i;
    for (i = 0; i < bw * bh; i++)
        bucket[i] = -1;
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        int bx, bz, b;
        if (!u->alive)
            continue;
        bx = (u->x >> FIX) / BUCKET;
        bz = (u->z >> FIX) / BUCKET;
        if (bx < 0)
            bx = 0;
        if (bz < 0)
            bz = 0;
        if (bx >= bw)
            bx = bw - 1;
        if (bz >= bh)
            bz = bh - 1;
        b = bz * bw + bx;
        u->next = bucket[b];
        bucket[b] = (int16_t)i;
    }
}

/* The buckets x0..x1, z0..z1 a circle of radius r round x, z touches */
static void bucket_range(int x, int z, int r, int* x0, int* x1, int* z0, int* z1) {
    *x0 = (x - r) / BUCKET;
    *x1 = (x + r) / BUCKET;
    *z0 = (z - r) / BUCKET;
    *z1 = (z + r) / BUCKET;
    if (*x0 < 0)
        *x0 = 0;
    if (*z0 < 0)
        *z0 = 0;
    if (*x1 >= bw)
        *x1 = bw - 1;
    if (*z1 >= bh)
        *z1 = bh - 1;
}

/* A player's unit sees the buckets within its sight: a disc, a row at a time */
static void update_sight(void) {
    int i;
    memset(seen, 0, (size_t)(bw * bh));
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        int bx, bz, r, dz;
        if (!u->alive || u->owner != PLAYER || (u->flags & UF_UNBUILT))
            continue;
        r = (unittypes[u->type].sight + BUCKET / 2) / BUCKET;
        bx = (u->x >> FIX) / BUCKET;
        bz = (u->z >> FIX) / BUCKET;
        for (dz = -r; dz <= r; dz++) {
            int z = bz + dz, w = r, reach = r * r + r - dz * dz, x0, x1;
            if (z < 0 || z >= bh)
                continue;
            while (w * w > reach)
                w--;
            x0 = bx - w < 0 ? 0 : bx - w;
            x1 = bx + w >= bw ? bw - 1 : bx + w;
            if (x0 <= x1)
                memset(seen + z * bw + x0, 1, (size_t)(x1 - x0 + 1));
        }
    }
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        int bx, bz;
        if (!u->alive || u->owner == PLAYER)
            continue;
        bx = (u->x >> FIX) / BUCKET;
        bz = (u->z >> FIX) / BUCKET;
        if (bx >= 0 && bz >= 0 && bx < bw && bz < bh && seen[bz * bw + bx])
            u->flags |= UF_SEEN | UF_WAS_SEEN;
        else
            u->flags &= (uint8_t)~UF_SEEN;
        /* what was seen once stays on the map as it was: buildings */
        if (unittypes[u->type].kind == UK_MOBILE && !(u->flags & UF_SEEN))
            u->flags &= (uint8_t)~UF_WAS_SEEN;
    }
}

/* ------------------------------------------------------------ units */

static void recount_storage(int p) {
    int i;
    Player* pl = &players[p];
    pl->mstore = pl->base_mstore;
    pl->estore = pl->base_estore;
    pl->units = 0;
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        if (!u->alive || u->owner != p)
            continue;
        pl->units++;
        if (u->flags & UF_UNBUILT)
            continue;
        pl->mstore += unittypes[u->type].mstore;
        pl->estore += unittypes[u->type].estore;
    }
}

static void finish_unit(Unit* u) {
    const UnitType* t = &unittypes[u->type];
    u->flags &= (uint8_t)~UF_UNBUILT;
    u->hp = t->maxhp;
    if (t->flags & UF_MEX) {
        int x0 = foot_x0(t, u->x >> FIX), z0 = foot_z0(t, u->z >> FIX);
        int i, j;
        int32_t sum = 0;
        for (j = 0; j < t->footz; j++)
            for (i = 0; i < t->footx; i++) {
                int cx = x0 + i, cz = z0 + j;
                if (cx >= 0 && cz >= 0 && cx < mission.w && cz < mission.h)
                    sum += mission.metal[cz * mission.w + cx];
            }
        /* extractsmetal * the metal under it, three times over, a second */
        u->mex = (int32_t)((int64_t)t->extract * sum * 3 * 256 / TICKS / 1000000L);
    }
    recount_storage(u->owner);
}

static int spawn_unit(int type, int owner, int x, int z, int unbuilt) {
    int i;
    Unit* u;
    const UnitType* t = &unittypes[type];
    for (i = 0; i < MAX_UNITS; i++)
        if (!units[i].alive)
            break;
    if (i == MAX_UNITS)
        return -1;
    if (i >= unit_hi)
        unit_hi = i + 1;
    u = &units[i];
    memset(u, 0, sizeof *u);
    u->alive = 1;
    u->type = (uint8_t)type;
    u->owner = (uint8_t)owner;
    u->x = (int32_t)x << FIX;
    u->z = (int32_t)z << FIX;
    u->target = -1;
    u->nano = -1;
    u->attacker = -1;
    u->built_by = -1;
    u->firestate = 2;
    u->movestate = 1;
    u->homex = (int16_t)x;
    u->homez = (int16_t)z;
    u->attacked = -10000;
    if (unbuilt) {
        u->flags |= UF_UNBUILT;
        u->hp = 1;
    } else {
        u->hp = t->maxhp;
    }
    if (t->kind != UK_MOBILE)
        set_occupancy(i, 1);
    if (!unbuilt)
        finish_unit(u);
    else
        recount_storage(owner);
    if ((t->flags & UF_COMMANDER) && owner == PLAYER)
        had_commander = 1;
    return i;
}

static void add_effect(int anim, int x, int y) {
    int i;
    if (anim < 0 || anim >= common.nanim || !common.anim[anim].nframes)
        return;
    for (i = 0; i < MAX_FX; i++)
        if (!fx[i].alive) {
            fx[i].alive = 1;
            fx[i].anim = (uint8_t)anim;
            fx[i].frame = 0;
            fx[i].x = (int16_t)x;
            fx[i].y = (int16_t)y;
            return;
        }
}

/* A feature appeared or went at x, z: the screen redraws round it */
static void feature_changed(int x, int z) {
    int y = ground_screen_y(x, z);
    feat_version++;
    if (feat_dirty_x0 > feat_dirty_x1) {
        feat_dirty_x0 = feat_dirty_x1 = x;
        feat_dirty_y0 = feat_dirty_y1 = y;
        return;
    }
    if (x < feat_dirty_x0)
        feat_dirty_x0 = x;
    if (x > feat_dirty_x1)
        feat_dirty_x1 = x;
    if (y < feat_dirty_y0)
        feat_dirty_y0 = y;
    if (y > feat_dirty_y1)
        feat_dirty_y1 = y;
}

static void add_feature(int type, int x, int z) {
    int i;
    if (type < 0)
        return;
    for (i = 0; i < nfeats; i++)
        if (!feats[i].alive)
            break;
    if (i == MAX_FEATS)
        return;
    if (i == nfeats)
        nfeats++;
    feats[i].alive = 1;
    feats[i].type = (uint8_t)type;
    feats[i].x = (int16_t)x;
    feats[i].z = (int16_t)z;
    feats[i].metal = (int32_t)common.feat[type].metal;
    feats[i].energy = (int32_t)common.feat[type].energy;
    feature_changed(x, z);
}

static void damage_unit(int idx, int dmg, int attacker, int paralyze);

static void area_damage(int x, int z, int radius, int dmg, int attacker, int direct) {
    int bx0, bx1, bz0, bz1, bx, bz;
    bucket_range(x, z, radius, &bx0, &bx1, &bz0, &bz1);
    if (radius < 1)
        radius = 1;
    for (bz = bz0; bz <= bz1; bz++) {
        for (bx = bx0; bx <= bx1; bx++) {
            int i;
            for (i = bucket[bz * bw + bx]; i >= 0; i = units[i].next) {
                Unit* u = &units[i];
                int dx = (u->x >> FIX) - x, dz = (u->z >> FIX) - z, half, d;
                if (!u->alive || i == direct)
                    continue;
                half = unittypes[u->type].radius / 2;
                if (!within(dx, dz, radius + half))
                    continue;
                d = dist(dx, dz) - half;
                damage_unit(i, d <= 0 ? dmg : dmg * (radius - d) / radius, attacker, 0);
            }
        }
    }
}

static void kill_unit(int idx) {
    Unit* u = &units[idx];
    const UnitType* t = &unittypes[u->type];
    int x = u->x >> FIX, z = u->z >> FIX, sy = unit_screen_y(u);
    int i, j, owner = u->owner, wasunbuilt = u->flags & UF_UNBUILT;
    int anim = ANIM_EXPLOSION, big = t->kind != UK_MOBILE || t->maxhp > 1500;
    if (!u->alive)
        return;
    u->alive = 0;
    if (t->kind != UK_MOBILE)
        set_occupancy(idx, 0);
    players[owner].losses++;
    players[!owner].kills++;
    if (owner == ENEMY)
        kills_of_type[u->type]++;
    if (t->explode >= 0) {
        const WeaponType* w = &weapons[t->explode];
        anim = w->anim;
        if (!wasunbuilt)
            area_damage(x, z, w->aoe / 2, w->damage, -1, idx);
        sim_sound_at(w->snd_hit >= 0 ? w->snd_hit : SND_XPLOMED, x, z);
    } else {
        sim_sound_at(big ? SND_XPLOBLD : SND_XPLOMED, x, z);
    }
    add_effect(anim >= 0 ? anim : ANIM_EXPLOSION, x, sy - t->top / 3);
    if (big)
        add_effect(ANIM_EXPLOSION, x + 8, sy - t->top / 2);
    if (!wasunbuilt)
        add_feature(t->corpse, x, z);
    /* no unit may still point at this one */
    for (i = 0; i < unit_hi; i++) {
        Unit* v = &units[i];
        if (!v->alive)
            continue;
        if (v->target == idx)
            v->target = -1;
        if (v->nano == idx)
            v->nano = -1;
        if (v->attacker == idx)
            v->attacker = -1;
        for (j = 0; j < v->norders; j++)
            if ((v->order[j].kind == O_ATTACK || v->order[j].kind == O_GUARD) &&
                v->order[j].target == idx)
                v->order[j].kind = O_NONE;
    }
    for (i = 0; i < MAX_PROJ; i++)
        if (projs[i].alive && projs[i].target == idx)
            projs[i].target = -1;
    recount_storage(owner);
    if (owner == PLAYER && (t->flags & UF_COMMANDER))
        message("Commander destroyed!");
}

void sim_kill(int idx) {
    kill_unit(idx);
}

static void damage_unit(int idx, int dmg, int attacker, int paralyze) {
    Unit* u = &units[idx];
    if (!u->alive)
        return;
    if (attacker >= 0 && units[attacker].owner == u->owner && !paralyze)
        dmg /= 2; /* friendly splash still hurts, a little */
    if (paralyze) {
        int t = dmg * TICKS / 60;
        if (u->paralyzed < t)
            u->paralyzed = (uint16_t)t;
    } else {
        u->hp -= dmg;
    }
    u->attacked = sim_tick_count;
    if (attacker >= 0 && units[attacker].alive && units[attacker].owner != u->owner)
        u->attacker = (int16_t)attacker;
    if (u->norders && u->order[0].kind == O_WAITATTACK)
        u->order[0].kind = O_NONE;
    if (u->owner == PLAYER && sim_tick_count - last_warning > TICKS * 12 && attacker >= 0) {
        last_warning = sim_tick_count;
        snd_play(SND_WARNING, 2);
        message("Unit under attack");
    }
    if (u->hp <= 0)
        kill_unit(idx);
}

/* ------------------------------------------------------------ orders */

void order_clear(Unit* u) {
    u->norders = 0;
    u->has_goal = 0;
    u->npath = 0;
    u->nano = -1;
}

void order_add(Unit* u, const Order* o, int queue) {
    if (!queue)
        order_clear(u);
    if (u->norders < MAX_ORDERS)
        u->order[u->norders++] = *o;
}

void order_at(Unit* u, int kind, int x, int z, int queue) {
    Order o;
    memset(&o, 0, sizeof o);
    o.kind = (uint8_t)kind;
    o.x = (int16_t)x;
    o.z = (int16_t)z;
    o.target = -1;
    order_add(u, &o, queue);
}

void order_stop(Unit* u) {
    order_clear(u);
    u->target = -1;
    u->speed = 0;
}

/* The queue slot that holds a type, else the first empty one, else -1 */
static int queue_slot(const Unit* u, int type) {
    int i;
    for (i = 0; i < 6; i++)
        if (u->fq_count[i] && u->fq_type[i] == type)
            return i;
    for (i = 0; i < 6; i++)
        if (!u->fq_count[i])
            return i;
    return -1;
}

int factory_queue(Unit* u, int type, int n) {
    int i = queue_slot(u, type);
    if (i < 0)
        return 0;
    if (n < 0) {
        if (u->fq_count[i] && u->fq_type[i] == type)
            u->fq_count[i] = (uint8_t)(u->fq_count[i] > -n ? u->fq_count[i] + n : 0);
    } else {
        u->fq_type[i] = (uint8_t)type;
        u->fq_count[i] = (uint8_t)(u->fq_count[i] + n > 99 ? 99 : u->fq_count[i] + n);
    }
    return 1;
}

int factory_queued(const Unit* u, int type) {
    int i, n = 0;
    for (i = 0; i < 6; i++)
        if (u->fq_count[i] && u->fq_type[i] == type)
            n += u->fq_count[i];
    return n;
}

static void pop_order(Unit* u) {
    int i;
    for (i = 1; i < u->norders; i++)
        u->order[i - 1] = u->order[i];
    if (u->norders)
        u->norders--;
    u->has_goal = 0;
    u->npath = 0;
    u->nano = -1;
}

static void set_goal(Unit* u, int x, int z) {
    int dx = x - u->goalx, dz = z - u->goalz;
    if (u->has_goal && dx > -12 && dx < 12 && dz > -12 && dz < 12)
        return;
    u->goalx = (int16_t)x;
    u->goalz = (int16_t)z;
    u->has_goal = 1;
    u->want_path = 1;
    u->npath = u->pathi = 0;
    u->stuck = 0;
    u->repaths = 0;
}

/* One step along the way; 1 when the goal is reached */
static int move_step(Unit* u) {
    const UnitType* t = &unittypes[u->type];
    int x = u->x >> FIX, z = u->z >> FIX, tx, tz, dx, dz, d, final;
    int16_t diff;
    uint16_t want;
    int32_t nx, nz;
    int cls = t->moveclass;
    if (!u->has_goal || t->kind != UK_MOBILE || !t->speed)
        return 1;
    if (u->pathi < u->npath) {
        int c = u->path[u->pathi];
        tx = cell_x[c] * 32 + 16;
        tz = cell_z[c] * 32 + 16;
        final = 0;
    } else {
        tx = u->goalx;
        tz = u->goalz;
        final = 1;
    }
    dx = tx - x;
    dz = tz - z;
    d = dist(dx, dz);
    if (!final && d < 20) {
        u->pathi++;
        return 0;
    }
    if (final && d <= 6) {
        if (u->npath >= MAX_PATH && u->want_path == 0 && dist(u->goalx - x, u->goalz - z) > 24) {
            u->want_path = 1;
            return 0;
        }
        u->has_goal = 0;
        u->speed = 0;
        return 1;
    }
    want = heading_to(dx, dz);
    diff = (int16_t)(want - u->heading);
    if (diff > (int16_t)t->turn)
        u->heading = (uint16_t)(u->heading + t->turn);
    else if (diff < -(int16_t)t->turn)
        u->heading = (uint16_t)(u->heading - t->turn);
    else
        u->heading = want;
    if (diff > 0x2400 || diff < -0x2400) {
        u->speed -= (int16_t)(t->accel * 2);
        if (u->speed < 0)
            u->speed = 0;
    } else {
        u->speed += (int16_t)t->accel;
        if (u->speed > (int16_t)t->speed)
            u->speed = (int16_t)t->speed;
        if (final && d < 24 && u->speed > (int16_t)(t->speed / 2))
            u->speed = (int16_t)(t->speed / 2);
    }
    if (!u->speed)
        return 0;
    nx = u->x - (((int32_t)SIN(u->heading) * u->speed) >> 14);
    nz = u->z + (((int32_t)COS(u->heading) * u->speed) >> 14);
    {
        int mask = 1 << cls;
        int nxp = (int)(nx >> FIX), nzp = (int)(nz >> FIX);
        if (((nxp >> 5) != (x >> 5) || (nzp >> 5) != (z >> 5)) && pcell_blocked(mask, nxp, nzp) &&
            !pcell_blocked(mask, x, z)) {
            /* slide along whichever axis is open */
            if (!pcell_blocked(mask, nxp, z))
                nz = u->z;
            else if (!pcell_blocked(mask, x, nzp))
                nx = u->x;
            else {
                u->speed = 0;
                if (++u->stuck > 10) {
                    u->stuck = 0;
                    u->want_path = 1;
                    /* after a few searches that lead nowhere, give the place up */
                    if ((final && d < 48) || ++u->repaths > 4) {
                        u->has_goal = 0;
                        u->repaths = 0;
                        return 1;
                    }
                }
                return 0;
            }
        }
    }
    u->x = nx;
    u->z = nz;
    return 0;
}

/* v's edge is within range of u */
static int in_range(const Unit* u, const Unit* v, int range) {
    return within(
        (v->x - u->x) >> FIX, (v->z - u->z) >> FIX, range + unittypes[v->type].radius / 2 + 1
    );
}

/* The reach of the weapons that fire by themselves */
static int max_range(const UnitType* t) {
    int i, r = 0;
    for (i = 0; i < 3; i++)
        if (t->weapon[i] >= 0) {
            const WeaponType* w = &weapons[t->weapon[i]];
            if (!(w->flags & WF_MANUAL) && w->range > r)
                r = w->range;
        }
    return r;
}

int manual_weapon(const UnitType* t) {
    int i;
    for (i = 0; i < 3; i++)
        if (t->weapon[i] >= 0 && (weapons[t->weapon[i]].flags & WF_MANUAL))
            return i;
    return -1;
}

/* The nearest enemy within radius, or -1 */
static int nearest_enemy(const Unit* u, int radius, int type) {
    int x = u->x >> FIX, z = u->z >> FIX;
    int bx0, bx1, bz0, bz1, bx, bz, best = -1, bestd = radius + 1;
    bucket_range(x, z, radius, &bx0, &bx1, &bz0, &bz1);
    for (bz = bz0; bz <= bz1; bz++) {
        for (bx = bx0; bx <= bx1; bx++) {
            int i;
            for (i = bucket[bz * bw + bx]; i >= 0; i = units[i].next) {
                const Unit* v = &units[i];
                int dx, dz;
                if (!v->alive || v->owner == u->owner)
                    continue;
                if (type >= 0 && v->type != type)
                    continue;
                if (u->owner == PLAYER && !(v->flags & UF_SEEN))
                    continue;
                dx = (v->x >> FIX) - x;
                dz = (v->z >> FIX) - z;
                if (within(dx, dz, bestd)) {
                    bestd = dist(dx, dz);
                    best = i;
                }
            }
        }
    }
    return best;
}

static void fire(Unit* u, int wi, int tx, int tz, int target) {
    const UnitType* t = &unittypes[u->type];
    const WeaponType* w = &weapons[t->weapon[wi]];
    int x = u->x >> FIX, z = u->z >> FIX, sy = unit_screen_y(u) - t->top / 2;
    int i;
    sim_sound_at(w->snd_start, x, z);
    if (target >= 0)
        u->heading = heading_to(tx - x, tz - z);
    if (w->kind == WK_BEAM) {
        int ty = ground_screen_y(tx, tz);
        if (target >= 0) {
            const Unit* v = &units[target];
            ty = unit_screen_y(v) - unittypes[v->type].top / 3;
            damage_unit(target, w->damage, (int)(u - units), w->flags & WF_PARALYZE);
        }
        for (i = 0; i < MAX_BEAMS; i++)
            if (!beams[i].alive) {
                beams[i].alive = 1;
                beams[i].colour = w->colour;
                beams[i].ticks = 2;
                beams[i].x0 = (int16_t)x;
                beams[i].y0 = (int16_t)sy;
                beams[i].x1 = (int16_t)tx;
                beams[i].y1 = (int16_t)ty;
                break;
            }
        if (target >= 0 && !(rand16() & 3))
            add_effect(w->anim, tx, ty);
        return;
    }
    for (i = 0; i < MAX_PROJ; i++)
        if (!projs[i].alive)
            break;
    if (i == MAX_PROJ)
        return;
    {
        Projectile* p = &projs[i];
        int dx = tx - x, dz = tz - z, d = dist(dx, dz), ticks;
        int sp = w->speed ? w->speed : 256;
        if (w->kind == WK_SHOT || w->kind == WK_CANNON) {
            /* a little spread, as TA's sprayed shells have */
            dx += (rand16() & 7) - 4;
            dz += (rand16() & 7) - 4;
        }
        ticks = (d << 8) / sp;
        if (ticks < 1)
            ticks = 1;
        if (ticks > 250)
            ticks = 250;
        p->alive = 1;
        p->weapon = (uint8_t)t->weapon[wi];
        p->owner = (uint8_t)(u - units);
        p->ticks = (uint8_t)ticks;
        p->x = u->x;
        p->z = u->z;
        p->vx = ((int32_t)dx << FIX) / ticks;
        p->vz = ((int32_t)dz << FIX) / ticks;
        p->y = (int16_t)((t->top / 2) << 8);
        p->vy = (w->flags & WF_BALLISTIC) ? (int16_t)(ticks * 48) : 0;
        p->target = (int16_t)target;
        if (w->kind == WK_DGUN) {
            /* the D-gun goes on to its range whatever it meets */
            int r = w->range;
            p->ticks = (uint8_t)((r << 8) / sp);
            p->vx = d ? ((int32_t)dx << FIX) * sp / 256 / d : 0;
            p->vz = d ? ((int32_t)dz << FIX) * sp / 256 / d : 0;
            p->vy = 0;
        }
    }
}

static void weapons_step(Unit* u, int idx) {
    const UnitType* t = &unittypes[u->type];
    int i;
    if (!(t->flags & UF_ARMED))
        return;
    for (i = 0; i < 3; i++)
        if (u->reload[i])
            u->reload[i]--;
    /* look for something to shoot every fourth tick */
    if (((sim_tick_count + idx) & 3) == 0) {
        int keep = u->target >= 0 && units[u->target].alive && units[u->target].owner != u->owner &&
                   in_range(u, &units[u->target], max_range(t) + 16);
        int forced = u->norders && u->order[0].kind == O_ATTACK;
        if (!keep && !forced) {
            u->target = -1;
            if (u->firestate == 2)
                u->target = (int16_t)nearest_enemy(u, max_range(t), -1);
            else if (
                u->firestate == 1 && u->attacker >= 0 && units[u->attacker].alive &&
                in_range(u, &units[u->attacker], max_range(t))
            )
                u->target = u->attacker;
        }
    }
    if (u->target < 0 || !units[u->target].alive)
        return;
    for (i = 0; i < 3; i++) {
        const WeaponType* w;
        const Unit* v = &units[u->target];
        if (t->weapon[i] < 0)
            continue;
        w = &weapons[t->weapon[i]];
        if (w->flags & WF_MANUAL)
            continue;
        if (u->reload[i] || !in_range(u, v, w->range))
            continue;
        fire(u, i, v->x >> FIX, v->z >> FIX, u->target);
        if (w->burst > 1) {
            if (u->burst[i] == 0)
                u->burst[i] = (uint8_t)(w->burst - 1);
            else
                u->burst[i]--;
            u->reload[i] = u->burst[i] ? w->gap : (uint8_t)w->reload;
        } else {
            u->reload[i] = (uint8_t)w->reload;
        }
    }
}

/* Spend toward building: returns 1 when the work this tick was paid for */
static int pay_work(int owner, const UnitType* built, int worker) {
    /* the cost of a tick's work, kept for the last type and worker asked about */
    static const UnitType* last_built;
    static int last_worker;
    static int32_t m, e;
    Player* p = &players[owner];
    if (built != last_built || worker != last_worker) {
        int64_t ticks = (int64_t)built->buildtime * TICKS;
        last_built = built;
        last_worker = worker;
        m = (int32_t)((int64_t)built->metalcost * worker * 256 / ticks);
        e = (int32_t)((int64_t)built->energycost * worker * 256 / ticks);
    }
    if (p->metal < m || p->energy < e)
        return 0;
    p->metal -= m;
    p->energy -= e;
    return 1;
}

/* A builder's work on an unbuilt unit; 1 when it is finished */
static int build_work(Unit* b, int target) {
    Unit* v = &units[target];
    const UnitType* vt = &unittypes[v->type];
    int worker = unittypes[b->type].workertime;
    uint32_t need;
    if (!worker)
        worker = 100;
    if (!(v->flags & UF_UNBUILT))
        return 1;
    if (!pay_work(b->owner, vt, worker))
        return 0;
    v->progress += (uint32_t)worker;
    need = vt->buildtime * TICKS;
    v->hp = (int32_t)((int64_t)vt->maxhp * v->progress / need);
    if (v->hp < 1)
        v->hp = 1;
    if (v->progress >= need) {
        finish_unit(v);
        if (v->owner == PLAYER) {
            snd_play(SND_UNITDONE, 2);
        }
        return 1;
    }
    return 0;
}

static void builder_order(Unit* u, Order* o) {
    const UnitType* t = &unittypes[u->type];
    const UnitType* bt = &unittypes[o->a];
    int x = u->x >> FIX, z = u->z >> FIX;
    int reach = t->builddist + (bt->footx > bt->footz ? bt->footx : bt->footz) * 8 + 8;
    int d = dist(o->x - x, o->z - z);
    if (u->nano >= 0 && units[u->nano].alive && (units[u->nano].flags & UF_UNBUILT)) {
        if (d > reach + 8) {
            set_goal(u, o->x, o->z);
            move_step(u);
            return;
        }
        u->has_goal = 0;
        u->speed = 0;
        u->heading = heading_to(o->x - x, o->z - z);
        if (build_work(u, u->nano))
            pop_order(u);
        return;
    }
    if (u->nano >= 0) {
        pop_order(u); /* finished, or lost */
        return;
    }
    if (d > reach) {
        set_goal(u, o->x, o->z);
        if (move_step(u) && d > reach + 32) {
            /* cannot get there */
            pop_order(u);
            if (u->owner == PLAYER)
                snd_play(SND_CANTDO, 1);
        }
        return;
    }
    u->has_goal = 0;
    u->speed = 0;
    if (can_build_at(o->a, o->x, o->z)) {
        int n = spawn_unit(o->a, u->owner, o->x, o->z, 1);
        if (n < 0) {
            pop_order(u);
            return;
        }
        units[n].built_by = (int16_t)(u - units);
        u->nano = (int16_t)n;
        return;
    }
    /* another builder's frame of the same kind there: help it */
    {
        int c = ((o->z >> 4) * mission.w + (o->x >> 4)), other = occ[c] - 1;
        if (other >= 0 && units[other].alive && units[other].type == o->a &&
            (units[other].flags & UF_UNBUILT) && units[other].owner == u->owner) {
            u->nano = (int16_t)other;
            return;
        }
    }
    pop_order(u);
    if (u->owner == PLAYER)
        snd_play(SND_CANTDO, 1);
}

static void factory_step(Unit* u, int idx) {
    const UnitType* t = &unittypes[u->type];
    if (u->nano >= 0 && units[u->nano].alive && (units[u->nano].flags & UF_UNBUILT)) {
        Unit* v = &units[u->nano];
        if (build_work(u, u->nano)) {
            int ex = (int)(u->x >> FIX), ez = (int)(u->z >> FIX) + t->footz * 8 + 24;
            order_at(v, O_MOVE, ex, ez, 0);
            v->homex = (int16_t)ex;
            v->homez = (int16_t)ez;
            u->nano = -1;
            if (u->fq_count[0])
                u->fq_count[0]--;
            if (!u->fq_count[0]) {
                int i;
                for (i = 1; i < 6; i++) {
                    u->fq_type[i - 1] = u->fq_type[i];
                    u->fq_count[i - 1] = u->fq_count[i];
                }
                u->fq_count[5] = 0;
            }
        }
        return;
    }
    u->nano = -1;
    if (u->fq_count[0]) {
        int n = spawn_unit(u->fq_type[0], u->owner, (int)(u->x >> FIX), (int)(u->z >> FIX), 1);
        if (n >= 0) {
            units[n].built_by = (int16_t)idx;
            u->nano = (int16_t)n;
        }
    }
}

static void reclaim_order(Unit* u, Order* o) {
    Feature* f;
    int x = u->x >> FIX, z = u->z >> FIX, d, worker, need;
    if (o->target < 0 || o->target >= nfeats || !feats[o->target].alive) {
        pop_order(u);
        return;
    }
    f = &feats[o->target];
    d = dist(f->x - x, f->z - z);
    if (d > unittypes[u->type].builddist + 24) {
        set_goal(u, f->x, f->z);
        if (move_step(u) && d > unittypes[u->type].builddist + 56)
            pop_order(u);
        return;
    }
    u->has_goal = 0;
    u->speed = 0;
    u->nano = (int16_t)~o->target;
    worker = unittypes[u->type].workertime;
    if (worker < 50)
        worker = 50;
    need = (int)((f->metal + f->energy / 4) * 2 * TICKS / worker) + TICKS;
    if (++u->reclaim >= need) {
        Player* p = &players[u->owner];
        p->metal += f->metal << 8;
        p->energy += f->energy << 8;
        f->alive = 0;
        feature_changed(f->x, f->z);
        u->reclaim = 0;
        pop_order(u);
    }
}

/* Runs the mission script until it queues an order that takes time */
static void run_script(Unit* u) {
    while (u->nscript > 0 && u->norders == 0) {
        const ScriptOp* op = &mission.ops[u->script];
        Order o;
        memset(&o, 0, sizeof o);
        o.target = -1;
        u->script++;
        u->nscript--;
        switch (op->op) {
        case OP_MOVE:
            order_at(u, O_MOVE, op->a, op->b, 1);
            break;
        case OP_PATROL:
            u->homex = (int16_t)(u->x >> FIX);
            u->homez = (int16_t)(u->z >> FIX);
            order_at(u, O_PATROL, op->a, op->b, 1);
            break;
        case OP_WAIT:
            o.kind = O_WAIT;
            o.n = op->a;
            order_add(u, &o, 1);
            break;
        case OP_WAITATTACK:
            o.kind = O_WAITATTACK;
            order_add(u, &o, 1);
            break;
        case OP_ATTACKTYPE:
            o.kind = O_ATTACKTYPE;
            o.a = (uint8_t)op->a;
            order_add(u, &o, 1);
            break;
        case OP_ORDERS:
            u->movestate = (uint8_t)op->a;
            u->firestate = (uint8_t)op->b;
            break;
        case OP_BUILD:
            if (unittypes[u->type].kind == UK_FACTORY)
                factory_queue(u, op->a, op->b > 0 ? op->b : 1);
            break;
        case OP_GUARD:
            o.kind = O_GUARD;
            o.target = (int16_t)op->a;
            order_add(u, &o, 1);
            break;
        case OP_ATTACKPOS:
            order_at(u, O_FIGHT, op->a, op->b, 1);
            break;
        default:
            break;
        }
    }
}

static void unit_step(int idx) {
    Unit* u = &units[idx];
    const UnitType* t = &unittypes[u->type];
    Order* o;
    if (u->flags & UF_UNBUILT)
        return;
    if (u->paralyzed) {
        u->paralyzed--;
        u->speed = 0;
        return;
    }
    if (u->nscript > 0 && u->norders == 0)
        run_script(u);
    if (t->kind == UK_FACTORY)
        factory_step(u, idx);
    weapons_step(u, idx);
    while (u->norders && u->order[0].kind == O_NONE)
        pop_order(u);
    if (!u->norders) {
        /* idle: a unit hit by something out of reach goes after it, then comes home */
        if (t->kind == UK_MOBILE && (t->flags & UF_ARMED) && u->movestate > 0 && u->attacker >= 0 &&
            units[u->attacker].alive && u->target < 0 && sim_tick_count - u->attacked < TICKS * 4) {
            const Unit* a = &units[u->attacker];
            if (dist((a->x >> FIX) - u->homex, (a->z >> FIX) - u->homez) < 500) {
                set_goal(u, a->x >> FIX, a->z >> FIX);
            }
        } else if (u->has_goal && u->target >= 0 && t->kind == UK_MOBILE) {
            u->has_goal = 0;
            u->speed = 0;
        }
        if (u->has_goal)
            move_step(u);
        else
            u->speed = 0;
        return;
    }
    o = &u->order[0];
    switch (o->kind) {
    case O_MOVE:
        set_goal(u, o->x, o->z);
        if (move_step(u))
            pop_order(u);
        break;
    case O_FIGHT:
    case O_PATROL: {
        /* go on, but stop to fight whatever is in sight */
        int seek = u->target >= 0
                       ? -1
                       : (((sim_tick_count + idx) & 7) == 0 ? nearest_enemy(u, t->sight, -1) : -1);
        if (u->target >= 0) {
            u->speed = 0;
            break;
        }
        if (seek >= 0) {
            set_goal(u, units[seek].x >> FIX, units[seek].z >> FIX);
            move_step(u);
            break;
        }
        set_goal(u, o->x, o->z);
        if (move_step(u)) {
            if (o->kind == O_PATROL) {
                int hx = u->homex, hz = u->homez;
                u->homex = o->x;
                u->homez = o->z;
                o->x = (int16_t)hx;
                o->z = (int16_t)hz;
            } else {
                pop_order(u);
            }
        }
        break;
    }
    case O_ATTACK: {
        Unit* v;
        if (o->target < 0 || !units[o->target].alive || units[o->target].owner == u->owner) {
            pop_order(u);
            break;
        }
        v = &units[o->target];
        u->target = o->target;
        if (in_range(u, v, max_range(t) - 8) || !max_range(t)) {
            u->has_goal = 0;
            u->speed = 0;
        } else {
            if (((sim_tick_count + idx) & 15) == 0)
                u->has_goal = 0;
            set_goal(u, v->x >> FIX, v->z >> FIX);
            move_step(u);
        }
        break;
    }
    case O_ATTACKTYPE: {
        int e = nearest_enemy(u, 4000, o->a);
        if (e < 0) {
            pop_order(u);
            break;
        }
        o->kind = O_ATTACK;
        o->target = (int16_t)e;
        break;
    }
    case O_GUARD: {
        Unit* v;
        if (o->target < 0 || !units[o->target].alive) {
            pop_order(u);
            break;
        }
        v = &units[o->target];
        if (v->attacker >= 0 && units[v->attacker].alive && u->target < 0)
            u->target = v->attacker;
        if (dist((v->x - u->x) >> FIX, (v->z - u->z) >> FIX) > 72) {
            set_goal(u, v->x >> FIX, (v->z >> FIX) + 40);
            move_step(u);
        } else {
            u->has_goal = 0;
            u->speed = 0;
        }
        break;
    }
    case O_BUILD:
        if (t->flags & UF_BUILDER && t->kind == UK_MOBILE)
            builder_order(u, o);
        else
            pop_order(u);
        break;
    case O_RECLAIM:
        reclaim_order(u, o);
        break;
    case O_DGUN: {
        int i = manual_weapon(t), x = u->x >> FIX, z = u->z >> FIX;
        const WeaponType* w;
        if (i < 0) {
            pop_order(u);
            break;
        }
        w = &weapons[t->weapon[i]];
        if (dist(o->x - x, o->z - z) > w->range) {
            set_goal(u, o->x, o->z);
            move_step(u);
            break;
        }
        u->has_goal = 0;
        u->speed = 0;
        if (!u->reload[i]) {
            if (players[u->owner].energy >= (int32_t)w->energy << 8) {
                players[u->owner].energy -= (int32_t)w->energy << 8;
                fire(u, i, o->x, o->z, -1);
                u->reload[i] = (uint8_t)w->reload;
            } else if (u->owner == PLAYER) {
                snd_play(SND_NOMETAL, 1);
                message("Not enough energy for the D-gun");
            }
            pop_order(u);
        }
        break;
    }
    case O_WAIT:
        if (--o->n <= 0)
            pop_order(u);
        break;
    case O_WAITATTACK:
        break;
    default:
        pop_order(u);
        break;
    }
}

/* ------------------------------------------------------------ projectiles and effects */

static void projectiles_step(void) {
    int i;
    for (i = 0; i < MAX_PROJ; i++) {
        Projectile* p = &projs[i];
        const WeaponType* w;
        if (!p->alive)
            continue;
        w = &weapons[p->weapon];
        if ((w->flags & WF_TRACKS) && p->target >= 0 && units[p->target].alive && p->ticks > 1) {
            const Unit* v = &units[p->target];
            p->vx = (v->x - p->x) / p->ticks;
            p->vz = (v->z - p->z) / p->ticks;
        }
        p->x += p->vx;
        p->z += p->vz;
        if (p->vy || (w->flags & WF_BALLISTIC)) {
            p->y = (int16_t)(p->y + p->vy / 16);
            p->vy = (int16_t)(p->vy - 96);
            if (p->y < 0)
                p->y = 0;
        }
        if (w->kind == WK_DGUN) {
            int x = p->x >> FIX, z = p->z >> FIX, j;
            for (j = 0; j < unit_hi; j++) {
                Unit* v = &units[j];
                if (v->alive && j != p->owner &&
                    within(
                        (v->x >> FIX) - x, (v->z >> FIX) - z, 14 + unittypes[v->type].radius / 2
                    ))
                    damage_unit(j, w->damage, p->owner, w->flags & WF_PARALYZE);
            }
            if (!(p->ticks & 1))
                add_effect(w->anim, x, ground_screen_y(x, z));
        }
        if (--p->ticks == 0) {
            int x = p->x >> FIX, z = p->z >> FIX;
            int hit = -1;
            p->alive = 0;
            if (w->kind == WK_DGUN)
                continue;
            if (p->target >= 0 && units[p->target].alive) {
                const Unit* v = &units[p->target];
                if (dist((v->x >> FIX) - x, (v->z >> FIX) - z) <= unittypes[v->type].radius + 4)
                    hit = p->target;
            }
            if (hit >= 0)
                damage_unit(
                    hit, w->damage, units[p->owner].alive ? p->owner : -1, w->flags & WF_PARALYZE
                );
            if (w->aoe > 16)
                area_damage(
                    x, z, w->aoe / 2, w->damage / 2, units[p->owner].alive ? p->owner : -1, hit
                );
            add_effect(w->anim, x, ground_screen_y(x, z));
            sim_sound_at(w->snd_hit, x, z);
            /* trees catch a little of it */
            if (w->aoe >= 32) {
                int k, r = w->aoe / 3;
                for (k = 0; k < nfeats; k++) {
                    Feature* f = &feats[k];
                    const FeatureType* ft = &common.feat[f->type];
                    if (!f->alive || !ft->energy || ft->metal || (ft->flags & FF_INDESTRUCT) ||
                        f->x - x >= r || x - f->x >= r || f->z - z >= r || z - f->z >= r ||
                        !within(f->x - x, f->z - z, r))
                        continue;
                    f->alive = 0;
                    feature_changed(f->x, f->z);
                    add_effect(ANIM_EXPLOSION, f->x, ground_screen_y(f->x, f->z) - 16);
                }
            }
        }
    }
    for (i = 0; i < MAX_BEAMS; i++)
        if (beams[i].alive && --beams[i].ticks == 0)
            beams[i].alive = 0;
    for (i = 0; i < MAX_FX; i++) {
        Effect* e = &fx[i];
        if (!e->alive)
            continue;
        if (++e->frame >= common.anim[e->anim].nframes)
            e->alive = 0;
    }
}

/* ------------------------------------------------------------ economy */

static void economy_step(void) {
    int i, p;
    int32_t m[2] = {0, 0}, e[2] = {0, 0};
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        const UnitType* t;
        if (!u->alive || (u->flags & UF_UNBUILT))
            continue;
        t = &unittypes[u->type];
        m[u->owner] += t->mmake + u->mex;
        e[u->owner] += t->emake;
        if (t->flags & UF_WIND) {
            int wv = wind_now < t->wind ? wind_now : t->wind;
            e[u->owner] += wv * 256 / TICKS;
        }
    }
    for (p = 0; p < 2; p++) {
        Player* pl = &players[p];
        pl->mincome = m[p];
        pl->eincome = e[p];
        pl->metal += m[p];
        pl->energy += e[p];
        if (pl->metal > pl->mstore << 8)
            pl->metal = pl->mstore << 8;
        if (pl->energy > pl->estore << 8)
            pl->energy = pl->estore << 8;
        if (pl->metal < 0)
            pl->metal = 0;
        if (pl->energy < 0)
            pl->energy = 0;
    }
    if (sim_tick_count % (TICKS * 8) == 0) {
        int span = mission.maxwind - mission.minwind;
        wind_now = (mission.minwind + (span > 0 ? rand16() % span : 0)) / 100;
    }
}

/* ------------------------------------------------------------ the computer player */

static int32_t next_build;
static int difficulty_level;

/* Idle Core factories build, as TA's computer player would, from three minutes in */
static void computer_build(void) {
    int i;
    if (sim_tick_count < next_build)
        return;
    next_build = sim_tick_count + TICKS * (40 - difficulty_level * 12);
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        const UnitType* t;
        int k, pick;
        if (!u->alive || u->owner != ENEMY || (u->flags & UF_UNBUILT))
            continue;
        t = &unittypes[u->type];
        if (t->kind != UK_FACTORY || u->fq_count[0] || !t->nbuild)
            continue;
        pick = t->build[rand16() % t->nbuild];
        for (k = 0; k < 4 && !(unittypes[pick].flags & UF_ARMED); k++)
            pick = t->build[rand16() % t->nbuild];
        if (pick < 0 || players[ENEMY].metal < (int32_t)unittypes[pick].metalcost << 7)
            continue;
        u->fq_type[0] = (uint8_t)pick;
        u->fq_count[0] = 1;
    }
}

/* A Core unit a factory built that has nothing to do: it joins the next wave */
static int idle_raider(const Unit* u) {
    const UnitType* t = &unittypes[u->type];
    return u->alive && u->owner == ENEMY && !(u->flags & UF_UNBUILT) && t->kind == UK_MOBILE &&
           (t->flags & UF_ARMED) && !u->norders && !u->nscript && u->built_by >= 0;
}

static void computer_step(void) {
    int i, n = 0, cx = 0, cz = 0, best = -1, bestd = 1 << 30;
    computer_build();
    if (sim_tick_count < next_wave)
        return;
    next_wave = sim_tick_count + TICKS * 25;
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        if (!idle_raider(u))
            continue;
        n++;
        cx += u->x >> FIX;
        cz += u->z >> FIX;
    }
    if (n < 3 + difficulty_level)
        return;
    cx /= n;
    cz /= n;
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        int d;
        if (!u->alive || u->owner != PLAYER)
            continue;
        d = dist((u->x >> FIX) - cx, (u->z >> FIX) - cz);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    if (best < 0)
        return;
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        int x, z;
        if (!idle_raider(u))
            continue;
        x = (units[best].x >> FIX) + (rand16() & 63) - 32;
        z = (units[best].z >> FIX) + (rand16() & 63) - 32;
        order_at(u, O_FIGHT, x, z, 0);
    }
}

/* ------------------------------------------------------------ objectives */

static void check_objectives(void) {
    int i, c, won = 0, lost = 0, any_victory = 0;
    int pl_units = 0, en_units = 0;
    int have_type[MAX_UNITTYPES];
    memset(have_type, 0, sizeof have_type);
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        if (!u->alive)
            continue;
        if (u->owner == PLAYER) {
            pl_units++;
            have_type[u->type]++;
        } else {
            en_units++;
        }
    }
    for (c = 0; c < mission.ncond; c++) {
        const Condition* k = &mission.cond[c];
        switch (k->kind) {
        case COND_MOVETO:
            any_victory = 1;
            for (i = 0; i < unit_hi; i++) {
                Unit* u = &units[i];
                if (u->alive && u->owner == PLAYER && !(u->flags & UF_UNBUILT) &&
                    unittypes[u->type].kind == UK_MOBILE && (k->type < 0 || u->type == k->type) &&
                    within((u->x >> FIX) - k->a, (u->z >> FIX) - k->b, k->c + 1))
                    won = 1;
            }
            break;
        case COND_KILLTYPE:
            any_victory = 1;
            if (k->type >= 0 && kills_of_type[k->type] >= (k->a > 0 ? k->a : 1))
                won = 1;
            break;
        case COND_COMMANDER:
            if (had_commander) {
                int alive = 0;
                for (i = 0; i < nunittypes; i++)
                    if ((unittypes[i].flags & UF_COMMANDER) && have_type[i])
                        alive = 1;
                if (!alive)
                    lost = 1;
            }
            break;
        case COND_ALLKILLED:
            if (!pl_units)
                lost = 1;
            break;
        case COND_ALLOFTYPE:
            if (k->type >= 0 && !have_type[k->type])
                lost = 1;
            break;
        }
    }
    if (!any_victory && !en_units)
        won = 1;
    if (lost)
        sim_result = -1;
    else if (won)
        sim_result = 1;
}

/* ------------------------------------------------------------ start and tick */

void sim_start(int schema) {
    const Schema* s = &mission.schema[schema < mission.nschema ? schema : 0];
    int i, j;
    memset(units, 0, sizeof units);
    memset(projs, 0, sizeof projs);
    memset(fx, 0, sizeof fx);
    memset(beams, 0, sizeof beams);
    memset(players, 0, sizeof players);
    memset(kills_of_type, 0, sizeof kills_of_type);
    unit_hi = 0;
    path_rr = 0;
    sim_tick_count = 0;
    sim_result = 0;
    had_commander = 0;
    next_wave = TICKS * 60;
    next_build = TICKS * 180;
    difficulty_level = schema < 3 ? schema : 1;
    last_warning = -1000;
    sim_message_tick = -1000;
    bw = (mission.w * 16 + BUCKET - 1) / BUCKET;
    bh = (mission.h * 16 + BUCKET - 1) / BUCKET;
    pw = (mission.w + 1) / 2;
    ph = (mission.h + 1) / 2;
    memset(occ, 0, sizeof occ);
    for (j = 0; j < ph; j++)
        for (i = 0; i < pw; i++) {
            cell_x[j * pw + i] = (uint8_t)i;
            cell_z[j * pw + i] = (uint8_t)j;
            update_pcell(i, j);
        }
    /* the map's trees and shrubs */
    nfeats = 0;
    for (i = 0; i < mission.nfeat; i++)
        add_feature(mission.featpos[i * 3], mission.featpos[i * 3 + 1], mission.featpos[i * 3 + 2]);
    players[PLAYER].metal = s->human_metal << 8;
    players[PLAYER].energy = s->human_energy << 8;
    players[ENEMY].metal = s->computer_metal << 8;
    players[ENEMY].energy = s->computer_energy << 8;
    for (i = 0; i < 2; i++) {
        players[i].base_mstore = 1000;
        players[i].base_estore = 1000;
    }
    if (s->human_metal > 1000)
        players[PLAYER].base_mstore = s->human_metal;
    if (s->human_energy > 1000)
        players[PLAYER].base_estore = s->human_energy;
    if (s->computer_metal > 1000)
        players[ENEMY].base_mstore = s->computer_metal;
    if (s->computer_energy > 1000)
        players[ENEMY].base_estore = s->computer_energy;
    wind_now = (mission.minwind + mission.maxwind) / 200;
    for (i = 0; i < s->nunits; i++) {
        const PlacedUnit* p = &s->units[i];
        int x = p->x, z = p->z, n;
        const UnitType* t = &unittypes[p->type];
        if (t->kind != UK_MOBILE) {
            x = snap_build_x(p->type, x);
            z = snap_build_z(p->type, z);
        }
        n = spawn_unit(p->type, p->player ? ENEMY : PLAYER, x, z, 0);
        if (n < 0)
            break;
        units[n].heading = 0;
        units[n].hp = (int32_t)t->maxhp * (p->hp ? p->hp : 100) / 100;
        units[n].script = (int16_t)p->script;
        units[n].nscript = (int16_t)p->nops;
    }
    recount_storage(PLAYER);
    recount_storage(ENEMY);
    build_buckets();
    update_sight();
    feat_dirty_x0 = 0;
    feat_dirty_x1 = -1;
}

void sim_tick(void) {
    int i, k, paths = 0;
    sim_tick_count++;
    build_buckets();
    for (i = 0; i < unit_hi; i++)
        if (units[i].alive)
            unit_step(i);
    /* a few path searches a tick, taking turns from where the last ones stopped */
    k = path_rr < unit_hi ? path_rr : 0;
    for (i = 0; i < unit_hi && paths < 2; i++, k = k + 1 < unit_hi ? k + 1 : 0) {
        Unit* u = &units[k];
        if (u->alive && u->want_path && u->has_goal) {
            find_path(u);
            paths++;
            path_rr = k + 1;
        }
    }
    projectiles_step();
    economy_step();
    computer_step();
    if ((sim_tick_count & 7) == 0)
        update_sight();
    if (sim_tick_count % TICKS == 0 && !sim_result)
        check_objectives();
}

/* ------------------------------------------------------------ picking */

int unit_at(int x, int sy, int owner_mask) {
    int i, best = -1, bestd = 1 << 30;
    for (i = 0; i < unit_hi; i++) {
        const Unit* u = &units[i];
        const UnitType* t;
        int ux, uy, r, d;
        if (!u->alive || !(owner_mask & (1 << u->owner)))
            continue;
        if (u->owner != PLAYER && !(u->flags & (UF_SEEN | UF_WAS_SEEN)))
            continue;
        t = &unittypes[u->type];
        ux = u->x >> FIX;
        /* far from the point: no need for its height or a square root */
        if (ux < x - 80 || ux > x + 80 || (u->z >> FIX) < sy - 80 || (u->z >> FIX) > sy + 140)
            continue;
        uy = unit_screen_y(u) - t->top / 2;
        r = t->radius + 2;
        if (t->kind != UK_MOBILE) {
            int hx = t->footx * 8, hz = t->footz * 8 + t->top / 2;
            if (x < ux - hx || x > ux + hx || sy < uy - hz || sy > uy + hz)
                continue;
            d = 1000 + dist(x - ux, sy - uy);
        } else {
            d = dist(x - ux, sy - uy);
            if (d > r)
                continue;
        }
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

int feature_at(int x, int sy) {
    int i, best = -1, bestd = 24;
    for (i = 0; i < nfeats; i++) {
        const Feature* f = &feats[i];
        int fy, d;
        if (!f->alive || !(common.feat[f->type].flags & FF_RECLAIM))
            continue;
        fy = ground_screen_y(f->x, f->z) - 8;
        d = dist(x - f->x, sy - fy);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}
