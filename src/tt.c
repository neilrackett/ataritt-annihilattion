/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Atari TT's hardware for the demake (see tt.h).
 */
#include <string.h>
#include <mint/osbind.h>
#include <mint/cookie.h>
#include <mint/falcon.h>
#include "tt.h"

#define TT_SHIFT (*(volatile uint16_t*)0xFFFF8262UL)
#define TT_PALETTE ((volatile uint16_t*)0xFFFF8400UL)
#define TT_MODE_MED 0x0400
#define VBASE_HI (*(volatile uint8_t*)0xFFFF8201UL)
#define VBASE_MID (*(volatile uint8_t*)0xFFFF8203UL)
#define VBASE_LO (*(volatile uint8_t*)0xFFFF820DUL)
#define VBLCLOCK (*(volatile uint32_t*)0x462UL)

/* A Falcon's own: its 256 colours (6 bits a gun, 0xRRGG00BB), and the
 * sound matrix's connections and clock dividers */
#define FALCON_PALETTE ((volatile uint32_t*)0xFFFF9800UL)
#define XB_SRC (*(volatile uint16_t*)0xFFFF8930UL)
#define XB_DST (*(volatile uint16_t*)0xFFFF8932UL)
#define XB_EXTPRE (*(volatile uint8_t*)0xFFFF8934UL)
#define XB_INTPRE (*(volatile uint8_t*)0xFFFF8935UL)
#define FALCON_MODE (VGA | COL80 | BPS4) /* 640x480 in 16 colours, 4 planes */
/* The megabytes a Falcon must have free in one block at start: the three
 * screens, the data and mission 3, as measured, with room to spare */
#define FALCON_MEGABYTES 6
#define STR_(x) #x
#define STR(x) STR_(x)

#define SND_CTRL (*(volatile uint8_t*)0xFFFF8901UL)
#define SND_START_HI (*(volatile uint8_t*)0xFFFF8903UL)
#define SND_START_MID (*(volatile uint8_t*)0xFFFF8905UL)
#define SND_START_LO (*(volatile uint8_t*)0xFFFF8907UL)
#define SND_END_HI (*(volatile uint8_t*)0xFFFF890FUL)
#define SND_END_MID (*(volatile uint8_t*)0xFFFF8911UL)
#define SND_END_LO (*(volatile uint8_t*)0xFFFF8913UL)
#define SND_MODE (*(volatile uint8_t*)0xFFFF8921UL)

volatile int16_t mouse_x = 320, mouse_y = 240;
volatile uint8_t mouse_buttons;
uint8_t *tt_back, *tt_front;
uint8_t* tt_screens[NSCREENS];
int tt_plays_6258;
static int falcon;                /* a Falcon, not a TT: an experiment, see the README */
static uint32_t set_at[NSCREENS]; /* the blank count when each was last made the base */
static int latest, shown;         /* the base last set, and the screen surely on show */
static uint32_t old_cacr;
static uint16_t shown_palette[16]; /* the colours tt_set_palette() last set */

static uint16_t old_shift, old_palette[256]; /* the TT's, as the game found them */
static uint32_t old_base;
static void* old_mousevec;
static _KBDVECS* kbdv;
static long old_ssp;

/* The IKBD hands a relative mouse packet to the handler in a0:
 * header (0xF8 | buttons; bit 1 left, bit 0 right), dx, dy. */
void mouse_handler(void);
__asm__("    .text\n"
        "    .globl mouse_handler\n"
        "mouse_handler:\n"
        "    movem.l %d0-%d1/%a1,-(%sp)\n"
        "    moveq #0,%d0\n"
        "    move.b (%a0),%d0\n"
        "    moveq #0,%d1\n"
        "    btst #1,%d0\n"
        "    beq.s 1f\n"
        "    moveq #1,%d1\n"
        "1:  btst #0,%d0\n"
        "    beq.s 2f\n"
        "    or.b #2,%d1\n"
        "2:  move.b %d1,mouse_buttons\n"
        "    move.b 1(%a0),%d0\n"
        "    ext.w %d0\n"
        "    add.w mouse_x,%d0\n"
        "    bge.s 3f\n"
        "    moveq #0,%d0\n"
        "3:  cmp.w #639,%d0\n"
        "    ble.s 4f\n"
        "    move.w #639,%d0\n"
        "4:  move.w %d0,mouse_x\n"
        "    move.b 2(%a0),%d0\n"
        "    ext.w %d0\n"
        "    add.w mouse_y,%d0\n"
        "    bge.s 5f\n"
        "    moveq #0,%d0\n"
        "5:  cmp.w #479,%d0\n"
        "    ble.s 6f\n"
        "    move.w #479,%d0\n"
        "6:  move.w %d0,mouse_y\n"
        "    movem.l (%sp)+,%d0-%d1/%a1\n"
        "    rts\n");

void tt_sorry(const char* why) {
    /* a 40-column console cuts long lines off: they wrap while this writes */
    (void)Cconws("\033v\r\nSorry, AnnihilaTTion ");
    (void)Cconws(why);
    (void)Cconws(".\r\nPress a key.\r\n\033w");
    (void)Cnecin();
}

void* tt_alloc_st(int32_t bytes) {
    long p = (long)Mxalloc(bytes, 0);
    return p > 0 ? (void*)p : 0;
}

void* tt_alloc_any(int32_t bytes) {
    long p = (long)Mxalloc(bytes, 3);
    return p > 0 ? (void*)p : 0;
}

void tt_free(void* p) {
    if (p)
        (void)Mfree(p);
}

static void set_base(const void* a) {
    uint32_t v = (uint32_t)a;
    VBASE_HI = (uint8_t)(v >> 16);
    VBASE_MID = (uint8_t)(v >> 8);
    VBASE_LO = (uint8_t)v;
}

void tt_wait_vbl(void) {
    uint32_t t = VBLCLOCK;
    while (VBLCLOCK == t)
        ;
}

/* ------------------------------------------------------------ TT medium on a TT */

/* The shifter's mode and colours saved, then TT medium set; the 68030's
 * caches on, with burst fill */
static void tt_medium_start(void) {
    int i;
    old_shift = TT_SHIFT;
    for (i = 0; i < 256; i++)
        old_palette[i] = TT_PALETTE[i];
    old_base = (uint32_t)VBASE_HI << 16 | (uint32_t)VBASE_MID << 8 | VBASE_LO;
    __asm__ __volatile__("move.l #0x3919,%%d0\n\tmovec %%d0,%%cacr" ::: "d0");
    for (i = 0; i < 256; i++)
        TT_PALETTE[i] = 0;
    set_base(tt_front);
    tt_wait_vbl();
    TT_SHIFT = TT_MODE_MED;
}

static void tt_medium_stop(void) {
    int i;
    for (i = 0; i < 256; i++)
        TT_PALETTE[i] = 0;
    set_base((void*)old_base);
    TT_SHIFT = old_shift;
    tt_wait_vbl();
    tt_wait_vbl();
    for (i = 0; i < 256; i++)
        TT_PALETTE[i] = old_palette[i];
}

/* ------------------------------------------------------------ the Falcon, an experiment */

/* What a Falcon had before the game: its mode, screen, colours, and the
 * sound matrix's connections, clock dividers and mixer inputs */
static struct {
    int16_t mode;
    void *log, *phys;
    uint32_t palette[256];
    uint16_t src, dst;
    uint8_t extpre, intpre;
    long adder;
} falcon_had;

/* A 0x0RGB colour as a Falcon register's: 6 bits a gun, 0xRRGG00BB */
static uint32_t falcon_colour(uint16_t c) {
    uint32_t r = (c >> 8 & 15) * 17 & 0xFC, g = (c >> 4 & 15) * 17 & 0xFC, b = (c & 15) * 17 & 0xFC;
    return r << 24 | g << 16 | b;
}

/* The same picture as a TT's: what the Falcon had saved, then 640x480 in 16
 * colours set through the XBIOS and DMA sound connected to the DAC at the
 * STE's rates; the caches on without burst fill, which a Falcon's memory
 * does not do */
static void falcon_start(void) {
    int i;
    falcon_had.mode = VsetMode(-1);
    falcon_had.log = Logbase();
    falcon_had.phys = Physbase();
    for (i = 0; i < 256; i++)
        falcon_had.palette[i] = FALCON_PALETTE[i];
    falcon_had.src = XB_SRC;
    falcon_had.dst = XB_DST;
    falcon_had.extpre = XB_EXTPRE;
    falcon_had.intpre = XB_INTPRE;
    falcon_had.adder = Soundcmd(ADDERIN, SND_INQUIRE);
    (void)Devconnect(DMAPLAY, DAC, CLK25M, CLKOLD, NO_SHAKE);
    (void)Soundcmd(ADDERIN, (short)((falcon_had.adder & 3) | MATIN));
    __asm__ __volatile__("move.l #0x3111,%%d0\n\tmovec %%d0,%%cacr" ::: "d0");
    (void)VsetScreen(tt_front, tt_front, 3, FALCON_MODE);
    tt_wait_vbl();
    for (i = 0; i < 16; i++)
        FALCON_PALETTE[i] = 0;
}

/* The mode and the screen back through the XBIOS, as they were set; the
 * colours after a blank, so none the XBIOS still holds lands on them */
static void falcon_stop(void) {
    int i;
    (void)VsetScreen(falcon_had.log, falcon_had.phys, 3, falcon_had.mode);
    tt_wait_vbl();
    for (i = 0; i < 256; i++)
        FALCON_PALETTE[i] = falcon_had.palette[i];
    XB_SRC = falcon_had.src;
    XB_DST = falcon_had.dst;
    XB_EXTPRE = falcon_had.extpre;
    XB_INTPRE = falcon_had.intpre;
    (void)Soundcmd(ADDERIN, (short)falcon_had.adder);
}

/* ------------------------------------------------------------ either machine */

void tt_set_palette(const uint16_t* pal) {
    int i;
    memcpy(shown_palette, pal, sizeof shown_palette);
    if (falcon) {
        for (i = 0; i < 16; i++)
            FALCON_PALETTE[i] = falcon_colour(pal[i]);
        return;
    }
    for (i = 0; i < 16; i++)
        TT_PALETTE[i] = pal[i];
}

void tt_get_palette(uint16_t* pal) {
    memcpy(pal, shown_palette, sizeof shown_palette);
}

void tt_flip(void) {
    int i, b = 0;
    for (i = 0; i < NSCREENS; i++)
        if (tt_screens[i] == tt_back)
            b = i;
    /* a blank since the last base was set means that one is on show now */
    if (VBLCLOCK > set_at[latest])
        shown = latest;
    set_base(tt_back);
    set_at[b] = VBLCLOCK;
    latest = b;
    tt_front = tt_back;
    for (i = 0; i < NSCREENS; i++)
        if (i != latest && i != shown)
            break;
    tt_back = tt_screens[i];
}

static long cookie(long id) {
    long v = 0;
    if (Getcookie(id, &v) != C_FOUND)
        return 0;
    return v;
}

/* mintlib's start-up calls _checkcpu() before main(). Its 68020-60 library's
 * own also stops the program where there is no FPU, as on a stock Falcon;
 * the game uses no floating point, so this one, which the linker takes in
 * its place, asks only for the 68030 the game is made for. */
void _checkcpu(void);

void _checkcpu(void) {
    if (cookie(C__CPU) < 30) {
        tt_sorry("needs a 68030");
        Pterm(1);
    }
}

/* What the machine lacks for the game, as words for tt_sorry(), or null */
static const char* missing(long video) {
    if (video == 2)
        return 0;
    if (video != 3)
        return "needs a TT or a Falcon";
    if (VgetMonitor() != MON_VGA)
        return "needs a VGA monitor on a Falcon";
    if ((long)Mxalloc(-1, 0) < (long)FALCON_MEGABYTES << 20)
        return "needs " STR(FALCON_MEGABYTES) "MB free RAM on a Falcon";
    return 0;
}

int tt_init(void) {
    int i;
    long video = cookie(C__VDO) >> 16;
    const char* why = missing(video);
    if (why) {
        tt_sorry(why);
        return 0;
    }
    falcon = video == 3;
    tt_plays_6258 = !falcon;
    for (i = 0; i < NSCREENS; i++) {
        long b = (long)Mxalloc(SCREEN_BYTES + 256, 0);
        if (b <= 0) {
            tt_sorry("needs more free ST-RAM");
            return 0;
        }
        tt_screens[i] = (uint8_t*)((b + 255) & ~255L);
        memset(tt_screens[i], 0, SCREEN_BYTES);
    }
    tt_front = tt_screens[0];
    tt_back = tt_screens[1];
    latest = shown = 0;
    old_ssp = Super(0L);
    __asm__ __volatile__("movec %%cacr,%0" : "=d"(old_cacr));
    if (falcon)
        falcon_start();
    else
        tt_medium_start();
    kbdv = Kbdvbase();
    old_mousevec = kbdv->mousevec;
    kbdv->mousevec = (void (*)(void*))mouse_handler;
    return 1;
}

void tt_exit(void) {
    tt_sound_stop();
    if (kbdv)
        kbdv->mousevec = old_mousevec;
    if (falcon)
        falcon_stop();
    else
        tt_medium_stop();
    __asm__ __volatile__("movec %0,%%cacr" ::"d"(old_cacr | 0x0808UL));
    (void)Super((void*)old_ssp);
    while (Cconis())
        (void)Cnecin();
}

uint16_t tt_key(void) {
    long k;
    if (!Bconstat(2))
        return 0;
    k = Bconin(2);
    return (uint16_t)(((k >> 8) & 0xFF00) | (k & 0xFF));
}

int tt_shift(void) {
    long s = Kbshift(-1);
    return ((s & 3) ? 1 : 0) | ((s & 4) ? 2 : 0) | ((s & 8) ? 4 : 0);
}

void tt_sound_play(const int8_t* start, int32_t len, int rate) {
    uint32_t a = (uint32_t)start, e = a + (uint32_t)len;
    SND_CTRL = 0;
    SND_START_HI = (uint8_t)(a >> 16);
    SND_START_MID = (uint8_t)(a >> 8);
    SND_START_LO = (uint8_t)a;
    SND_END_HI = (uint8_t)(e >> 16);
    SND_END_MID = (uint8_t)(e >> 8);
    SND_END_LO = (uint8_t)e;
    SND_MODE = (uint8_t)(0x80 | (rate & 3));
    SND_CTRL = 1;
}

void tt_sound_stop(void) {
    SND_CTRL = 0;
}

int tt_sound_busy(void) {
    return SND_CTRL & 1;
}
