/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The mixer (see snd.h).
 *
 * DMA sound plays RING samples in ST-RAM over and over. snd_update() finds
 * where it plays and mixes from where the last mixing stopped up to a lead
 * ahead of it: enough to cover the frames to come, and no more, as an
 * effect started now is heard only after what is mixed already. The lead
 * grows when frames take long. The stream is read from its file into a
 * ring of its own in any RAM, STREAM_READ bytes at a time, well ahead.
 */
#include "snd.h"
#include <string.h>
#include <stdio.h>
#include <mint/osbind.h>
#include "data.h"
#include "tt.h"

#define RING 8192L        /* the DMA ring: 0.65 seconds */
#define LEAD_MIN 1600     /* samples mixed ahead of the DMA at least: 0.13 seconds */
#define CHUNK 512         /* samples mixed at a time */
#define NCHAN 4
#define SBUF 65536L       /* the stream's ring */
#define STREAM_READ 8192L /* bytes read from its file at a time */

int snd_enabled = 1;
int snd_music_on = 1;

typedef struct {
    const int8_t* p;
    int32_t left;   /* samples still to play, at its own rate */
    uint8_t half;   /* a 6258 Hz sound: each sample twice */
    uint8_t odd;    /* which of the two */
    int16_t prio, id;
    uint32_t start; /* HZ200 when it started */
} Chan;

static int8_t* ring;
static int32_t fill;       /* the ring's next sample to mix */
static int32_t last_play;  /* where the DMA was at the last update */
static int32_t last_ahead; /* how far mixing was ahead of it then */
static uint32_t last_hz;
static Chan ch[NCHAN];
static int8_t clamp_tab[2048]; /* a sum -1024..1023 as a sample */

static int8_t* sbuf;
static long sfh = -1;
static int32_t s_read, s_written; /* the stream's samples taken and read in, from its start */
static int s_eof, s_half_volume;

/* the music: which track plays, whether the loud ones are wanted, and
 * whether its files are missing */
static int music_track = -1, music_battle, music_missing;
static uint32_t calm_since;

int snd_init(void) {
    int i;
    for (i = 0; i < 2048; i++) {
        int v = i - 1024;
        clamp_tab[i] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
    }
    ring = tt_alloc_st(RING);
    sbuf = tt_alloc_any(SBUF);
    if (!ring || !sbuf)
        return 0;
    memset(ring, 0, RING);
    fill = 0;
    last_play = 0;
    last_ahead = 0;
    last_hz = HZ200;
    tt_sound_loop(ring, RING, 1);
    return 1;
}

static void stream_close(void) {
    if (sfh >= 0)
        (void)Fclose((short)sfh);
    sfh = -1;
    s_read = s_written = 0;
    s_eof = 1;
}

void snd_exit(void) {
    tt_sound_stop();
    stream_close();
}

void snd_play(int id, int prio) {
    int i, pick = -1;
    Chan* c;
    if (!snd_enabled || !ring || id < 0 || id >= sounds.n || !sounds.data)
        return;
    for (i = 0; i < NCHAN; i++) {
        c = &ch[i];
        if (c->left > 0 && c->id == id && HZ200 - c->start < 12)
            return; /* a volley sounds once */
    }
    for (i = 0; i < NCHAN; i++)
        if (ch[i].left <= 0) {
            pick = i;
            break;
        }
    if (pick < 0) {
        /* the one that matters least, the oldest of those */
        for (i = 0; i < NCHAN; i++)
            if (ch[i].prio <= prio &&
                (pick < 0 || ch[i].prio < ch[pick].prio ||
                 (ch[i].prio == ch[pick].prio && ch[i].start < ch[pick].start)))
                pick = i;
        if (pick < 0)
            return;
    }
    c = &ch[pick];
    c->p = sounds.data + sounds.ofs[id];
    c->left = (int32_t)sounds.len[id];
    c->half = !sounds.rate[id];
    c->odd = 0;
    c->prio = (int16_t)prio;
    c->id = (int16_t)id;
    c->start = HZ200;
}

/* Reads more of the stream's file when its ring has room */
static void stream_read(void) {
    while (sfh >= 0 && !s_eof && s_written - s_read <= SBUF - STREAM_READ) {
        long got = data_read(sfh, STREAM_READ, sbuf + (s_written & (SBUF - 1)));
        if (got > 0)
            s_written += got;
        if (got < STREAM_READ)
            s_eof = 1;
    }
}

int snd_stream_file(const char* name) {
    stream_close();
    if (!snd_enabled || !ring)
        return 0;
    sfh = data_open(name);
    if (sfh < 0)
        return 0;
    s_eof = 0;
    s_half_volume = 0;
    stream_read();
    return 1;
}

int snd_stream_busy(void) {
    return sfh >= 0 && !(s_eof && s_read >= s_written);
}

/* The mixer's inner loops, each for n > 0 samples: GCC's take half as many
 * instructions again, and three times as many for a 6258 Hz sound */

/* a[i] = s[i], or s[i] >> 1 when halved */
static inline void put_samples(int16_t* a, const int8_t* s, int n, int halved) {
    if (halved)
        __asm__ volatile("subq.w #1,%2\n"
                         "1:\tmove.b (%1)+,%%d0\n\t"
                         "asr.b #1,%%d0\n\t"
                         "ext.w %%d0\n\t"
                         "move.w %%d0,(%0)+\n\t"
                         "dbra %2,1b"
                         : "+a"(a), "+a"(s), "+d"(n)
                         :
                         : "d0", "memory");
    else
        __asm__ volatile("subq.w #1,%2\n"
                         "1:\tmove.b (%1)+,%%d0\n\t"
                         "ext.w %%d0\n\t"
                         "move.w %%d0,(%0)+\n\t"
                         "dbra %2,1b"
                         : "+a"(a), "+a"(s), "+d"(n)
                         :
                         : "d0", "memory");
}

/* a[i] += s[i] */
static inline void add_samples(int16_t* a, const int8_t* s, int n) {
    __asm__ volatile("subq.w #1,%2\n"
                     "1:\tmove.b (%1)+,%%d0\n\t"
                     "ext.w %%d0\n\t"
                     "add.w %%d0,(%0)+\n\t"
                     "dbra %2,1b"
                     : "+a"(a), "+a"(s), "+d"(n)
                     :
                     : "d0", "memory");
}

/* a[2i] += s[i] and a[2i + 1] += s[i], for n pairs */
static inline void add_doubled(int16_t* a, const int8_t* s, int n) {
    __asm__ volatile("subq.w #1,%2\n"
                     "1:\tmove.b (%1)+,%%d0\n\t"
                     "ext.w %%d0\n\t"
                     "add.w %%d0,(%0)+\n\t"
                     "add.w %%d0,(%0)+\n\t"
                     "dbra %2,1b"
                     : "+a"(a), "+a"(s), "+d"(n)
                     :
                     : "d0", "memory");
}

/* d[i] = t[a[i]]: t is clamp_tab's middle */
static inline void clamp_samples(int8_t* d, const int16_t* a, const int8_t* t, int n) {
    __asm__ volatile("subq.w #1,%2\n"
                     "1:\tmove.w (%1)+,%%d0\n\t"
                     "move.b (%3,%%d0.w),(%0)+\n\t"
                     "dbra %2,1b"
                     : "+a"(d), "+a"(a), "+d"(n)
                     : "a"(t)
                     : "d0", "memory");
}

/* Mixes n samples (n <= CHUNK) into the ring from fill on, wrapping. The
 * stream and four effects sum to within -640..639, inside clamp_tab. */
static void mix(int n) {
    int16_t acc[CHUNK];
    int i, k, any = 0;
    int8_t* d;
    int first;
    /* the stream, in runs up to its ring's end */
    if (snd_stream_busy()) {
        int32_t have = s_written - s_read;
        int m = have < n ? (int)have : n, done = 0;
        while (done < m) {
            int32_t r = s_read & (SBUF - 1);
            int run = m - done < SBUF - r ? m - done : (int)(SBUF - r);
            put_samples(acc + done, sbuf + r, run, s_half_volume);
            s_read += run;
            done += run;
        }
        for (i = m; i < n; i++)
            acc[i] = 0;
        if (s_eof && s_read >= s_written)
            stream_close();
        any = 1;
    } else {
        memset(acc, 0, n * sizeof acc[0]);
    }
    /* the effects */
    for (k = 0; k < NCHAN; k++) {
        Chan* c = &ch[k];
        const int8_t* p;
        if (c->left <= 0)
            continue;
        any = 1;
        p = c->p;
        if (!c->half) {
            int m = c->left < n ? (int)c->left : n;
            add_samples(acc, p, m);
            c->p += m;
            c->left -= m;
        } else {
            /* each sample twice: once more if it has been once already,
             * then in pairs, then the first of the next pair */
            int pairs;
            i = 0;
            if (c->odd) {
                acc[i++] += *p++;
                c->left--;
                c->odd = 0;
            }
            pairs = (n - i) >> 1;
            if (pairs > c->left)
                pairs = (int)c->left;
            if (pairs > 0) {
                add_doubled(acc + i, p, pairs);
                i += pairs * 2;
                p += pairs;
                c->left -= pairs;
            }
            if (i < n && c->left > 0) {
                acc[i] += *p;
                c->odd = 1;
            }
            c->p = p;
        }
    }
    /* into the ring, in two runs at most */
    d = ring + fill;
    first = RING - fill < n ? (int)(RING - fill) : n;
    if (!any) {
        memset(d, 0, (size_t)first);
        memset(ring, 0, (size_t)(n - first));
    } else {
        const int8_t* ct = clamp_tab + 1024;
        if (first > 0)
            clamp_samples(d, acc, ct, first);
        if (n > first)
            clamp_samples(ring, acc + first, ct, n - first);
    }
    fill = (fill + n) & (RING - 1);
}

void snd_update(void) {
    int32_t play, ahead, elapsed, lead;
    uint32_t now = HZ200, dt = now - last_hz;
    if (!ring)
        return;
    last_hz = now;
    if (!snd_enabled) {
        snd_stop();
    }
    stream_read();
    play = (int32_t)(tt_sound_pos() - ring);
    if (play < 0 || play >= RING)
        play = 0;
    elapsed = (play - last_play) & (RING - 1);
    ahead = (fill - play) & (RING - 1);
    /* what the DMA played since is silenced, so that a wait without an
     * update (a disk read) plays silence, not the ring over again */
    if (elapsed) {
        int32_t first = RING - last_play < elapsed ? RING - last_play : elapsed;
        memset(ring + last_play, 0, (size_t)first);
        memset(ring, 0, (size_t)(elapsed - first));
    }
    if (elapsed > last_ahead) {
        /* the DMA passed what was mixed: start again just ahead of it */
        fill = (play + 64) & (RING - 1);
        ahead = 64;
    }
    /* enough for the next frames: twice the last one's time, at least LEAD_MIN */
    lead = (int32_t)(dt > 40 ? 40 : dt) * 12517 / 100;
    if (lead < LEAD_MIN)
        lead = LEAD_MIN;
    if (lead > RING - 1024)
        lead = RING - 1024;
    while (ahead < lead) {
        int n = lead - ahead > CHUNK ? CHUNK : (int)(lead - ahead);
        mix(n);
        ahead += n;
    }
    last_play = play;
    last_ahead = ahead;
}

void snd_silence(void) {
    snd_stop();
    if (ring)
        memset(ring, 0, RING);
}

void snd_stop(void) {
    int i;
    for (i = 0; i < NCHAN; i++)
        ch[i].left = 0;
    stream_close();
    music_track = -1;
}

/* A track of a kind, other than the one that played, chosen at random */
static int pick_track(int kind) {
    static uint32_t seed = 7;
    int i, n = 0, pick;
    for (i = 0; i < ntracks; i++)
        if (tracks[i].kind == kind && i != music_track)
            n++;
    if (!n)
        return music_track;
    seed = seed * 1103515245UL + 12345UL + HZ200;
    pick = (int)((seed >> 16) % (uint32_t)n);
    for (i = 0; i < ntracks; i++)
        if (tracks[i].kind == kind && i != music_track && !pick--)
            return i;
    return -1;
}

void snd_music(int battle) {
    char name[16];
    int want, t;
    if (!ntracks || !snd_enabled || !snd_music_on || !ring || music_missing)
        return;
    if (battle) {
        music_battle = 1;
        calm_since = HZ200;
    } else if (music_battle && HZ200 - calm_since > 200 * 30) {
        music_battle = 0; /* half a minute without fighting */
    }
    want = music_battle;
    /* a battle cuts a calm track short; calm waits for a loud one to end */
    if (snd_stream_busy() && music_track >= 0 && !(want && !tracks[music_track].kind))
        return;
    t = pick_track(want);
    if (t < 0)
        return;
    sprintf(name, "MUSIC%02d.SND", tracks[t].num);
    if (snd_stream_file(name)) {
        s_half_volume = 1;
        music_track = t;
    } else {
        music_missing = 1;
    }
}
