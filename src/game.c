/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A mission on screen (see game.h).
 */
#include <string.h>
#include <stdio.h>
#include "game.h"
#include "tt.h"
#include "gfx.h"
#include "data.h"
#include "sim.h"
#include "snd.h"

TestHooks test_hooks;
int32_t game_frames;
uint32_t game_draw_hz, game_sim_hz;
int32_t game_ticks_run;
int game_fast;

static int camx, camy, mapw, maph;
static int mode;            /* what a left click on the battlefield does */
static int build_type = -1; /* the building being placed */
static int page;            /* side panel: 0 build, 1 orders */
static int build_page;
static uint8_t prev_buttons;
static int drag, drag_x, drag_y; /* a box being dragged, in screen pixels */
static int minimap_ox, minimap_oy;
static int paused;
static int test_mx = -1, test_my, test_mb;
static uint16_t test_k;
static int nselected;

enum { M_NONE, M_MOVE, M_ATTACK, M_PATROL, M_GUARD, M_DGUN, M_RECLAIM, M_BUILD };

/* ------------------------------------------------------------ helpers */

static int mx(void) {
    return test_mx >= 0 ? test_mx : mouse_x;
}

static int my(void) {
    return test_mx >= 0 ? test_my : mouse_y;
}

static int mb(void) {
    return test_mx >= 0 ? test_mb : mouse_buttons;
}

int input_x(void) {
    return mx();
}

int input_y(void) {
    return my();
}

int input_buttons(void) {
    return mb();
}

uint16_t input_key(void) {
    uint16_t k = test_k ? test_k : tt_key();
    test_k = 0;
    return k;
}

void test_mouse(int x, int y, int buttons) {
    test_mx = x;
    test_my = y;
    test_mb = buttons;
}

void test_key(uint16_t key) {
    test_k = key;
}

static void clamp_camera(void) {
    if (camx > mapw - VIEW_W)
        camx = mapw - VIEW_W;
    if (camy > maph - VIEW_H)
        camy = maph - VIEW_H;
    if (camx < 0)
        camx = 0;
    if (camy < 0)
        camy = 0;
    camx &= ~15;
}

static void centre_on(int x, int z) {
    camx = x - VIEW_W / 2;
    camy = z - VIEW_H / 2;
    clamp_camera();
}

void game_list_features(void) {
    int i, n = 0;
    for (i = 0; i < nfeats && n < 8; i++) {
        const Feature* f = &feats[i];
        int sx = VIEW_X + f->x - camx, sy = VIEW_Y + ground_screen_y(f->x, f->z) - camy - 8;
        if (!f->alive || sx < VIEW_X + 16 || sx > VIEW_X + VIEW_W - 16 || sy < VIEW_Y + 16 ||
            sy > VIEW_Y + VIEW_H - 16)
            continue;
        test_log(
            "feature %d %s on screen at %d,%d metal %ld energy %ld",
            i,
            common.feat[f->type].name,
            sx,
            sy,
            (long)f->metal,
            (long)f->energy
        );
        n++;
    }
}

void game_camera(int x, int z) {
    centre_on(x, z);
}

/* The battlefield point under a screen point, z found through the ground's height */
static void screen_to_world(int sx, int sy, int* wx, int* wz) {
    int x = sx - VIEW_X + camx, y = sy - VIEW_Y + camy, z = y, i;
    for (i = 0; i < 2; i++)
        z = y + ground_height(x, z) / 2;
    *wx = x;
    *wz = z;
}

static int in_view(int x, int y) {
    return x >= VIEW_X && x < VIEW_X + VIEW_W && y >= VIEW_Y && y < VIEW_Y + VIEW_H;
}

/* How far along an unbuilt unit is, out of scale */
static int built_of(const Unit* u, int scale) {
    return (int)((int64_t)u->progress * scale / ((int64_t)unittypes[u->type].buildtime * TICKS));
}

/* The battlefield point a point on the minimap stands for */
static void minimap_to_world(int x, int y, int* wx, int* wz) {
    *wx = (x - minimap_ox) * mapw / mission.mw;
    *wz = (y - minimap_oy) * maph / mission.mh;
}

/* What the selection holds: how many units of each sort, and the first of them */
enum { SEL_ALL, SEL_MOBILE, SEL_ARMED, SEL_BUILDER, SEL_MOBILE_BUILDER, SEL_FACTORY, SEL_SORTS };

static struct {
    int32_t frame, tick, version; /* when it was counted */
    int n[SEL_SORTS], first[SEL_SORTS];
} sel;

static int32_t sel_version; /* goes up when the player changes the selection */

static void sel_add(int sort, int i) {
    if (!sel.n[sort]++)
        sel.first[sort] = i;
}

/* How many selected units are of a sort, counted once a frame and tick; first
 * gets the lowest numbered (-1 when none) */
static int selected(int sort, int* first) {
    int i;
    if (sel.frame != game_frames || sel.tick != sim_tick_count || sel.version != sel_version) {
        sel.frame = game_frames;
        sel.tick = sim_tick_count;
        sel.version = sel_version;
        for (i = 0; i < SEL_SORTS; i++) {
            sel.n[i] = 0;
            sel.first[i] = -1;
        }
        for (i = 0; i < unit_hi; i++) {
            const Unit* u = &units[i];
            const UnitType* t;
            if (!u->alive || !(u->flags & UF_SELECTED))
                continue;
            t = &unittypes[u->type];
            sel_add(SEL_ALL, i);
            if (t->kind == UK_MOBILE)
                sel_add(SEL_MOBILE, i);
            if (t->flags & UF_ARMED)
                sel_add(SEL_ARMED, i);
            if (t->flags & UF_BUILDER) {
                sel_add(SEL_BUILDER, i);
                if (t->kind == UK_MOBILE)
                    sel_add(SEL_MOBILE_BUILDER, i);
                if (t->kind == UK_FACTORY)
                    sel_add(SEL_FACTORY, i);
            }
        }
    }
    if (first)
        *first = sel.first[sort];
    return sel.n[sort];
}

/* The first selected unit of a sort answers an order */
static void acknowledge(int sort) {
    int f;
    if (selected(sort, &f))
        snd_play(unittypes[units[f].type].snd_ok, 1);
}

/* A unit a box or Ctrl+A selects */
static int selectable(const Unit* u) {
    return u->alive && u->owner == PLAYER && unittypes[u->type].kind == UK_MOBILE &&
           !(u->flags & UF_UNBUILT);
}

static void deselect_all(void) {
    int i;
    sel_version++;
    for (i = 0; i < unit_hi; i++)
        units[i].flags &= (uint8_t)~UF_SELECTED;
}

static void selection_changed(void) {
    sel_version++;
    nselected = selected(SEL_ALL, 0);
    build_page = 0;
    page = selected(SEL_BUILDER, 0) ? 0 : 1;
    if (mode == M_BUILD)
        mode = M_NONE;
}

/* The unit whose build menu the side panel shows: a builder, preferring a factory */
static int menu_unit(void) {
    int f;
    if (selected(SEL_FACTORY, &f))
        return f;
    selected(SEL_BUILDER, &f);
    return f;
}

/* ------------------------------------------------------------ orders from clicks */

static void order_selected_move(int wx, int wz, int kind, int queue) {
    int i, n = selected(SEL_MOBILE, 0), cols = 1, k = 0;
    while (cols * cols < n)
        cols++;
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        if (!u->alive || !(u->flags & UF_SELECTED))
            continue;
        if (unittypes[u->type].kind != UK_MOBILE)
            continue;
        if (kind == O_PATROL) {
            u->homex = (int16_t)(u->x >> FIX);
            u->homez = (int16_t)(u->z >> FIX);
        }
        /* a block of places around the point, 28 pixels apart */
        order_at(
            u,
            kind,
            wx + ((k % cols) - (cols - 1) / 2) * 28,
            wz + ((k / cols) - (cols - 1) / 2) * 28,
            queue
        );
        k++;
    }
    acknowledge(SEL_MOBILE);
}

static void order_selected_unit(int kind, int target, int queue) {
    int i, any = 0;
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        const UnitType* t;
        Order o;
        if (!u->alive || !(u->flags & UF_SELECTED) || i == target)
            continue;
        t = &unittypes[u->type];
        if (kind == O_ATTACK && !(t->flags & UF_ARMED))
            continue;
        if (kind == O_RECLAIM && !((t->flags & UF_BUILDER) && t->kind == UK_MOBILE))
            continue;
        if (kind == O_GUARD && t->kind != UK_MOBILE)
            continue;
        memset(&o, 0, sizeof o);
        o.kind = (uint8_t)kind;
        o.target = (int16_t)target;
        order_add(u, &o, queue);
        if (kind == O_ATTACK)
            u->target = (int16_t)target;
        any = 1;
    }
    if (any)
        acknowledge(SEL_ALL);
}

static void order_build(int wx, int wz, int queue) {
    int i, x = snap_build_x(build_type, wx), z = snap_build_z(build_type, wz), any = 0;
    if (!can_build_at(build_type, x, z)) {
        snd_play(SND_CANTDO, 1);
        return;
    }
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        const UnitType* t;
        Order o;
        int k, ok = 0;
        if (!u->alive || !(u->flags & UF_SELECTED))
            continue;
        t = &unittypes[u->type];
        if (!(t->flags & UF_BUILDER) || t->kind != UK_MOBILE)
            continue;
        for (k = 0; k < t->nbuild; k++)
            if (t->build[k] == build_type)
                ok = 1;
        if (!ok)
            continue;
        memset(&o, 0, sizeof o);
        o.kind = O_BUILD;
        o.a = (uint8_t)build_type;
        o.x = (int16_t)x;
        o.z = (int16_t)z;
        o.target = -1;
        order_add(u, &o, queue);
        any = 1;
    }
    if (any)
        snd_play(SND_ADDBUILD, 1);
}

static void click_battlefield(int sx, int sy, int queue) {
    int wx, wz, wsy = sy - VIEW_Y + camy, wxs = sx - VIEW_X + camx;
    int own = unit_at(wxs, wsy, 1 << PLAYER);
    int enemy = unit_at(wxs, wsy, 1 << ENEMY);
    screen_to_world(sx, sy, &wx, &wz);
    switch (mode) {
    case M_BUILD:
        order_build(wx, wz, queue);
        if (!queue)
            mode = M_NONE;
        return;
    case M_MOVE:
        order_selected_move(wx, wz, O_MOVE, queue);
        mode = M_NONE;
        return;
    case M_PATROL:
        order_selected_move(wx, wz, O_PATROL, queue);
        mode = M_NONE;
        return;
    case M_ATTACK:
        if (enemy >= 0)
            order_selected_unit(O_ATTACK, enemy, queue);
        else
            order_selected_move(wx, wz, O_FIGHT, queue);
        mode = M_NONE;
        return;
    case M_GUARD:
        if (own >= 0)
            order_selected_unit(O_GUARD, own, queue);
        mode = M_NONE;
        return;
    case M_RECLAIM: {
        int f = feature_at(wxs, wsy);
        if (f >= 0)
            order_selected_unit(O_RECLAIM, f, queue);
        mode = M_NONE;
        return;
    }
    case M_DGUN: {
        int i;
        for (i = 0; i < unit_hi; i++) {
            Unit* u = &units[i];
            if (u->alive && (u->flags & UF_SELECTED) && manual_weapon(&unittypes[u->type]) >= 0)
                order_at(u, O_DGUN, wx, wz, queue);
        }
        mode = M_NONE;
        return;
    }
    }
    if (own >= 0) {
        if (!queue)
            deselect_all();
        units[own].flags ^= UF_SELECTED;
        selection_changed();
        snd_play(unittypes[units[own].type].snd_select, 1);
        return;
    }
    if (!nselected)
        return;
    if (enemy >= 0 && selected(SEL_ARMED, 0)) {
        order_selected_unit(O_ATTACK, enemy, queue);
        return;
    }
    {
        int f = feature_at(wxs, wsy);
        if (f >= 0 && selected(SEL_MOBILE_BUILDER, 0)) {
            order_selected_unit(O_RECLAIM, f, queue);
            return;
        }
    }
    order_selected_move(wx, wz, O_MOVE, queue);
}

static void box_select(int x0, int y0, int x1, int y1, int add) {
    int i, n = 0, t;
    if (x0 > x1) {
        t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        t = y0;
        y0 = y1;
        y1 = t;
    }
    x0 = x0 - VIEW_X + camx;
    x1 = x1 - VIEW_X + camx;
    y0 = y0 - VIEW_Y + camy;
    y1 = y1 - VIEW_Y + camy;
    if (!add)
        deselect_all();
    for (i = 0; i < unit_hi; i++) {
        Unit* u = &units[i];
        int ux, uy;
        if (!selectable(u))
            continue;
        ux = u->x >> FIX;
        uy = unit_screen_y(u);
        if (ux >= x0 && ux <= x1 && uy >= y0 && uy <= y1) {
            u->flags |= UF_SELECTED;
            n++;
        }
    }
    selection_changed();
    if (n) {
        int f;
        selected(SEL_ALL, &f);
        snd_play(unittypes[units[f].type].snd_select, 1);
    }
}

/* ------------------------------------------------------------ the side panel */

#define PIC_Y 154
#define BTN_Y 154

static int build_option(int unit, int slot) {
    const UnitType* t = &unittypes[units[unit].type];
    int k = build_page * 6 + slot;
    if (k >= t->nbuild)
        return -1;
    return t->build[k];
}

/* The ORDERS page's buttons and their keys; mode -1 is Stop */
static const struct {
    int btn, mode, x, y;
    char key;
} order_buttons[] = {
    {BTN_MOVE, M_MOVE, 6, BTN_Y, 'M'},
    {BTN_ATTACK, M_ATTACK, 68, BTN_Y, 'A'},
    {BTN_STOP, -1, 6, BTN_Y + 34, 'S'},
    {BTN_PATROL, M_PATROL, 68, BTN_Y + 34, 'P'},
    {BTN_GUARD, M_GUARD, 6, BTN_Y + 68, 'G'},
    {BTN_DGUN, M_DGUN, 68, BTN_Y + 68, 'D'},
    {BTN_RECLAIM, M_RECLAIM, 6, BTN_Y + 102, 'R'},
};

#define NORDER_BUTTONS ((int)(sizeof order_buttons / sizeof order_buttons[0]))

static void stop_selected(void) {
    int i;
    for (i = 0; i < unit_hi; i++)
        if (units[i].alive && (units[i].flags & UF_SELECTED))
            order_stop(&units[i]);
}

/* What an order button or its key does */
static void order_button(int b) {
    if (order_buttons[b].mode < 0)
        stop_selected();
    else if (nselected)
        mode = order_buttons[b].mode;
}

static void panel_click(int x, int y, int right) {
    int mu = menu_unit();
    /* the two tabs */
    if (y >= 130 && y < 150) {
        page = x < 64 ? 0 : 1;
        snd_play(SND_BUTTON, 1);
        return;
    }
    if (page == 0 && mu >= 0) {
        int slot;
        if (y >= PIC_Y && y < PIC_Y + 192) {
            slot = ((y - PIC_Y) / 64) * 2 + (x >= 64);
            {
                int bt = build_option(mu, slot);
                Unit* u = &units[mu];
                if (bt < 0)
                    return;
                if (unittypes[u->type].kind == UK_FACTORY) {
                    int n = (tt_shift() & 1) ? 5 : 1;
                    if (factory_queue(u, bt, right ? -n : n))
                        snd_play(SND_ADDBUILD, 1);
                } else {
                    build_type = bt;
                    mode = M_BUILD;
                    snd_play(SND_BUTTON, 1);
                }
            }
            return;
        }
        if (y >= PIC_Y + 196 && y < PIC_Y + 216) {
            int pages = (unittypes[units[mu].type].nbuild + 5) / 6;
            build_page = (build_page + (x < 64 ? pages - 1 : 1)) % (pages ? pages : 1);
            snd_play(SND_BUTTON, 1);
        }
        return;
    }
    {
        int i;
        for (i = 0; i < NORDER_BUTTONS; i++) {
            int bx = order_buttons[i].x, by = order_buttons[i].y;
            if (x >= bx && x < bx + 54 && y >= by && y < by + 30) {
                snd_play(SND_BUTTON, 1);
                if (order_buttons[i].mode < 0)
                    stop_selected();
                else
                    mode = order_buttons[i].mode;
                return;
            }
        }
    }
}

static void draw_button(uint8_t* scr, int b, int x, int y, int lit) {
    int f = lit && common.button_frames[b] > 1 ? 1 : 0;
    if (!common.button_frames[b])
        return;
    gfx_sprite(scr, common_sprite(common.button[b][f]), x, y);
}

static void draw_side_panel(uint8_t* scr) {
    int mu = menu_unit();
    char s[32];
    gfx_clip(0, 128, 127, 447);
    draw_button(scr, BTN_BUILD, 4, 132, page == 0);
    draw_button(scr, BTN_ORDERS, 66, 132, page == 1);
    if (page == 0 && mu >= 0) {
        const Unit* u = &units[mu];
        int slot;
        for (slot = 0; slot < 6; slot++) {
            int bt = build_option(mu, slot), px = (slot & 1) * 64, py = PIC_Y + (slot >> 1) * 64, n;
            if (bt < 0)
                continue;
            gfx_image(scr, (const uint16_t*)(common.bank + common.pic[bt]), px, py, 4, 64);
            n = factory_queued(u, bt);
            if (n) {
                sprintf(s, "%d", n);
                gfx_fill(scr, px + 2, py + 2, px + 4 + gfx_text_width(s), py + 13, C_BLACK);
                gfx_text(scr, px + 3, py + 2, s, C_YELLOW);
            }
            if (mode == M_BUILD && bt == build_type)
                gfx_rect(scr, px, py, px + 63, py + 63, C_GREEN);
            /* a frame being built shows how far along it is */
            if (u->nano >= 0 && units[u->nano].alive && units[u->nano].type == bt &&
                (units[u->nano].flags & UF_UNBUILT)) {
                int w = built_of(&units[u->nano], 60);
                gfx_fill(scr, px + 2, py + 58, px + 2 + w, py + 61, C_GREEN);
            }
        }
        if (unittypes[u->type].nbuild > 6) {
            draw_button(scr, BTN_PREV, 8, PIC_Y + 198, 0);
            draw_button(scr, BTN_NEXT, 74, PIC_Y + 198, 0);
        }
    } else {
        int i;
        for (i = 0; i < NORDER_BUTTONS; i++)
            draw_button(
                scr,
                order_buttons[i].btn,
                order_buttons[i].x,
                order_buttons[i].y,
                order_buttons[i].mode == mode
            );
        if (!nselected)
            gfx_text(scr, 8, BTN_Y + 150, "No units selected", C_LGREY);
    }
}

/* ------------------------------------------------------------ what each screen holds */

/* The battlefield is drawn from a background in TT-RAM: the terrain with the
 * trees and wrecks over it, for the current view. A screen that already shows
 * this view only has the 16x16 cells its moving things covered put back. */
#define BG_STRIDE (VIEW_W / 2)
#define CELL_COLS (VIEW_W / 16)
#define CELL_ROWS (VIEW_H / 16)

#define NMARKS (NSCREENS + 1)

typedef struct {
    uint8_t* scr;
    int32_t last_frame;                           /* the frame it was last drawn in */
    int32_t side_serial, bar_serial, mini_serial; /* the versions of ui's parts it shows */
    int32_t full_serial;                          /* the composite's full refresh it last copied */
    int panel_ok;
    int cur_x0, cur_y0, cur_x1, cur_y1;
    uint8_t extra[CELL_ROWS][CELL_COLS]; /* cells its own pointer covered */
} Screen;

static Screen screens[NSCREENS];
static Screen* S;
static uint8_t* bg;      /* terrain and features of the view */
static uint8_t* comp;    /* bg with this frame's moving things */
static uint8_t* comp_at; /* comp, addressed with screen coordinates */
static int bg_camx, bg_camy;
static int comp_camx = -1, comp_camy = -1;
static int32_t bg_version = -1, bg_stamp, comp_stamp = -1, comp_full_serial;
static uint8_t marks[NMARKS][CELL_ROWS][CELL_COLS];
static int mark_cur;
static int16_t feat_order[MAX_FEATS];
static int nfeat_order;

static void mark(int x0, int y0, int x1, int y1) {
    int r, c;
    if (x0 < VIEW_X)
        x0 = VIEW_X;
    if (y0 < VIEW_Y)
        y0 = VIEW_Y;
    if (x1 > VIEW_X + VIEW_W - 1)
        x1 = VIEW_X + VIEW_W - 1;
    if (y1 > VIEW_Y + VIEW_H - 1)
        y1 = VIEW_Y + VIEW_H - 1;
    if (x0 > x1 || y0 > y1)
        return;
    for (r = (y0 - VIEW_Y) >> 4; r <= (y1 - VIEW_Y) >> 4; r++)
        for (c = (x0 - VIEW_X) >> 4; c <= (x1 - VIEW_X) >> 4; c++)
            marks[mark_cur][r][c] = 1;
}

/* The screen rectangle a sprite drawn at x, y can cover */
static void sprite_box(const int16_t* spr, int x, int y, int* x0, int* y0, int* x1, int* y1) {
    *x0 = x - spr[2];
    *y0 = y - spr[3];
    *x1 = *x0 + spr[0] * 16 + 15;
    *y1 = *y0 + spr[1] - 1;
}

static void mark_sprite(const int16_t* spr, int x, int y) {
    int x0, y0, x1, y1;
    if (spr[0] <= 0)
        return;
    sprite_box(spr, x, y, &x0, &y0, &x1, &y1);
    mark(x0, y0, x1, y1);
}

static void draw_spr(uint8_t* scr, const int16_t* spr, int x, int y) {
    gfx_sprite(scr, spr, x, y);
    mark_sprite(spr, x, y);
}

static void draw_line(uint8_t* scr, int x0, int y0, int x1, int y1, int c) {
    gfx_line(scr, x0, y0, x1, y1, c);
    mark(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 > x1 ? x0 : x1, y0 > y1 ? y0 : y1);
}

static void draw_fill(uint8_t* scr, int x0, int y0, int x1, int y1, int c) {
    gfx_fill(scr, x0, y0, x1, y1, c);
    mark(x0, y0, x1, y1);
}

/* Features back to front, kept sorted as they come and go */
static void sort_features(void) {
    int i, j;
    nfeat_order = 0;
    for (i = 0; i < nfeats; i++)
        if (feats[i].alive && common.feat[feats[i].type].sprite != 0xFFFFFFFFUL &&
            !(common.feat[feats[i].type].flags & FF_BAKED))
            feat_order[nfeat_order++] = (int16_t)i;
    for (i = 1; i < nfeat_order; i++) {
        int16_t v = feat_order[i];
        for (j = i; j > 0 && feats[feat_order[j - 1]].z > feats[v].z; j--)
            feat_order[j] = feat_order[j - 1];
        feat_order[j] = v;
    }
}

/* A feature's sprite reaches FEAT_SIDE pixels either side of its place on
 * screen, FEAT_UP above it and FEAT_DOWN below */
#define FEAT_SIDE 80
#define FEAT_UP 96
#define FEAT_DOWN 16

/* Draws the background's rectangle x0..x1, y0..y1 (view pixels; x on group edges) */
static void bg_render(int x0, int y0, int x1, int y1) {
    int i, lo, hi, zlo, zhi;
    if (x0 > x1 || y0 > y1)
        return;
    gfx_stride = BG_STRIDE;
    gfx_terrain(
        bg + y0 * BG_STRIDE,
        mission.tiles,
        mission.tilemap,
        mission.tw,
        bg_camx + x0,
        bg_camy + y0,
        x0,
        0,
        x1 - x0 + 1,
        y1 - y0 + 1
    );
    gfx_clip(x0, y0, x1, y1);
    /* the features are in order of z, and a feature's y on screen is z less at
     * most 127 (half the greatest height): find the first that can reach */
    zlo = bg_camy + y0 - FEAT_DOWN;
    zhi = bg_camy + y1 + FEAT_UP + 127;
    lo = 0;
    hi = nfeat_order;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (feats[feat_order[mid]].z < zlo)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (i = lo; i < nfeat_order && feats[feat_order[i]].z <= zhi; i++) {
        const Feature* f = &feats[feat_order[i]];
        int sx = f->x - bg_camx, sy;
        if (sx < x0 - FEAT_SIDE || sx > x1 + FEAT_SIDE)
            continue;
        sy = ground_screen_y(f->x, f->z) - bg_camy;
        if (sy < y0 - FEAT_DOWN || sy > y1 + FEAT_UP)
            continue;
        gfx_sprite(bg, common_sprite(common.feat[f->type].sprite), sx, sy);
    }
    gfx_stride = LINE_BYTES;
}

/* Draws the background again where features came or went, and has the
 * composite and the screens take those cells from it */
static void bg_features_changed(void) {
    int x0 = feat_dirty_x0 - bg_camx - FEAT_SIDE, x1 = feat_dirty_x1 - bg_camx + FEAT_SIDE;
    int y0 = feat_dirty_y0 - bg_camy - FEAT_UP, y1 = feat_dirty_y1 - bg_camy + FEAT_DOWN;
    if (feat_dirty_x0 > feat_dirty_x1)
        return;
    feat_dirty_x0 = 0;
    feat_dirty_x1 = -1;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > VIEW_W - 1)
        x1 = VIEW_W - 1;
    if (y1 > VIEW_H - 1)
        y1 = VIEW_H - 1;
    if (x0 > x1 || y0 > y1)
        return;
    x0 &= ~15;
    x1 |= 15;
    bg_render(x0, y0, x1, y1);
    mark(VIEW_X + x0, VIEW_Y + y0, VIEW_X + x1, VIEW_Y + y1);
}

static void bg_update(void) {
    int dx = camx - bg_camx, dy = camy - bg_camy, y;
    if (bg_version != feat_version) {
        bg_version = feat_version;
        sort_features();
        bg_features_changed();
    }
    if (!dx && !dy)
        return;
    bg_stamp++;
    if (dx <= -VIEW_W || dx >= VIEW_W || dy <= -VIEW_H || dy >= VIEW_H) {
        bg_camx = camx;
        bg_camy = camy;
        bg_render(0, 0, VIEW_W - 1, VIEW_H - 1);
        return;
    }
    /* move what is still in view, then draw the strips that came into it */
    {
        int bytes = BG_STRIDE - (dx < 0 ? -dx : dx) / 2;
        int so = dx > 0 ? dx / 2 : 0, doff = dx < 0 ? -dx / 2 : 0;
        if (dy >= 0) {
            for (y = 0; y < VIEW_H - dy; y++)
                memmove(bg + y * BG_STRIDE + doff, bg + (y + dy) * BG_STRIDE + so, bytes);
        } else {
            for (y = VIEW_H - 1; y >= -dy; y--)
                memmove(bg + y * BG_STRIDE + doff, bg + (y + dy) * BG_STRIDE + so, bytes);
        }
    }
    bg_camx = camx;
    bg_camy = camy;
    if (dy > 0)
        bg_render(0, VIEW_H - dy, VIEW_W - 1, VIEW_H - 1);
    else if (dy < 0)
        bg_render(0, 0, VIEW_W - 1, -dy - 1);
    {
        int ry0 = dy > 0 ? 0 : -dy, ry1 = dy > 0 ? VIEW_H - dy - 1 : VIEW_H - 1;
        if (dx > 0)
            bg_render(VIEW_W - dx, ry0, VIEW_W - 1, ry1);
        else if (dx < 0)
            bg_render(0, ry0, -dx - 1, ry1);
    }
}

static void
copy_rows(uint8_t* dst, int dstride, const uint8_t* src, int sstride, int rows, int longs) {
    int y, i;
    for (y = 0; y < rows; y++, dst += dstride, src += sstride) {
        uint32_t* d = (uint32_t*)dst;
        const uint32_t* s2 = (const uint32_t*)src;
        for (i = 0; i < longs; i++)
            d[i] = s2[i];
    }
}

/* Readies the composite: the background where last frame's things were */
static void comp_begin(void) {
    int r, c, prev = mark_cur; /* the last frame's marks, before the ring moves on */
    if (comp_camx != camx || comp_camy != camy || comp_stamp != bg_stamp) {
        memcpy(comp, bg, (size_t)BG_STRIDE * VIEW_H);
        comp_camx = camx;
        comp_camy = camy;
        comp_stamp = bg_stamp;
        comp_full_serial++;
        memset(marks, 0, sizeof marks);
    } else {
        for (r = 0; r < CELL_ROWS; r++)
            for (c = 0; c < CELL_COLS; c++) {
                int n = 0;
                if (!marks[prev][r][c])
                    continue;
                while (c + n < CELL_COLS && marks[prev][r][c + n])
                    n++;
                copy_rows(
                    comp + r * 16 * BG_STRIDE + c * 8,
                    BG_STRIDE,
                    bg + r * 16 * BG_STRIDE + c * 8,
                    BG_STRIDE,
                    16,
                    n * 2
                );
                c += n - 1;
            }
    }
    mark_cur = (mark_cur + 1) % NMARKS;
    memset(marks[mark_cur], 0, sizeof marks[mark_cur]);
}

/* Copies to the screen what changed in the composite since it was last copied there */
static void comp_to_screen(uint8_t* scr) {
    int r, c;
    uint8_t* dst = scr + VIEW_Y * LINE_BYTES + VIEW_X / 2;
    if (S->full_serial != comp_full_serial || game_frames - S->last_frame >= NMARKS) {
        S->full_serial = comp_full_serial;
        S->last_frame = game_frames;
        copy_rows(dst, LINE_BYTES, comp, BG_STRIDE, VIEW_H, BG_STRIDE / 4);
        memset(S->extra, 0, sizeof S->extra);
        return;
    }
    for (r = 0; r < CELL_ROWS; r++)
        for (c = 0; c < CELL_COLS; c++) {
            int n = 0;
#define CHANGED(rr, cc)                                                                            \
    (marks[0][rr][cc] | marks[1][rr][cc] | marks[2][rr][cc] | marks[3][rr][cc] | S->extra[rr][cc])
            if (!CHANGED(r, c))
                continue;
            while (c + n < CELL_COLS && CHANGED(r, c + n))
                n++;
            copy_rows(
                dst + r * 16 * LINE_BYTES + c * 8,
                LINE_BYTES,
                comp + r * 16 * BG_STRIDE + c * 8,
                BG_STRIDE,
                16,
                n * 2
            );
            c += n - 1;
        }
    memset(S->extra, 0, sizeof S->extra);
    S->last_frame = game_frames;
}

/* ------------------------------------------------------------ the bars */

static void
draw_resource(uint8_t* scr, int x, int32_t have, int32_t store, int32_t income, int colour) {
    char s[32];
    int w = store > 0 ? (int)((int64_t)(have >> 8) * 100 / store) : 0;
    if (w > 100)
        w = 100;
    if (w < 0)
        w = 0;
    gfx_fill(scr, x, 9, x + 100, 12, C_BLACK);
    gfx_fill(scr, x, 9, x + w, 12, colour);
    sprintf(s, "%ld", (long)(have >> 8));
    gfx_text(scr, x, 16, s, C_WHITE);
    sprintf(s, "%ld", (long)store);
    gfx_text(scr, x + 100 - gfx_text_width(s), 16, s, C_LGREY);
    sprintf(
        s,
        "+%ld.%ld",
        (long)(income * TICKS / 256),
        (long)((income < 0 ? -income : income) * TICKS * 10 / 256 % 10)
    );
    gfx_text(scr, x + 108, 5, s, income >= 0 ? C_GREEN : C_RED);
}

/* The unit under the pointer, found once a frame */
static int hover_unit(void) {
    static int32_t frame = -1;
    static int hover;
    if (frame != game_frames) {
        frame = game_frames;
        hover = in_view(mx(), my()) ? unit_at(mx() - VIEW_X + camx, my() - VIEW_Y + camy, 3) : -1;
    }
    return hover;
}

/* The unit type of the build picture under the pointer, or -1 */
static int hover_pic(void) {
    int mu = menu_unit(), x = mx(), y = my();
    if (page != 0 || mu < 0 || x >= 128 || y < PIC_Y || y >= PIC_Y + 192)
        return -1;
    return build_option(mu, ((y - PIC_Y) / 64) * 2 + (x >= 64));
}

static uint32_t bars_key(void) {
    const Player* p = &players[PLAYER];
    uint32_t k = (uint32_t)(p->metal >> 8) * 31 + (uint32_t)(p->energy >> 8) * 17 +
                 (uint32_t)p->mstore * 7 + (uint32_t)p->estore * 13 + (uint32_t)p->mincome * 3 +
                 (uint32_t)p->eincome * 5;
    int h = hover_unit(), sel = -1;
    if (h >= 0)
        sel = h;
    else if (nselected)
        selected(SEL_ALL, &sel);
    k = k * 33 + (uint32_t)(sel + 1) * 1009 + (uint32_t)nselected * 77;
    if (sel >= 0)
        k += (uint32_t)units[sel].hp * 3 + (units[sel].progress >> 6) + units[sel].paralyzed;
    k += (sim_tick_count - sim_message_tick < TICKS * 4) ? (uint32_t)sim_message_tick : 0;
    k += (uint32_t)(hover_pic() + 2) * 7919;
    return k;
}

static void draw_bars(uint8_t* scr) {
    const Player* p = &players[PLAYER];
    char s[96];
    int sel = -1, hover;
    gfx_clip_full();
    draw_resource(scr, 212, p->metal, p->mstore, p->mincome, C_SILVER);
    draw_resource(scr, 466, p->energy, p->estore, p->eincome, C_YELLOW);
    hover = hover_unit();
    if (hover >= 0)
        sel = hover;
    else if (nselected)
        selected(SEL_ALL, &sel);
    if (hover_pic() >= 0)
        sel = -1;
    if (sel >= 0) {
        const Unit* u = &units[sel];
        const UnitType* t = &unittypes[u->type];
        int w = (int)((int64_t)u->hp * 100 / (t->maxhp ? t->maxhp : 1));
        if (w < 0)
            w = 0;
        if (nselected > 1 && hover < 0)
            sprintf(s, "%s - %s  (%d selected)", t->title, t->desc, nselected);
        else
            sprintf(s, "%s - %s", t->title, t->desc);
        gfx_text(scr, 140, 452, s, u->owner == PLAYER ? C_WHITE : C_RED);
        gfx_fill(scr, 140, 468, 240, 472, C_BLACK);
        gfx_fill(scr, 140, 468, 140 + w, 472, w > 50 ? C_GREEN : (w > 25 ? C_YELLOW : C_RED));
        if (u->flags & UF_UNBUILT) {
            sprintf(s, "Being built: %d%%", built_of(u, 100));
            gfx_text(scr, 250, 464, s, C_GREEN);
        } else if (u->paralyzed) {
            gfx_text(scr, 250, 464, "Paralyzed", C_YELLOW);
        } else {
            sprintf(s, "%ld/%d", (long)u->hp, t->maxhp);
            gfx_text(scr, 250, 464, s, C_LGREY);
        }
    }
    {
        int bt = hover_pic();
        if (bt >= 0) {
            const UnitType* t = &unittypes[bt];
            sprintf(s, "%s - %s", t->title, t->desc);
            gfx_text(scr, 140, 452, s, C_WHITE);
            sprintf(
                s,
                "Metal %lu   Energy %lu",
                (unsigned long)t->metalcost,
                (unsigned long)t->energycost
            );
            gfx_text(scr, 140, 464, s, C_YELLOW);
        }
    }
    if (sim_tick_count - sim_message_tick < TICKS * 4)
        gfx_text(scr, 420, 464, sim_message, C_YELLOW);
}

static uint32_t side_key(void) {
    int mu = menu_unit(), i;
    uint32_t k = (uint32_t)page * 7 + (uint32_t)mode * 131 + (uint32_t)(build_type + 1) * 1031 +
                 (uint32_t)build_page * 17 + (uint32_t)(mu + 1) * 4099 + (uint32_t)nselected * 3;
    if (mu >= 0) {
        const Unit* u = &units[mu];
        for (i = 0; i < 6; i++)
            k = k * 31 + u->fq_count[i] * 7 + u->fq_type[i];
        if (u->nano >= 0 && units[u->nano].alive)
            k += units[u->nano].progress >> 8;
    }
    return k;
}

static void draw_minimap(uint8_t* scr) {
    int i;
    int ox = minimap_ox, oy = minimap_oy;
    gfx_image(scr, mission.minimap, ox, oy, mission.mw / 16, mission.mh);
    gfx_clip(ox, oy, ox + mission.mw - 1, oy + mission.mh - 1);
    for (i = 0; i < unit_hi; i++) {
        const Unit* u = &units[i];
        int x, y;
        if (!u->alive)
            continue;
        if (u->owner != PLAYER && !(u->flags & (UF_SEEN | UF_WAS_SEEN)))
            continue;
        x = ox + (int)((u->x >> FIX) * mission.mw / mapw);
        y = oy + (int)((u->z >> FIX) * mission.mh / maph);
        gfx_pixel(scr, x, y, u->owner == PLAYER ? C_BLUE : C_RED);
        gfx_pixel(scr, x + 1, y, u->owner == PLAYER ? C_BLUE : C_RED);
        if (u->flags & UF_SELECTED)
            gfx_pixel(scr, x, y + 1, C_WHITE);
    }
    gfx_rect(
        scr,
        ox + camx * mission.mw / mapw,
        oy + camy * mission.mh / maph,
        ox + (camx + VIEW_W) * mission.mw / mapw - 1,
        oy + (camy + VIEW_H) * mission.mh / maph - 1,
        C_WHITE
    );
}

/* ------------------------------------------------------------ the battlefield */

#define MAX_DRAW 450
static uint32_t drawlist[MAX_DRAW];

static void sort_draw(int n) {
    /* Shell sort on depth (high 16 bits), by Ciura's gaps */
    static const int gaps[] = {132, 57, 23, 10, 4, 1};
    int g, i, j;
    for (g = 0; g < 6; g++) {
        int gap = gaps[g];
        for (i = gap; i < n; i++) {
            uint32_t v = drawlist[i];
            for (j = i; j >= gap && drawlist[j - gap] > v; j -= gap)
                drawlist[j] = drawlist[j - gap];
            drawlist[j] = v;
        }
    }
}

static void draw_selection(uint8_t* scr, const Unit* u, int sx, int sy) {
    const UnitType* t = &unittypes[u->type];
    int r = t->radius + 2, top = sy - t->top - 2, bot = sy + 4, l = 4, w;
    if (t->kind != UK_MOBILE) {
        r = t->footx * 8;
        bot = sy + t->footz * 8 - 4;
    }
    gfx_fill(scr, sx - r, top, sx - r + l, top, C_WHITE);
    gfx_fill(scr, sx - r, top, sx - r, top + l, C_WHITE);
    gfx_fill(scr, sx + r - l, top, sx + r, top, C_WHITE);
    gfx_fill(scr, sx + r, top, sx + r, top + l, C_WHITE);
    gfx_fill(scr, sx - r, bot, sx - r + l, bot, C_WHITE);
    gfx_fill(scr, sx - r, bot - l, sx - r, bot, C_WHITE);
    gfx_fill(scr, sx + r - l, bot, sx + r, bot, C_WHITE);
    gfx_fill(scr, sx + r, bot - l, sx + r, bot, C_WHITE);
    /* the health bar: hp is at most 65535, so this fits 32 bits */
    w = (int)(u->hp * 2 * r / (t->maxhp ? t->maxhp : 1));
    if (w < 0)
        w = 0;
    gfx_fill(scr, sx - r, bot + 2, sx + r, bot + 3, C_BLACK);
    gfx_fill(
        scr, sx - r, bot + 2, sx - r + w, bot + 3, w > r ? C_GREEN : (w > r / 2 ? C_YELLOW : C_RED)
    );
    mark(sx - r, top, sx + r, bot + 3);
}

static void draw_battlefield(void) {
    uint8_t* scr = comp_at;
    int i, n = 0;
    int x0 = camx - 96, x1 = camx + VIEW_W + 96, y0 = camy - 64, y1 = camy + VIEW_H + 128;
    uint32_t tt0 = HZ200;
    comp_begin();
    stage_hz[5] += HZ200 - tt0;
    tt0 = HZ200;
    gfx_stride = BG_STRIDE;
    gfx_clip(VIEW_X, VIEW_Y, VIEW_X + VIEW_W - 1, VIEW_Y + VIEW_H - 1);
    for (i = 0; i < unit_hi && n < MAX_DRAW; i++) {
        const Unit* u = &units[i];
        int ux, uz;
        if (!u->alive)
            continue;
        if (u->owner != PLAYER && !(u->flags & (UF_SEEN | UF_WAS_SEEN)))
            continue;
        ux = u->x >> FIX;
        uz = u->z >> FIX;
        if (ux < x0 || ux > x1 || uz < y0 || uz > y1)
            continue;
        /* buildings sort by their back edge, so units in front of them come later */
        if (unittypes[u->type].kind != UK_MOBILE)
            uz -= unittypes[u->type].footz * 8 - 8;
        drawlist[n++] = (uint32_t)(uz + 64) << 16 | (uint32_t)i;
    }
    sort_draw(n);
    for (i = 0; i < n; i++) {
        const Unit* u = &units[drawlist[i] & 0xFFFF];
        const UnitType* t = &unittypes[u->type];
        int dir = t->ndirs == 16 ? ((u->heading + 2048) >> 12) & 15 : 0;
        int sx = VIEW_X + (u->x >> FIX) - camx, sy = VIEW_Y + unit_screen_y(u) - camy;
        const int16_t* spr = unit_frame(t, dir);
        if (u->flags & UF_UNBUILT) {
            gfx_sprite_nano(scr, spr, sx, sy, built_of(u, 256));
        } else {
            /* a walking unit bobs a pixel as it goes */
            if (u->speed && t->kind == UK_MOBILE && ((game_frames >> 1) & 1))
                sy--;
            gfx_sprite(scr, spr, sx, sy);
            if (u->paralyzed && (game_frames & 2))
                gfx_sprite_tint(scr, spr, sx, sy, C_WHITE);
        }
        mark_sprite(spr, sx, sy);
    }
    stage_hz[6] += HZ200 - tt0;
    /* nanolathe sprays */
    for (i = 0; i < unit_hi; i++) {
        const Unit* u = &units[i];
        int sx, sy, tx, ty;
        if (!u->alive || u->nano == -1 || (u->flags & UF_UNBUILT))
            continue;
        if (u->owner != PLAYER && !(u->flags & UF_SEEN))
            continue;
        sx = VIEW_X + (u->x >> FIX) - camx;
        sy = VIEW_Y + unit_screen_y(u) - camy - unittypes[u->type].top / 2;
        if (u->nano >= 0) {
            const Unit* v = &units[u->nano];
            if (!v->alive || !(v->flags & UF_UNBUILT))
                continue;
            tx = VIEW_X + (v->x >> FIX) - camx + ((game_frames * 7 + i * 3) % 17) - 8;
            ty = VIEW_Y + unit_screen_y(v) - camy - unittypes[v->type].top / 2 +
                 ((game_frames * 5 + i) % 11) - 5;
        } else {
            const Feature* f;
            if (~u->nano >= nfeats)
                continue;
            f = &feats[~u->nano];
            if (!f->alive)
                continue;
            tx = VIEW_X + f->x - camx + ((game_frames * 7) % 13) - 6;
            ty = VIEW_Y + ground_screen_y(f->x, f->z) - camy - 8;
        }
        if (sx < -200 || sx > 840 || tx < -200 || tx > 840 || sy < -200 || sy > 680)
            continue;
        draw_line(scr, sx, sy, tx, ty, C_GREEN);
    }
    /* shots and beams */
    for (i = 0; i < MAX_PROJ; i++) {
        const Projectile* p = &projs[i];
        const WeaponType* w;
        int px, pz, sx, sy;
        if (!p->alive)
            continue;
        w = &weapons[p->weapon];
        px = p->x >> FIX;
        pz = p->z >> FIX;
        if (px < x0 || px > x1 || pz < y0 || pz > y1)
            continue;
        sx = VIEW_X + px - camx;
        sy = VIEW_Y + ground_screen_y(px, pz) - (p->y >> 8) - camy;
        if (w->kind == WK_DGUN) {
            draw_fill(scr, sx - 3, sy - 3, sx + 3, sy + 3, C_YELLOW);
            gfx_fill(scr, sx - 1, sy - 1, sx + 1, sy + 1, C_WHITE);
        } else if (w->kind == WK_ROCKET) {
            int bx = sx - (int)(p->vx >> (FIX + 1)), by = sy - (int)(p->vz >> (FIX + 1));
            draw_line(scr, bx, by, sx, sy, C_LGREY);
            gfx_pixel(scr, sx, sy, C_WHITE);
        } else {
            draw_fill(scr, sx, sy, sx + 1, sy + 1, C_YELLOW);
        }
    }
    for (i = 0; i < MAX_BEAMS; i++) {
        const Beam* b = &beams[i];
        if (!b->alive)
            continue;
        if (b->x0 < x0 - 400 || b->x0 > x1 + 400 || b->y0 < y0 - 400 || b->y0 > y1 + 400)
            continue;
        draw_line(
            scr,
            VIEW_X + b->x0 - camx,
            VIEW_Y + b->y0 - camy,
            VIEW_X + b->x1 - camx,
            VIEW_Y + b->y1 - camy,
            b->colour
        );
    }
    for (i = 0; i < MAX_FX; i++) {
        const Effect* e = &fx[i];
        if (!e->alive || e->x < x0 || e->x > x1 || e->y < y0 || e->y > y1)
            continue;
        draw_spr(scr, anim_frame(e->anim, e->frame), VIEW_X + e->x - camx, VIEW_Y + e->y - camy);
    }
    for (i = 0; i < unit_hi; i++) {
        const Unit* u = &units[i];
        int ux, uy;
        if (!u->alive || !(u->flags & UF_SELECTED))
            continue;
        ux = u->x >> FIX;
        uy = unit_screen_y(u);
        if (ux < x0 || ux > x1 || uy < y0 || uy > y1)
            continue;
        draw_selection(scr, u, VIEW_X + ux - camx, VIEW_Y + uy - camy);
    }
    /* the building being placed */
    if (mode == M_BUILD && build_type >= 0 && in_view(mx(), my())) {
        int wx, wz, bx, bz, sy, ok;
        const UnitType* t = &unittypes[build_type];
        const int16_t* spr = unit_frame(t, 0);
        screen_to_world(mx(), my(), &wx, &wz);
        bx = snap_build_x(build_type, wx);
        bz = snap_build_z(build_type, wz);
        ok = can_build_at(build_type, bx, bz);
        sy = ground_screen_y(bx, bz);
        gfx_sprite_tint(scr, spr, VIEW_X + bx - camx, VIEW_Y + sy - camy, ok ? C_GREEN : C_RED);
        mark_sprite(spr, VIEW_X + bx - camx, VIEW_Y + sy - camy);
        gfx_rect(
            scr,
            VIEW_X + bx - t->footx * 8 - camx,
            VIEW_Y + sy - t->footz * 8 - camy,
            VIEW_X + bx + t->footx * 8 - 1 - camx,
            VIEW_Y + sy + t->footz * 8 - 1 - camy,
            ok ? C_GREEN : C_RED
        );
        mark(
            VIEW_X + bx - t->footx * 8 - camx,
            VIEW_Y + sy - t->footz * 8 - camy,
            VIEW_X + bx + t->footx * 8 - camx,
            VIEW_Y + sy + t->footz * 8 - camy
        );
    }
    if (drag && (mb() & 1)) {
        int dx = mx(), dy = my();
        if (dx < VIEW_X)
            dx = VIEW_X;
        if (dy < VIEW_Y)
            dy = VIEW_Y;
        gfx_rect(scr, drag_x, drag_y, dx, dy, C_WHITE);
        mark(
            drag_x < dx ? drag_x : dx,
            drag_y < dy ? drag_y : dy,
            drag_x > dx ? drag_x : dx,
            drag_y > dy ? drag_y : dy
        );
    }
    gfx_stride = LINE_BYTES;
}

static void draw_cursor(uint8_t* scr) {
    int c = CUR_NORMAL, x = mx(), y = my();
    const int16_t* spr;
    if (in_view(x, y)) {
        int wx = x - VIEW_X + camx, wy = y - VIEW_Y + camy;
        if (mode == M_ATTACK || mode == M_DGUN)
            c = CUR_ATTACK;
        else if (mode == M_MOVE)
            c = CUR_MOVE;
        else if (mode == M_PATROL)
            c = CUR_PATROL;
        else if (mode == M_GUARD)
            c = CUR_GUARD;
        else if (mode == M_RECLAIM)
            c = CUR_RECLAIM;
        else if (mode == M_BUILD)
            c = CUR_NORMAL;
        else if (unit_at(wx, wy, 1 << PLAYER) >= 0)
            c = CUR_SELECT;
        else if (nselected && unit_at(wx, wy, 1 << ENEMY) >= 0 && selected(SEL_ARMED, 0))
            c = CUR_ATTACK;
        else if (nselected && selected(SEL_MOBILE, 0))
            c = CUR_MOVE;
    }
    spr = common_sprite(common.cursor[c]);
    gfx_clip_full();
    gfx_sprite(scr, spr, x, y);
    sprite_box(spr, x, y, &S->cur_x0, &S->cur_y0, &S->cur_x1, &S->cur_y1);
}

/* The interface around the battlefield is drawn once a change into ui, a
 * screen-sized buffer in TT-RAM, and each screen copies the parts it lacks */
static uint8_t* ui;
static uint32_t ui_side_key, ui_bar_key;
static int32_t ui_bar_frame, ui_mini_frame = -100;
static int32_t side_serial, bar_serial, mini_serial;

/* Takes away last time's pointer outside the battlefield */
static void erase_cursor(uint8_t* scr) {
    int x0 = S->cur_x0, y0 = S->cur_y0, x1 = S->cur_x1, y1 = S->cur_y1;
    if (x1 < x0)
        return;
    {
        /* the battlefield under it comes back from the composite */
        int r0 = (y0 - VIEW_Y) >> 4, r1 = (y1 - VIEW_Y) >> 4, c0 = (x0 - VIEW_X) >> 4,
            c1 = (x1 - VIEW_X) >> 4, r, c;
        for (r = r0 < 0 ? 0 : r0; r <= r1 && r < CELL_ROWS; r++)
            for (c = c0 < 0 ? 0 : c0; c <= c1 && c < CELL_COLS; c++)
                S->extra[r][c] = 1;
    }
    if (x0 < VIEW_X)
        gfx_copy_rect(scr, ui, x0, y0, x1 < VIEW_X - 1 ? x1 : VIEW_X - 1, y1);
    if (y0 < VIEW_Y)
        gfx_copy_rect(scr, ui, x0, y0, x1, VIEW_Y - 1);
    if (y1 >= VIEW_Y + VIEW_H)
        gfx_copy_rect(scr, ui, x0, VIEW_Y + VIEW_H, x1, y1);
    S->cur_x1 = S->cur_x0 - 1;
}

uint32_t stage_hz[7];

/* Checks: rows of the composite that differ from the background outside this
 * frame's marks, and rows of the front screen that differ from the composite */
void game_check_layers(void) {
    int y, c, bad_comp = 0, bad_scr = 0, first_c = -1, first_s = -1;
    for (y = 0; y < VIEW_H; y++)
        for (c = 0; c < CELL_COLS; c++) {
            const uint8_t *a = comp + y * BG_STRIDE + c * 8, *b = bg + y * BG_STRIDE + c * 8;
            const uint8_t* f = tt_front + (VIEW_Y + y) * LINE_BYTES + VIEW_X / 2 + c * 8;
            if (!marks[mark_cur][y >> 4][c] && memcmp(a, b, 8)) {
                if (first_c < 0)
                    first_c = y * 1000 + c;
                bad_comp++;
            }
            if (memcmp(a, f, 8)) {
                if (first_s < 0)
                    first_s = y * 1000 + c;
                if (bad_scr < 12 && (y & 15) == 0)
                    test_log(
                        "  stale cell row %d col %d marks %d%d%d cur %d",
                        y >> 4,
                        c,
                        marks[0][y >> 4][c],
                        marks[1][y >> 4][c],
                        marks[2][y >> 4][c],
                        mark_cur
                    );
                bad_scr++;
            }
        }
    test_log(
        "check: composite differs from background in %d group rows (first row %d col %d); screen "
        "differs from composite in %d (first row %d col %d)",
        bad_comp,
        first_c / 1000,
        first_c % 1000,
        bad_scr,
        first_s / 1000,
        first_s % 1000
    );
}

static void draw_frame(void) {
    uint8_t* scr = tt_back;
    const uint8_t* panel = (const uint8_t*)common.panel;
    uint32_t t = HZ200, t2, k;
    {
        int i;
        S = &screens[0];
        for (i = 0; i < NSCREENS; i++)
            if (screens[i].scr == tt_back)
                S = &screens[i];
    }
    if (!S->panel_ok) {
        memcpy(scr, ui, SCREEN_BYTES);
        S->panel_ok = 1;
        S->full_serial = -1;
        S->side_serial = S->bar_serial = S->mini_serial = -1;
        S->cur_x1 = S->cur_x0 - 1;
    }
    erase_cursor(scr);
    bg_update();
    t2 = HZ200;
    stage_hz[0] += t2 - t;
    t = t2;
    draw_battlefield();
    if (paused || sim_result) {
        gfx_stride = BG_STRIDE;
        gfx_clip(VIEW_X, VIEW_Y, VIEW_X + VIEW_W - 1, VIEW_Y + VIEW_H - 1);
        if (paused) {
            draw_spr(
                comp_at, common_sprite(common.title[TITLE_PAUSED]), VIEW_X + 200, VIEW_Y + 150
            );
            gfx_text(comp_at, VIEW_X + 150, VIEW_Y + 190, "Esc: resume   Q: quit to menu", C_WHITE);
        } else {
            draw_spr(
                comp_at,
                common_sprite(common.title[sim_result > 0 ? TITLE_VICTORY : TITLE_DEFEAT]),
                VIEW_X + 190,
                VIEW_Y + 150
            );
            gfx_text(comp_at, VIEW_X + 180, VIEW_Y + 190, "Click to continue", C_WHITE);
        }
        mark(VIEW_X + 140, VIEW_Y + 150, VIEW_X + 400, VIEW_Y + 210);
        gfx_stride = LINE_BYTES;
    }
    comp_to_screen(scr);
    t2 = HZ200;
    stage_hz[1] += t2 - t;
    t = t2;
    /* the interface: drawn into ui when it changes, the minimap and bars no more
     * often than every few frames */
    gfx_clip_full();
    if (game_frames - ui_mini_frame >= 6) {
        ui_mini_frame = game_frames;
        draw_minimap(ui);
        mini_serial++;
    }
    t2 = HZ200;
    stage_hz[2] += t2 - t;
    t = t2;
    k = side_key();
    if (k != ui_side_key || !side_serial) {
        ui_side_key = k;
        gfx_copy_rect(ui, panel, 0, 128, VIEW_X - 1, SCREEN_H - 1);
        draw_side_panel(ui);
        side_serial++;
    }
    t2 = HZ200;
    stage_hz[3] += t2 - t;
    t = t2;
    if (game_frames - ui_bar_frame >= 8 || !bar_serial) {
        k = bars_key();
        if (k != ui_bar_key || !bar_serial) {
            ui_bar_frame = game_frames;
            ui_bar_key = k;
            gfx_copy_rect(ui, panel, VIEW_X, 0, SCREEN_W - 1, VIEW_Y - 1);
            gfx_copy_rect(ui, panel, VIEW_X, VIEW_Y + VIEW_H, SCREEN_W - 1, SCREEN_H - 1);
            draw_bars(ui);
            bar_serial++;
        }
    }
    gfx_clip_full();
    if (S->mini_serial != mini_serial) {
        S->mini_serial = mini_serial;
        gfx_copy_rect(scr, ui, 0, 0, VIEW_X - 1, 127);
    }
    if (S->side_serial != side_serial) {
        S->side_serial = side_serial;
        gfx_copy_rect(scr, ui, 0, 128, VIEW_X - 1, SCREEN_H - 1);
    }
    if (S->bar_serial != bar_serial) {
        S->bar_serial = bar_serial;
        gfx_copy_rect(scr, ui, VIEW_X, 0, SCREEN_W - 1, VIEW_Y - 1);
        gfx_copy_rect(scr, ui, VIEW_X, VIEW_Y + VIEW_H, SCREEN_W - 1, SCREEN_H - 1);
    }
    t2 = HZ200;
    stage_hz[4] += t2 - t;
    t = t2;
    draw_cursor(scr);
}

/* ------------------------------------------------------------ input */

static void select_all(void) {
    int i;
    for (i = 0; i < unit_hi; i++)
        if (selectable(&units[i]))
            units[i].flags |= UF_SELECTED;
    selection_changed();
}

/* 1 to leave the mission */
static int handle_keys(void) {
    uint16_t k;
    while ((k = input_key()) != 0) {
        int scan = k >> 8, ch = k & 0xFF, b;
        if (ch >= 'a' && ch <= 'z')
            ch -= 32;
        if (paused) {
            if (scan == 0x01)
                paused = 0;
            else if (ch == 'Q')
                return 1;
            continue;
        }
        if (sim_result)
            return 1;
        switch (scan) {
        case 0x01:
            paused = 1;
            continue; /* Esc */
        case 0x48:
            camy -= 64;
            break; /* arrows */
        case 0x50:
            camy += 64;
            break;
        case 0x4B:
            camx -= 64;
            break;
        case 0x4D:
            camx += 64;
            break;
        case 0x62:
            paused = 1;
            continue; /* Help */
        }
        clamp_camera();
        if (ch == 1) {
            select_all();
            continue;
        } /* Ctrl+A */
        for (b = 0; b < NORDER_BUTTONS; b++)
            if (order_buttons[b].key == ch) {
                order_button(b);
                break;
            }
        switch (ch) {
        case 'B':
            page = 0;
            break;
        case 'O':
            page = 1;
            break;
        case 'C': {
            int i;
            for (i = 0; i < unit_hi; i++)
                if (units[i].alive && units[i].owner == PLAYER &&
                    (unittypes[units[i].type].flags & UF_COMMANDER)) {
                    deselect_all();
                    units[i].flags |= UF_SELECTED;
                    selection_changed();
                    centre_on(units[i].x >> FIX, units[i].z >> FIX);
                }
            break;
        }
        case 'X':
            snd_enabled = !snd_enabled;
            break;
        }
    }
    return 0;
}

static int handle_mouse(void) {
    uint8_t b = (uint8_t)mb();
    uint8_t pressed = (uint8_t)(b & ~prev_buttons), released = (uint8_t)(~b & prev_buttons);
    int x = mx(), y = my(), queue = tt_shift() & 1;
    prev_buttons = b;
    if (sim_result) {
        if (pressed)
            return 1;
        return 0;
    }
    if (paused)
        return 0;
    /* scrolling at the screen's edges */
    if (x <= 1)
        camx -= 16;
    if (x >= SCREEN_W - 2)
        camx += 16;
    if (y <= 1)
        camy -= 16;
    if (y >= SCREEN_H - 2)
        camy += 16;
    clamp_camera();
    if (pressed & 1) {
        if (in_view(x, y)) {
            if (mode == M_NONE) {
                drag = 1;
                drag_x = x;
                drag_y = y;
            } else {
                click_battlefield(x, y, queue);
            }
        } else if (x < 128 && y >= 128) {
            panel_click(x, y, 0);
        }
    }
    /* holding the left button on the minimap moves the view */
    if ((b & 1) && x < 128 && y < 128 && !drag) {
        int wx, wz;
        minimap_to_world(x, y, &wx, &wz);
        centre_on(wx, wz);
    }
    if (released & 1) {
        if (drag) {
            int dx = x - drag_x, dy = y - drag_y;
            drag = 0;
            if (dx > 4 || dx < -4 || dy > 4 || dy < -4)
                box_select(drag_x, drag_y, x, y, queue);
            else
                click_battlefield(drag_x, drag_y, queue);
        }
    }
    if (pressed & 2) {
        if (x < 128 && y >= 128)
            panel_click(x, y, 1);
        else if (x < 128 && y < 128 && nselected) {
            int wx, wz;
            minimap_to_world(x, y, &wx, &wz);
            order_selected_move(wx, wz, O_MOVE, queue);
        } else if (mode != M_NONE)
            mode = M_NONE;
        else {
            deselect_all();
            selection_changed();
        }
    }
    return 0;
}

/* ------------------------------------------------------------ the mission */

int game_run(int difficulty) {
    uint32_t last, acc = 0;
    int i, result = 0;
    mapw = mission.w * 16;
    maph = mission.h * 16;
    sim_start(difficulty);
    mode = M_NONE;
    page = 1;
    build_page = 0;
    paused = 0;
    drag = 0;
    prev_buttons = (uint8_t)mb();
    nselected = 0;
    minimap_ox = ((128 - mission.mw) / 2) & ~15;
    minimap_oy = (128 - mission.mh) / 2;
    game_frames = 0;
    game_draw_hz = game_sim_hz = 0;
    game_ticks_run = 0;
    /* start looking at the Commander, or else the first of the player's units */
    {
        int first = -1;
        for (i = 0; i < unit_hi; i++)
            if (units[i].alive && units[i].owner == PLAYER) {
                if (first < 0 || (unittypes[units[i].type].flags & UF_COMMANDER))
                    first = i;
            }
        if (first >= 0)
            centre_on(units[first].x >> FIX, units[first].z >> FIX);
    }
    gfx_set_font(common.font);
    tt_set_palette(common.palette);
    if (!bg) {
        bg = tt_alloc_any((int32_t)BG_STRIDE * VIEW_H);
        comp = tt_alloc_any((int32_t)BG_STRIDE * VIEW_H);
        ui = tt_alloc_any(SCREEN_BYTES);
        comp_at = comp - VIEW_Y * BG_STRIDE - VIEW_X / 2;
    }
    comp_camx = -1;
    memcpy(ui, common.panel, SCREEN_BYTES);
    side_serial = bar_serial = mini_serial = 0;
    ui_mini_frame = -100;
    memset(screens, 0, sizeof screens);
    for (i = 0; i < NSCREENS; i++)
        screens[i].scr = tt_screens[i];
    bg_version = -1;
    bg_camx = -100000;
    last = HZ200;
    for (;;) {
        uint32_t now, t0;
        int ticks = 0;
        if (test_hooks.next && test_hooks.next() < 0)
            break;
        if (handle_keys() || handle_mouse()) {
            result = sim_result;
            break;
        }
        view_x0 = camx;
        view_y0 = camy;
        view_x1 = camx + VIEW_W;
        view_y1 = camy + VIEW_H;
        now = HZ200;
        t0 = now;
        if (game_fast) {
            if (!paused && !sim_result) {
                sim_tick();
                game_ticks_run++;
            }
        } else {
            /* 15 ticks a second: 40/3 of the 200 Hz clock a tick */
            acc += (now - last) * 3;
            last = now;
            if (acc > 40 * 3)
                acc = 40 * 3; /* the machine is behind: let the game slow down */
            while (acc >= 40 && ticks < 3) {
                acc -= 40;
                if (!paused && !sim_result) {
                    sim_tick();
                    game_ticks_run++;
                }
                ticks++;
            }
        }
        game_sim_hz += HZ200 - t0;
        t0 = HZ200;
        draw_frame();
        game_draw_hz += HZ200 - t0;
        tt_flip();
        game_frames++;
    }
    snd_stop();
    return result;
}
