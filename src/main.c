/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * AnnihilaTTion: Total Annihilation in TT medium resolution, the demo's
 * three Arm missions or the full game's two campaigns, as the data beside
 * it holds. The title menu, the campaigns' menus, the briefings, and the
 * scripted test driver (OATEST.TXT beside the data) that automated checks
 * use.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <mint/osbind.h>
#include "tt.h"
#include "gfx.h"
#include "data.h"
#include "sim.h"
#include "snd.h"
#include "game.h"
#include "sprites.h"
#include "film.h"

static const char* difficulty_names[3] = {"Easy", "Medium", "Hard"};

/* How far the player has got in each campaign: the missions open to play,
 * kept in PROGRESS.DAT */
static uint8_t progress[8];

static void load_progress(void) {
    long fh = Fopen("PROGRESS.DAT", 0);
    int i;
    memset(progress, 1, sizeof progress);
    if (fh >= 0) {
        (void)Fread((short)fh, sizeof progress, progress);
        (void)Fclose((short)fh);
    }
    for (i = 0; i < (int)sizeof progress; i++)
        if (!progress[i])
            progress[i] = 1;
}

static void save_progress(void) {
    long fh = Fcreate("PROGRESS.DAT", 0);
    if (fh >= 0) {
        (void)Fwrite((short)fh, sizeof progress, progress);
        (void)Fclose((short)fh);
    }
}

/* The campaign a mission is in */
static int campaign_of(int m) {
    int c;
    for (c = 0; c < ncampaigns; c++)
        if (m >= campaigns[c].first && m < campaigns[c].first + campaigns[c].count)
            return c;
    return 0;
}

/* What skips a film: a key or a click; in a test, its frames running out */
static int film_frames_left = -1;
static uint8_t film_buttons;

static int film_skip(int shown) {
    uint8_t b = mouse_buttons, pressed = (uint8_t)(b & ~film_buttons);
    if (film_frames_left >= 0)
        return shown >= film_frames_left;
    film_buttons = b;
    return tt_key() != 0 || (pressed & 3);
}

/* Plays a film when the data has it; 1 when it played */
static int film(int n) {
    int k;
    film_buttons = mouse_buttons;
    k = film_play(n, film_skip);
    return k >= 0;
}

/* The side the player takes in a skirmish: 0 Arm, 1 Core */
static int skirmish_side;

/* Readies a loaded mission: a skirmish's computer builds its base, and the
 * player playing Core takes the second start, the computer the first */
static void prepare_mission(int m) {
    int i, j;
    if (campaigns[campaign_of(m)].side != 2)
        return;
    mission.skirmish = 1;
    if (!skirmish_side)
        return;
    mission.side = 1;
    for (i = 0; i < mission.nschema; i++)
        for (j = 0; j < mission.schema[i].nunits; j++)
            mission.schema[i].units[j].player ^= 1;
}

/* The demo: one campaign of a few missions, chosen on its own screen */
static int is_demo(void) {
    return ncampaigns == 1 && campaigns[0].count <= 3;
}

/* ------------------------------------------------------------ the test driver */

static char* test_text;
static char* test_cur;
static int test_wait;
static int test_mission = -1, test_difficulty = 1;
static char test_mission_name[16]; /* named in the header, found once GAME.DAT is read */
static long logfh = -1;
static int test_quit;

void test_log(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    if (logfh < 0)
        return;
    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    strcat(buf, "\r\n");
    (void)Fwrite((short)logfh, (long)strlen(buf), buf);
}

void test_screenshot(const char* name) {
    char fn[32];
    long fh;
    sprintf(fn, "%s.RAW", name);
    fh = Fcreate(fn, 0);
    if (fh < 0)
        return;
    {
        uint16_t pal[16];
        tt_get_palette(pal);
        (void)Fwrite((short)fh, 32, pal);
    }
    (void)Fwrite((short)fh, SCREEN_BYTES, tt_front);
    (void)Fclose((short)fh);
}

static int next_line(char* line, int n) {
    int i = 0;
    if (!test_cur || !*test_cur)
        return 0;
    while (*test_cur && *test_cur != '\n' && i < n - 1) {
        if (*test_cur != '\r')
            line[i++] = *test_cur;
        test_cur++;
    }
    if (*test_cur == '\n')
        test_cur++;
    line[i] = 0;
    return 1;
}

static void log_stats(const char* what) {
    test_log(
        "stages ms: bg %ld field %ld (to screen %ld, units %ld) minimap %ld side %ld bars %ld",
        (long)stage_hz[0] * 5,
        (long)stage_hz[1] * 5,
        (long)stage_hz[5] * 5,
        (long)stage_hz[6] * 5,
        (long)stage_hz[2] * 5,
        (long)stage_hz[3] * 5,
        (long)stage_hz[4] * 5
    );
    test_log(
        "%s frames %ld ticks %ld draw %ld ms sim %ld ms (ms/frame draw %ld sim %ld) units %d",
        what,
        (long)game_frames,
        (long)game_ticks_run,
        (long)game_draw_hz * 5,
        (long)game_sim_hz * 5,
        game_frames ? (long)game_draw_hz * 5 / game_frames : 0L,
        game_frames ? (long)game_sim_hz * 5 / game_frames : 0L,
        players[0].units + players[1].units
    );
}

/* One frame of the script: runs commands until a wait */
static int test_next(void) {
    char line[128];
    if (test_wait > 0) {
        test_wait--;
        return 0;
    }
    while (next_line(line, sizeof line)) {
        int a = 0, b = 0, c = 0;
        char word[32];
        if (sscanf(line, "%31s", word) != 1 || word[0] == '#')
            continue;
        if (!strcmp(word, "wait")) {
            sscanf(line + 4, "%d", &a);
            test_wait = a - 1;
            return 0;
        } else if (!strcmp(word, "shot")) {
            sscanf(line + 4, "%31s", word);
            test_screenshot(word);
            test_log(
                "shot %s at frame %ld tick %ld", word, (long)game_frames, (long)sim_tick_count
            );
        } else if (!strcmp(word, "mouse")) {
            sscanf(line + 5, "%d %d %d", &a, &b, &c);
            test_mouse(a, b, c);
        } else if (!strcmp(word, "key")) {
            sscanf(line + 3, "%x %x", (unsigned*)&a, (unsigned*)&b);
            test_key((uint16_t)(a << 8 | b));
        } else if (!strcmp(word, "stats")) {
            log_stats("stats");
        } else if (!strcmp(word, "fast")) {
            game_fast = 1;
        } else if (!strcmp(word, "slow")) {
            game_fast = 0;
        } else if (!strcmp(word, "end")) {
            log_stats("end");
            test_log("result %d", sim_result);
            return -1;
        } else if (!strcmp(word, "quit")) {
            test_quit = 1;
            return -1;
        } else if (!strcmp(word, "units")) {
            int i;
            for (i = 0; i < unit_hi; i++)
                if (units[i].alive)
                    test_log(
                        "unit %d %s owner %d at %ld,%ld hp %ld orders %d kind %d",
                        i,
                        unittypes[units[i].type].name,
                        units[i].owner,
                        (long)(units[i].x >> FIX),
                        (long)(units[i].z >> FIX),
                        (long)units[i].hp,
                        units[i].norders,
                        units[i].norders ? units[i].order[0].kind : 0
                    );
        } else if (!strcmp(word, "debug")) {
            Unit* u;
            sscanf(line + 5, "%d", &a);
            u = &units[a];
            test_log(
                "debug %d %s alive %d at %ld,%ld goal %d,%d has %d want %d path %d/%d stuck %d "
                "repaths %d speed %d heading %u",
                a,
                unittypes[u->type].name,
                u->alive,
                (long)(u->x >> FIX),
                (long)(u->z >> FIX),
                u->goalx,
                u->goalz,
                u->has_goal,
                u->want_path,
                u->pathi,
                u->npath,
                u->stuck,
                u->repaths,
                u->speed,
                u->heading
            );
            {
                int k;
                for (k = 0; k < u->npath; k++)
                    test_log("  path %d: cell %u", k, u->path[k]);
            }
        } else if (!strcmp(word, "kill")) {
            char nm[16];
            int i, n = 0;
            sscanf(line + 4, "%15s", nm);
            for (i = 0; i < unit_hi; i++)
                if (units[i].alive && units[i].owner == ENEMY &&
                    !strcmp(unittypes[units[i].type].name, nm)) {
                    sim_kill(i);
                    n++;
                }
            test_log("killed %d %s", n, nm);
        } else if (!strcmp(word, "killall")) {
            int i, n = 0;
            for (i = 0; i < unit_hi; i++)
                if (units[i].alive && units[i].owner == ENEMY) {
                    sim_kill(i);
                    n++;
                }
            test_log("killed %d", n);
        } else if (!strcmp(word, "give") || !strcmp(word, "spawn")) {
            char nm[16];
            int i, t = -1;
            a = b = c = 0;
            sscanf(line + strlen(word), "%15s %d %d %d", nm, &a, &b, &c);
            for (i = 0; i < nunittypes; i++)
                if (!strcmp(unittypes[i].name, nm))
                    t = i;
            if (word[0] == 'g') {
                for (i = 0; i < unit_hi; i++)
                    if (units[i].alive && units[i].owner == ENEMY && units[i].type == t) {
                        sim_capture(i, PLAYER);
                        break;
                    }
                test_log("gave %s: unit %d", nm, i < unit_hi ? i : -1);
            } else {
                test_log("spawned %s: unit %d", nm, t >= 0 ? sim_spawn(t, c ? ENEMY : PLAYER, a, b) : -1);
            }
        } else if (!strcmp(word, "move") || !strcmp(word, "attack") || !strcmp(word, "capture")) {
            sscanf(line + strlen(word), "%d %d %d", &a, &b, &c);
            if (a >= 0 && a < unit_hi && units[a].alive) {
                if (word[0] == 'm') {
                    order_at(&units[a], O_MOVE, b, c, 0);
                } else {
                    Order o;
                    memset(&o, 0, sizeof o);
                    o.kind = word[0] == 'a' ? O_ATTACK : O_CAPTURE;
                    o.target = (int16_t)b;
                    order_add(&units[a], &o, 0);
                }
            }
        } else if (!strcmp(word, "teleport")) {
            sscanf(line + 8, "%d %d %d", &a, &b, &c);
            units[a].x = (int32_t)b << FIX;
            units[a].z = (int32_t)c << FIX;
            order_stop(&units[a]);
        } else if (!strcmp(word, "feat")) {
            game_list_features();
        } else if (!strcmp(word, "result")) {
            test_log("result now %d", sim_result);
        } else if (!strcmp(word, "check")) {
            game_check_layers();
        } else if (!strcmp(word, "proj")) {
            int i, n = 0, nb = 0;
            for (i = 0; i < MAX_PROJ; i++)
                if (projs[i].alive) {
                    if (n++ < 12)
                        test_log(
                            "proj %d weapon %s owner %d at %ld,%ld ticks %d",
                            i,
                            weapons[projs[i].weapon].name,
                            projs[i].owner,
                            (long)(projs[i].x >> FIX),
                            (long)(projs[i].z >> FIX),
                            projs[i].ticks
                        );
                }
            for (i = 0; i < MAX_BEAMS; i++)
                nb += beams[i].alive;
            test_log("projectiles %d beams %d", n, nb);
        } else if (!strcmp(word, "camera")) {
            sscanf(line + 6, "%d %d", &a, &b);
            game_camera(a, b);
        } else if (!strcmp(word, "follow")) {
            game_follow(sscanf(line + 6, "%d", &a) == 1 ? a : 1);
        } else if (!strcmp(word, "explore")) {
            game_explore_all();
        } else if (!strcmp(word, "explored")) {
            game_log_explored();
        } else if (!strcmp(word, "film")) {
            uint32_t t0 = HZ200;
            int k;
            a = 0;
            b = -1;
            sscanf(line + 4, "%d %d", &a, &b);
            film_frames_left = b;
            k = film_play(a, film_skip);
            film_frames_left = -1;
            test_log("film %d: %d frames in %ld ms", a, k, (long)(HZ200 - t0) * 5);
        } else if (!strcmp(word, "sprites")) {
            int32_t used, size;
            int n = spr_stats(&used, &size);
            test_log("sprites: %d types held, %ld of %ld bytes", n, (long)used, (long)size);
        } else if (!strcmp(word, "res")) {
            for (a = 0; a < 2; a++)
                test_log(
                    "%s metal %ld/%ld (+%ld) energy %ld/%ld (+%ld) units %d kills %d losses %d",
                    a ? "core" : "player",
                    (long)(players[a].metal >> 8),
                    (long)players[a].mstore,
                    (long)players[a].mincome,
                    (long)(players[a].energy >> 8),
                    (long)players[a].estore,
                    (long)players[a].eincome,
                    players[a].units,
                    players[a].kills,
                    players[a].losses
                );
        }
    }
    test_log("script ran out");
    test_quit = 1;
    return -1;
}

static int load_test(void) {
    char line[128];
    test_text = load_text("OATEST.TXT");
    if (!test_text)
        return 0;
    test_cur = test_text;
    logfh = Fcreate("OALOG.TXT", 0);
    /* the header: the mission to start, by number (1 on) or by its file's name */
    while (next_line(line, sizeof line)) {
        if (!strncmp(line, "mission", 7)) {
            char which[16], side[8] = "";
            if (sscanf(line + 7, "%15s %d %7s", which, &test_difficulty, side) >= 1) {
                skirmish_side = !strcmp(side, "core");
                if (which[0] >= '0' && which[0] <= '9')
                    test_mission = atoi(which);
                else
                    strcpy(test_mission_name, which);
            }
            break;
        }
        if (!strncmp(line, "menu", 4))
            break;
    }
    test_hooks.next = test_next;
    return 1;
}

/* ------------------------------------------------------------ screens */

typedef struct {
    int x0, y0, x1, y1;
} Slot;

/* The button slots in TA's own menu pictures */
static const Slot title_slots[4] = {
    {139, 393, 235, 412}, {409, 393, 505, 412}, {139, 430, 235, 449}, {408, 430, 505, 449}
};
static const Slot single_slots[5] = {
    {447, 136, 567, 155},
    {447, 178, 568, 197},
    {446, 220, 568, 239},
    {447, 262, 568, 281},
    {447, 346, 568, 365}
};

static void show_picture(uint8_t* pic, uint8_t* dst) {
    memcpy(dst, pic + 32, SCREEN_BYTES);
}

static void draw_arrow(uint8_t* scr, int x, int y) {
    int i;
    gfx_clip_full();
    for (i = 0; i < 12; i++) {
        gfx_fill(scr, x, y + i, x + i / 2 + 1, y + i, C_WHITE);
        gfx_pixel(scr, x + i / 2 + 2, y + i, C_BLACK);
    }
    gfx_fill(scr, x, y + 12, x + 7, y + 12, C_BLACK);
}

static void text_centre(uint8_t* scr, int y, const char* s, int colour) {
    gfx_text(scr, (SCREEN_W - gfx_text_width(s)) / 2, y, s, colour);
}

/* A screen of text: its picture, or black, and a box framed in grey with a title */
static void boxed_screen(uint8_t* pic, const char* title) {
    if (pic)
        show_picture(pic, tt_back);
    else
        memset(tt_back, 0, SCREEN_BYTES);
    gfx_clip_full();
    gfx_fill(tt_back, 40, 40, 600, 410, C_BLACK);
    gfx_rect(tt_back, 40, 40, 600, 410, C_GREY);
    gfx_text(tt_back, 60, 52, title, C_WHITE);
}

static int in_slot(const Slot* sl, int x, int y) {
    return x >= sl->x0 - 2 && x <= sl->x1 + 2 && y >= sl->y0 - 2 && y <= sl->y1 + 2;
}

/* A button in a slot; framed means a grey frame when the pointer is not on it */
static void draw_slot(uint8_t* scr, const Slot* sl, const char* s, int hover, int framed) {
    gfx_fill(scr, sl->x0, sl->y0, sl->x1, sl->y1, C_BLACK);
    if (hover || framed)
        gfx_rect(scr, sl->x0, sl->y0, sl->x1, sl->y1, hover ? C_YELLOW : C_GREY);
    gfx_text(
        scr,
        (sl->x0 + sl->x1 - gfx_text_width(s)) / 2,
        sl->y0 + (sl->y1 - sl->y0 - 11) / 2, /* a 12-pixel line, centred */
        s,
        hover && !framed ? C_WHITE : C_YELLOW
    );
}

/* A frame of input for a menu: the pointer, and 1 when the left button went down */
static int menu_input(int* x, int* y, uint8_t* prev, uint16_t* key) {
    uint8_t b;
    int pressed;
    if (test_hooks.next && test_hooks.next() < 0)
        test_quit = 1;
    snd_update();
    *x = input_x();
    *y = input_y();
    b = (uint8_t)input_buttons();
    pressed = (b & ~*prev) & 1;
    *prev = b;
    *key = input_key();
    return pressed;
}

static void help_screen(void) {
    uint16_t pal[16], k;
    uint8_t* pic = load_picture("BRIEF.IMG", pal);
    uint8_t prev = (uint8_t)input_buttons();
    int x, y;
    static const char* help =
        "&YCOMMANDS&W\n"
        "Left click a unit to select it, or drag a box around several. Shift adds to the "
        "selection. "
        "With units selected, left click the ground to move, an enemy to attack, a wreck or tree "
        "to reclaim it. "
        "Right click cancels an order button, or clears the selection.\n\n"
        "&YSIDE PANEL&W\n"
        "BUILD shows what the selected builder can make: click a picture, then a place on the map. "
        "For a factory, left click adds one to its queue (shift: five) and right click takes one "
        "away. "
        "ORDERS holds Move, Attack, Stop, Patrol, Guard, D-Gun (and a silo's missile), Reclaim, "
        "Capture and Repair. "
        "Click the minimap to look somewhere; right click it to send the selection there.\n\n"
        "&YKEYS&W\n"
        "M move, A attack, S stop, P patrol, G guard, D D-gun, R reclaim, T capture, E repair, "
        "B build menu, O orders, "
        "C find the Commander, Ctrl+A select every unit, arrows or the screen edges scroll, "
        "Esc pauses (Q in the pause quits the mission), X turns sound off and on. "
        "On the title, I shows the introduction and C the credits.\n\n"
        "&YRESOURCES&W\n"
        "Metal comes from extractors on metal deposits and from reclaiming wrecks; energy from "
        "solar collectors, "
        "wind generators and trees. Building spends both as it goes: when either runs out, "
        "building waits.";
    if (pic)
        tt_set_palette(pal);
    for (;;) {
        boxed_screen(pic, "HOW TO PLAY");
        gfx_text_box(tt_back, 60, 76, 520, help, C_SILVER);
        gfx_text(tt_back, 200, 428, "Click or press a key to go back", C_YELLOW);
        draw_arrow(tt_back, input_x(), input_y());
        tt_flip();
        if (menu_input(&x, &y, &prev, &k) || k || test_quit)
            break;
    }
    tt_free(pic);
}

/* The title: 0 single player, -1 exit */
static int title_screen(uint8_t* pic, const uint16_t* pal) {
    uint8_t prev = (uint8_t)input_buttons();
    tt_set_palette(pal);
    for (;;) {
        uint16_t k;
        int i, x, y, hover = -1, click;
        static const char* labels[4] = {"SINGLE PLAYER", "HOW TO PLAY", "SOUND", "EXIT"};
        click = menu_input(&x, &y, &prev, &k);
        if (test_quit)
            return -1;
        show_picture(pic, tt_back);
        gfx_clip_full();
        for (i = 0; i < 4; i++) {
            char s[24];
            if (in_slot(&title_slots[i], x, y))
                hover = i;
            if (i == 2)
                sprintf(s, "SOUND %s", snd_enabled ? "ON" : "OFF");
            else
                strcpy(s, labels[i]);
            draw_slot(tt_back, &title_slots[i], s, hover == i, 0);
        }
        text_centre(tt_back, 462, "AnnihilaTTion - neilrackett.com/atarist", C_GREY);
        draw_arrow(tt_back, x, y);
        tt_flip();
        if ((k & 0xFF) == 13 || (k & 0xFF) == ' ')
            return 0;
        if ((k >> 8) == 1 || (k & 0xDF) == 'Q')
            return -1;
        /* the films, where the data has them: I the introduction, C the credits */
        if ((k & 0xDF) == 'I' || (k & 0xDF) == 'C') {
            if (film((k & 0xDF) == 'I' ? FILM_INTRO : FILM_CREDITS))
                tt_set_palette(pal);
            prev = (uint8_t)input_buttons();
        }
        if (click && hover >= 0) {
            snd_play(SND_BUTTON, 1);
            if (hover == 0)
                return 0;
            if (hover == 1) {
                help_screen();
                tt_set_palette(pal);
                prev = (uint8_t)input_buttons();
            }
            if (hover == 2)
                snd_enabled = !snd_enabled;
            if (hover == 3)
                return -1;
        }
    }
}

/* The single player screen: in its first slots the demo's missions, or the
 * full game's campaigns; then the difficulty and BACK. Returns the one
 * chosen (a mission for the demo, a campaign else), or -1 to go back */
static int single_screen(int* difficulty, const char* note) {
    uint16_t pal[16];
    uint8_t* pic = load_picture("SINGLE.IMG", pal);
    uint8_t prev = (uint8_t)input_buttons();
    int demo = is_demo(), result = -1;
    int n = demo ? campaigns[0].count : ncampaigns < 3 ? ncampaigns : 3, ty = demo ? 220 : 400;
    if (pic)
        tt_set_palette(pal);
    for (;;) {
        uint16_t k;
        int i, x, y, hover = -1, click;
        click = menu_input(&x, &y, &prev, &k);
        if (test_quit)
            break;
        if (pic)
            show_picture(pic, tt_back);
        else
            memset(tt_back, 0, SCREEN_BYTES);
        gfx_clip_full();
        for (i = 0; i < 5; i++) {
            char s[32];
            if (i < n && demo)
                sprintf(s, "MISSION %d", i + 1);
            else if (i < n)
                strcpy(s, campaigns[i].name);
            else if (i == 3)
                strcpy(s, difficulty_names[*difficulty]);
            else if (i == 4)
                strcpy(s, "BACK");
            else
                continue;
            if (in_slot(&single_slots[i], x, y))
                hover = i;
            draw_slot(tt_back, &single_slots[i], s, hover == i, 0);
        }
        if (demo) {
            gfx_text(tt_back, 60, 110, campaigns[0].name, C_WHITE);
            for (i = 0; i < n; i++)
                gfx_text(tt_back, 60, 140 + i * 22, missions[i].name, hover == i ? C_YELLOW : C_SILVER);
        }
        gfx_text(tt_back, 60, ty, "Difficulty:", C_SILVER);
        gfx_text(tt_back, 140, ty, difficulty_names[*difficulty], hover == 3 ? C_YELLOW : C_WHITE);
        if (note)
            gfx_text_box(tt_back, 60, demo ? 400 : 420, 330, note, C_YELLOW);
        draw_arrow(tt_back, x, y);
        tt_flip();
        if ((k & 0xFF) >= '1' && (k & 0xFF) < '1' + n) {
            result = (k & 0xFF) - '1';
            break;
        }
        if ((k >> 8) == 1)
            break;
        if ((k & 0xDF) == 'D')
            *difficulty = (*difficulty + 1) % 3;
        if (click && hover >= 0) {
            snd_play(SND_BUTTON, 1);
            if (hover < n) {
                result = hover;
                break;
            }
            if (hover == 3)
                *difficulty = (*difficulty + 1) % 3;
            if (hover == 4)
                break;
        }
    }
    tt_free(pic);
    return result;
}

/* The briefing's buttons */
static const Slot start_slot = {120, 424, 280, 452}, back_slot = {360, 424, 520, 452};

static int on_slot(const Slot* sl, int x, int y) {
    return x >= sl->x0 && x <= sl->x1 && y >= sl->y0 && y <= sl->y1;
}

/* A campaign's missions, those reached open: the one chosen, as an index
 * in missions[], or -1 to go back */
static int mission_screen(int c) {
    const Campaign* cp = &campaigns[c];
    uint16_t pal[16];
    uint8_t* pic = load_picture(cp->side == 1 ? "BRIEFCOR.IMG" : "BRIEFARM.IMG", pal);
    uint8_t prev = (uint8_t)input_buttons();
    int result = -1, rows = (cp->count + 1) / 2, skirmish = cp->side == 2;
    if (pic)
        tt_set_palette(pal);
    for (;;) {
        uint16_t k;
        int i, x, y, hover = -1, click;
        click = menu_input(&x, &y, &prev, &k);
        if (test_quit)
            break;
        boxed_screen(pic, cp->name);
        for (i = 0; i < cp->count; i++) {
            int col = i / rows, row = i % rows, mx = 60 + col * 270, my = 84 + row * 24;
            int open = skirmish || i < progress[c];
            if (open && x >= mx - 4 && x < mx + 264 && y >= my - 3 && y < my + 17)
                hover = i;
            gfx_text(tt_back, mx, my, missions[cp->first + i].name,
                     !open ? C_GREY : hover == i ? C_YELLOW : C_SILVER);
        }
        draw_slot(tt_back, &back_slot, "BACK", on_slot(&back_slot, x, y), 1);
        if (skirmish)
            draw_slot(tt_back, &start_slot, skirmish_side ? "PLAY AS CORE" : "PLAY AS ARM",
                      on_slot(&start_slot, x, y), 1);
        draw_arrow(tt_back, x, y);
        tt_flip();
        if ((k >> 8) == 1)
            break;
        if (click && hover >= 0) {
            snd_play(SND_BUTTON, 1);
            result = cp->first + hover;
            break;
        }
        if (skirmish && click && on_slot(&start_slot, x, y)) {
            snd_play(SND_BUTTON, 1);
            skirmish_side ^= 1;
        }
        if (click && on_slot(&back_slot, x, y))
            break;
    }
    tt_free(pic);
    return result;
}

/* The briefing: 1 to start, 0 to go back */
static int briefing(int m) {
    uint16_t pal[16];
    int core = campaigns[campaign_of(m)].side == 2 ? skirmish_side : missions[m].side;
    uint8_t* pic = load_picture(core ? "BRIEFCOR.IMG" : "BRIEFARM.IMG", pal);
    char name[24];
    char* text;
    uint8_t prev = (uint8_t)input_buttons();
    int result = 1;
    sprintf(name, "%s.TXT", missions[m].file);
    text = load_text(name);
    sprintf(name, "%s.SND", missions[m].file);
    if (pic)
        tt_set_palette(pal);
    snd_stream_file(name); /* the narration, read from the disk as it plays */
    for (;;) {
        uint16_t k;
        int x, y, click;
        click = menu_input(&x, &y, &prev, &k);
        if (test_quit) {
            result = 0;
            break;
        }
        boxed_screen(pic, missions[m].name);
        gfx_text_box(tt_back, 60, 80, 520, text ? text : "", C_SILVER);
        draw_slot(tt_back, &start_slot, "START", on_slot(&start_slot, x, y), 1);
        draw_slot(tt_back, &back_slot, "BACK", on_slot(&back_slot, x, y), 1);
        draw_arrow(tt_back, x, y);
        tt_flip();
        if ((k & 0xFF) == 13 || (k & 0xFF) == ' ')
            break;
        if ((k >> 8) == 1) {
            result = 0;
            break;
        }
        if (click && on_slot(&start_slot, x, y))
            break;
        if (click && on_slot(&back_slot, x, y)) {
            result = 0;
            break;
        }
    }
    snd_stop();
    tt_free(pic);
    tt_free(text);
    return result;
}

/* Loading: the mission's glamour picture */
static void loading(int m) {
    uint16_t pal[16];
    char name[24];
    uint8_t* pic;
    snd_silence();
    sprintf(name, "%s.IMG", missions[m].file);
    pic = load_picture(name, pal);
    if (pic) {
        show_picture(pic, tt_back);
        tt_set_palette(pal);
        tt_free(pic);
    } else {
        static const uint16_t grey[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x777, 0xBBB, 0xFFF};
        memset(tt_back, 0, SCREEN_BYTES);
        tt_set_palette(grey);
    }
    gfx_clip_full();
    gfx_fill(tt_back, 240, 440, 400, 464, C_BLACK);
    text_centre(tt_back, 446, "Loading...", C_WHITE);
    tt_flip();
}

/* Gives the machine back and says what went wrong (see tt_sorry()) */
static int fatal(const char* why) {
    tt_exit();
    tt_sorry(why);
    return 1;
}

/* Plays a mission and the ones after it while they are won (a skirmish
 * once): 0 back to the menu, 1 when its campaign is won, -1 to quit, or -2
 * when the game cannot go on (fatal() has said why) */
static int play_from(int m, int difficulty, const char** note) {
    int c = campaign_of(m);
    for (;;) {
        int r;
        free_mission();
        if (!briefing(m) || test_quit)
            return test_quit ? -1 : 0;
        loading(m);
        if (!load_mission(m)) {
            fatal(data_error());
            return -2;
        }
        prepare_mission(m);
        r = game_run(difficulty);
        if (r == GAME_FAILED) {
            fatal(game_error());
            return -2;
        }
        test_log("game_run returned %d", r);
        if (test_quit)
            return -1;
        if (campaigns[c].side == 2) {
            if (r)
                *note = r > 0 ? "&YVictory!&W The enemy Commander is destroyed." : "&RDefeat.&W Your Commander is lost.";
            return 0;
        }
        if (r > 0) {
            int next = m + 1 - campaigns[c].first;
            if (next < campaigns[c].count && progress[c] < next + 1) {
                progress[c] = (uint8_t)(next + 1);
                save_progress();
            }
            if (m + 1 >= campaigns[c].first + campaigns[c].count) {
                if (is_demo()) {
                    *note = "&YVictory!&W You have won the Total Annihilation demo campaign.";
                } else {
                    *note = campaigns[c].side ? "&YVictory!&W The Core campaign is won."
                                              : "&YVictory!&W The Arm campaign is won.";
                    if (film(campaigns[c].side ? FILM_CORE_WINS : FILM_ARM_WINS))
                        film(FILM_CREDITS);
                }
                return 1;
            }
            m++;
            continue;
        }
        if (r < 0)
            continue; /* lost: brief again, to try again */
        return 0;
    }
}

int main(void) {
    uint16_t title_pal[16];
    uint8_t* title;
    int difficulty = 1;
    const char* note = 0;
    if (!tt_init())
        return 1;
    load_test();
    if (!load_game() || !load_common() || !load_units() || !load_sounds())
        return fatal(data_error());
    gfx_set_font(common.font);
    snd_init();
    load_progress();
    if (test_mission_name[0] && (test_mission = find_mission(test_mission_name) + 1) <= 0)
        return fatal("can't find the test's mission");
    if (test_mission > 0) {
        int r, m = test_mission - 1;
        if (m >= nmissions)
            return fatal("can't find the test's mission");
        loading(m);
        if (!load_mission(m))
            return fatal(data_error());
        prepare_mission(m);
        test_log("mission %d loaded: %s, %d tiles", test_mission, mission.name, mission.ntiles);
        test_log(
            "free memory: ST-RAM %ld, TT-RAM %ld (largest blocks)",
            (long)Mxalloc(-1, 0),
            (long)Mxalloc(-1, 1)
        );
        r = game_run(test_difficulty);
        if (r == GAME_FAILED) {
            test_log("game_run failed: %s", game_error());
            r = 0;
        }
        test_log("game_run returned %d", r);
        if (logfh >= 0)
            (void)Fclose((short)logfh);
        tt_exit();
        return 0;
    }
    title = load_picture("TITLE.IMG", title_pal);
    if (!title)
        return fatal(data_error());
    /* the logo and the introduction, as TA starts; not for a test's menus */
    if (!test_hooks.next && film(FILM_LOGO))
        film(FILM_INTRO);
    while (!test_quit && title_screen(title, title_pal) == 0) {
        for (;;) {
            int m, r;
            m = single_screen(&difficulty, note);
            note = 0;
            if (m < 0 || test_quit)
                break;
            if (!is_demo() && (m = mission_screen(m)) < 0)
                continue;
            r = play_from(m, difficulty, &note);
            if (r == -2)
                return 1;
            if (r < 0 || test_quit)
                break;
        }
    }
    if (logfh >= 0)
        (void)Fclose((short)logfh);
    tt_exit();
    return 0;
}
