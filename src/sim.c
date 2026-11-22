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
#include "tt.h"

Unit units[MAX_UNITS];
Player players[2];
Projectile projs[MAX_PROJ];
Effect fx[MAX_FX];
Beam beams[MAX_BEAMS];
Feature feats[MAX_FEATS];
int nfeats;
int32_t feat_version;
int32_t sim_tick_count;
int32_t sim_last_fight = -10000;
int sim_result;
int wind_now;
int unit_hi;
int feat_dirty_x0, feat_dirty_y0, feat_dirty_x1 = -1, feat_dirty_y1 = -1;
int view_x0, view_y0, view_x1, view_y1;
char sim_message[64];
int32_t sim_message_tick = -1000;

#define BUCKET 64
#define PATH_NODES 4000

/* What the sim keeps for the map, sized to it when a mission starts (one
 * block): the buckets of 64 pixels, and the path cells of 32. A path cell's
 * index is its row shifted by pshift, or'd with its column, so that the
 * search finds both with a mask and a shift. */
static void* map_block;
static int bw, bh; /* buckets across and down */
static int16_t* bucket;
static uint8_t* seen;
static int pw, ph, pshift, pmask; /* path cells across and down; a row is 1 << pshift */
static uint8_t* pblock;           /* per path cell: the move classes it is closed to */
static uint8_t* occbits;          /* per map cell, a bit: a building's footprint covers it */
static uint8_t* pbuilt;           /* per path cell: how many buildings stand on it */
static uint16_t* gcost;
static uint8_t *stampv, *from;
/* per move class, per path cell: which part of the map it is in for that
 * class, from the ground alone (1 on; 0 closed; 255 one of many beyond 254) */
static uint8_t* regions[MAX_CLASSES];
static int path_rr;
static int kills_of_type[MAX_UNITTYPES];  /* the computer's units of each type destroyed */
static int deaths_of_type[MAX_UNITTYPES]; /* anyone's */
static uint8_t existed[MAX_UNITTYPES];    /* bit 0: placed at the start; bit 1: the computer's */
static int16_t placed_idx[MAX_UNITS];     /* the units the layout placed, by its order */
static int had_commander, enemy_had_commander;
static uint8_t* radar; /* per bucket: whether the player's radar or sonar covers it */
int32_t sim_timer = -1;
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

int type_ground(int type, int x, int z) {
    int h = ground_height(x, z);
    if ((unittypes[type].flags & (UF_SHIP | UF_FLY)) && h < mission.sealevel)
        h = mission.sealevel;
    return h;
}

/* Where a unit is: on the ground, or for a ship on the water */
static int unit_ground(const Unit* u) {
    return type_ground(u->type, u->x >> FIX, u->z >> FIX);
}

int unit_screen_y(const Unit* u) {
    return (int)(u->z >> FIX) - unit_ground(u) / 2;
}

/* An aircraft's height above where it stands, once built */
static int unit_alt(const Unit* u) {
    const UnitType* t = &unittypes[u->type];
    return (t->flags & UF_FLY) && !(u->flags & UF_UNBUILT) ? t->cruisealt : 0;
}

int unit_draw_y(const Unit* u) {
    return unit_screen_y(u) - unit_alt(u) / 2;
}

static int flies(const Unit* u) {
    return unit_alt(u) > 0;
}

/* Whether a unit is in the water: a ship, a submarine, or one wading */
static int in_water(const Unit* u) {
    return ground_height(u->x >> FIX, u->z >> FIX) < mission.sealevel;
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
    int i, j, cls, b = 0, built;
    if (px < 0 || pz < 0 || px >= pw || pz >= ph)
        return;
    built = pbuilt[pz << pshift | px];
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
    pblock[pz << pshift | px] = (uint8_t)b;
}

static int pcell_blocked(int mask, int x, int z) {
    int px = x >> 5, pz = z >> 5;
    if (px < 0 || pz < 0 || px >= pw || pz >= ph)
        return 1;
    return pblock[pz << pshift | px] & mask;
}

/* The first map cell across and down that a building centred at x, z covers */
static int foot_x0(const UnitType* t, int x) {
    return (x - t->footx * 8 + 8) >> 4;
}

static int foot_z0(const UnitType* t, int z) {
    return (z - t->footz * 8 + 8) >> 4;
}

/* Whether a building's footprint closes map cell i, j of it to units */
static int foot_solid(const UnitType* t, int i, int j) {
    return !(j < 8 && i < 8 && (t->yard[j] & (0x80 >> i)));
}

/* Sets or clears the occupancy bits of a building's footprint */
static void mark_footprint(const Unit* u, int on) {
    const UnitType* t = &unittypes[u->type];
    int x0 = foot_x0(t, u->x >> FIX), z0 = foot_z0(t, u->z >> FIX), i, j;
    for (j = z0; j < z0 + t->footz; j++)
        for (i = x0; i < x0 + t->footx; i++) {
            int32_t c;
            if (i < 0 || j < 0 || i >= mission.w || j >= mission.h)
                continue;
            c = (int32_t)j * mission.w + i;
            if (on)
                occbits[c >> 3] |= (uint8_t)(0x80 >> (c & 7));
            else
                occbits[c >> 3] &= (uint8_t)~(0x80 >> (c & 7));
        }
}

/* A building put up or taken down: its footprint's cells, and the path
 * cells under its solid cells */
static void set_occupancy(int idx, int on) {
    Unit* u = &units[idx];
    const UnitType* t = &unittypes[u->type];
    int x0 = foot_x0(t, u->x >> FIX), z0 = foot_z0(t, u->z >> FIX);
    int i, j, a, b;
    mark_footprint(u, on);
    if (!on) {
        /* a mission's own layout may overlap buildings: theirs stay marked */
        for (i = 0; i < unit_hi; i++) {
            const Unit* v = &units[i];
            const UnitType* vt = &unittypes[v->type];
            int vx0, vz0;
            if (!v->alive || i == idx || vt->kind == UK_MOBILE)
                continue;
            vx0 = foot_x0(vt, v->x >> FIX);
            vz0 = foot_z0(vt, v->z >> FIX);
            if (vx0 < x0 + t->footx && x0 < vx0 + vt->footx && vz0 < z0 + t->footz && z0 < vz0 + vt->footz)
                mark_footprint(v, 1);
        }
    }
    for (j = z0 >> 1; j <= (z0 + t->footz - 1) >> 1; j++)
        for (i = x0 >> 1; i <= (x0 + t->footx - 1) >> 1; i++) {
            int solid = 0;
            if (i < 0 || j < 0 || i >= pw || j >= ph)
                continue;
            for (b = 0; b < 2; b++)
                for (a = 0; a < 2; a++) {
                    int fi = i * 2 + a - x0, fj = j * 2 + b - z0;
                    if (fi >= 0 && fj >= 0 && fi < t->footx && fj < t->footz && foot_solid(t, fi, fj))
                        solid = 1;
                }
            if (solid)
                pbuilt[j << pshift | i] += on ? 1 : -1;
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

/* A building stands where the ground under it is level enough and as deep
 * as its type wants (on land, or in water for one that floats), off rocks
 * and other buildings; a geothermal plant on a vent */
int can_build_at(int type, int x, int z) {
    const UnitType* t = &unittypes[type];
    int x0 = (x - t->footx * 8) >> 4, z0 = (z - t->footz * 8) >> 4;
    int i, j;
    for (j = 0; j < t->footz; j++)
        for (i = 0; i < t->footx; i++) {
            int cx = x0 + i, cz = z0 + j, c, depth, k, lo, hi;
            if (cx < 0 || cz < 0 || cx >= mission.w || cz >= mission.h)
                return 0;
            c = cz * mission.w + cx;
            if (mission.flags[c] & CELL_ROCK)
                return 0;
            if (occbits[c >> 3] & (0x80 >> (c & 7)))
                return 0; /* another building */
            depth = mission.sealevel - mission.height[c];
            if (t->flags & UF_WATERBLD) {
                if (depth < t->minwater)
                    return 0;
            } else if (depth > t->maxwater) {
                return 0;
            }
            /* the cell's slope: the spread of its corners' heights */
            lo = hi = mission.height[c];
            for (k = 1; k < 4; k++) {
                int ax = cx + (k & 1), az = cz + (k >> 1), hk;
                if (ax >= mission.w)
                    ax = mission.w - 1;
                if (az >= mission.h)
                    az = mission.h - 1;
                hk = mission.height[az * mission.w + ax];
                if (hk < lo)
                    lo = hk;
                if (hk > hi)
                    hi = hk;
            }
            if (!(t->flags & UF_WATERBLD) && hi - lo > t->maxslope)
                return 0;
        }
    if (t->flags & UF_GEO) {
        for (i = 0; i < nfeats; i++) {
            const Feature* f = &feats[i];
            if (f->alive && (feat_type(f->type)->flags & FF_GEO) && f->x >= x0 * 16 &&
                f->x < (x0 + t->footx) * 16 && f->z >= z0 * 16 && f->z < (z0 + t->footz) * 16)
                return 1;
        }
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------ path search */

/* A search's marks: a cell met in this search holds cur_stamp, one done with
 * cur_stamp + 1; stamps go up by two a search, and the marks are cleared
 * when they wrap */
static uint8_t cur_stamp;

/* The open cells: a bucket queue on a cell's estimated cost f, in buckets
 * of 8, each a list; pushing and taking the cheapest are a few
 * instructions each. The weighted estimate can put a cell a little below
 * the one it came from, so the queue keeps the lowest bucket that may hold
 * one, and the highest used, to clear no more than those. */
#define QCAP (PATH_NODES * 8 + 16)
#define QBUCKETS 8192
#define QNONE 0xFFFF
static uint16_t qcell[QCAP], qnext[QCAP], qhead[QBUCKETS];
static int qcount, qused, qmin, qtop = QBUCKETS - 1;

static void q_reset(void) {
    memset(qhead, 0xFF, (size_t)(qtop + 1) * sizeof qhead[0]);
    qcount = qused = qtop = 0;
    qmin = QBUCKETS - 1;
}

static void q_push(int f, int c) {
    int b = f >> 3, e = qused++;
    qcell[e] = (uint16_t)c;
    qnext[e] = qhead[b];
    qhead[b] = (uint16_t)e;
    qcount++;
    if (b < qmin)
        qmin = b;
    if (b > qtop)
        qtop = b;
}

/* Takes a cell of the cheapest bucket */
static int q_pop(void) {
    int e;
    while (qhead[qmin] == QNONE)
        qmin++;
    e = qhead[qmin];
    qhead[qmin] = qnext[e];
    qcount--;
    return qcell[e];
}

static const int8_t ddx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
static const int8_t ddz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
static const uint8_t dcost[8] = {10, 10, 10, 10, 14, 14, 14, 14};

static int heur(int x, int z, int gx, int gz) {
    int dx = x > gx ? x - gx : gx - x, dz = z > gz ? z - gz : gz - z;
    return dx > dz ? 10 * dx + 4 * dz : 10 * dz + 4 * dx;
}

/* Labels each class's parts of the map, by flooding the path cells open to
 * it four ways (a diagonal step needs both its sides open anyway); gcost
 * holds the queue */
static void label_regions(void) {
    int cls, c, np = ph << pshift;
    uint16_t* q = gcost;
    for (cls = 0; cls < nclasses; cls++) {
        uint8_t* r = regions[cls];
        int next = 1, mask = 1 << cls;
        memset(r, 0, (size_t)np);
        for (c = 0; c < np; c++) {
            int head = 0, tail = 0, lab;
            if (r[c] || (c & pmask) >= pw || (pblock[c] & mask))
                continue;
            lab = next < 255 ? next++ : 255;
            r[c] = (uint8_t)lab;
            q[tail++] = (uint16_t)c;
            while (head < tail) {
                int k = q[head++], x = k & pmask, z = k >> pshift, d;
                for (d = 0; d < 4; d++) {
                    int nx = x + ddx[d], nz = z + ddz[d], nk;
                    if (nx < 0 || nz < 0 || nx >= pw || nz >= ph)
                        continue;
                    nk = nz << pshift | nx;
                    if (r[nk] || (pblock[nk] & mask))
                        continue;
                    r[nk] = (uint8_t)lab;
                    q[tail++] = (uint16_t)nk;
                }
            }
        }
    }
}

/* The path cell nearest gx, gz in a class's part of the map, ring by ring
 * up to 24 cells away, or -1 */
static int nearest_in_region(const uint8_t* r, int lab, int gx, int gz) {
    int k, i;
    for (k = 1; k <= 24; k++)
        for (i = -k; i <= k; i++) {
            int xs[4] = {gx + i, gx + i, gx - k, gx + k}, zs[4] = {gz - k, gz + k, gz + i, gz + i}, j;
            for (j = 0; j < 4; j++) {
                int c;
                if (xs[j] < 0 || zs[j] < 0 || xs[j] >= pw || zs[j] >= ph)
                    continue;
                c = zs[j] << pshift | xs[j];
                if (r[c] == lab)
                    return c;
            }
        }
    return -1;
}

/* Whether a unit can get to a place at all, as far as the ground goes */
static int reachable(const Unit* u, int x, int z) {
    const UnitType* t = &unittypes[u->type];
    int cls = t->moveclass, a, b;
    if ((t->flags & UF_FLY) || cls >= nclasses)
        return 1;
    x >>= 5;
    z >>= 5;
    if (x < 0 || z < 0 || x >= pw || z >= ph)
        return 0;
    a = regions[cls][((u->z >> FIX) >> 5) << pshift | ((u->x >> FIX) >> 5)];
    b = regions[cls][z << pshift | x];
    return !a || a == 255 || a == b;
}

/* The path search: one at a time, given PATH_BUDGET cells a tick and
 * carried on the next tick where it stopped, so that no tick waits on a
 * long search; a unit heads straight for its goal until its path comes */
#define PATH_BUDGET 250

static struct {
    int u; /* the unit searched for, or -1 */
    int start, goal, gx, gz, best, besth, n, mask;
    int doff[8]; /* a step's change of cell index */
    int16_t goalx, goalz; /* the goal searched for: another means the unit wants another */
} sr;

/* Starts the search for a unit's path; 0 when none is needed */
static int path_begin(Unit* u) {
    const UnitType* t = &unittypes[u->type];
    int cls = t->moveclass;
    int sx = (u->x >> FIX) >> 5, sz = (u->z >> FIX) >> 5;
    int gx = u->goalx >> 5, gz = u->goalz >> 5, d;
    u->want_path = 0;
    u->npath = u->pathi = 0;
    if (sx < 0 || sz < 0 || sx >= pw || sz >= ph || (t->flags & UF_FLY))
        return 0;
    if (gx < 0)
        gx = 0;
    if (gz < 0)
        gz = 0;
    if (gx >= pw)
        gx = pw - 1;
    if (gz >= ph)
        gz = ph - 1;
    sr.start = sz << pshift | sx;
    sr.goal = gz << pshift | gx;
    for (d = 0; d < 8; d++)
        sr.doff[d] = (ddz[d] << pshift) + ddx[d];
    /* a goal the class cannot get to: the nearest place it can, or none */
    if (cls < nclasses) {
        const uint8_t* r = regions[cls];
        int rs = r[sr.start];
        if (rs && rs != 255 && r[sr.goal] != rs) {
            int g = nearest_in_region(r, rs, gx, gz);
            if (g < 0) {
                u->has_goal = 0;
                return 0;
            }
            sr.goal = g;
            gx = g & pmask;
            gz = g >> pshift;
            u->goalx = (int16_t)(gx * 32 + 16);
            u->goalz = (int16_t)(gz * 32 + 16);
        }
    }
    if (sr.start == sr.goal)
        return 0;
    sr.u = (int)(u - units);
    sr.gx = gx;
    sr.gz = gz;
    sr.goalx = u->goalx;
    sr.goalz = u->goalz;
    sr.mask = 1 << cls;
    sr.n = 0;
    cur_stamp += 2;
    if (cur_stamp < 2) {
        memset(stampv, 0, (size_t)(ph << pshift));
        cur_stamp = 2;
    }
    q_reset();
    gcost[sr.start] = 0;
    stampv[sr.start] = cur_stamp;
    from[sr.start] = 255;
    sr.best = sr.start;
    sr.besth = heur(sx, sz, gx, gz);
    /* the heuristic counts twice: a path a little longer than the
     * shortest, for far fewer cells looked at */
    q_push(sr.besth * 2, sr.start);
    return 1;
}

/* Gives the unit the path the search found: to its goal, or to the cell
 * nearest it when the goal could not be reached */
static void path_finish(void) {
    Unit* u = &units[sr.u];
    uint16_t tmp[256];
    int nt = 0, i, c = sr.best, lastd = -1;
    sr.u = -1;
    /* walk back from the best cell, keeping the cells where the way turns */
    while (c != sr.start && nt < 256) {
        int d = from[c];
        if (d == 255)
            break;
        if (d != lastd) {
            tmp[nt++] = (uint16_t)c;
            lastd = d;
        }
        c -= sr.doff[d];
    }
    /* tmp holds the turns from the end back; the unit wants them from its end */
    for (i = 0; i < nt && i < MAX_PATH; i++)
        u->path[i] = tmp[nt - 1 - i];
    u->npath = (uint8_t)(nt < MAX_PATH ? nt : MAX_PATH);
    u->pathi = 0;
    if (sr.best != sr.goal) {
        if (u->npath) {
            c = u->path[u->npath - 1];
            u->goalx = (int16_t)((c & pmask) * 32 + 16);
            u->goalz = (int16_t)((c >> pshift) * 32 + 16);
        } else {
            u->has_goal = 0;
        }
    }
}

/* Runs the search for up to budget cells, finishing it when it is done;
 * returns the cells it took */
static int path_run(int budget) {
    int used = 0, mask = sr.mask, closed = cur_stamp + 1, goal = sr.goal, d;
    while (qcount && sr.n < PATH_NODES && used < budget) {
        int c = q_pop(), cx = c & pmask, cz = c >> pshift, h;
        if (stampv[c] == closed)
            continue;
        stampv[c] = (uint8_t)closed;
        sr.n++;
        used++;
        if (c == goal) {
            sr.best = c;
            path_finish();
            return used;
        }
        h = heur(cx, cz, sr.gx, sr.gz);
        if (h < sr.besth) {
            sr.besth = h;
            sr.best = c;
        }
        for (d = 0; d < 8; d++) {
            int nx = cx + ddx[d], nz = cz + ddz[d], nc, g;
            if (nx < 0 || nz < 0 || nx >= pw || nz >= ph)
                continue;
            nc = c + sr.doff[d];
            if ((pblock[nc] & mask) && nc != goal)
                continue;
            if (d >= 4 && ((pblock[c + ddx[d]] & mask) || (pblock[c + sr.doff[d] - ddx[d]] & mask)))
                continue;
            if (stampv[nc] == closed)
                continue;
            g = gcost[c] + dcost[d];
            if (stampv[nc] == cur_stamp && gcost[nc] <= g)
                continue;
            stampv[nc] = cur_stamp;
            gcost[nc] = (uint16_t)g;
            from[nc] = (uint8_t)d;
            if (qused < QCAP - 1)
                q_push(g + 2 * heur(nx, nz, sr.gx, sr.gz), nc);
        }
    }
    if (!qcount || sr.n >= PATH_NODES)
        path_finish();
    return used ? used : 1;
}

/* A tick's path searching: the search under way, then the next units that
 * want paths, taking turns, while the budget lasts */
static void paths_step(void) {
    int budget = PATH_BUDGET, looked = 0;
    while (budget > 0) {
        if (sr.u >= 0) {
            Unit* u = &units[sr.u];
            if (!u->alive || !u->has_goal || u->goalx != sr.goalx || u->goalz != sr.goalz) {
                sr.u = -1; /* it went, or wants somewhere else now */
                continue;
            }
            budget -= path_run(budget);
            continue;
        }
        {
            int i, k = path_rr < unit_hi ? path_rr : 0, found = -1;
            for (i = 0; i < unit_hi; i++, k = k + 1 < unit_hi ? k + 1 : 0)
                if (units[k].alive && units[k].want_path && units[k].has_goal) {
                    found = k;
                    break;
                }
            if (found < 0 || ++looked > 40)
                break;
            path_rr = found + 1;
            if (!path_begin(&units[found]))
                budget -= 16;
        }
    }
}

/* ------------------------------------------------------------ buckets and sight */

/* Each player's living units, a list each, for searches too wide for the buckets */
static int16_t side_units[2][MAX_UNITS];
static int nside[2];

static void build_buckets(void) {
    int i;
    for (i = 0; i < bw * bh; i++)
        bucket[i] = -1;
    nside[0] = nside[1] = 0;
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        int bx, bz, b;
        if (!u->alive)
            continue;
        side_units[u->owner][nside[u->owner]++] = (int16_t)i;
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

const uint8_t* disc_widths(int r) {
    static uint8_t widths[32][32];
    static uint32_t made;
    if (r > 31)
        r = 31;
    if (!(made & (1UL << r))) {
        int w = r, d;
        for (d = 0; d <= r; d++) {
            while (w * w > r * r + r - d * d)
                w--;
            widths[r][d] = (uint8_t)w;
        }
        made |= 1UL << r;
    }
    return widths[r];
}

/* Marks the buckets within range of a unit: a disc, a row at a time */
static void cover_disc(uint8_t* map, const Unit* u, int range) {
    const uint8_t* wt;
    int bx = (u->x >> FIX) / BUCKET, bz = (u->z >> FIX) / BUCKET, dz;
    int r = (range + BUCKET / 2) / BUCKET;
    if (r > 31)
        r = 31;
    wt = disc_widths(r);
    for (dz = -r; dz <= r; dz++) {
        int z = bz + dz, w = wt[dz < 0 ? -dz : dz], x0, x1;
        if (z < 0 || z >= bh)
            continue;
        x0 = bx - w < 0 ? 0 : bx - w;
        x1 = bx + w >= bw ? bw - 1 : bx + w;
        if (x0 <= x1)
            memset(map + z * bw + x0, 1, (size_t)(x1 - x0 + 1));
    }
}

/* What the player's units see, and what their radar and sonar find */
static void update_sight(void) {
    int i;
    memset(seen, 0, (size_t)(bw * bh));
    memset(radar, 0, (size_t)(bw * bh));
    for (i = 0; i < unit_hi; i++) {
        const Unit* u = &units[i];
        const UnitType* t = &unittypes[u->type];
        if (!u->alive || u->owner != PLAYER || (u->flags & UF_UNBUILT))
            continue;
        cover_disc(seen, u, t->sight);
        if (t->flags & (UF_RADAR | UF_SONAR))
            cover_disc(radar, u, t->radar);
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
        if (bx >= 0 && bz >= 0 && bx < bw && bz < bh && radar[bz * bw + bx])
            u->flags |= UF_ON_RADAR;
        else
            u->flags &= (uint8_t)~UF_ON_RADAR;
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
    if (anim < 0 || anim >= world.nanim || !world.anim[anim].nframes)
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
    feats[i].type = (uint16_t)type;
    feats[i].x = (int16_t)x;
    feats[i].z = (int16_t)z;
    feats[i].metal = (int32_t)feat_type(type)->metal;
    feats[i].energy = (int32_t)feat_type(type)->energy;
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
                if (!u->alive || i == direct || flies(u))
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

/* No unit or shot may still point at a unit that died or changed sides */
static void forget_unit(int idx) {
    int i, j;
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
            if ((v->order[j].kind == O_ATTACK || v->order[j].kind == O_GUARD || v->order[j].kind == O_CAPTURE ||
                 v->order[j].kind == O_REPAIR) &&
                v->order[j].target == idx)
                v->order[j].kind = O_NONE;
    }
    for (i = 0; i < MAX_PROJ; i++)
        if (projs[i].alive && projs[i].target == idx)
            projs[i].target = -1;
}

/* Destroys a unit: its blast is the weapon given (its own explosion, or its
 * self-destruct's), and it leaves its wreck */
static void kill_unit_as(int idx, int blast) {
    Unit* u = &units[idx];
    const UnitType* t = &unittypes[u->type];
    int x = u->x >> FIX, z = u->z >> FIX, sy = unit_draw_y(u);
    int owner = u->owner, wasunbuilt = u->flags & UF_UNBUILT;
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
    deaths_of_type[u->type]++;
    if (blast >= 0) {
        const WeaponType* w = &weapons[blast];
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
    if (!wasunbuilt && t->corpse >= 0 && blast == t->explode)
        add_feature(wreck_feat(u->type), x, z);
    forget_unit(idx);
    recount_storage(owner);
    if (owner == PLAYER && (t->flags & UF_COMMANDER))
        message("Commander destroyed!");
}

static void kill_unit(int idx) {
    kill_unit_as(idx, unittypes[units[idx].type].explode);
}

void sim_kill(int idx) {
    kill_unit(idx);
}

void sim_self_destruct(int idx) {
    const UnitType* t = &unittypes[units[idx].type];
    kill_unit_as(idx, t->selfd >= 0 ? t->selfd : t->explode);
}

int sim_spawn(int type, int owner, int x, int z) {
    if (unittypes[type].kind != UK_MOBILE) {
        x = snap_build_x(type, x);
        z = snap_build_z(type, z);
    }
    return spawn_unit(type, owner, x, z, 0);
}

/* A unit taken over by another player, as a Commander captures */
static void capture_unit(int idx, int owner) {
    Unit* v = &units[idx];
    int old = v->owner;
    v->owner = (uint8_t)owner;
    order_clear(v);
    v->target = -1;
    v->attacker = -1;
    v->nscript = 0;
    v->flags &= (uint8_t)~(UF_SELECTED | UF_SEEN | UF_WAS_SEEN | UF_ON_RADAR);
    forget_unit(idx);
    recount_storage(old);
    recount_storage(owner);
    if (owner == PLAYER) {
        message("Unit captured");
        snd_play(SND_UNITDONE, 2);
    }
}

void sim_capture(int idx, int owner) {
    capture_unit(idx, owner);
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
    if (u->owner == PLAYER || (attacker >= 0 && units[attacker].owner == PLAYER))
        sim_last_fight = sim_tick_count;
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
    u->want_path = !(unittypes[u->type].flags & UF_FLY);
    u->npath = u->pathi = 0;
    u->stuck = 0;
    u->repaths = 0;
}

/* Turns a unit toward a heading, as fast as its type turns; returns how far
 * it had to turn before this step */
static int16_t turn_to(Unit* u, uint16_t want) {
    int16_t diff = (int16_t)(want - u->heading), turn = (int16_t)unittypes[u->type].turn;
    if (diff > turn)
        u->heading = (uint16_t)(u->heading + turn);
    else if (diff < -turn)
        u->heading = (uint16_t)(u->heading - turn);
    else
        u->heading = want;
    return diff;
}

/* An aircraft's step: straight at the goal, over anything, kept on the map */
static int fly_step(Unit* u) {
    const UnitType* t = &unittypes[u->type];
    int x = u->x >> FIX, z = u->z >> FIX, dx = u->goalx - x, dz = u->goalz - z, d = dist(dx, dz);
    if (d <= 8) {
        u->has_goal = 0;
        u->speed = 0;
        return 1;
    }
    turn_to(u, heading_to(dx, dz));
    u->speed += (int16_t)t->accel;
    if (u->speed > (int16_t)t->speed)
        u->speed = (int16_t)t->speed;
    if (d < 48 && u->speed > (int16_t)(t->speed / 2))
        u->speed = (int16_t)(t->speed / 2);
    u->x -= ((int32_t)SIN(u->heading) * u->speed) >> 14;
    u->z += ((int32_t)COS(u->heading) * u->speed) >> 14;
    if (u->x < (8 << FIX))
        u->x = 8 << FIX;
    if (u->z < (8 << FIX))
        u->z = 8 << FIX;
    if (u->x > ((int32_t)mission.w * 16 - 8) << FIX)
        u->x = ((int32_t)mission.w * 16 - 8) << FIX;
    if (u->z > ((int32_t)mission.h * 16 - 8) << FIX)
        u->z = ((int32_t)mission.h * 16 - 8) << FIX;
    return 0;
}

/* One step along the way; 1 when the goal is reached */
static int move_step(Unit* u) {
    const UnitType* t = &unittypes[u->type];
    int x = u->x >> FIX, z = u->z >> FIX, tx, tz, dx, dz, d, final;
    int16_t diff;
    int32_t nx, nz;
    int cls = t->moveclass;
    if (!u->has_goal || t->kind != UK_MOBILE || !t->speed)
        return 1;
    if (t->flags & UF_FLY)
        return fly_step(u);
    if (u->pathi < u->npath) {
        int c = u->path[u->pathi];
        tx = (c & pmask) * 32 + 16;
        tz = (c >> pshift) * 32 + 16;
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
    diff = turn_to(u, heading_to(dx, dz));
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

/* Whether a weapon can hit a unit: an aircraft only with one that reaches
 * the air, a submarine only with a torpedo, and a torpedo only what is in
 * the water; a bomb nothing in the air */
static int can_hit(const WeaponType* w, const Unit* v) {
    const UnitType* vt = &unittypes[v->type];
    if (flies(v))
        return (w->flags & WF_AIR) != 0;
    if (w->flags & WF_WATER)
        return in_water(v) || (vt->flags & (UF_SHIP | UF_SUB));
    if (vt->flags & UF_SUB)
        return 0;
    return 1;
}

int can_attack(const Unit* a, const Unit* v) {
    const UnitType* t = &unittypes[a->type];
    int i;
    if (flies(v) && (t->flags & UF_NOAIR))
        return 0;
    for (i = 0; i < 3; i++)
        if (t->weapon[i] >= 0 && !(weapons[t->weapon[i]].flags & WF_MANUAL) &&
            can_hit(&weapons[t->weapon[i]], v))
            return 1;
    return 0;
}

/* The damage a weapon does to a unit: some do more to aircraft */
static int damage_to(const WeaponType* w, const Unit* v) {
    return flies(v) ? w->damage_air : w->damage;
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

/* The nearest enemy within radius that the unit can fight (any, for type
 * -2), or of a type, or -1; aircraft before the rest for a unit that prefers
 * them */
static int nearest_enemy(const Unit* u, int radius, int type) {
    int x = u->x >> FIX, z = u->z >> FIX;
    int bx0, bx1, bz0, bz1, bx, bz, best = -1, bestd = radius + 1, air = -1, aird = radius + 1;
    int pref = (unittypes[u->type].flags & UF_AIRPREF) != 0;
    int wide = radius > BUCKET * 3, k = 0, i;
    const int16_t* list = side_units[!u->owner];
    bucket_range(x, z, radius, &bx0, &bx1, &bz0, &bz1);
    bx = bx0;
    bz = bz0;
    i = wide ? (nside[!u->owner] ? list[0] : -1) : bucket[bz * bw + bx];
    for (;;) {
        const Unit* v;
        int dx, dz, near, nearair;
        /* the next unit: down the list, or bucket by bucket */
        while (i < 0) {
            if (wide)
                goto done;
            if (++bx > bx1) {
                bx = bx0;
                if (++bz > bz1)
                    goto done;
            }
            i = bucket[bz * bw + bx];
        }
        v = &units[i];
        if (wide)
            i = ++k < nside[!u->owner] ? list[k] : -1;
        else
            i = v->next;
        if (!v->alive || v->owner == u->owner)
            continue;
        dx = (v->x >> FIX) - x;
        dz = (v->z >> FIX) - z;
        if (dx > radius || dx < -radius || dz > radius || dz < -radius)
            continue;
        if (type >= 0 && v->type != type)
            continue;
        if (u->owner == PLAYER && !(v->flags & UF_SEEN))
            continue;
        near = within(dx, dz, bestd);
        nearair = pref && within(dx, dz, aird) && flies(v);
        if (!near && !nearair)
            continue;
        if (type == -1 && !can_attack(u, v))
            continue;
        if (nearair) {
            aird = dist(dx, dz);
            air = (int)(v - units);
        }
        if (near) {
            bestd = dist(dx, dz);
            best = (int)(v - units);
        }
    }
done:
    return air >= 0 ? air : best;
}

static void fire(Unit* u, int wi, int tx, int tz, int target) {
    const UnitType* t = &unittypes[u->type];
    const WeaponType* w = &weapons[t->weapon[wi]];
    int x = u->x >> FIX, z = u->z >> FIX, sy = unit_draw_y(u) - t->top / 2;
    int i;
    sim_sound_at(w->snd_start, x, z);
    if (target >= 0)
        u->heading = heading_to(tx - x, tz - z);
    if (w->kind == WK_BEAM) {
        int ty = ground_screen_y(tx, tz);
        if (target >= 0) {
            const Unit* v = &units[target];
            ty = unit_draw_y(v) - unittypes[v->type].top / 3;
            damage_unit(target, damage_to(w, v), (int)(u - units), w->flags & WF_PARALYZE);
        }
        for (i = 0; i < MAX_BEAMS; i++)
            if (!beams[i].alive) {
                beams[i].alive = 1;
                beams[i].colour = world.beam[w->colour];
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
        p->owner = (int16_t)(u - units);
        p->side = u->owner;
        p->ticks = (uint8_t)ticks;
        p->x = u->x;
        p->z = u->z;
        p->vx = ((int32_t)dx << FIX) / ticks;
        p->vz = ((int32_t)dz << FIX) / ticks;
        p->y = (int16_t)((t->top / 2 + unit_alt(u) / 2) << 8);
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
    /* what it is capturing, it does not shoot */
    if (u->norders && u->order[0].kind == O_CAPTURE && u->target == u->order[0].target)
        u->target = -1;
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
        if (u->reload[i] || !in_range(u, v, w->range) || !can_hit(w, v))
            continue;
        fire(u, i, v->x >> FIX, v->z >> FIX, u->target);
        if (w->burst > 1) {
            if (u->burst[i] == 0)
                u->burst[i] = (uint8_t)(w->burst - 1);
            else
                u->burst[i]--;
            u->reload[i] = u->burst[i] ? w->gap : w->reload;
        } else {
            u->reload[i] = w->reload;
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
        int other;
        for (other = 0; other < unit_hi; other++) {
            const Unit* v = &units[other];
            if (v->alive && v->type == o->a && (v->flags & UF_UNBUILT) && v->owner == u->owner &&
                (v->x >> FIX) == o->x && (v->z >> FIX) == o->z) {
                u->nano = (int16_t)other;
                return;
            }
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
            o.target = op->a >= 0 && op->a < MAX_UNITS ? placed_idx[op->a] : -1;
            order_add(u, &o, 1);
            break;
        case OP_SELFDESTRUCT:
            sim_self_destruct((int)(u - units));
            return;
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
            o.target = op->a >= 0 && op->a < MAX_UNITS ? placed_idx[op->a] : -1;
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

/* A bomber's pass: at its target, then on past it, to turn and come again */
static void bomber_run(Unit* u, const Unit* v) {
    int x = u->x >> FIX, z = u->z >> FIX, vx = v->x >> FIX, vz = v->z >> FIX;
    if (dist(vx - x, vz - z) < 56) {
        set_goal(u, vx - (int)(((int32_t)SIN(u->heading) * 220) >> 14),
                 vz + (int)(((int32_t)COS(u->heading) * 220) >> 14));
    } else if (!u->has_goal || dist(u->goalx - x, u->goalz - z) < 24 ||
               dist(u->goalx - vx, u->goalz - vz) < 56) {
        set_goal(u, vx, vz);
    }
    move_step(u);
}

/* A silo or an anti-missile system builds missiles, paying for them as it
 * goes, as a factory pays for a unit; it holds two at most */
static void stockpile_step(Unit* u) {
    const UnitType* t = &unittypes[u->type];
    const WeaponType* w = 0;
    Player* p = &players[u->owner];
    int32_t m, e;
    int i;
    for (i = 0; i < 3; i++)
        if (t->weapon[i] >= 0 && (weapons[t->weapon[i]].flags & WF_STOCKPILE))
            w = &weapons[t->weapon[i]];
    if (!w || u->stock >= 2)
        return;
    m = (int32_t)w->metal * 256 / w->reload;
    e = (int32_t)w->energy * 256 / w->reload;
    if (p->metal < m || p->energy < e)
        return;
    p->metal -= m;
    p->energy -= e;
    if (++u->stockwork >= w->reload) {
        u->stockwork = 0;
        u->stock++;
        if (u->owner == PLAYER && !(t->flags & UF_ANTINUKE))
            message("Missile ready");
    }
}

/* The ticks a capture takes: longer for what took longer to build */
static int capture_ticks(const Unit* v, int worker) {
    int32_t n = (int32_t)unittypes[v->type].buildtime * TICKS / 4 / (worker ? worker : 100) + TICKS * 3;
    return n > TICKS * 45 ? TICKS * 45 : (int)n;
}

/* Goes within a builder's reach of a unit; 1 once there */
static int reach_unit(Unit* u, const Unit* v) {
    const UnitType* t = &unittypes[u->type];
    int x = u->x >> FIX, z = u->z >> FIX, vx = v->x >> FIX, vz = v->z >> FIX;
    int reach = t->builddist + unittypes[v->type].radius + 16;
    if (dist(vx - x, vz - z) > reach) {
        set_goal(u, vx, vz);
        move_step(u);
        return 0;
    }
    u->has_goal = 0;
    u->speed = 0;
    u->heading = heading_to(vx - x, vz - z);
    return 1;
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
    if (!u->alive)
        return; /* its script blew it up */
    if (t->kind == UK_FACTORY)
        factory_step(u, idx);
    if (t->flags & UF_STOCKPILE)
        stockpile_step(u);
    weapons_step(u, idx);
    /* the computer's crawling bombs go off by themselves beside an enemy */
    if ((t->flags & UF_KAMIKAZE) && u->owner == ENEMY && ((sim_tick_count + idx) & 3) == 0 &&
        nearest_enemy(u, 36, -2) >= 0) {
        sim_self_destruct(idx);
        return;
    }
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
            if (t->flags & UF_BOMBER)
                bomber_run(u, &units[u->target]);
            else
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
        if (t->flags & UF_KAMIKAZE) {
            if (dist((v->x - u->x) >> FIX, (v->z - u->z) >> FIX) < 40) {
                sim_self_destruct(idx);
                return;
            }
            set_goal(u, v->x >> FIX, v->z >> FIX);
            move_step(u);
            break;
        }
        if (t->flags & UF_BOMBER) {
            bomber_run(u, v);
            break;
        }
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
        if (!u->reload[i] && (w->flags & WF_STOCKPILE)) {
            if (u->stock) {
                u->stock--;
                fire(u, i, o->x, o->z, -1);
                u->reload[i] = TICKS * 2;
            } else if (u->owner == PLAYER) {
                snd_play(SND_CANTDO, 1);
                message("No missile ready");
            }
            pop_order(u);
        } else if (!u->reload[i]) {
            if (players[u->owner].energy >= (int32_t)w->energy << 8) {
                players[u->owner].energy -= (int32_t)w->energy << 8;
                fire(u, i, o->x, o->z, -1);
                u->reload[i] = w->reload;
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
        /* waiting on another unit: until it is attacked, or gone */
        if (o->target >= 0 && (!units[o->target].alive || sim_tick_count - units[o->target].attacked < 2))
            pop_order(u);
        break;
    case O_CAPTURE: {
        Unit* v = o->target >= 0 ? &units[o->target] : 0;
        if (!v || !v->alive || v->owner == u->owner || !(t->flags & UF_CAPTURE)) {
            u->capture = 0;
            pop_order(u);
            break;
        }
        if (!reach_unit(u, v)) {
            u->capture = 0;
            u->nano = -1;
            break;
        }
        u->nano = o->target;
        if (++u->capture >= capture_ticks(v, t->workertime)) {
            u->capture = 0;
            capture_unit(o->target, u->owner);
            pop_order(u);
        }
        break;
    }
    case O_REPAIR: {
        Unit* v = o->target >= 0 ? &units[o->target] : 0;
        const UnitType* vt;
        if (!v || !v->alive || v->owner != u->owner || (v->flags & UF_UNBUILT) || o->target == idx) {
            pop_order(u);
            break;
        }
        vt = &unittypes[v->type];
        if (v->hp >= vt->maxhp) {
            v->hp = vt->maxhp;
            pop_order(u);
            break;
        }
        if (!reach_unit(u, v)) {
            u->nano = -1;
            break;
        }
        u->nano = o->target;
        v->hp += (t->workertime + TICKS * 2 - 1) / (TICKS * 2);
        break;
    }
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
        if ((w->flags2 & WF2_NUKE) && !(sim_tick_count & 3)) {
            int j, px = p->x >> FIX, pz = p->z >> FIX;
            for (j = 0; j < unit_hi; j++) {
                Unit* a = &units[j];
                if (a->alive && a->owner != p->side && a->stock && (unittypes[a->type].flags & UF_ANTINUKE) &&
                    within((a->x >> FIX) - px, (a->z >> FIX) - pz, 1000)) {
                    a->stock--;
                    p->alive = 0;
                    add_effect(ANIM_EXPLOSION, px, ground_screen_y(px, pz) - 40);
                    sim_sound_at(SND_XPLOLRG, px, pz);
                    if (a->owner == PLAYER)
                        message("Nuclear missile intercepted");
                    break;
                }
            }
            if (!p->alive)
                continue;
        }
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
                    hit, damage_to(w, &units[hit]), units[p->owner].alive ? p->owner : -1,
                    w->flags & WF_PARALYZE
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
                    const FeatureType* ft;
                    if (!f->alive || f->x - x >= r || x - f->x >= r || f->z - z >= r || z - f->z >= r)
                        continue;
                    ft = feat_type(f->type);
                    if (!ft->energy || ft->metal || (ft->flags & FF_INDESTRUCT) || !within(f->x - x, f->z - z, r))
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
        if (++e->frame >= world.anim[e->anim].nframes)
            e->alive = 0;
    }
}

/* ------------------------------------------------------------ economy */

/* The energy a type makes net of its upkeep, which only buildings switched
 * on pay: a solar collector's is negative upkeep, and a radar makes what it
 * uses */
static int32_t energy_net(const UnitType* t) {
    return t->emake - (t->kind != UK_MOBILE && (t->flags & UF_ACTIVE) ? t->euse : 0);
}

static void economy_step(void) {
    int i, p;
    int32_t m[2] = {0, 0}, e[2] = {0, 0};
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        const UnitType* t;
        if (!u->alive || (u->flags & UF_UNBUILT))
            continue;
        t = &unittypes[u->type];
        /* a metal maker stands idle while energy runs short */
        if ((t->flags & UF_MAKER) && players[u->owner].energy < t->euse * 2)
            continue;
        m[u->owner] += t->mmake + u->mex;
        e[u->owner] += energy_net(t);
        if (t->flags & UF_TIDAL)
            e[u->owner] += mission.tidal * 256 / TICKS;
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

/* How the computer plays: a skirmish's builds its base, keeps builders and
 * keeps its factories busy; a campaign's follows its scripts and builds
 * from its factories now and then */
static struct {
    int base;        /* builds its base with its builders */
    int build_gap;   /* seconds between a factory's units */
    int builders;    /* builders it keeps */
} ai;
static int difficulty_level;

static int enemy_builders(void) {
    int i, n = 0;
    for (i = 0; i < unit_hi; i++)
        if (units[i].alive && units[i].owner == ENEMY && unittypes[units[i].type].kind == UK_MOBILE &&
            (unittypes[units[i].type].flags & UF_BUILDER))
            n++;
    return n;
}

/* Idle Core factories build, as TA's computer player would, from three minutes in */
static void computer_build(void) {
    int i;
    if (sim_tick_count < next_build)
        return;
    /* a skirmish's factories hardly stop: the computer has an army to raise */
    next_build = sim_tick_count + TICKS * ai.build_gap;
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
        /* a skirmish's computer keeps a few builders for its base */
        if (enemy_builders() < ai.builders) {
            for (k = 0; k < t->nbuild; k++)
                if (unittypes[t->build[k]].kind == UK_MOBILE && (unittypes[t->build[k]].flags & UF_BUILDER) &&
                    !(unittypes[t->build[k]].flags & UF_FLY)) {
                    pick = t->build[k];
                    break;
                }
        }
        if (pick < 0 || players[ENEMY].metal < (int32_t)unittypes[pick].metalcost << 7)
            continue;
        u->fq_type[0] = (uint8_t)pick;
        u->fq_count[0] = 1;
    }
}

/* ------------------------------------------------------------ a skirmish's base */

/* What a skirmish's computer builds, by what each building does */
enum { ROLE_MEX, ROLE_ENERGY, ROLE_FACTORY, ROLE_DEFENCE, ROLE_NONE };

static int role_of(const UnitType* t) {
    if (t->kind == UK_MOBILE)
        return ROLE_NONE;
    if (t->flags & UF_MEX)
        return ROLE_MEX;
    if (t->kind == UK_FACTORY)
        return ROLE_FACTORY;
    if (t->flags & UF_ARMED)
        return ROLE_DEFENCE;
    if ((energy_net(t) > 0 || (t->flags & UF_WIND)) &&
        !(t->flags & (UF_MAKER | UF_WATERBLD)))
        return ROLE_ENERGY;
    return ROLE_NONE;
}

/* The cheapest of a builder's options in a role (factories: the first that
 * can stand on land), or -1 */
static int pick_option(const UnitType* b, int role) {
    int k, best = -1;
    for (k = 0; k < b->nbuild; k++) {
        int o = b->build[k];
        const UnitType* t = &unittypes[o];
        if (role_of(t) != role || (t->flags & UF_GEO))
            continue;
        if (role == ROLE_FACTORY && (t->flags & UF_WATERBLD))
            continue;
        if (best < 0 || t->metalcost < unittypes[best].metalcost)
            best = o;
    }
    return best;
}

/* Whether a building at x, z would stand in the lane in front of a factory:
 * 80 pixels wide, from the factory's front edge to 128 pixels out */
static int before_factory(const UnitType* t, int x, int z) {
    int i, x0 = x - t->footx * 8 - 32, x1 = x + t->footx * 8 + 32;
    int z0 = z - t->footz * 8 - 32, z1 = z + t->footz * 8 + 32;
    for (i = 0; i < unit_hi; i++) {
        const Unit* f = &units[i];
        const UnitType* ft;
        int fx, fz;
        if (!f->alive)
            continue;
        ft = &unittypes[f->type];
        if (ft->kind != UK_FACTORY)
            continue;
        fx = f->x >> FIX;
        fz = (f->z >> FIX) + ft->footz * 8;
        if (x1 > fx - 40 && x0 < fx + 40 && z1 > fz - 8 && z0 < fz + 128)
            return 1;
    }
    return 0;
}

/* A place for a building near x, z: for an extractor the richest metal in
 * reach, for the rest the first place that fits, ring by ring, on a grid
 * that leaves room to walk between buildings */
static int find_site(int type, int x, int z, int* bx, int* bz) {
    const UnitType* t = &unittypes[type];
    int r, a, best = -1;
    if (t->flags & UF_MEX) {
        int dx, dz;
        for (dz = -480; dz <= 480; dz += 16)
            for (dx = -480; dx <= 480; dx += 16) {
                int cx = (x + dx) >> 4, cz = (z + dz) >> 4, m, sx, sz;
                if (cx < 0 || cz < 0 || cx >= mission.w || cz >= mission.h)
                    continue;
                m = mission.metal[cz * mission.w + cx];
                if (m <= best || dx * dx + dz * dz > 480 * 480)
                    continue;
                sx = snap_build_x(type, x + dx);
                sz = snap_build_z(type, z + dz);
                if (can_build_at(type, sx, sz)) {
                    best = m;
                    *bx = sx;
                    *bz = sz;
                }
            }
        return best >= 0;
    }
    for (r = 96; r <= 640; r += 48)
        for (a = 0; a < 65536; a += 65536 / (r / 12)) {
            int px = x + (int)(((int32_t)SIN((uint16_t)a) * r) >> 14);
            int pz = z + (int)(((int32_t)COS((uint16_t)a) * r) >> 14);
            int sx = snap_build_x(type, px), sz = snap_build_z(type, pz);
            /* the footprint with a path cell (32 pixels) to spare round it,
             * as a building touching a path cell closes it, and off the
             * ground in front of a factory, where its units come out */
            if (can_build_at(type, sx, sz) && can_build_at(type, sx - 32, sz - 32) &&
                can_build_at(type, sx + 32, sz + 32) && can_build_at(type, sx - 32, sz + 32) &&
                can_build_at(type, sx + 32, sz - 32) && !before_factory(t, sx, sz)) {
                *bx = sx;
                *bz = sz;
                return 1;
            }
        }
    return 0;
}

/* A skirmish's computer builds its base as a player would: extractors and
 * power first, then a factory, more of each, defences, and more factories.
 * One idle builder is given work every two seconds. */
static int32_t next_base;

static void computer_base(void) {
    int i, n[4] = {0, 0, 0, 0};
    if (!ai.base || sim_tick_count < next_base)
        return;
    next_base = sim_tick_count + TICKS * 2;
    for (i = 0; i < unit_hi; i++) {
        const Unit* u = &units[i];
        int r;
        if (!u->alive || u->owner != ENEMY)
            continue;
        r = role_of(&unittypes[u->type]);
        if (r < ROLE_NONE)
            n[r]++;
    }
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        const UnitType* t = &unittypes[u->type];
        int role, type, x, z;
        Order o;
        if (!u->alive || u->owner != ENEMY || (u->flags & UF_UNBUILT) || u->norders || u->nscript ||
            t->kind != UK_MOBILE || !(t->flags & UF_BUILDER))
            continue;
        if (n[ROLE_MEX] < 2)
            role = ROLE_MEX;
        else if (n[ROLE_ENERGY] < 2)
            role = ROLE_ENERGY;
        else if (n[ROLE_FACTORY] < 1)
            role = ROLE_FACTORY;
        else if (n[ROLE_MEX] < 5)
            role = ROLE_MEX;
        else if (n[ROLE_ENERGY] < 5)
            role = ROLE_ENERGY;
        else if (n[ROLE_DEFENCE] < 2 + difficulty_level)
            role = ROLE_DEFENCE;
        else if (n[ROLE_FACTORY] < 2)
            role = ROLE_FACTORY;
        else if (n[ROLE_MEX] < 10)
            role = ROLE_MEX;
        else if (n[ROLE_ENERGY] < 10)
            role = ROLE_ENERGY;
        else if (n[ROLE_DEFENCE] < 4 + difficulty_level * 2)
            role = ROLE_DEFENCE;
        else
            role = n[ROLE_FACTORY] < 4 ? ROLE_FACTORY : ROLE_ENERGY;
        type = pick_option(t, role);
        if (type < 0 && role == ROLE_MEX)
            type = pick_option(t, ROLE_ENERGY); /* no deposits left: power */
        if (type < 0 || players[ENEMY].metal < (int32_t)unittypes[type].metalcost << 6)
            continue;
        if (!find_site(type, u->homex, u->homez, &x, &z))
            continue;
        memset(&o, 0, sizeof o);
        o.kind = O_BUILD;
        o.a = (uint8_t)type;
        o.x = (int16_t)x;
        o.z = (int16_t)z;
        o.target = -1;
        order_add(u, &o, 0);
        return;
    }
}

/* A Core unit a factory built that has nothing to do: it joins the next wave */
static int idle_raider(const Unit* u) {
    const UnitType* t = &unittypes[u->type];
    return u->alive && u->owner == ENEMY && !(u->flags & UF_UNBUILT) && t->kind == UK_MOBILE &&
           (t->flags & UF_ARMED) && !u->norders && !u->nscript && u->built_by >= 0;
}

/* The computer's silos fire what they hold at one of the player's buildings,
 * now and then */
static void computer_nukes(void) {
    int i, target = -1, n = 0;
    if (sim_tick_count % (TICKS * 20))
        return;
    for (i = 0; i < unit_hi; i++)
        if (units[i].alive && units[i].owner == PLAYER && unittypes[units[i].type].kind != UK_MOBILE &&
            rand16() % ++n == 0)
            target = i;
    if (target < 0)
        return;
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        const UnitType* t = &unittypes[u->type];
        int w = manual_weapon(t);
        if (u->alive && u->owner == ENEMY && u->stock && w >= 0 && !u->norders &&
            (weapons[t->weapon[w]].flags2 & WF2_NUKE)) {
            order_at(u, O_DGUN, units[target].x >> FIX, units[target].z >> FIX, 0);
            return;
        }
    }
}

static void computer_step(void) {
    int i, n = 0, cx = 0, cz = 0, best = -1, bestd = 1 << 30;
    computer_build();
    computer_base();
    computer_nukes();
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
        int x, z, k, t = best, td = 1 << 30;
        if (!idle_raider(u))
            continue;
        /* the wave's target, or else the nearest of the player's it can get to */
        if (!reachable(u, units[best].x >> FIX, units[best].z >> FIX)) {
            t = -1;
            for (k = 0; k < unit_hi; k++) {
                const Unit* v = &units[k];
                int d;
                if (!v->alive || v->owner != PLAYER || !reachable(u, v->x >> FIX, v->z >> FIX))
                    continue;
                d = dist((v->x - u->x) >> FIX, (v->z - u->z) >> FIX);
                if (d < td) {
                    td = d;
                    t = k;
                }
            }
            if (t < 0)
                continue;
        }
        x = (units[t].x >> FIX) + (rand16() & 63) - 32;
        z = (units[t].z >> FIX) + (rand16() & 63) - 32;
        order_at(u, O_FIGHT, x, z, 0);
    }
}

/* ------------------------------------------------------------ objectives */

/* A mission's objectives: whichever of its winning conditions comes first
 * wins (TA's missions offer them as alternatives, such as destroying every
 * Core unit or capturing the Gate); any losing one loses. A mission with no
 * winning condition is won when the computer has no units left. */
static void check_objectives(void) {
    static uint16_t have[MAX_UNITTYPES], have_any[MAX_UNITTYPES], en_have[MAX_UNITTYPES];
    int i, c, won = 0, lost = 0, any_victory = 0, en_commander = 0;
    int pl_units = 0, en_units = 0;
    memset(have, 0, sizeof have);
    memset(have_any, 0, sizeof have_any);
    memset(en_have, 0, sizeof en_have);
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        if (!u->alive)
            continue;
        have_any[u->type]++;
        if (u->owner == PLAYER) {
            pl_units++;
            if (!(u->flags & UF_UNBUILT))
                have[u->type]++;
        } else {
            en_units++;
            en_have[u->type]++;
            if (unittypes[u->type].flags & UF_COMMANDER)
                en_commander = 1;
        }
    }
    for (c = 0; c < mission.ncond; c++) {
        const Condition* k = &mission.cond[c];
        int t = k->type >= 0 && k->type < nunittypes ? k->type : -1;
        switch (k->kind) {
        case COND_MOVETO:
            any_victory = 1;
            for (i = 0; i < unit_hi; i++) {
                Unit* u = &units[i];
                if (u->alive && u->owner == PLAYER && !(u->flags & UF_UNBUILT) &&
                    unittypes[u->type].kind == UK_MOBILE && (t < 0 || u->type == t) &&
                    within((u->x >> FIX) - k->a, (u->z >> FIX) - k->b, k->c + 1))
                    won = 1;
            }
            break;
        case COND_KILLTYPE:
            any_victory = 1;
            if (t >= 0 && kills_of_type[t] >= (k->a > 0 ? k->a : 1))
                won = 1;
            break;
        case COND_DESTROYALL:
            any_victory = 1;
            if (!en_units)
                won = 1;
            break;
        case COND_CAPTURE:
        case COND_BUILD:
            any_victory = 1;
            if (t >= 0 && have[t])
                won = 1;
            break;
        case COND_KILLCOMMANDER:
            any_victory = 1;
            if (enemy_had_commander && !en_commander)
                won = 1;
            break;
        case COND_KILLALLOFTYPE:
            any_victory = 1;
            if (t >= 0 && (existed[t] & 2) && !en_have[t])
                won = 1;
            break;
        case COND_TYPEPASSESZ:
            any_victory = 1;
            for (i = 0; i < unit_hi; i++) {
                Unit* u = &units[i];
                if (u->alive && u->owner == PLAYER && !(u->flags & UF_UNBUILT) && u->type == t &&
                    (u->z >> FIX) < k->a)
                    won = 1;
            }
            break;
        case COND_COMMANDER:
            if (had_commander) {
                int alive = 0;
                for (i = 0; i < nunittypes; i++)
                    if ((unittypes[i].flags & UF_COMMANDER) && have[i])
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
            if (t >= 0 && (existed[t] & 1) && !have_any[t])
                lost = 1;
            break;
        case COND_TYPEKILLED:
            if (t >= 0 && deaths_of_type[t] >= (k->a > 0 ? k->a : 1))
                lost = 1;
            break;
        case COND_ANYPASSESZ:
            for (i = 0; i < unit_hi; i++) {
                Unit* u = &units[i];
                if (u->alive && u->owner == ENEMY && unittypes[u->type].kind == UK_MOBILE &&
                    (u->z >> FIX) < k->a)
                    lost = 1;
            }
            break;
        case COND_TIMER:
            if (!sim_timer)
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

/* The map's arrays, in one block */
static int map_alloc(void) {
    int32_t nb = (int32_t)bw * bh, np = (int32_t)ph << pshift;
    int32_t no = ((int32_t)mission.w * mission.h + 7) / 8;
    uint8_t* p;
    int i;
    sim_stop();
    map_block = tt_alloc_any(nb * 4 + np * (6 + nclasses) + no + 16);
    if (!map_block)
        return 0;
    p = map_block;
    bucket = (int16_t*)p;
    p += nb * 2;
    gcost = (uint16_t*)p;
    p += np * 2;
    seen = p;
    p += nb;
    radar = p;
    p += nb;
    pblock = p;
    p += np;
    pbuilt = p;
    p += np;
    stampv = p;
    p += np;
    from = p;
    p += np;
    for (i = 0; i < nclasses; i++, p += np)
        regions[i] = p;
    occbits = p;
    memset(occbits, 0, (size_t)no);
    memset(pbuilt, 0, (size_t)np);
    memset(stampv, 0, (size_t)np);
    cur_stamp = 0;
    return 1;
}

void sim_stop(void) {
    tt_free(map_block);
    map_block = 0;
}

int sim_start(int schema) {
    const Schema* s = &mission.schema[schema < mission.nschema ? schema : 0];
    int i, j;
    memset(units, 0, sizeof units);
    memset(projs, 0, sizeof projs);
    memset(fx, 0, sizeof fx);
    memset(beams, 0, sizeof beams);
    memset(players, 0, sizeof players);
    memset(kills_of_type, 0, sizeof kills_of_type);
    memset(deaths_of_type, 0, sizeof deaths_of_type);
    memset(existed, 0, sizeof existed);
    memset(placed_idx, 0xFF, sizeof placed_idx);
    enemy_had_commander = 0;
    sim_timer = -1;
    unit_hi = 0;
    path_rr = 0;
    sr.u = -1;
    sim_tick_count = 0;
    sim_last_fight = -10000;
    sim_result = 0;
    had_commander = 0;
    next_wave = TICKS * 60;
    difficulty_level = schema < 3 ? schema : 1;
    ai.base = mission.skirmish;
    ai.build_gap = mission.skirmish ? 6 - difficulty_level * 2 : 40 - difficulty_level * 12;
    ai.builders = mission.skirmish ? 3 : 0;
    next_build = TICKS * (mission.skirmish ? 10 : 180);
    next_base = 0;
    last_warning = -1000;
    sim_message_tick = -1000;
    bw = (mission.w * 16 + BUCKET - 1) / BUCKET;
    bh = (mission.h * 16 + BUCKET - 1) / BUCKET;
    pw = (mission.w + 1) / 2;
    ph = (mission.h + 1) / 2;
    for (pshift = 0; (1 << pshift) < pw; pshift++)
        ;
    pmask = (1 << pshift) - 1;
    if (!map_alloc())
        return 0;
    for (j = 0; j < ph; j++)
        for (i = 0; i < pw; i++)
            update_pcell(i, j);
    label_regions();
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
        placed_idx[i] = (int16_t)n;
        existed[p->type] |= (uint8_t)(p->player ? 3 : 1);
        if (p->player && (t->flags & UF_COMMANDER))
            enemy_had_commander = 1;
        units[n].heading = 0;
        units[n].hp = (int32_t)t->maxhp * (p->hp ? p->hp : 100) / 100;
        units[n].script = (int16_t)p->script;
        units[n].nscript = (int16_t)p->nops;
    }
    recount_storage(PLAYER);
    recount_storage(ENEMY);
    for (i = 0; i < mission.ncond; i++)
        if (mission.cond[i].kind == COND_TIMER)
            sim_timer = (int32_t)mission.cond[i].a * TICKS;
    build_buckets();
    update_sight();
    feat_dirty_x0 = 0;
    feat_dirty_x1 = -1;
    return 1;
}

void sim_tick(void) {
    int i;
    sim_tick_count++;
    build_buckets();
    for (i = 0; i < unit_hi; i++)
        if (units[i].alive)
            unit_step(i);
    paths_step();
    projectiles_step();
    economy_step();
    computer_step();
    if (sim_timer > 0) {
        sim_timer--;
        if (sim_timer % (TICKS * 30) == 0 || (sim_timer < TICKS * 30 && sim_timer % (TICKS * 5) == 0)) {
            char s[40];
            sprintf(s, "Time left %ld:%02ld", (long)(sim_timer / TICKS / 60), (long)(sim_timer / TICKS % 60));
            message(s);
        }
    }
    if ((sim_tick_count & 7) == 0)
        update_sight();
    if (sim_tick_count % TICKS == 0 && !sim_result)
        check_objectives();
}

/* ------------------------------------------------------------ picking */

int unit_at(int x, int sy, int owner_mask, int (*shown)(const Unit*)) {
    int i, best = -1, bestd = 1 << 30;
    for (i = 0; i < unit_hi; i++) {
        const Unit* u = &units[i];
        const UnitType* t;
        int ux, uy, r, d;
        if (!u->alive || !(owner_mask & (1 << u->owner)))
            continue;
        t = &unittypes[u->type];
        ux = u->x >> FIX;
        /* far from the point: no need for its height or a square root */
        if (ux < x - 80 || ux > x + 80 || (u->z >> FIX) < sy - 80 || (u->z >> FIX) > sy + 260)
            continue;
        if (!shown(u))
            continue;
        uy = unit_draw_y(u) - t->top / 2;
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
        if (!f->alive || !(feat_type(f->type)->flags & FF_RECLAIM))
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
