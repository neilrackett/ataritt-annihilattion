# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""The 16-colour palette and the ordered dither that maps art onto it.

The TT's palette has 4 bits per gun, so every entry is a multiple of 17.
"""
import numpy as np

BAYER4 = (np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.float32) + 0.5) / 16 - 0.5

# Entries the engine names: everything else is chosen from the art.
BLACK, WHITE, ARMBLUE, CORERED, YELLOW, NANOGREEN = 0, 15, 1, 2, 3, 4
FIXED = {
    BLACK: (0, 0, 0),
    WHITE: (255, 255, 255),
    ARMBLUE: (34, 85, 221),
    CORERED: (204, 34, 17),
    YELLOW: (255, 221, 68),
    NANOGREEN: (102, 238, 85),
    13: (119, 119, 119),
    14: (187, 187, 187),
}
WEIGHT = np.array([0.30, 0.59, 0.11], np.float32) ** 0.5 * 1.7


def to_tt(c):
    """Rounds 8-bit guns to the TT's 16 levels (multiples of 17)."""
    return tuple(int(round(v / 17.0)) * 17 for v in c)


FREE = [i for i in range(16) if i not in FIXED]


def _kmeans(samples, iters, seed):
    """Centres for the free entries, beside the fixed ones: k-means over the
    sample colours (N x 3), weighted, seeded from samples chosen at random."""
    rng = np.random.default_rng(seed)
    fixed = np.array([FIXED[i] for i in sorted(FIXED)], np.float32) * WEIGHT
    x = samples.astype(np.float32) * WEIGHT
    cent = x[rng.choice(len(x), len(FREE), replace=False)]
    for _ in range(iters):
        allc = np.concatenate([fixed, cent])
        lab = ((x[:, None, :] - allc[None, :, :]) ** 2).sum(-1).argmin(1)
        for k in range(len(FREE)):
            m = lab == len(fixed) + k
            if m.any():
                cent[k] = x[m].mean(0)
    return cent


def build(samples, iters=25, seed=1):
    """The battle palette: the fixed entries, and the rest from the sample
    colours (N x 3), darkest first."""
    cent = _kmeans(samples, iters, seed)
    pal = [None] * 16
    for i, c in FIXED.items():
        pal[i] = to_tt(c)
    order = np.argsort((cent / WEIGHT) @ np.array([0.3, 0.59, 0.11]))
    for slot, k in zip(FREE, order):
        c = to_tt(cent[k] / WEIGHT)
        step = 0
        while c in pal:          # two clusters rounded to one TT colour
            step += 1
            c = to_tt((cent[k] / WEIGHT) + (17 * step, -17 * step if step % 2 else 17 * step, 0))
            c = tuple(min(255, max(0, v)) for v in c)
        pal[slot] = c
    return pal


class Mapper:
    def __init__(self, pal, spread=34.0):
        self.pal = np.array(pal, np.float32)
        self.pw = self.pal * WEIGHT
        self.spread = spread

    def nearest(self, rgb):
        """rgb: (..., 3) float -> palette index array."""
        flat = rgb.reshape(-1, 3) * WEIGHT
        out = np.empty(len(flat), np.uint8)
        for s in range(0, len(flat), 65536):
            d = ((flat[s:s + 65536, None, :] - self.pw[None]) ** 2).sum(-1)
            out[s:s + 65536] = d.argmin(1)
        return out.reshape(rgb.shape[:-1])

    def dither(self, rgb, x0=0, y0=0):
        """Ordered dither of an (h, w, 3) image whose top left sits at (x0, y0)."""
        h, w = rgb.shape[:2]
        ys = (np.arange(h) + y0) & 3
        xs = (np.arange(w) + x0) & 3
        t = BAYER4[ys][:, xs][..., None] * self.spread
        return self.nearest(np.clip(rgb.astype(np.float32) + t, 0, 255))


def build_picture(samples, iters=20, seed=2):
    """A picture's palette: the battle palette's fixed entries, so text colours
    mean the same everywhere, and the rest from the picture."""
    cent = _kmeans(samples, iters, seed)
    pal = [None] * 16
    for i, c in FIXED.items():
        pal[i] = to_tt(c)
    for slot, c in zip(FREE, cent / WEIGHT):
        c = to_tt(c)
        # a colour taken already: a step brighter, or darker once at white
        step = 17
        while c in pal:
            n = tuple(min(255, max(0, v + step)) for v in c)
            if n == c:
                step = -17
                continue
            c = n
        pal[slot] = c
    return pal


def diffuse(rgb, pal):
    """Floyd-Steinberg error diffusion onto pal; returns indices."""
    h, w = rgb.shape[:2]
    p = np.array(pal, np.float32)
    pw = p * WEIGHT
    img = rgb.astype(np.float32).copy()
    out = np.zeros((h, w), np.uint8)
    for y in range(h):
        row = img[y]
        for x in range(w):
            c = np.clip(row[x], 0, 255)
            k = int((((c * WEIGHT) - pw) ** 2).sum(1).argmin())
            out[y, x] = k
            e = c - p[k]
            if x + 1 < w:
                row[x + 1] += e * 7 / 16
            if y + 1 < h:
                if x > 0:
                    img[y + 1, x - 1] += e * 3 / 16
                img[y + 1, x] += e * 5 / 16
                if x + 1 < w:
                    img[y + 1, x + 1] += e * 1 / 16
    return out
