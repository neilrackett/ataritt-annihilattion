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
#include "sprites.h"
#include <mint/osbind.h>

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

static int unit_shown(const Unit* u);

enum { M_NONE, M_MOVE, M_ATTACK, M_PATROL, M_GUARD, M_DGUN, M_RECLAIM, M_BUILD, M_CAPTURE, M_REPAIR };

#define EDGE_SPEED 512 /* pixels a second the screen's edges scroll the view */

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
    camx &= ~15; /* across by whole groups: the copy to the screen moves groups */
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
            feat_type(f->type)->name,
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

/* The player's Commander, or -1 once it is gone */
static int find_commander(void) {
    int i;
    for (i = 0; i < unit_hi; i++)
        if (units[i].alive && units[i].owner == PLAYER &&
            (unittypes[units[i].type].flags & UF_COMMANDER))
            return i;
    return -1;
}

static int following;        /* the pointer scrolls the view after the Commander (test scripts) */
static int follow_x, follow_y; /* where the pointer rests between pushes */

void game_follow(int on) {
    following = on;
    follow_x = mx();
    follow_y = my();
}

/* A test script's follow: when the Commander strays from the middle of the
 * view, the pointer goes to the screen's edges, as a player's would, and it
 * comes back once the view has passed it a little. */
static void follow_pointer(void) {
    static uint32_t then;
    static int push_x, push_y;
    uint32_t dt = HZ200 - then;
    int c = find_commander(), x = mx(), y = my(), tx, ty, dx, dy, d;
    then += dt;
    if (c < 0)
        return;
    dx = (units[c].x >> FIX) - (camx + VIEW_W / 2);
    dy = unit_screen_y(&units[c]) - (camy + VIEW_H / 2);
    push_x = dx > 96 ? 1 : dx < -96 ? -1 : push_x * dx < -32 ? 0 : push_x;
    push_y = dy > 96 ? 1 : dy < -96 ? -1 : push_y * dy < -32 ? 0 : push_y;
    tx = push_x > 0 ? SCREEN_W - 1 : push_x < 0 ? 0 : follow_x;
    ty = push_y > 0 ? SCREEN_H - 1 : push_y < 0 ? 0 : follow_y;
    /* 600 pixels a second */
    d = 3 * (int)(dt > 20 ? 20 : dt);
    x += tx > x + d ? d : tx < x - d ? -d : tx - x;
    y += ty > y + d ? d : ty < y - d ? -d : ty - y;
    test_mouse(x, y, mb());
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
            if (t->flags & (UF_ARMED | UF_KAMIKAZE))
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
        if (kind == O_ATTACK && !(t->flags & (UF_ARMED | UF_KAMIKAZE)))
            continue;
        if (kind == O_CAPTURE && !(t->flags & UF_CAPTURE))
            continue;
        if (kind == O_REPAIR && !(t->flags & UF_REPAIR))
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
    int own = unit_at(wxs, wsy, 1 << PLAYER, unit_shown);
    int enemy = unit_at(wxs, wsy, 1 << ENEMY, unit_shown);
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
    case M_CAPTURE:
        if (enemy >= 0)
            order_selected_unit(O_CAPTURE, enemy, queue);
        mode = M_NONE;
        return;
    case M_REPAIR:
        if (own >= 0)
            order_selected_unit(O_REPAIR, own, queue);
        mode = M_NONE;
        return;
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
        uy = unit_draw_y(u);
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

/* What a builder's menu offers: its options the mission lets the player build */
static int menu_options(int unit, uint8_t* out) {
    const UnitType* t = &unittypes[units[unit].type];
    int k, n = 0;
    for (k = 0; k < t->nbuild; k++)
        if (build_allowed(t->build[k]))
            out[n++] = t->build[k];
    return n;
}

static int build_option(int unit, int slot) {
    uint8_t opts[MAX_BUILD];
    int n = menu_options(unit, opts), k = build_page * 6 + slot;
    return k < n ? opts[k] : -1;
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
    {BTN_CAPTURE, M_CAPTURE, 68, BTN_Y + 102, 'T'},
    {BTN_REPAIR, M_REPAIR, 6, BTN_Y + 136, 'E'},
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
            uint8_t opts[MAX_BUILD];
            int pages = (menu_options(mu, opts) + 5) / 6;
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
    int f = lit && panel.button_frames[b] > 1 ? 1 : 0;
    if (!panel.button_frames[b])
        return;
    gfx_sprite(scr, panel_sprite(panel.button[b][f]), x, y);
}

static void draw_side_panel(uint8_t* scr) {
    int mu = menu_unit();
    uint8_t opts[MAX_BUILD];
    char s[32];
    gfx_clip(0, 128, 127, 447);
    draw_button(scr, BTN_BUILD, 4, 132, page == 0);
    draw_button(scr, BTN_ORDERS, 66, 132, page == 1);
    if (page == 0 && mu >= 0) {
        const Unit* u = &units[mu];
        int slot, nopts = menu_options(mu, opts);
        for (slot = 0; slot < 6; slot++) {
            int k = build_page * 6 + slot, bt, px = (slot & 1) * 64, py = PIC_Y + (slot >> 1) * 64, n;
            if (k >= nopts)
                continue;
            bt = opts[k];
            {
                const uint16_t* pic = unit_pic(bt);
                if (pic)
                    gfx_image(scr, pic, px, py, 4, 64);
                else
                    gfx_text(scr, px + 4, py + 24, unittypes[bt].name, C_WHITE);
            }
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
        if (nopts > 6) {
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
            gfx_text(scr, 8, BTN_Y + 176, "No units selected", C_LGREY);
    }
}

/* ------------------------------------------------------------ what each screen holds */

/* The battlefield is drawn from two buffers in TT-RAM: a background, the
 * terrain with the trees and wrecks over it and the fog over what is
 * unexplored, and a composite, the background
 * with this frame's moving things. Both are rings the size of the view: the
 * battlefield's 16-pixel group g and row y (as on screen) are kept at group
 * g mod RING_G and row y mod RING_H, so when the view moves only the strips
 * that come into it are drawn, and nothing already there moves. The view
 * moves across by whole groups (clamp_camera()).
 *
 * A screen that already shows this view has only the 16x16 cells that changed
 * copied to it; one that shows another has the whole view copied. */
#define RING_G (VIEW_W / 16) /* groups across */
#define RING_H VIEW_H
#define RING_STRIDE (RING_G * 8)
#define CELL_ROWS (RING_H / 16)
#define VIEW_COLS (VIEW_W / 16)
#define VIEW_ROWS (VIEW_H / 16)

#define NMARKS (NSCREENS + 1)

typedef struct {
    uint8_t* scr;
    int32_t last_frame;                           /* the frame it was last drawn in */
    int32_t side_serial, bar_serial, mini_serial; /* the versions of ui's parts it shows */
    int cam_x, cam_y;                             /* the view it shows; -1 for none */
    int panel_ok;
    int cur_x0, cur_y0, cur_x1, cur_y1;
    uint8_t extra[VIEW_ROWS][VIEW_COLS] __attribute__((aligned(4))); /* cells its own pointer covered */
} Screen;

/* A part of the rings that holds a rectangle of the battlefield without
 * wrapping: battlefield x, y is at x + dx, y + dy in it */
typedef struct {
    int x0, y0, x1, y1; /* the battlefield rectangle */
    int dx, dy;
} Piece;

/* ------------------------------------------------------------ the explored map */

/* The missions start unmapped: ground no unit of the player's has seen is
 * black, on the battlefield and the minimap. explored[] holds a byte a
 * 32x32 sight cell of the map as drawn (x, and y on screen). After each tick
 * a unit that came into a new cell explores the disc of its sight. The
 * black's edges are the demo's fog tiles (common.fog): tile tx, ty lies at
 * 32 * tx + 16, 32 * ty + 16, across the corners of four sight cells, so a
 * cell uncovered changes the four tiles over it, and the background draws
 * those again. */
#define EX_SHIFT 5 /* a sight cell is 32 pixels */
#define EX_NONE 0xFFFF
/* sized to the map when a mission starts (ex_alloc()): explored is ex_h
 * rows of ex_stride bytes, fog_dirty fog_stride */
static int ex_stride;  /* explored's stride: the sight cells across, a multiple of 4 */
static int fog_stride; /* fog_dirty's: tiles -1 to ex_stride - 1, and room */
static uint8_t* explored;
static int ex_w, ex_h;
static uint16_t ex_x[MAX_UNITS], ex_y[MAX_UNITS]; /* the cell a unit last explored from (y on screen) */
static uint16_t ex_z[MAX_UNITS];                  /* and its z cell, to tell when it has moved */
static uint8_t* fog_dirty;  /* tiles to draw again, tile tx, ty at ty + 1, tx + 1 */
static int fog_t0, fog_t1;  /* the rows of fog_dirty in use */
static int fog_all;         /* the whole view to draw again */
static uint8_t* mini_dirty; /* cell rows the minimap has still to show */
static int mini_any;
static void* ex_block;

static void ex_free(void) {
    tt_free(ex_block);
    ex_block = 0;
}

static int ex_alloc(void) {
    uint8_t* p;
    ex_free();
    ex_w = (mission.w + 1) / 2;
    ex_h = (mission.h + 1) / 2;
    ex_stride = (ex_w + 3) & ~3;
    fog_stride = ex_stride + 4;
    ex_block = tt_alloc_any((int32_t)ex_stride * ex_h + (int32_t)(ex_h + 2) * fog_stride + ex_h + 16);
    if (!ex_block)
        return 0;
    p = (uint8_t*)(((uint32_t)ex_block + 3) & ~3UL);
    explored = p;
    p += ex_stride * ex_h;
    fog_dirty = p;
    p += (ex_h + 2) * fog_stride;
    mini_dirty = p;
    memset(explored, 0, (size_t)ex_stride * ex_h);
    memset(fog_dirty, 0, (size_t)(ex_h + 2) * fog_stride);
    memset(mini_dirty, 0, (size_t)ex_h);
    return 1;
}

static int explored_at(int x, int y) {
    int cx = x >> EX_SHIFT, cy = y >> EX_SHIFT;
    return cx < 0 || cy < 0 || cx >= ex_w || cy >= ex_h || explored[cy * ex_stride + cx];
}

/* Whether a unit is shown: the player's always; an enemy where the sim's
 * sight saw it (a building, where it was last seen), on explored ground */
static int unit_shown(const Unit* u) {
    return u->owner == PLAYER ||
           ((u->flags & (UF_SEEN | UF_WAS_SEEN)) && explored_at(u->x >> FIX, unit_screen_y(u)));
}

/* Explores cells c0..c1 of row r, and notes the fog tiles and minimap rows
 * over those it uncovers */
static void explore_row(int r, int c0, int c1) {
    uint8_t* e = explored + r * ex_stride;
    int c = c0;
    for (;;) {
        int start;
        /* past what is explored: four cells at a time where they line up */
        while (c <= c1) {
            if (!(c & 3) && c + 3 <= c1 && *(const uint32_t*)(e + c) == 0x01010101UL) {
                c += 4;
                continue;
            }
            if (!e[c])
                break;
            c++;
        }
        if (c > c1)
            break;
        start = c;
        while (c <= c1 && !e[c])
            e[c++] = 1;
        /* tiles start - 1..c - 1 in tile rows r - 1 and r */
        memset(fog_dirty + r * fog_stride + start, 1, (size_t)(c - start + 1));
        memset(fog_dirty + (r + 1) * fog_stride + start, 1, (size_t)(c - start + 1));
        if (r < fog_t0)
            fog_t0 = r;
        if (r + 1 > fog_t1)
            fog_t1 = r + 1;
        mini_dirty[r] = 1;
        mini_any = 1;
    }
}

/* Each of the player's units that came into a new cell explores the disc of
 * its sight. Everything in the disc it explored last time is explored
 * already, so a row is looked at only beyond that disc. */
static void explore(void) {
    int i;
    for (i = 0; i < unit_hi; i++) {
        const Unit* u = &units[i];
        const uint8_t* wt;
        int cx, cz, cy, ox = ex_x[i], oy, r, d;
        if (!u->alive || u->owner != PLAYER || (u->flags & UF_UNBUILT)) {
            ex_x[i] = EX_NONE;
            continue;
        }
        cx = (u->x >> FIX) >> EX_SHIFT;
        cz = (u->z >> FIX) >> EX_SHIFT;
        if (cx == ox && cz == ex_z[i])
            continue;
        cy = unit_screen_y(u) >> EX_SHIFT;
        if (cx < 0 || cy < 0 || cx >= ex_w || cy >= ex_h || cz < 0)
            continue;
        oy = ex_y[i];
        ex_x[i] = (uint16_t)cx;
        ex_z[i] = (uint16_t)cz;
        ex_y[i] = (uint16_t)cy;
        r = unittypes[u->type].sight >> EX_SHIFT;
        if (r > 31)
            r = 31;
        wt = disc_widths(r);
        for (d = -r; d <= r; d++) {
            int y = cy + d, w = wt[d < 0 ? -d : d], x0, x1, od = y - oy;
            if (y < 0 || y >= ex_h)
                continue;
            x0 = cx - w < 0 ? 0 : cx - w;
            x1 = cx + w >= ex_w ? ex_w - 1 : cx + w;
            if (ox != EX_NONE && od >= -r && od <= r) {
                /* the old disc's part of the row is explored */
                int ow = wt[od < 0 ? -od : od], o0 = ox - ow, o1 = ox + ow;
                if (o0 <= x1 && o1 >= x0) {
                    if (x0 < o0)
                        explore_row(y, x0, o0 - 1);
                    if (x1 > o1)
                        explore_row(y, o1 + 1, x1);
                    continue;
                }
            }
            explore_row(y, x0, x1);
        }
    }
}

void game_log_explored(void) {
    char line[260];
    int y, x;
    for (y = 0; y < ex_h; y++) {
        for (x = 0; x < ex_w && x < 255; x++)
            line[x] = explored[y * ex_stride + x] ? '#' : '.';
        line[x] = 0;
        test_log("ex %3d %s", y, line);
    }
}

void game_explore_all(void) {
    memset(explored, 1, (size_t)ex_stride * ex_h);
    memset(mini_dirty, 1, (size_t)ex_h);
    mini_any = 1;
    fog_all = 1;
}

static Screen screens[NSCREENS];
static Screen* S;
static uint8_t* bg;   /* terrain, features and fog of the view */
static uint8_t* comp; /* bg with this frame's moving things */
static int ring_g0 = -100000, ring_y0; /* the rings hold groups ring_g0.. and rows ring_y0.. */
static Piece pieces[4];
static int npieces;
static int32_t bg_version = -1;
static uint8_t marks[NMARKS][CELL_ROWS][RING_G] __attribute__((aligned(4))); /* ring cells changed, a frame each */
static int mark_cur;
static int16_t feat_order[MAX_FEATS];  /* the features drawn on their own, back to front */
static int16_t feat_order_z[MAX_FEATS]; /* and each one's z when it went in */
static uint8_t in_order[MAX_FEATS];

/* A feature type's sprite: the mission's, or a unit type's wreck; null for none */
static const int16_t* feat_sprite(int type) {
    const FeatureType* ft = feat_type(type);
    if (ft->sprite == FEAT_WRECK)
        return unit_wreck(type - mission.nftypes);
    if (ft->sprite == FEAT_NONE)
        return 0;
    return (const int16_t*)(mission.bank + ft->sprite);
}
static int nfeat_order;

static int piece_cols, seam_x, seam_y; /* how the pieces lie: where the second column and row start */

static int ring_mod(int v, int n) {
    v %= n;
    return v < 0 ? v + n : v;
}

/* Splits what the rings hold, from ring_g0 and ring_y0, into the pieces that
 * do not wrap: two across and two down at most */
static void set_pieces(void) {
    int c0 = ring_mod(ring_g0, RING_G), r0 = ring_mod(ring_y0, RING_H), i, j;
    int ncols = c0 ? 2 : 1, nrows = r0 ? 2 : 1;
    npieces = 0;
    piece_cols = ncols;
    seam_x = c0 ? (ring_g0 + RING_G - c0) * 16 : 0x7FFF;
    seam_y = r0 ? ring_y0 + RING_H - r0 : 0x7FFF;
    for (j = 0; j < nrows; j++)
        for (i = 0; i < ncols; i++) {
            Piece* p = &pieces[npieces++];
            int g = ring_g0 + (i ? RING_G - c0 : 0), y = ring_y0 + (j ? RING_H - r0 : 0);
            p->x0 = g * 16;
            p->x1 = (i || !c0 ? ring_g0 + RING_G : ring_g0 + RING_G - c0) * 16 - 1;
            p->y0 = y;
            p->y1 = (j || !r0 ? ring_y0 + RING_H : ring_y0 + RING_H - r0) - 1;
            p->dx = (i ? 0 : c0 * 16) - p->x0;
            p->dy = (j ? 0 : r0) - p->y0;
        }
}

/* Marks the ring cells under a battlefield rectangle as changed this frame */
static void mark(int x0, int y0, int x1, int y1) {
    int r, g;
    if (x0 < ring_g0 * 16)
        x0 = ring_g0 * 16;
    if (y0 < ring_y0)
        y0 = ring_y0;
    if (x1 > (ring_g0 + RING_G) * 16 - 1)
        x1 = (ring_g0 + RING_G) * 16 - 1;
    if (y1 > ring_y0 + RING_H - 1)
        y1 = ring_y0 + RING_H - 1;
    if (x0 > x1 || y0 > y1)
        return;
    {
        /* the ring's row stepped rather than divided for; its 32 columns wrap by a mask */
        int rr = ring_mod(y0 >> 4, CELL_ROWS), c0 = (x0 >> 4) & (RING_G - 1), n = (x1 >> 4) - (x0 >> 4);
        for (r = y0 >> 4; r <= y1 >> 4; r++, rr = rr + 1 < CELL_ROWS ? rr + 1 : 0) {
            uint8_t* row = marks[mark_cur][rr];
            for (g = 0; g <= n; g++)
                row[(c0 + g) & (RING_G - 1)] = 1;
        }
    }
}

/* The battlefield rectangle a sprite drawn at x, y can cover */
static void sprite_box(const int16_t* spr, int x, int y, int* x0, int* y0, int* x1, int* y1) {
    *x0 = x - spr[2];
    *y0 = y - spr[3];
    *x1 = *x0 + spr[0] * 16 + 15;
    *y1 = *y0 + spr[1] - 1;
}


/* Drawing on the composite at battlefield coordinates. Most things lie in
 * one piece, which draws them; what crosses a seam is drawn by each piece it
 * meets, clipped to that piece. FOR_PIECES(p, rectangle) runs its body for
 * each, with the clip set: the body draws at x + p->dx, y + p->dy. */
static const Piece* clipped; /* the piece the clip is set to, or null */
static int piece_whole;      /* whether the rectangle lies in the one piece */

static void clip_to(const Piece* p) {
    if (p != clipped) {
        gfx_clip(p->x0 + p->dx, p->y0 + p->dy, p->x1 + p->dx, p->y1 + p->dy);
        clipped = p;
    }
}

/* The piece after p (or the first, for null) that a rectangle meets, with
 * the clip set to it, or null */
static const Piece* next_piece(const Piece* p, int x0, int y0, int x1, int y1) {
    if (piece_whole)
        return 0;
    for (p = p ? p + 1 : pieces; p < pieces + npieces; p++)
        if (x0 <= p->x1 && x1 >= p->x0 && y0 <= p->y1 && y1 >= p->y0) {
            clip_to(p);
            return p;
        }
    return 0;
}

static const Piece* first_piece(int x0, int y0, int x1, int y1) {
    const Piece* p = clipped; /* the last piece drawn in, most likely */
    if (!p || x0 < p->x0 || x1 > p->x1 || y0 < p->y0 || y1 > p->y1)
        p = &pieces[(y0 >= seam_y ? piece_cols : 0) + (x0 >= seam_x)];
    piece_whole = x0 >= p->x0 && x1 <= p->x1 && y0 >= p->y0 && y1 <= p->y1;
    if (!piece_whole)
        return next_piece(0, x0, y0, x1, y1);
    clip_to(p);
    return p;
}

#define FOR_PIECES(p, x0, y0, x1, y1)                                                              \
    for (p = first_piece(x0, y0, x1, y1); p; p = next_piece(p, x0, y0, x1, y1))

enum { BLIT_SPRITE, BLIT_NANO, BLIT_TINT };

/* Draws a sprite on the composite and marks where it went */
static void comp_blit(int how, const int16_t* spr, int x, int y, int arg) {
    int x0, y0, x1, y1;
    const Piece* p;
    if (spr[0] <= 0)
        return;
    sprite_box(spr, x, y, &x0, &y0, &x1, &y1);
    mark(x0, y0, x1, y1);
    FOR_PIECES(p, x0, y0, x1, y1) {
        if (how == BLIT_NANO)
            gfx_sprite_nano(comp, spr, x + p->dx, y + p->dy, arg);
        else if (how == BLIT_TINT)
            gfx_sprite_tint(comp, spr, x + p->dx, y + p->dy, arg);
        else
            gfx_sprite(comp, spr, x + p->dx, y + p->dy);
    }
}

static void comp_fill(int x0, int y0, int x1, int y1, int c) {
    const Piece* p;
    FOR_PIECES(p, x0, y0, x1, y1)
        gfx_fill(comp, x0 + p->dx, y0 + p->dy, x1 + p->dx, y1 + p->dy, c);
}

static void comp_rect(int x0, int y0, int x1, int y1, int c) {
    const Piece* p;
    FOR_PIECES(p, x0, y0, x1, y1)
        gfx_rect(comp, x0 + p->dx, y0 + p->dy, x1 + p->dx, y1 + p->dy, c);
}

static void comp_line(int x0, int y0, int x1, int y1, int c) {
    const Piece* p;
    FOR_PIECES(p, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 > x1 ? x0 : x1, y0 > y1 ? y0 : y1)
        gfx_line(comp, x0 + p->dx, y0 + p->dy, x1 + p->dx, y1 + p->dy, c);
}

static void comp_text(int x, int y, const char* s, int c) {
    const Piece* p;
    FOR_PIECES(p, x, y - 4, x + gfx_text_width(s), y + 16)
        gfx_text(comp, x + p->dx, y + p->dy, s, c);
}

static void draw_line(int x0, int y0, int x1, int y1, int c) {
    comp_line(x0, y0, x1, y1, c);
    mark(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 > x1 ? x0 : x1, y0 > y1 ? y0 : y1);
}

static void draw_fill(int x0, int y0, int x1, int y1, int c) {
    comp_fill(x0, y0, x1, y1, c);
    mark(x0, y0, x1, y1);
}

/* Whether a feature is drawn on its own: it has a sprite, not baked in the map */
static int drawn_alone(int i) {
    const FeatureType* ft = feat_type(feats[i].type);
    return ft->sprite != FEAT_NONE && !(ft->flags & FF_BAKED);
}

/* Features back to front, kept in order as they come and go: those gone
 * (or whose slot another took, at another z) leave the order, and each new
 * one goes in at its place, after those of its z */
static void sort_features(void) {
    int i, n = 0;
    for (i = 0; i < nfeat_order; i++) {
        int k = feat_order[i];
        if (feats[k].alive && feats[k].z == feat_order_z[i] && drawn_alone(k)) {
            feat_order[n] = (int16_t)k;
            feat_order_z[n++] = feats[k].z;
        } else {
            in_order[k] = 0;
        }
    }
    nfeat_order = n;
    for (i = 0; i < nfeats; i++) {
        int lo = 0, hi = nfeat_order, z = feats[i].z;
        if (in_order[i] || !feats[i].alive || !drawn_alone(i))
            continue;
        while (lo < hi) {
            int mid = (lo + hi) >> 1;
            if (feat_order_z[mid] <= z)
                lo = mid + 1;
            else
                hi = mid;
        }
        memmove(feat_order + lo + 1, feat_order + lo, (size_t)(nfeat_order - lo) * sizeof feat_order[0]);
        memmove(feat_order_z + lo + 1, feat_order_z + lo, (size_t)(nfeat_order - lo) * sizeof feat_order_z[0]);
        feat_order[lo] = (int16_t)i;
        feat_order_z[lo] = (int16_t)z;
        nfeat_order++;
        in_order[i] = 1;
    }
}

/* A feature's sprite reaches FEAT_SIDE pixels either side of its place on
 * screen, FEAT_UP above it and FEAT_DOWN below */
#define FEAT_SIDE 80
#define FEAT_UP 96
#define FEAT_DOWN 16

/* Lays the fog over rows ya..yb (of one 16-pixel cell row gy) of a piece,
 * cells gx0..gx1. A 16-pixel cell gx, gy is one quarter of fog tile
 * (gx - 1) / 2, (gy - 1) / 2; past the map's edges, the nearest sight cell
 * on it stands in. */
static void fog_rows(const Piece* p, int gy, int ya, int yb, int gx0, int gx1) {
    int tj = (gy - 1) >> 1, qy = (gy - 1) & 1, gx, r0 = tj, r1 = tj + 1;
    const uint8_t *e0, *e1;
    if (r0 < 0)
        r0 = 0;
    if (r0 >= ex_h)
        r0 = ex_h - 1;
    if (r1 >= ex_h)
        r1 = ex_h - 1;
    e0 = explored + r0 * ex_stride;
    e1 = explored + r1 * ex_stride;
    for (gx = gx0; gx <= gx1; gx++) {
        int ti = (gx - 1) >> 1, a = ti < 0 ? 0 : ti >= ex_w ? ex_w - 1 : ti, b = ti + 1 >= ex_w ? ex_w - 1 : ti + 1;
        int pat = (e0[a] ? 0 : 1) | (e0[b] ? 0 : 2) | (e1[a] ? 0 : 4) | (e1[b] ? 0 : 8), y;
        const uint16_t* keep;
        uint16_t* w;
        if (!pat)
            continue;
        w = (uint16_t*)(bg + (ya + p->dy) * RING_STRIDE + ((gx * 16 + p->dx) >> 4) * 8);
        if (pat == 15) {
            for (y = ya; y <= yb; y++, w += RING_STRIDE / 2) {
                ((uint32_t*)w)[0] = 0;
                ((uint32_t*)w)[1] = 0;
            }
            continue;
        }
        keep = common.fog + ((((ti * 7 + tj * 13) & 3) * 16 + pat) * 4 + qy * 2 + ((gx - 1) & 1)) * 16;
        for (y = ya; y <= yb; y++, w += RING_STRIDE / 2) {
            uint16_t k = keep[y & 15];
            w[0] &= k;
            w[1] &= k;
            w[2] &= k;
            w[3] &= k;
        }
    }
}

/* Draws one piece's part of the background: battlefield x0..x1 (on group
 * edges), y0..y1, all inside the piece */
static void bg_render_piece(const Piece* p, int x0, int y0, int x1, int y1) {
    int i, lo, hi, zlo, zhi;
    gfx_terrain(
        bg + (y0 + p->dy) * RING_STRIDE,
        mission.tiles,
        mission.tilemap,
        mission.tw,
        x0,
        y0,
        x0 + p->dx,
        0,
        x1 - x0 + 1,
        y1 - y0 + 1
    );
    gfx_clip(x0 + p->dx, y0 + p->dy, x1 + p->dx, y1 + p->dy);
    /* the features are in order of z, and a feature's y on screen is z less at
     * most 127 (half the greatest height): find the first that can reach */
    zlo = y0 - FEAT_DOWN;
    zhi = y1 + FEAT_UP + 127;
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
        int sy;
        if (f->x < x0 - FEAT_SIDE || f->x > x1 + FEAT_SIDE)
            continue;
        sy = ground_screen_y(f->x, f->z);
        if (sy < y0 - FEAT_DOWN || sy > y1 + FEAT_UP)
            continue;
        gfx_sprite(bg, feat_sprite(f->type), f->x + p->dx, sy + p->dy);
    }
    /* the fog over what no unit has explored */
    for (i = (y0 < 0 ? 0 : y0) >> 4; i <= y1 >> 4; i++)
        fog_rows(p, i, i * 16 > y0 ? i * 16 : y0, i * 16 + 15 < y1 ? i * 16 + 15 : y1, x0 >> 4, x1 >> 4);
}

/* Draws the background's battlefield rectangle x0..x1, y0..y1, as much of it
 * as the rings hold and the map has, and has the composite and the screens
 * take those cells from it */
static void bg_render(int x0, int y0, int x1, int y1) {
    int i;
    x0 &= ~15;
    x1 |= 15;
    if (x0 < 0)
        x0 = 0;
    if (x1 > mapw - 1)
        x1 = mapw - 1;
    gfx_stride = RING_STRIDE;
    clipped = 0; /* bg_render_piece() sets clips of its own */
    for (i = 0; i < npieces; i++) {
        const Piece* p = &pieces[i];
        int ax = x0 > p->x0 ? x0 : p->x0, bx = x1 < p->x1 ? x1 : p->x1;
        int ay = y0 > p->y0 ? y0 : p->y0, by = y1 < p->y1 ? y1 : p->y1;
        if (ax <= bx && ay <= by)
            bg_render_piece(p, ax, ay, bx, by);
    }
    gfx_stride = LINE_BYTES;
    mark(x0, y0, x1, y1);
}

/* Brings the background up to date with the view, the features and the
 * explored map: draws only what came into the rings, the area round a
 * feature that came or went, and the fog tiles over cells uncovered */
static void bg_update(void) {
    int g0 = camx >> 4, dg = g0 - ring_g0, dy = camy - ring_y0, t;
    int left = g0 * 16, right = (g0 + RING_G) * 16 - 1, top = camy, bottom = camy + RING_H - 1;
    /* the features in order first, for whatever is drawn below */
    if (bg_version != feat_version) {
        bg_version = feat_version;
        sort_features();
    }
    if (dg || dy) {
        ring_g0 = g0;
        ring_y0 = camy;
        set_pieces();
    }
    if (fog_all || dg <= -RING_G || dg >= RING_G || dy <= -RING_H || dy >= RING_H) {
        bg_render(left, top, right, bottom);
    } else {
        if (dy > 0)
            bg_render(left, bottom - dy + 1, right, bottom);
        else if (dy < 0)
            bg_render(left, top, right, top - dy - 1);
        if (dg > 0)
            bg_render((g0 + RING_G - dg) * 16, top, right, bottom);
        else if (dg < 0)
            bg_render(left, top, (g0 - dg) * 16 - 1, bottom);
        /* the fog tiles over cells uncovered: a run of them at a time */
        for (t = fog_t0; t <= fog_t1; t++) {
            const uint8_t* row = fog_dirty + t * fog_stride;
            int y0 = t * 32 - 16, c = 0, start;
            if (y0 + 31 < top || y0 > bottom)
                continue;
            for (;;) {
                while (c < fog_stride && !row[c])
                    c++;
                if (c == fog_stride)
                    break;
                start = c;
                while (c < fog_stride && row[c])
                    c++;
                if (c * 32 - 17 >= left && start * 32 - 16 <= right)
                    bg_render(start * 32 - 16, y0, c * 32 - 17, y0 + 31);
            }
        }
        if (feat_dirty_x0 <= feat_dirty_x1)
            bg_render(
                feat_dirty_x0 - FEAT_SIDE,
                feat_dirty_y0 - FEAT_UP,
                feat_dirty_x1 + FEAT_SIDE,
                feat_dirty_y1 + FEAT_DOWN
            );
    }
    for (t = fog_t0; t <= fog_t1; t++)
        memset(fog_dirty + t * fog_stride, 0, fog_stride);
    fog_t0 = ex_h + 1;
    fog_t1 = -1;
    fog_all = 0;
    feat_dirty_x0 = 0;
    feat_dirty_x1 = -1;
}

/* Readies the composite: the background where last frame's things were, and
 * where the background changed */
/* Copies n groups (n > 0) from s to d, and returns where d got to: a group a
 * pass, which GCC's own loops do not manage */
static inline uint32_t* copy_run(uint32_t* d, const uint32_t* s, int n) {
    __asm__ volatile("subq.w #1,%2\n"
                     "1:\tmove.l (%1)+,(%0)+\n\t"
                     "move.l (%1)+,(%0)+\n\t"
                     "dbra %2,1b"
                     : "+a"(d), "+a"(s), "+d"(n)
                     :
                     : "memory");
    return d;
}

/* Copies a cell row of n groups from s to d, both in a ring */
static void copy_groups(uint32_t* d, const uint32_t* s, int n) {
    int y;
    for (y = 0; y < 16; y++, d += RING_STRIDE / 4, s += RING_STRIDE / 4)
        copy_run(d, s, n);
}

/* Whether a row of 32 cells has none marked, looked at four cells at a time */
_Static_assert(RING_G == 32 && VIEW_COLS == 32, "row_clear() looks at rows of 32 cells");
static int row_clear(const uint8_t* m) {
    const uint32_t* w = (const uint32_t*)m;
    return !(w[0] | w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7]);
}

static void comp_begin(void) {
    int r, c, prev = mark_cur; /* the last frame's marks, before the ring moves on */
    for (r = 0; r < CELL_ROWS; r++) {
        if (row_clear(marks[prev][r]))
            continue;
        for (c = 0; c < RING_G; c++) {
            int n = 0;
            if (!marks[prev][r][c])
                continue;
            while (c + n < RING_G && marks[prev][r][c + n])
                n++;
            copy_groups(
                (uint32_t*)(comp + r * 16 * RING_STRIDE + c * 8),
                (const uint32_t*)(bg + r * 16 * RING_STRIDE + c * 8),
                n
            );
            c += n - 1;
        }
    }
    mark_cur = (mark_cur + 1) % NMARKS;
    memset(marks[mark_cur], 0, sizeof marks[mark_cur]);
}

/* Copies n groups of the composite's ring row row, from ring group c on, to d */
static void view_row(uint32_t* d, const uint8_t* row, int c, int n) {
    const uint32_t* a = (const uint32_t*)(row + c * 8);
    int i, first = (RING_G - c < n ? RING_G - c : n) * 2;
    for (i = 0; i < first; i++)
        d[i] = a[i];
    a = (const uint32_t*)row - first;
    for (; i < n * 2; i++)
        d[i] = a[i];
}

/* Copies view rows y0..y1, screen groups k0..k1, from the composite to the
 * screen: a row's groups at most in two runs, where they wrap */
static void copy_view(uint8_t* scr, int y0, int y1, int k0, int k1) {
    int y, c = ring_mod((camx >> 4) + k0, RING_G), n = k1 - k0 + 1;
    int first = RING_G - c < n ? RING_G - c : n, rest = n - first;
    uint32_t* d = (uint32_t*)(scr + (VIEW_Y + y0) * LINE_BYTES + VIEW_X / 2 + k0 * 8);
    const uint32_t* row = (const uint32_t*)(comp + ring_mod(camy + y0, RING_H) * RING_STRIDE);
    const uint32_t* end = (const uint32_t*)(comp + RING_H * RING_STRIDE);
    for (y = y0; y <= y1; y++) {
        uint32_t* e = copy_run(d, row + c * 2, first);
        if (rest)
            copy_run(e, row, rest);
        d += LINE_BYTES / 4;
        row += RING_STRIDE / 4;
        if (row == end)
            row = (const uint32_t*)comp;
    }
}

/* Copies to the screen what changed in the composite since it was last copied there */
static void comp_to_screen(uint8_t* scr) {
    int r, k, g0 = camx >> 4;
    if (S->cam_x != camx || S->cam_y != camy || game_frames - S->last_frame >= NMARKS) {
        S->cam_x = camx;
        S->cam_y = camy;
        copy_view(scr, 0, VIEW_H - 1, 0, VIEW_COLS - 1);
    } else {
        /* the battlefield's cells in view that a frame since marked: screen
         * group k shows the battlefield's group g0 + k */
        static uint8_t any[CELL_ROWS][RING_G] __attribute__((aligned(4)));
        const uint32_t *m0 = (const uint32_t*)marks[0][0], *m1 = (const uint32_t*)marks[1][0],
                       *m2 = (const uint32_t*)marks[2][0], *m3 = (const uint32_t*)marks[3][0];
        uint32_t* u = (uint32_t*)any[0];
        int i, rr = ring_mod(camy >> 4, CELL_ROWS), cg = ring_mod(g0, RING_G), last = VIEW_COLS - 1;
        for (i = 0; i < CELL_ROWS * RING_G / 4; i++) /* four cells at a time */
            u[i] = m0[i] | m1[i] | m2[i] | m3[i];
        for (r = camy >> 4; r <= (camy + VIEW_H - 1) >> 4; r++, rr = rr + 1 < CELL_ROWS ? rr + 1 : 0) {
            uint8_t m[RING_G * 2]; /* the row twice over, so the view's cells run on unwrapped */
            const uint8_t* v = m + cg;
            int y0 = r * 16 > camy ? r * 16 - camy : 0;
            int y1 = r * 16 + 15 < camy + VIEW_H - 1 ? r * 16 + 15 - camy : VIEW_H - 1;
            int j = 0;
            if (row_clear(any[rr]))
                continue;
            memcpy(m, any[rr], RING_G);
            memcpy(m + RING_G, any[rr], RING_G);
            while (j <= last) {
                int j0;
                if (!v[j]) {
                    j++;
                    continue;
                }
                j0 = j;
                while (j <= last && v[j])
                    j++;
                copy_view(scr, y0, y1, j0, j - 1);
            }
        }
        /* and the cells this screen's own pointer covered */
        for (r = 0; r < VIEW_ROWS; r++) {
            if (row_clear(S->extra[r]))
                continue;
            for (k = 0; k < VIEW_COLS; k++) {
                int n = k;
                if (!S->extra[r][k])
                    continue;
                while (n + 1 < VIEW_COLS && S->extra[r][n + 1])
                    n++;
                copy_view(scr, r * 16, r * 16 + 15, k, n);
                k = n;
            }
        }
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
        "%c%ld.%ld",
        income < 0 ? '-' : '+',
        (long)((income < 0 ? -income : income) * TICKS / 256),
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
        hover = in_view(mx(), my()) ? unit_at(mx() - VIEW_X + camx, my() - VIEW_Y + camy, 3, unit_shown) : -1;
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

/* The minimap as explored: the mission's, black where no unit has been */
static uint16_t* mini;
static uint16_t mini_cx[128], mini_cy[128]; /* the sight cell under the minimap's each column and row */

static void mini_start(void) {
    int i;
    for (i = 0; i < mission.mw && i < 128; i++)
        mini_cx[i] = (uint16_t)(((2 * i + 1) * mapw / (2 * mission.mw)) >> EX_SHIFT);
    for (i = 0; i < mission.mh && i < 128; i++)
        mini_cy[i] = (uint16_t)(((2 * i + 1) * maph / (2 * mission.mh)) >> EX_SHIFT);
    memset(mini_dirty, 1, (size_t)ex_h);
    mini_any = 1;
}

/* Brings the minimap's rows over the cell rows explored since up to date;
 * rows over the same cell row share their masks */
static void mini_update(void) {
    int y, g, b, groups = mission.mw / 16, last = -1;
    uint16_t m[8];
    for (y = 0; y < mission.mh; y++) {
        const uint16_t* s = mission.minimap + y * groups * 4;
        uint16_t* d = mini + y * groups * 4;
        int cy = mini_cy[y];
        if (!mini_dirty[cy])
            continue;
        if (cy != last) {
            const uint8_t* e = explored + cy * ex_stride;
            last = cy;
            for (g = 0; g < groups; g++) {
                uint16_t k = 0;
                for (b = 0; b < 16; b++)
                    if (e[mini_cx[g * 16 + b]])
                        k |= (uint16_t)(0x8000 >> b);
                m[g] = k;
            }
        }
        for (g = 0; g < groups; g++, s += 4, d += 4) {
            d[0] = s[0] & m[g];
            d[1] = s[1] & m[g];
            d[2] = s[2] & m[g];
            d[3] = s[3] & m[g];
        }
    }
    memset(mini_dirty, 0, (size_t)ex_h);
    mini_any = 0;
}

static void draw_minimap(uint8_t* scr) {
    int i;
    int ox = minimap_ox, oy = minimap_oy;
    if (mini_any)
        mini_update();
    gfx_image(scr, mini, ox, oy, mission.mw / 16, mission.mh);
    gfx_clip(ox, oy, ox + mission.mw - 1, oy + mission.mh - 1);
    for (i = 0; i < unit_hi; i++) {
        const Unit* u = &units[i];
        int x, y;
        if (!u->alive)
            continue;
        if (!unit_shown(u) && !(u->flags & UF_ON_RADAR))
            continue;
        x = ox + (int)((u->x >> FIX) * mission.mw / mapw);
        y = oy + (int)((u->z >> FIX) * mission.mh / maph);
        gfx_pixel(scr, x, y, unittypes[u->type].side ? C_RED : C_BLUE);
        gfx_pixel(scr, x + 1, y, unittypes[u->type].side ? C_RED : C_BLUE);
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

static void draw_selection(const Unit* u, int x, int y) {
    const UnitType* t = &unittypes[u->type];
    int r = t->radius + 2, top = y - t->top - 2, bot = y + 4, l = 4, w;
    const Piece* p;
    if (t->kind != UK_MOBILE) {
        r = t->footx * 8;
        bot = y + t->footz * 8 - 4;
    }
    /* the health bar: hp is at most 65535, so this fits 32 bits */
    w = (int)(u->hp * 2 * r / (t->maxhp ? t->maxhp : 1));
    if (w < 0)
        w = 0;
    FOR_PIECES(p, x - r, top, x + r, bot + 3) {
        int sx = x + p->dx, t0 = top + p->dy, b0 = bot + p->dy;
        gfx_fill(comp, sx - r, t0, sx - r + l, t0, C_WHITE);
        gfx_fill(comp, sx - r, t0, sx - r, t0 + l, C_WHITE);
        gfx_fill(comp, sx + r - l, t0, sx + r, t0, C_WHITE);
        gfx_fill(comp, sx + r, t0, sx + r, t0 + l, C_WHITE);
        gfx_fill(comp, sx - r, b0, sx - r + l, b0, C_WHITE);
        gfx_fill(comp, sx - r, b0 - l, sx - r, b0, C_WHITE);
        gfx_fill(comp, sx + r - l, b0, sx + r, b0, C_WHITE);
        gfx_fill(comp, sx + r, b0 - l, sx + r, b0, C_WHITE);
        gfx_fill(comp, sx - r, b0 + 2, sx + r, b0 + 3, C_BLACK);
        gfx_fill(comp, sx - r, b0 + 2, sx - r + w, b0 + 3, w > r ? C_GREEN : (w > r / 2 ? C_YELLOW : C_RED));
    }
    mark(x - r, top, x + r, bot + 3);
}

/* Draws this frame's moving things on the composite, at battlefield
 * coordinates (y as on screen) */
static void draw_battlefield(void) {
    int i, n = 0;
    int x0 = camx - 96, x1 = camx + VIEW_W + 96, y0 = camy - 64, y1 = camy + VIEW_H + 128;
    uint32_t tt0 = HZ200;
    comp_begin();
    stage_hz[5] += HZ200 - tt0;
    tt0 = HZ200;
    gfx_stride = RING_STRIDE;
    clipped = 0;
    for (i = 0; i < unit_hi && n < MAX_DRAW; i++) {
        const Unit* u = &units[i];
        int ux, uz;
        if (!u->alive)
            continue;
        ux = u->x >> FIX;
        uz = u->z >> FIX;
        if (ux < x0 || ux > x1 || uz < y0 || uz > y1 || !unit_shown(u))
            continue;
        /* buildings sort by their back edge, so units in front of them come
         * later; aircraft come after everything on the ground */
        if (unittypes[u->type].kind != UK_MOBILE)
            uz -= unittypes[u->type].footz * 8 - 8;
        else if (unit_draw_y(u) != unit_screen_y(u))
            uz += 0x4000;
        drawlist[n++] = (uint32_t)(uz + 64) << 16 | (uint32_t)i;
    }
    sort_draw(n);
    for (i = 0; i < n; i++) {
        const Unit* u = &units[drawlist[i] & 0xFFFF];
        const UnitType* t = &unittypes[u->type];
        int dir = t->ndirs == 16 ? ((u->heading + 2048) >> 12) & 15 : 0;
        int sx = u->x >> FIX, sy = unit_draw_y(u);
        const int16_t* spr = unit_frame(u->type, dir);
        if (u->flags & UF_UNBUILT) {
            comp_blit(BLIT_NANO, spr, sx, sy, built_of(u, 256));
        } else {
            /* a walking unit bobs a pixel as it goes */
            if (u->speed && t->kind == UK_MOBILE && ((game_frames >> 1) & 1))
                sy--;
            comp_blit(BLIT_SPRITE, spr, sx, sy, 0);
            if (u->paralyzed && (game_frames & 2))
                comp_blit(BLIT_TINT, spr, sx, sy, C_WHITE);
        }
    }
    stage_hz[6] += HZ200 - tt0;
    /* nanolathe sprays */
    for (i = 0; i < unit_hi; i++) {
        const Unit* u = &units[i];
        int sx, sy, tx, ty;
        if (!u->alive || u->nano == -1 || (u->flags & UF_UNBUILT))
            continue;
        if (u->owner != PLAYER && !((u->flags & UF_SEEN) && unit_shown(u)))
            continue;
        sx = u->x >> FIX;
        sy = unit_draw_y(u) - unittypes[u->type].top / 2;
        if (u->nano >= 0) {
            const Unit* v = &units[u->nano];
            if (!v->alive)
                continue;
            tx = (v->x >> FIX) + ((game_frames * 7 + i * 3) % 17) - 8;
            ty = unit_draw_y(v) - unittypes[v->type].top / 2 + ((game_frames * 5 + i) % 11) - 5;
        } else {
            const Feature* f;
            if (~u->nano >= nfeats)
                continue;
            f = &feats[~u->nano];
            if (!f->alive)
                continue;
            tx = f->x + ((game_frames * 7) % 13) - 6;
            ty = ground_screen_y(f->x, f->z) - 8;
        }
        if (sx < x0 - 200 || sx > x1 + 200 || tx < x0 - 200 || tx > x1 + 200 || sy < y0 - 200 ||
            sy > y1 + 200)
            continue;
        draw_line(sx, sy, tx, ty, C_GREEN);
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
        sx = px;
        sy = ground_screen_y(px, pz) - (p->y >> 8);
        if (w->kind == WK_DGUN) {
            draw_fill(sx - 3, sy - 3, sx + 3, sy + 3, C_YELLOW);
            comp_fill(sx - 1, sy - 1, sx + 1, sy + 1, C_WHITE);
        } else if (w->kind == WK_ROCKET) {
            int bx = sx - (int)(p->vx >> (FIX + 1)), by = sy - (int)(p->vz >> (FIX + 1));
            draw_line(bx, by, sx, sy, C_LGREY);
            comp_fill(sx, sy, sx, sy, C_WHITE);
        } else {
            draw_fill(sx, sy, sx + 1, sy + 1, C_YELLOW);
        }
    }
    for (i = 0; i < MAX_BEAMS; i++) {
        const Beam* b = &beams[i];
        if (!b->alive)
            continue;
        if (b->x0 < x0 - 400 || b->x0 > x1 + 400 || b->y0 < y0 - 400 || b->y0 > y1 + 400)
            continue;
        draw_line(b->x0, b->y0, b->x1, b->y1, b->colour);
    }
    for (i = 0; i < MAX_FX; i++) {
        const Effect* e = &fx[i];
        if (!e->alive || e->x < x0 || e->x > x1 || e->y < y0 || e->y > y1)
            continue;
        comp_blit(BLIT_SPRITE, anim_frame(e->anim, e->frame), e->x, e->y, 0);
    }
    for (i = 0; i < unit_hi; i++) {
        const Unit* u = &units[i];
        int ux, uy;
        if (!u->alive || !(u->flags & UF_SELECTED))
            continue;
        ux = u->x >> FIX;
        uy = unit_draw_y(u);
        if (ux < x0 || ux > x1 || uy < y0 || uy > y1)
            continue;
        draw_selection(u, ux, uy);
    }
    /* the building being placed */
    if (mode == M_BUILD && build_type >= 0 && in_view(mx(), my())) {
        int wx, wz, bx, bz, sy, ok;
        const UnitType* t = &unittypes[build_type];
        const int16_t* spr = unit_frame(build_type, 0);
        screen_to_world(mx(), my(), &wx, &wz);
        bx = snap_build_x(build_type, wx);
        bz = snap_build_z(build_type, wz);
        ok = can_build_at(build_type, bx, bz);
        sy = bz - type_ground(build_type, bx, bz) / 2;
        comp_blit(BLIT_TINT, spr, bx, sy, ok ? C_GREEN : C_RED);
        comp_rect(
            bx - t->footx * 8,
            sy - t->footz * 8,
            bx + t->footx * 8 - 1,
            sy + t->footz * 8 - 1,
            ok ? C_GREEN : C_RED
        );
        mark(bx - t->footx * 8, sy - t->footz * 8, bx + t->footx * 8, sy + t->footz * 8);
    }
    if (drag && (mb() & 1)) {
        int dx = mx(), dy = my();
        if (dx < VIEW_X)
            dx = VIEW_X;
        if (dy < VIEW_Y)
            dy = VIEW_Y;
        /* the box on screen, as battlefield coordinates */
        dx += camx - VIEW_X;
        dy += camy - VIEW_Y;
        {
            int ax = drag_x + camx - VIEW_X, ay = drag_y + camy - VIEW_Y;
            comp_rect(ax < dx ? ax : dx, ay < dy ? ay : dy, ax > dx ? ax : dx, ay > dy ? ay : dy, C_WHITE);
            mark(ax < dx ? ax : dx, ay < dy ? ay : dy, ax > dx ? ax : dx, ay > dy ? ay : dy);
        }
    }
    /* the pause and the mission's end, over the middle of the view */
    if (paused || sim_result) {
        int vx = camx, vy = camy;
        if (paused) {
            comp_blit(BLIT_SPRITE, world_sprite(world.title[TITLE_PAUSED]), vx + 200, vy + 150, 0);
            comp_text(vx + 150, vy + 190, "Esc: resume   Q: quit to menu", C_WHITE);
        } else {
            comp_blit(
                BLIT_SPRITE,
                world_sprite(world.title[sim_result > 0 ? TITLE_VICTORY : TITLE_DEFEAT]),
                vx + 190,
                vy + 150,
                0
            );
            comp_text(vx + 180, vy + 190, "Click to continue", C_WHITE);
        }
        mark(vx + 140, vy + 150, vx + 400, vy + 210);
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
        else if (mode == M_CAPTURE)
            c = CUR_CAPTURE;
        else if (mode == M_REPAIR)
            c = CUR_REPAIR;
        else if (mode == M_BUILD)
            c = CUR_NORMAL;
        else if (unit_at(wx, wy, 1 << PLAYER, unit_shown) >= 0)
            c = CUR_SELECT;
        else if (nselected && unit_at(wx, wy, 1 << ENEMY, unit_shown) >= 0 && selected(SEL_ARMED, 0))
            c = CUR_ATTACK;
        else if (nselected && selected(SEL_MOBILE, 0))
            c = CUR_MOVE;
    }
    spr = world_sprite(world.cursor[c]);
    gfx_clip_full();
    gfx_sprite(scr, spr, x, y);
    sprite_box(spr, x, y, &S->cur_x0, &S->cur_y0, &S->cur_x1, &S->cur_y1);
}

/* The interface around the battlefield is drawn once a change into ui, a
 * screen-sized buffer in TT-RAM, and each screen copies the parts it lacks.
 * The panels as loaded are kept where they show, to draw on afresh: the
 * side panel's 480 rows of 8 groups, then the top bar's 32 rows of 32
 * groups, then the bottom bar's. */
static uint8_t* ui;
static uint8_t* keep;
#define KEEP_SIDE (480 * 64)
#define KEEP_BAR (32 * 256)

static void keep_panels(void) {
    int y;
    for (y = 0; y < SCREEN_H; y++)
        memcpy(keep + y * 64, ui + y * LINE_BYTES, 64);
    for (y = 0; y < VIEW_Y; y++)
        memcpy(keep + KEEP_SIDE + y * 256, ui + y * LINE_BYTES + 64, 256);
    for (y = 0; y < SCREEN_H - VIEW_Y - VIEW_H; y++)
        memcpy(keep + KEEP_SIDE + KEEP_BAR + y * 256, ui + (VIEW_Y + VIEW_H + y) * LINE_BYTES + 64, 256);
}

/* The side panel's rows y0..y1 put back in ui as loaded */
static void restore_side(int y0, int y1) {
    int y;
    for (y = y0; y <= y1; y++)
        memcpy(ui + y * LINE_BYTES, keep + y * 64, 64);
}

/* The bars above and below the battlefield put back in ui as loaded */
static void restore_bars(void) {
    int y;
    for (y = 0; y < VIEW_Y; y++)
        memcpy(ui + y * LINE_BYTES + 64, keep + KEEP_SIDE + y * 256, 256);
    for (y = 0; y < SCREEN_H - VIEW_Y - VIEW_H; y++)
        memcpy(ui + (VIEW_Y + VIEW_H + y) * LINE_BYTES + 64, keep + KEEP_SIDE + KEEP_BAR + y * 256, 256);
}
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
        for (r = r0 < 0 ? 0 : r0; r <= r1 && r < VIEW_ROWS; r++)
            for (c = c0 < 0 ? 0 : c0; c <= c1 && c < VIEW_COLS; c++)
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

/* Checks: group rows of the composite that differ from the background outside
 * this frame's marks, and group rows of the front screen that differ from the
 * view the composite holds */
/* Counts the group rows where two rings differ, leaving out this frame's
 * marked cells when asked; first gets the first, as row * 1000 + column */
static int ring_diff(const uint8_t* a, const uint8_t* b, int skip_marked, int* first) {
    int y, c, n = 0;
    *first = -1;
    for (y = 0; y < RING_H; y++)
        for (c = 0; c < RING_G; c++)
            if (!(skip_marked && marks[mark_cur][y >> 4][c]) &&
                memcmp(a + y * RING_STRIDE + c * 8, b + y * RING_STRIDE + c * 8, 8)) {
                if (*first < 0)
                    *first = y * 1000 + c;
                n++;
            }
    return n;
}

void game_check_layers(void) {
    int y, c, bad_comp, bad_scr = 0, first_c, first_s = -1;
    uint32_t want[VIEW_COLS * 2];
    bad_comp = ring_diff(comp, bg, 1, &first_c);
    for (y = 0; y < VIEW_H; y++) {
        const uint8_t* f = tt_front + (VIEW_Y + y) * LINE_BYTES + VIEW_X / 2;
        view_row(want, comp + ring_mod(camy + y, RING_H) * RING_STRIDE, ring_mod(camx >> 4, RING_G), VIEW_COLS);
        for (c = 0; c < VIEW_COLS; c++)
            if (memcmp((const uint8_t*)want + c * 8, f + c * 8, 8)) {
                if (first_s < 0)
                    first_s = y * 1000 + c;
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
    /* the background as kept, against the same drawn afresh */
    {
        uint8_t* kept = tt_alloc_any((int32_t)RING_STRIDE * RING_H);
        int bad_bg, first_b;
        if (!kept)
            return;
        memcpy(kept, bg, (size_t)RING_STRIDE * RING_H);
        bg_render(ring_g0 * 16, ring_y0, (ring_g0 + RING_G) * 16 - 1, ring_y0 + RING_H - 1);
        bad_bg = ring_diff(kept, bg, 0, &first_b);
        tt_free(kept);
        test_log(
            "check: background differs from a fresh drawing in %d group rows (first row %d col %d)",
            bad_bg,
            first_b / 1000,
            first_b % 1000
        );
    }
}

static void draw_frame(void) {
    uint8_t* scr = tt_back;
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
        S->cam_x = -1;
        S->side_serial = S->bar_serial = S->mini_serial = -1;
        S->cur_x1 = S->cur_x0 - 1;
    }
    erase_cursor(scr);
    bg_update();
    t2 = HZ200;
    stage_hz[0] += t2 - t;
    t = t2;
    draw_battlefield();
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
        restore_side(128, SCREEN_H - 1);
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
            restore_bars();
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
            int i = find_commander();
            if (i >= 0) {
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
    /* scrolling at the screen's edges, EDGE_SPEED pixels a second however
     * fast the frames come: down by the pixel, across a group at a time once
     * the pixels add up to one. */
    {
        static uint32_t then;
        static int32_t part; /* 200ths of a pixel not yet moved */
        static int across;   /* pixels across not yet moved, as a whole group */
        uint32_t now = HZ200, dt = now - then;
        int step, push_x = x <= 1 ? -1 : x >= SCREEN_W - 2, push_y = y <= 1 ? -1 : y >= SCREEN_H - 2;
        then = now;
        if (dt > 20)
            dt = 20; /* the first frame, or one after a wait: no leap */
        part += (int32_t)dt * EDGE_SPEED;
        step = (int)(part / 200);
        part %= 200;
        if (push_x)
            across += push_x * step;
        else
            across = 0;
        while (across >= 16) {
            camx += 16;
            across -= 16;
        }
        while (across <= -16) {
            camx -= 16;
            across += 16;
        }
        camy += push_y * step;
    }
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

/* What a mission keeps back from the sprites, for its other needs */
#define SPR_RESERVE_TT (48 * 1024L)
#define SPR_RESERVE_ST (160 * 1024L)
#define SPR_MIN (128 * 1024L)
#define SPR_MAX (1536 * 1024L)

static const char* run_error;

const char* game_error(void) {
    return run_error;
}

/* Everything a mission needs beside its file: the screen's buffers, its
 * world's colours and panels, the sim's and the explored map's arrays, and
 * what is left for the units' sprites */
static int mission_start(int difficulty) {
    int32_t tt, st, room;
    int i;
    if (!bg) {
        bg = tt_alloc_any((int32_t)RING_STRIDE * RING_H);
        comp = tt_alloc_any((int32_t)RING_STRIDE * RING_H);
        ui = tt_alloc_any(SCREEN_BYTES);
        mini = tt_alloc_any(128 / 16 * 8 * 128);
        keep = tt_alloc_any(KEEP_SIDE + 2 * KEEP_BAR);
    }
    if (!bg || !comp || !ui || !mini || !keep) {
        run_error = "needs more free RAM for the battlefield";
        return 0;
    }
    if (!load_world(mission.world) || !load_panel(mission.world, mission.side, ui)) {
        run_error = data_error();
        return 0;
    }
    keep_panels();
    if (!sim_start(difficulty) || !ex_alloc()) {
        run_error = "needs more free RAM for this mission's map";
        return 0;
    }
    tt = (int32_t)Mxalloc(-1, 1) - SPR_RESERVE_TT;
    st = (int32_t)Mxalloc(-1, 0) - SPR_RESERVE_ST;
    room = tt > st ? tt : st;
    if (room > SPR_MAX)
        room = SPR_MAX;
    if (room < SPR_MIN || !spr_open(mission.world, room)) {
        run_error = room < SPR_MIN ? "needs more free RAM for the units" : "can't read the units' sprites";
        return 0;
    }
    for (i = 0; i < mission.nneed; i++)
        spr_load(mission.need[i]);
    return 1;
}

static void mission_end(void) {
    spr_close();
    ex_free();
    sim_stop();
}

int game_run(int difficulty) {
    uint32_t last, acc = 0;
    int i, result = 0;
    mapw = mission.w * 16;
    maph = mission.h * 16;
    if (!mission_start(difficulty)) {
        mission_end();
        return GAME_FAILED;
    }
    mode = M_NONE;
    page = 1;
    build_page = 0;
    paused = 0;
    drag = 0;
    prev_buttons = (uint8_t)mb();
    nselected = 0;
    minimap_ox = ((128 - mission.mw) / 2) & ~15;
    minimap_oy = (128 - mission.mh) / 2;
    memset(ex_x, 0xFF, sizeof ex_x);
    fog_t0 = ex_h + 1;
    fog_t1 = -1;
    explore(); /* what the units see from the start; the first frame draws it all */
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
    tt_set_palette(world.palette);
    mini_start();
    side_serial = bar_serial = mini_serial = 0;
    ui_mini_frame = -100;
    memset(screens, 0, sizeof screens);
    for (i = 0; i < NSCREENS; i++)
        screens[i].scr = tt_screens[i];
    /* the rings hold nothing yet: the first frame draws them whole */
    bg_version = -1;
    nfeat_order = 0;
    memset(in_order, 0, sizeof in_order);
    following = 0;
    ring_g0 = -100000;
    ring_y0 = 0;
    set_pieces();
    mark_cur = 0;
    memset(marks, 0, sizeof marks);
    last = HZ200;
    for (;;) {
        uint32_t now, t0;
        int ticks = 0;
        if (test_hooks.next && test_hooks.next() < 0)
            break;
        if (following)
            follow_pointer();
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
                explore();
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
                    explore();
                    game_ticks_run++;
                }
                ticks++;
            }
        }
        game_sim_hz += HZ200 - t0;
        t0 = HZ200;
        spr_frame();
        snd_music(sim_tick_count - sim_last_fight < TICKS * 5);
        snd_update();
        draw_frame();
        game_draw_hz += HZ200 - t0;
        tt_flip();
        game_frames++;
    }
    snd_silence();
    mission_end();
    return result;
}
