var e=`# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""Converts the Total Annihilation demo's game data for the Atari TT demake.

usage: convert.py DEMO_DATA_DIR OUT_DIR [PREVIEW_DIR]

DEMO_DATA_DIR is the demo's TADemo.hpi unpacked by hpi_extract.py. OUT_DIR
receives the files the TT program reads, all big-endian:

  UNITS.DAT   weapon and unit tables, and every unit's sprites
  COMMON.DAT  the battle palette, the panel picture, features, effects,
              cursors, unit pictures, buttons, the font and the fog's edges
  MISS1.DAT.. each mission's map, metal, objectives and placed units
  SOUNDS.DAT  sound effects, signed 8-bit at 12517 Hz
  BRIEF1.SND  each mission's spoken briefing, signed 8-bit at 12517 Hz
  BRIEF1.TXT..  each mission's briefing text
  TITLE.IMG, SINGLE.IMG, BRIEF.IMG, GLAM1.IMG..  full-screen pictures with
              their own palettes; GLAMn.IMG shows while mission n loads

Pictures of the converted art for checking go to PREVIEW_DIR, or previews/
beside OUT_DIR.

The layouts are mirrored in ../src/data.h; change both together.
"""
import math, os, shutil, struct, sys, wave
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ta, quant, render3do

TICKS = 15                  # simulation ticks a second on the TT
TA_FRAMES = 30              # the original's ticks a second
DIRS = 16                   # directions a mobile unit is drawn in
MISSIONS = ['ac01', 'ac02', 'ac03']
GLAMOUR = ['arm01', 'arm02', 'arm02']   # each mission's loading picture (the demo has two)
VIEW_W, VIEW_H = 512, 416
MAX_UNITTYPES = 64          # data.h's limit
ENGINE_ANIMS = ['explode5']  # effects the engine shows by number (data.h, ANIM_*)

# ------------------------------------------------------------------ helpers


def be(fmt, *v):
    return struct.pack('>' + fmt, *v)


def cstr(s, n):
    b = s.encode('latin-1', 'replace')[:n - 1]
    return b + b'\\0' * (n - len(b))


class Bank:
    """A blob that sprites and other records are appended to; offsets are 4-aligned."""

    def __init__(self):
        self.data = bytearray()

    def add(self, b):
        while len(self.data) & 3:
            self.data.append(0)
        o = len(self.data)
        self.data += b
        return o


BITS = 1 << (15 - np.arange(16))     # a 16-pixel group's pixels as a word's bits


def plane_words(idx, opaque=None):
    """Palette indices (h, w; w a multiple of 16) as words (h, w / 16, n): for
    each 16-pixel group, planes 0-3, after its mask when opaque is given."""
    h, w = idx.shape
    g = w // 16
    ix = idx.reshape(h, g, 16).astype(np.int32)
    words = [((ix >> p) & 1) @ BITS for p in range(4)]   # (h, g) each
    if opaque is not None:
        words.insert(0, opaque.reshape(h, g, 16).astype(np.int32) @ BITS)
    return np.stack(words, axis=-1)


def planar_rows(idx, opaque=None):
    """Palette indices as interleaved planes: every 16-pixel group is mask,
    p0, p1, p2, p3 with opaque, p0..p3 without."""
    return plane_words(idx, opaque).astype('>u2').tobytes()


def sprite(idx, opaque, ox, oy):
    """A masked sprite: i16 groups G, height, origin x, origin y, then for each
    row five runs (mask, planes 0-3) of G + 2 words: a zero, the row's G
    words, a zero. A long read at word g then holds groups g-1 and g, so a
    shift right by x & 15 gives screen group g's word with no carry.

    The image is trimmed to its opaque pixels and padded to whole groups;
    (ox, oy) is where the anchor point falls in the untrimmed image.
    """
    ys, xs = np.nonzero(opaque)
    if len(xs) == 0:
        return be('hhhh', 0, 0, 0, 0)
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    idx = idx[y0:y1, x0:x1]
    opaque = opaque[y0:y1, x0:x1]
    h, w = idx.shape
    gw = (w + 15) // 16 * 16
    G = gw // 16
    pi = np.zeros((h, gw), np.uint8); pi[:, :w] = idx
    po = np.zeros((h, gw), bool); po[:, :w] = opaque
    pi[~po] = 0
    arr = np.zeros((h, 5, G + 2), np.int32)
    arr[:, :, 1:G + 1] = plane_words(pi, po).transpose(0, 2, 1)
    return be('hhhh', G, h, ox - x0, oy - y0) + arr.astype('>u2').tobytes()


def image_planar(idx):
    """A full-width picture with no mask (width a multiple of 16)."""
    return planar_rows(idx)


def tt_word(c):
    return ((c[0] // 17) << 8) | ((c[1] // 17) << 4) | (c[2] // 17)


def blit_rgba(dst, rgba, x, y, shade=None):
    """Puts rgba's opaque pixels into dst (h, w, 3) with its top left at x, y,
    clipped; with shade, darkens dst under them by that factor instead."""
    h, w = rgba.shape[:2]
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(dst.shape[1], x + w), min(dst.shape[0], y + h)
    if x0 >= x1 or y0 >= y1:
        return
    sub = rgba[y0 - y:y1 - y, x0 - x:x1 - x]
    m = sub[..., 3] > 0
    if shade is None:
        dst[y0:y1, x0:x1][m] = sub[..., :3][m]
    else:
        dst[y0:y1, x0:x1][m] *= shade


_gafs = {}


def gaf(name):
    """anims/NAME.gaf's sequences ({} when there is none), read once."""
    if name not in _gafs:
        p = ta.path('anims/%s.gaf' % name)
        _gafs[name] = ta.read_gaf(p) if p else {}
    return _gafs[name]


# ------------------------------------------------------------------ palette

def gather_palette(maps):
    pal = np.array(ta.palette(), np.uint8)
    samples = []
    for t in maps.values():
        used = sorted(set(t.tilemap))
        px = np.frombuffer(b''.join(t.tiles[i] for i in used), np.uint8)
        samples.append(pal[px[::5]])
    # units and the interface weigh in too
    for n in ['armpw', 'armcom', 'armstump', 'armlab', 'corak', 'corthud', 'corlab', 'armsolar']:
        polys = render3do.collect(ta.read_3do(ta.path('objects3d/%s.3do' % n)))
        img, _, _ = render3do.render(polys, 0.5, team=0 if n.startswith('arm') else 1)
        rgb = img[..., :3][img[..., 3] > 128]
        samples += [rgb] * 6
    side = gaf('armint')['panelside'][0]
    samples.append(pal[np.array(side[4], np.uint8)][::3])
    return quant.build(np.concatenate(samples))


# ------------------------------------------------------------------ units

def load_defs():
    units = {}
    for f in sorted(os.listdir(ta.path('units'))):
        if f.endswith('.fbi'):
            d = ta.read_tdf(os.path.join(ta.path('units'), f))['unitinfo']
            units[d['unitname'].lower()] = d
    weapons = {}
    for f in sorted(os.listdir(ta.path('weapons'))):
        for k, v in ta.read_tdf(os.path.join(ta.path('weapons'), f)).items():
            weapons[k] = v
    return units, weapons


def build_options(name, units):
    """The units on a builder's menu pages (guis/<name>1.gui, <name>2.gui...)."""
    out = []
    for page in range(1, 6):
        p = ta.path('guis/%s%d.gui' % (name, page))
        if not p:
            break
        for k, v in ta.read_tdf(p).items():
            for kk, vv in v.items():
                if isinstance(vv, dict):
                    nm = vv.get('name', '').lower()
                    if nm in units and nm not in out:
                        out.append(nm)
    return out


class Sounds:
    """Sound effects, gathered by name as the tables refer to them."""

    def __init__(self):
        self.names = []

    def ref(self, name):
        if not name:
            return -1
        name = name.lower().replace('.wav', '')
        if not ta.path('sounds/%s.wav' % name):
            return -1
        if name not in self.names:
            self.names.append(name)
        return self.names.index(name)


def low_pass_taps(cut, n=127):
    """A windowed-sinc low-pass filter passing below cut (a fraction of the
    sample rate), as integers summing to about 65536."""
    k = np.arange(n, dtype=np.float64) - (n - 1) / 2
    h = np.sinc(2 * cut * k) * np.kaiser(n, 8.0)
    return np.round(h / h.sum() * 65536).astype(np.int64)


def read_wav(p, rate):
    """Signed 8-bit mono at rate: filtered first when rate is lower than the
    file's, so that what rate can't hold doesn't fold back as hiss, then
    resampled linearly."""
    w = wave.open(p)
    n, sw, ch, fr = w.getnframes(), w.getsampwidth(), w.getnchannels(), w.getframerate()
    raw = w.readframes(n)
    # integers until after the filter, so that every platform sums alike
    if sw == 1:
        a = (np.frombuffer(raw, np.uint8).astype(np.int64) - 128) * 256
    else:
        a = np.frombuffer(raw, '<i2').astype(np.int64)
    a = a.reshape(-1, ch).sum(1)
    scale = 256 * ch
    if rate < fr:
        h = low_pass_taps(0.45 * rate / fr)
        a = np.convolve(a, h)[len(h) // 2:len(h) // 2 + len(a)]
        scale *= int(h.sum())
    a = a / float(scale)
    m = int(len(a) * rate / fr)
    # positions as floats: integers would be 32-bit where numpy's are (the
    # web converter's), and a long briefing times 22050 overflows them
    a = np.interp(np.arange(m, dtype=np.float64) * fr / rate, np.arange(len(a)), a)
    return np.clip(np.round(a), -128, 127).astype(np.int8).tobytes()


WEAPON_KIND = {'beam': 0, 'cannon': 1, 'rocket': 2, 'shot': 3, 'dgun': 4}
ANIMS = []          # explosion animation names, in COMMON.DAT order


def anim_ref(name):
    name = (name or '').lower()
    if not name:
        return -1
    if name not in ANIMS:
        ANIMS.append(name)
    return ANIMS.index(name)


def weapon_record(w, mapper_pal, sounds):
    """A weapon: kind is how it reaches its target, the flags what else it does."""
    rt = int(ta.num(w, 'rendertype'))
    paralyzer = int(ta.num(w, 'paralyzer'))
    if w.get('name', '').lower().startswith('disintegrator') or 'disintegrator' in w.get('_key', ''):
        kind = 'dgun'
    elif paralyzer or int(ta.num(w, 'beamweapon')) or (rt == 0 and int(ta.num(w, 'lineofsight'))):
        kind = 'beam'
    elif int(ta.num(w, 'ballistic')):
        kind = 'cannon'
    elif rt == 1 or w.get('model'):
        kind = 'rocket'
    else:
        kind = 'shot'
    col = int(ta.num(w, 'color', 208))
    rgb = ta.palette()[col & 255] if kind == 'beam' else (255, 230, 120)
    colour = int(mapper_pal.nearest(np.array([[rgb]], np.float32))[0, 0])
    if paralyzer:
        colour = quant.WHITE        # the paralyzer's beam is white, whatever its colour
    dmg = w.get('damage', {})
    damage = int(ta.num(dmg, 'default', 10)) if isinstance(dmg, dict) else 10
    flags = 0
    if paralyzer:
        flags |= 1
    if int(ta.num(w, 'commandfire')):
        flags |= 2
    if int(ta.num(w, 'ballistic')):
        flags |= 4
    if int(ta.num(w, 'tracks')):
        flags |= 8
    reload = max(1, int(round(ta.num(w, 'reloadtime', 1) * TICKS)))
    speed = int(round(ta.num(w, 'weaponvelocity', 300) / TICKS * 256))
    burst = max(1, int(ta.num(w, 'burst', 1)))
    gap = max(1, int(round(ta.num(w, 'burstrate', 0.1) * TICKS)))
    return be('HHHHHBBBBhhhBBH',
              int(ta.num(w, 'range', 200)), reload, damage, min(speed, 65535),
              int(ta.num(w, 'areaofeffect', 16)), WEAPON_KIND[kind], colour, burst, gap,
              sounds.ref(w.get('soundstart')), sounds.ref(w.get('soundhit')),
              anim_ref(w.get('explosionart')), flags, 0,
              int(ta.num(w, 'energypershot'))) + cstr(w.get('_key', ''), 12)


MOVEINFO = None


def move_class(u):
    """(max slope, max water depth, min water depth) of a unit's movement class."""
    global MOVEINFO
    if MOVEINFO is None:
        MOVEINFO = {v.get('name', '').lower(): v for v in ta.read_tdf(ta.path('gamedata/moveinfo.tdf')).values()}
    c = MOVEINFO.get(u.get('movementclass', '').lower(), u)
    return (int(ta.num(c, 'maxslope', 255)), int(ta.num(c, 'maxwaterdepth', 255)),
            int(ta.num(c, 'minwaterdepth', 0)))


def yard_bits(u):
    """Eight bytes, a row each: bit 7 is the left cell, set where units may pass."""
    fx, fz = int(ta.num(u, 'footprintx', 1)), int(ta.num(u, 'footprintz', 1))
    ym = u.get('yardmap', '').replace(' ', '').lower()
    rows = []
    for z in range(8):
        b = 0
        for x in range(8):
            i = z * fx + x
            if x < fx and z < fz and i < len(ym) and ym[i] in 'cy':
                b |= 0x80 >> x
        rows.append(b)
    return bytes(rows)


def per_tick(v):
    """A per-second amount as 1/256ths a tick."""
    return int(round(v * 256 / TICKS))


def render_unit(name, team, dirs, mapper):
    ob = ta.read_3do(ta.path('objects3d/%s.3do' % name))
    polys = render3do.collect(ob)
    frames = []
    for d in range(dirs):
        img, ox, oy = render3do.render(polys, 2 * math.pi * d / dirs, team=team)
        frames.append(rgba_sprite(img, ox, oy, mapper))
        if d == 0:
            front = img, ox, oy
    # the unit's size on screen facing down it, for picking and selection brackets
    img, ox, oy = front
    ys, xs = np.nonzero(img[..., 3] > 128)
    rad = int(max(abs(xs - ox).max(), 4)) if len(xs) else 8
    top = int(max(oy - ys.min(), 4)) if len(ys) else 8
    return frames, rad, top


def rgba_sprite(img, ox, oy, mapper):
    opaque = img[..., 3] > 110
    idx = mapper.dither(img[..., :3].astype(np.float32), 0, 0) if opaque.any() else np.zeros(opaque.shape, np.uint8)
    return sprite(idx, opaque, ox, oy)


def write_units(out, units, weapons, mapper, sounds, fnames, feat_defs):
    names = sorted(units)
    assert len(names) <= MAX_UNITTYPES, len(names)
    # weapons in use
    wnames = []
    for n in names:
        for k in ('weapon1', 'weapon2', 'weapon3'):
            w = units[n].get(k, '').lower()
            if w and w in weapons and w not in wnames:
                wnames.append(w)
    for n in names:
        e = units[n].get('explodeas', '').lower()
        if e in weapons and e not in wnames:
            wnames.append(e)
    for w in wnames:
        weapons[w]['_key'] = w
    classes = []
    for n in names:
        if int(ta.num(units[n], 'bmcode')):
            c = move_class(units[n])
            if c not in classes:
                classes.append(c)
    assert len(classes) <= 6, classes
    bank = Bank()
    recs = []
    for i, n in enumerate(names):
        u = units[n]
        side = 0 if u.get('side', '').upper() == 'ARM' else 1
        mobile = int(ta.num(u, 'bmcode')) == 1
        builder = int(ta.num(u, 'builder'))
        opts = build_options(n, units)
        kind = 0 if mobile else (2 if builder and opts else 1)
        flags = 0
        if builder and opts:
            flags |= 1
        if 'commander' in u.get('category', '').lower():
            flags |= 2
        if int(ta.num(u, 'canattack', 1)) and u.get('weapon1'):
            flags |= 4
        if int(ta.num(u, 'windgenerator')):
            flags |= 8
        if int(ta.num(u, 'canpatrol', 0)):
            flags |= 16
        if float(ta.num(u, 'extractsmetal')) > 0:
            flags |= 32
        wl = [wnames.index(u[k].lower()) if u.get(k, '').lower() in wnames else -1
              for k in ('weapon1', 'weapon2', 'weapon3')]
        dirs = DIRS if mobile else 1
        frames, rad, top = render_unit(u.get('objectname', n).lower(), side, dirs, mapper)
        table = bytearray()
        for f in frames:
            table += be('I', bank.add(f))
        sprite_ofs = bank.add(bytes(table))
        mc = classes.index(move_class(u)) if mobile else 255
        cat = sounds_for(u)
        explode = wnames.index(u['explodeas'].lower()) if u.get('explodeas', '').lower() in wnames else -1
        opts_idx = [names.index(o) for o in opts][:24]
        # the wreck it leaves: a feature drawn on its own, not one of the map's baked ones
        c = u.get('corpse', '').lower()
        corpse = fnames.index(c) if c in fnames and not baked(c, feat_defs[c]) else -1
        rec = (cstr(n.upper(), 10) + cstr(u.get('name', n), 20) + cstr(u.get('description', ''), 32) +
               be('BBHBBBB', side, kind, flags, int(ta.num(u, 'footprintx', 1)), int(ta.num(u, 'footprintz', 1)),
                  mc, len(opts_idx)) +
               be('bbbb', wl[0], wl[1], wl[2], explode) +
               be('HHHHHHH',
                  int(ta.num(u, 'maxdamage', 100)),
                  int(round(ta.num(u, 'maxvelocity') * TA_FRAMES / TICKS * 256)),
                  int(round(ta.num(u, 'acceleration', 0.1) * (TA_FRAMES / TICKS) ** 2 * 256)),
                  min(65535, int(ta.num(u, 'turnrate', 500) * TA_FRAMES / TICKS)),
                  int(ta.num(u, 'sightdistance', 200)),
                  int(ta.num(u, 'builddistance', 60) or 60),
                  int(ta.num(u, 'workertime'))) +
               be('III', int(ta.num(u, 'buildcostmetal')), int(ta.num(u, 'buildcostenergy')),
                  int(ta.num(u, 'buildtime', 100))) +
               be('iiiiiii',
                  per_tick(ta.num(u, 'energymake')), per_tick(ta.num(u, 'metalmake')),
                  per_tick(ta.num(u, 'energyuse')),
                  int(round(ta.num(u, 'extractsmetal') * 1e6)),
                  int(ta.num(u, 'windgenerator')),
                  int(ta.num(u, 'metalstorage')), int(ta.num(u, 'energystorage'))) +
               be('I', sprite_ofs) + be('BB', dirs, rad) + be('hhhh', top, cat[0], cat[1], corpse) +
               bytes(b & 255 for b in (opts_idx + [-1] * 24)[:24]) + yard_bits(u))
        recs.append(rec)
        print('unit', n, 'kind', kind, 'dirs', dirs, 'opts', opts, 'weapons', wl)
    wrecs = [weapon_record(weapons[w], mapper, sounds) for w in wnames]
    body = (b'OAU2' + be('HHH', len(wrecs), len(recs), len(classes)) +
            b''.join(be('BBB', *c) for c in classes) + b''.join(wrecs) + b''.join(recs))
    while len(body) & 3:
        body += b'\\0'
    data = body + be('I', len(bank.data)) + bytes(bank.data)
    open(os.path.join(out, 'UNITS.DAT'), 'wb').write(data)
    print('UNITS.DAT', len(data), 'bytes; weapon rec', len(wrecs[0]), 'unit rec', len(recs[0]),
          'classes', classes)
    return names, classes


SOUND_TABLE = None


def sounds_for(u):
    global SOUND_TABLE
    if SOUND_TABLE is None:
        SOUND_TABLE = ta.read_tdf(ta.path('gamedata/sound.tdf'))
    cat = SOUND_TABLE.get(u.get('soundcategory', '').lower(), {})
    return SOUNDS.ref(cat.get('select1')), SOUNDS.ref(cat.get('ok1'))


# ------------------------------------------------------------------ common

def gaf_rgba(frame, pal):
    w, h, x, y, pix, tr = frame
    a = np.array(pix, np.uint8).reshape(h, w)
    rgb = np.array(pal, np.uint8)[a]
    alpha = (a != tr).astype(np.uint8) * 255
    return np.dstack([rgb, alpha]), x, y


FF_RECLAIM, FF_INDESTRUCT, FF_BAKED = 1, 4, 8      # data.h's FF_*


def baked(name, d):
    """Rocks and dragon's teeth are drawn into the map's tiles, not on their own."""
    return (d.get('category', '').lower() in ('rocks', 'dragonteeth') or 'rock' in name or
            name == 'dragonsteeth')


def feature_names(units, unit_names, feat_defs, map_feature_names):
    """The features COMMON.DAT holds, in its order: the maps', then the units' wrecks."""
    out = []
    wanted = [n.lower() for n in map_feature_names]
    wanted += [units[n].get('corpse', '').lower() for n in unit_names]
    for n in wanted:
        if n and n in feat_defs and n not in out:
            out.append(n)
    return out


def load_features():
    feats = {}
    # in name order, so that the result is the same whatever order the
    # filesystem lists them in (the web converter's is not the Mac's)
    for root, dirs, files in os.walk(ta.path('features')):
        dirs.sort()
        for f in sorted(files):
            if f.endswith('.tdf'):
                for k, v in ta.read_tdf(os.path.join(root, f)).items():
                    feats[k] = v
    return feats


def write_common(out, game_pal, mapper, unit_names, feat_defs, fnames):
    pal = ta.palette()
    bank = Bank()
    # -- the panels around the battlefield, as TA lays them out at 640x480
    panel = np.zeros((480, 640, 3), np.uint8)
    gui = gaf('armint')
    common = gaf('commongui')
    for fr, x, y in ((gui['panelside'][0], 0, 0), (gui['paneltop'][0], 128, 0),
                     (gui['panelbot'][0], 128, 447), (common['armpan'][0], 0, 128)):
        blit_rgba(panel, gaf_rgba(fr, pal)[0], x, y)
    pidx = quant.Mapper(game_pal, spread=20).dither(panel.astype(np.float32))
    Image.fromarray(np.array(game_pal, np.uint8)[pidx]).save(os.path.join(PREVIEWS, 'panel.png'))
    panel_ofs = bank.add(image_planar(pidx))

    # -- features: map trees and rocks, and every unit's wreck
    frecs = []
    for n in fnames:
        d = feat_defs[n]
        spr = b''
        if d.get('object'):
            p = ta.path('objects3d/%s.3do' % d['object'].lower())
            if p:
                polys = render3do.collect(ta.read_3do(p))
                side = 1 if n.startswith('cor') else 0
                img, ox, oy = render3do.render(polys, 0.3, team=side)
                spr = rgba_sprite(img, ox, oy, mapper)
        elif d.get('filename') and d.get('seqname'):
            g = gaf(d['filename'].lower())
            seq = g.get(d['seqname'].lower())
            if seq:
                img, ox, oy = gaf_rgba(seq[0], pal)
                spr = rgba_sprite(img, ox, oy, mapper)
        flags = ((FF_RECLAIM if int(ta.num(d, 'reclaimable', 0)) else 0) |
                 (FF_INDESTRUCT if int(ta.num(d, 'indestructible', 0)) else 0) |
                 (FF_BAKED if baked(n, d) else 0))
        frecs.append(cstr(n, 16) + be('HIIIBBBBI', flags, int(ta.num(d, 'metal')), int(ta.num(d, 'energy')),
                                      int(ta.num(d, 'damage', 100)), int(ta.num(d, 'footprintx', 1)),
                                      int(ta.num(d, 'footprintz', 1)), int(ta.num(d, 'height', 0)) & 255, 0,
                                      bank.add(spr) if spr else 0xFFFFFFFF))
    print('features', len(fnames))

    # -- explosion and other effect animations
    fx = dict(gaf('fx'))
    fx.update(gaf('exp1'))
    for extra in ('explode2', 'explode3', 'explode4', 'explode5', 'explosion', 'smoke 1', 'nuke1'):
        anim_ref(extra)
    arecs = []
    for a in ANIMS:
        seq = fx.get(a, [])
        offs = []
        if len(seq) > 12:
            seq = seq[::2]          # long animations at half their frames: the TT shows fewer anyway
        for fr in seq[:16]:
            img, ox, oy = gaf_rgba(fr, pal)
            offs.append(bank.add(rgba_sprite(img, ox, oy, mapper)))
        tab = b''.join(be('I', o) for o in offs)
        arecs.append(be('HHI', len(offs), 0, bank.add(tab) if offs else 0))
    print('anims', ANIMS)

    # -- cursors: normal, select, move, attack, build, patrol, cant, reclaim, guard, repair
    cur = gaf('cursors')
    cnames = ['cursornormal', 'cursorselect', 'cursormove', 'cursorattack', 'cursorfindsite',
              'cursorpatrol', 'cursortoofar', 'cursorreclamate', 'cursordefend', 'cursorrepair']
    crecs = []
    for c in cnames:
        fr = cur[c][0]
        img, ox, oy = gaf_rgba(fr, pal)
        crecs.append(be('I', bank.add(rgba_sprite(img, ox, oy, mapper))))

    # -- unit pictures for the build menu, 64x64
    prec = []
    for n in unit_names:
        p = ta.path('unitpics/%s.pcx' % n)
        im = Image.open(p).convert('RGB').resize((64, 64), Image.LANCZOS)
        idx = mapper.dither(np.array(im, np.float32))
        prec.append(be('I', bank.add(image_planar(idx))))

    # -- order buttons (each with its frames: up, down, lit)
    bnames = ['armmove', 'armattack', 'armstop', 'armpatrol', 'armdefend', 'armblast', 'armreclaim',
              'armrepair', 'armorders', 'armbuild', 'armprev', 'armnext', 'armfireord', 'armmoveord']
    brecs = []
    for b in bnames:
        frs = common.get(b, [])
        offs = []
        for fr in frs[:6]:
            img, ox, oy = gaf_rgba(fr, pal)
            offs.append(bank.add(rgba_sprite(img, 0, 0, mapper)))
        brecs.append(be('H', len(offs)) + b''.join(be('I', o) for o in (offs + [0] * 6)[:6]))

    # -- the font: hattfont12, as 1-bit glyphs up to 16 wide
    font = gaf('hattfont12')
    seq = list(font.values())[0]
    base = seq[0x49][1]
    # The glyphs are shaded greys: the bright ones are the letter, and the
    # dark ones its outline, shadow and counters (the hole in a 0)
    bright = np.array(pal, np.uint8).max(axis=1) >= 147
    glyphs = bytearray()
    gofs = []
    for c in range(256):
        w, h, x, y, pix, tr = seq[c] if c < len(seq) else seq[0]
        px = np.array(pix, np.uint8).reshape(h, w)
        body = (px != tr) & bright[px]
        line = (px != tr) & ~bright[px]
        w2 = min(w, 16)
        # each row: the letter's word, then its outline's
        rows = np.stack([body[:, :w2] @ BITS[:w2], line[:, :w2] @ BITS[:w2]], axis=1)
        gofs.append(len(glyphs))
        glyphs += be('bbbb', w, h, x, y - base) + rows.astype('>u2').tobytes()
    font_ofs = bank.add(be('HH', base + 2, 0) + b''.join(be('H', o) for o in gofs) + bytes(glyphs))

    # -- the fog over what is unexplored: fog.gaf's edge tiles. A tile is
    # 32x32 and lies across the corner of four 32-pixel sight cells, a
    # quarter on each; frame n of a set blacks out the quarters in the bits
    # of n + 1 (1 top left, 2 top right, 4 bottom left, 8 bottom right) with
    # an uneven edge. Black is solid; gray, a little larger, a fringe of
    # every other pixel. For each of the four sets, each of the 16
    # patterns, each quarter and each of its rows: the pixels left drawn.
    fog = gaf('fog')
    yy, xx = np.mgrid[0:32, 0:32]
    every_other = ((xx + yy) & 1) == 0

    def fog_mask(fr):
        w, h, x, y, pix, tr = fr
        a = np.array(pix, np.uint8).reshape(h, w) != tr
        m = np.zeros((32, 32), bool)
        ys, xs = np.nonzero(a)
        X, Y = xs - x, ys - y
        ok = (X >= 0) & (X < 32) & (Y >= 0) & (Y < 32)
        m[Y[ok], X[ok]] = True
        return m

    fog_words = bytearray()
    for v in range(1, 5):
        for pat in range(16):
            if pat == 0:
                dark = np.zeros((32, 32), bool)
            elif pat == 15:
                dark = np.ones((32, 32), bool)
            else:
                black = fog_mask(fog['black%d' % v][pat - 1])
                dark = black | (fog_mask(fog['gray%d' % v][pat - 1]) & every_other)
            for qy in range(2):
                for qx in range(2):
                    q = ~dark[qy * 16:qy * 16 + 16, qx * 16:qx * 16 + 16]
                    fog_words += (q @ BITS).astype('>u2').tobytes()
    fog_ofs = bank.add(bytes(fog_words))

    # -- victory, defeat and paused titles
    ig = gaf('igtitles')
    trecs = []
    for t in ('igvictory', 'igdefeat', 'igpaused'):
        img, ox, oy = gaf_rgba(ig[t][0], pal)
        trecs.append(be('I', bank.add(rgba_sprite(img, 0, 0, mapper))))

    head = (b'OAC2' + b''.join(be('H', tt_word(c)) for c in game_pal) +
            be('IHHHHHHII', panel_ofs, len(fnames), len(arecs), len(crecs), len(prec), len(brecs), len(trecs), font_ofs,
               fog_ofs))
    body = head + b''.join(frecs) + b''.join(arecs) + b''.join(crecs) + b''.join(prec) + b''.join(brecs) + b''.join(trecs)
    while len(body) & 3:
        body += b'\\0'
    data = body + be('I', len(bank.data)) + bytes(bank.data)
    open(os.path.join(out, 'COMMON.DAT'), 'wb').write(data)
    print('COMMON.DAT', len(data), 'bytes')


# ------------------------------------------------------------------ missions

COND = {'moveunittoradius': 1, 'killunittype': 2, 'commanderkilled': 10, 'allunitskilled': 11,
        'allunitskilledoftype': 12}


def compile_script(text, unit_names, idents):
    """InitialMission text -> ops: (op, a, b, c) as i16s.

    1 move x z, 2 patrol x z, 3 wait ticks, 4 wait-for-attack, 5 attack-type t,
    6 orders move fire, 7 build t count, 8 guard ident, 9 attack x z. 's' (make
    selectable) gives none: every unit is.
    """
    ops = []
    for tok in text.split(','):
        tok = tok.strip()
        if not tok:
            continue
        c, args = tok[0].lower(), tok[1:].split()
        try:
            if c == 'm':
                ops.append((1, int(float(args[0])), int(float(args[1])), 0))
            elif c == 'p':
                ops.append((2, int(float(args[0])), int(float(args[1])), 0))
            elif c == 'w':
                if tok[1:2].lower() == 'a':
                    ops.append((4, 0, 0, 0))
                else:
                    ops.append((3, min(32767, int(float(args[0]) * TICKS)), 0, 0))
            elif c == 'a':
                if len(args) >= 2 and args[0].lstrip('-').isdigit():
                    ops.append((9, int(args[0]), int(args[1]), 0))
                elif args and args[0].lower() in unit_names:
                    ops.append((5, unit_names.index(args[0].lower()), 0, 0))
            elif c == 'o':
                ops.append((6, int(args[0]) if args else 1, int(args[1]) if len(args) > 1 else 2, 0))
            elif c == 'b':
                if args and args[0].lower() in unit_names:
                    ops.append((7, unit_names.index(args[0].lower()), int(args[1]) if len(args) > 1 else 1, 0))
            elif c == 'g':
                if args and args[0] in idents:
                    ops.append((8, idents[args[0]], 0, 0))
        except (ValueError, IndexError):
            print('  script token skipped:', tok)
    return ops


MAX_TILES = 2600


def merge_tiles(img, firsts, uses, tilemap, tile_bytes):
    """Fits a map into MAX_TILES tiles by drawing tiles that look nearly alike
    with one of them: each tile's 8x8 colour thumbnail is compared, the most
    used tiles are kept first, and the threshold rises until the rest fit."""
    n = len(firsts)
    thumbs = np.stack([img[ty * 32:ty * 32 + 32, tx * 32:tx * 32 + 32].reshape(8, 4, 8, 4, 3).mean((1, 3)).ravel()
                       for ty, tx in firsts])
    # every pair of thumbnails' distance, worked out once for all the passes
    dist = np.empty((n, n), np.float32)
    for s in range(0, n, 16):
        dist[s:s + 16] = np.sqrt(((thumbs[s:s + 16, None, :] - thumbs[None, :, :]) ** 2).mean(2))
    order = np.argsort(-np.array(uses), kind='stable')
    lo, hi = 0.0, 64.0
    best = None
    for _ in range(14):
        t = (lo + hi) / 2
        reps = []
        assign = np.zeros(n, np.int32)
        for i in order:
            if len(reps):
                d = dist[i, reps]
                k = int(d.argmin())
                if d[k] < t:
                    assign[i] = k
                    continue
            assign[i] = len(reps)
            reps.append(i)
        if len(reps) <= MAX_TILES:
            best = (t, reps, assign.copy())
            hi = t
        else:
            lo = t
    t, reps, assign = best
    print('  merged %d tiles into %d (threshold %.1f)' % (n, len(reps), t))
    return [int(assign[v]) for v in tilemap], [tile_bytes[i] for i in reps]


def map_image(t):
    pal = np.array(ta.palette(), np.uint8)
    tw, th = t.w // 2, t.h // 2
    img = np.zeros((th * 32, tw * 32, 3), np.uint8)
    tiles = [pal[np.frombuffer(b, np.uint8).reshape(32, 32)] for b in t.tiles]
    for ty in range(th):
        for tx in range(tw):
            img[ty * 32:ty * 32 + 32, tx * 32:tx * 32 + 32] = tiles[t.tilemap[ty * tw + tx]]
    return img


def write_mission(out, num, m, t, mapper, game_pal, unit_names, classes, feat_defs, fnames):
    ota = ta.read_tdf(ta.path('maps/%s.ota' % m))
    gh = ota['globalheader']
    pal = ta.palette()
    img = map_image(t).astype(np.float32)
    W, H = t.w, t.h
    # features: bake rocks into the picture, every shadow too; keep the rest
    placed = []
    metal = np.zeros((H, W), np.int32)
    blocked_feat = np.zeros((H, W), bool)
    for cz in range(H):
        for cx in range(W):
            f = t.feature[cz * W + cx]
            if f >= len(t.features):
                continue
            name = t.features[f].lower()
            d = feat_defs.get(name)
            if not d:
                continue
            fx, fz = int(ta.num(d, 'footprintx', 1)), int(ta.num(d, 'footprintz', 1))
            if int(ta.num(d, 'metal')) and 'rock' in name:
                metal[cz:cz + fz, cx:cx + fx] = int(ta.num(d, 'metal'))
            px, pz = cx * 16 + fx * 8, cz * 16 + fz * 8
            py = pz - t.height[min(cz, H - 1) * W + min(cx, W - 1)] // 2
            fn = d.get('filename', '').lower()
            g = gaf(fn) if fn else {}
            shad = g.get(d.get('seqnameshad', '').lower())
            if shad:
                s, sx, sy = gaf_rgba(shad[0], pal)
                blit_rgba(img, s, px - sx, py - sy, shade=0.55)
            if name in fnames and baked(name, d):
                seq = g.get(d.get('seqname', '').lower())
                if seq:
                    s, sx, sy = gaf_rgba(seq[0], pal)
                    blit_rgba(img, s, px - sx, py - sy)
                # metal deposits are built on, not walked round
                if int(ta.num(d, 'blocking', 1)) and not int(ta.num(d, 'metal')):
                    blocked_feat[cz:cz + fz, cx:cx + fx] = True
            elif name in fnames:
                placed.append((fnames.index(name), px, pz))
    # tiles of the dithered picture
    tw, th = W // 2, H // 2
    q = mapper.dither(img)
    tiles = {}
    tilemap = []
    tile_bytes = []
    firsts = []                     # (ty, tx) where each unique tile first appears
    uses = []
    for ty in range(th):
        for tx in range(tw):
            tq = q[ty * 32:ty * 32 + 32, tx * 32:tx * 32 + 32]
            key = tq.tobytes()
            if key not in tiles:
                tiles[key] = len(tile_bytes)
                tile_bytes.append(planar_rows(tq))
                firsts.append((ty, tx))
                uses.append(0)
            uses[tiles[key]] += 1
            tilemap.append(tiles[key])
    if len(tile_bytes) > MAX_TILES:
        tilemap, tile_bytes = merge_tiles(img, firsts, uses, tilemap, tile_bytes)
    Image.fromarray(np.array(game_pal, np.uint8)[q]).save(os.path.join(PREVIEWS, 'map%d.png' % num))
    # passability for each movement class; a cell's slope is its corner spread
    hgt = np.array(t.height, np.int32).reshape(H, W)
    hp = np.pad(hgt, ((0, 1), (0, 1)), mode='edge')
    corners = np.stack([hp[:-1, :-1], hp[1:, :-1], hp[:-1, 1:], hp[1:, 1:]])
    slope = corners.max(0) - corners.min(0)
    depth = t.sealevel - hgt
    flags = np.zeros((H, W), np.uint8)
    for i, (ms, maxd, mind) in enumerate(classes):
        bad = (slope > ms) | (depth > maxd) | ((depth < mind) if mind > 0 else False) | blocked_feat
        flags |= (bad.astype(np.uint8) << i)
    flags |= (depth > 0).astype(np.uint8) << 6          # water
    flags |= blocked_feat.astype(np.uint8) << 7
    # minimap from the picture, fitted into 128x128
    sc = 128.0 / max(W * 16, H * 16)
    mw, mh = int(W * 16 * sc) // 16 * 16, int(H * 16 * sc)
    mm = Image.fromarray(img.astype(np.uint8)).resize((mw, mh), Image.BOX)
    mmi = mapper.dither(np.array(mm, np.float32))
    # objectives
    conds = []
    for k, v in gh.items():
        if k in COND:
            parts = [p.strip() for p in v.split(',')]
            typ = -1
            args = [0, 0, 0]
            if k == 'moveunittoradius':
                typ = unit_names.index(parts[0].lower()) if parts[0].lower() in unit_names else -1
                args = [int(parts[1]), int(parts[2]), int(parts[3])]
            elif k == 'killunittype':
                typ = unit_names.index(parts[0].lower())
                args = [int(parts[1]) if len(parts) > 1 else 1, 0, 0]
            elif k == 'allunitskilledoftype':
                typ = unit_names.index(parts[0].lower())
            elif v.strip() in ('0', ''):
                continue
            conds.append(be('hhhhh', COND[k], typ, *args))
    # schemas: units and their scripts
    schemas = []
    scripts = bytearray()
    for si in range(int(ta.num(gh, 'schemacount', 1))):
        sch = gh.get('schema %d' % si)
        if not sch:
            continue
        ulist = []
        idents = {}
        entries = [v for k, v in sorted(sch.get('units', {}).items(), key=lambda kv: int(kv[0][4:].split('#')[0]))]
        for i, u in enumerate(entries):
            if u.get('ident'):
                idents.setdefault(u['ident'], i)
        for u in entries:
            n = u.get('unitname', '').lower()
            if n not in unit_names:
                print('  unknown unit', n)
                continue
            ops = compile_script(u.get('initialmission', ''), unit_names, idents)
            so = len(scripts) // 8
            for op in ops:
                scripts += be('hhhh', *op)
            ang = int(ta.num(u, 'angle', 0))
            ulist.append(be('HBBhhHHH', unit_names.index(n), int(ta.num(u, 'player', 1)) - 1,
                            int(ta.num(u, 'healthpercentage', 100)), int(ta.num(u, 'xpos')),
                            int(ta.num(u, 'zpos')), ang & 0xffff, so, len(ops)))
        schemas.append(be('iiiiHH', int(ta.num(sch, 'humanmetal', 1000)), int(ta.num(sch, 'humanenergy', 1000)),
                          int(ta.num(sch, 'computermetal', 1000)), int(ta.num(sch, 'computerenergy', 1000)),
                          int(ta.num(sch, 'surfacemetal', 3)), len(ulist)) + b''.join(ulist))
        print('  schema', sch.get('type'), len(ulist), 'units')
    assert len(conds) <= 8 and len(schemas) <= 3, (len(conds), len(schemas))   # data.h's Mission
    # base metal everywhere else
    sm = int(ta.num(gh.get('schema 0', {}), 'surfacemetal', 3))
    metal[metal == 0] = sm
    head = (b'OAM2' + cstr(gh.get('missionname', m), 40) +
            be('HHHHHHHHHH', W, H, tw, th, len(tile_bytes), t.sealevel, mw, mh, len(placed), len(conds)) +
            be('HHHH', int(ta.num(gh, 'solarstrength')), int(ta.num(gh, 'minwindspeed')),
               int(ta.num(gh, 'maxwindspeed')), len(schemas)))
    body = head + b''.join(conds)
    body += b''.join(be('HHH', *p) for p in placed)
    body += be('I', len(scripts) // 8) + bytes(scripts)
    for s in schemas:
        body += s
    body += b''.join(be('H', v) for v in tilemap)
    body += hgt.astype(np.uint8).tobytes() + flags.tobytes() + np.clip(metal, 0, 255).astype(np.uint8).tobytes()
    body += image_planar(mmi)
    while len(body) & 3:
        body += b'\\0'
    body += b''.join(tile_bytes)
    open(os.path.join(out, 'MISS%d.DAT' % num), 'wb').write(body)
    print('MISS%d.DAT' % num, len(body), 'bytes,', len(tile_bytes), 'tiles,', len(placed), 'features')


# ------------------------------------------------------------------ pictures and sound

def write_picture(src, dst):
    """A 640x480 picture with its own 16-colour palette, error-diffused."""
    im = Image.open(src).convert('RGB').resize((640, 480))
    a = np.array(im, np.float32)
    pal = quant.build_picture(a.reshape(-1, 3)[::3])
    q = quant.diffuse(a, pal)
    Image.fromarray(np.array(pal, np.uint8)[q]).save(os.path.join(PREVIEWS, os.path.basename(dst) + '.png'))
    open(dst, 'wb').write(b''.join(be('H', tt_word(c)) for c in pal) + image_planar(q))


def write_sounds(out):
    """Short sounds at 12517 Hz, longer ones at 6258 Hz; none past MAX_SOUND seconds."""
    data = bytearray()
    offs = []
    for n in SOUNDS.names:
        p = ta.path('sounds/%s.wav' % n)
        w = wave.open(p)
        secs = w.getnframes() / float(w.getframerate())
        rate, code = (12517, 1) if secs <= 0.5 else (6258, 0)
        smp = np.frombuffer(read_wav(p, rate), np.int8).astype(np.float32)
        cap = int(MAX_SOUND * rate)
        if len(smp) > cap:
            fade = int(0.25 * rate)
            smp = smp[:cap]
            smp[-fade:] *= np.linspace(1, 0, fade)
        b = np.clip(np.round(smp), -128, 127).astype(np.int8).tobytes()
        if len(b) & 1:
            b += b'\\0'
        offs.append((len(data), len(b), code))
        data += b
    head = b'OAS2' + be('H', len(offs)) + b''.join(be('IIHH', o, l, c, 0) for o, l, c in offs)
    open(os.path.join(out, 'SOUNDS.DAT'), 'wb').write(head + bytes(data))
    print('SOUNDS.DAT', len(head) + len(data), 'bytes,', len(offs), 'sounds')


MAX_SOUND = 1.6
SOUNDS = Sounds()


PREVIEWS = None     # where pictures of the converted art go


def main():
    global PREVIEWS
    ta.ROOT = sys.argv[1]
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    PREVIEWS = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(out)), 'previews')
    os.makedirs(PREVIEWS, exist_ok=True)
    maps = {m: ta.read_tnt(ta.path('maps/%s.tnt' % m)) for m in MISSIONS}
    game_pal = gather_palette(maps)
    print('palette', game_pal)
    mapper = quant.Mapper(game_pal, spread=30)
    units, weapons = load_defs()
    feat_defs = load_features()
    mfeat = []
    for t in maps.values():
        for f in t.features:
            if f.lower() not in mfeat:
                mfeat.append(f.lower())
    fnames = feature_names(units, sorted(units), feat_defs, mfeat)
    for i, n in enumerate(ENGINE_ANIMS):
        assert anim_ref(n) == i, n
    # interface sounds the engine plays by number (see data.h, SND_*)
    for i, n in enumerate(('beep2', 'beep4', 'button9', 'cantdo4', 'xplomed2', 'xplosml3', 'xplolrg3',
                           'twractv1', 'unitdone', 'build1', 'metalno1', 'warning1', 'nanlath1',
                           'victory2', 'xplobld1')):
        assert SOUNDS.ref(n) == i, n
    unit_names, classes = write_units(out, units, weapons, mapper, SOUNDS, fnames, feat_defs)
    write_common(out, game_pal, mapper, unit_names, feat_defs, fnames)
    for i, m in enumerate(MISSIONS):
        write_mission(out, i + 1, m, maps[m], mapper, game_pal, unit_names, classes, feat_defs, fnames)
    write_sounds(out)
    for i in range(len(MISSIONS)):
        # the briefing's text: its first line ("MISSION n...") and the closing
        # END left out; the line break that ends the first line stays, a blank
        # line under the mission's name
        txt = open(ta.path('camps/briefs/armcampaign%d.txt' % (i + 1)), 'rb').read()
        if txt.startswith(b'MISSION'):
            txt = txt[txt.index(b'\\n'):]
        if txt.endswith(b'END'):
            txt = txt[:-3]
        open(os.path.join(out, 'BRIEF%d.TXT' % (i + 1)), 'wb').write(txt)
        s = read_wav(ta.path('camps/briefs/arm%02d.wav' % (i + 1)), 12517)
        open(os.path.join(out, 'BRIEF%d.SND' % (i + 1)), 'wb').write(s)
    write_picture(ta.path('bitmaps/frontendx.pcx'), os.path.join(out, 'TITLE.IMG'))
    write_picture(ta.path('bitmaps/singlebg.pcx'), os.path.join(out, 'SINGLE.IMG'))
    write_picture(ta.path('bitmaps/mbriefarm.pcx'), os.path.join(out, 'BRIEF.IMG'))
    done = {}
    for i, g in enumerate(GLAMOUR):
        dst = os.path.join(out, 'GLAM%d.IMG' % (i + 1))
        if g in done:
            shutil.copyfile(done[g], dst)
        else:
            write_picture(ta.path('bitmaps/glamour/%s.pcx' % g), dst)
            done[g] = dst


if __name__ == '__main__':
    main()
`,t=`# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unpacks a Total Annihilation HPI archive into a folder.

usage: hpi_extract.py archive.hpi outdir
"""
import os, struct, sys, zlib
import numpy as np

def lz77(src, size):
    out = bytearray()
    win = bytearray(4096)
    w = 1
    i = 0
    n = len(src)
    while i < n:
        flags = src[i]; i += 1
        for b in range(8):
            if not (flags >> b) & 1:
                c = src[i]; i += 1
                out.append(c); win[w] = c; w = (w + 1) & 0xfff
            else:
                v = src[i] | (src[i + 1] << 8); i += 2
                p = v >> 4
                if p == 0:
                    return bytes(out)
                for _ in range((v & 15) + 2):
                    c = win[p]; out.append(c); win[w] = c
                    w = (w + 1) & 0xfff; p = (p + 1) & 0xfff
    return bytes(out)

def main():
    src, outdir = sys.argv[1], sys.argv[2]
    raw = np.fromfile(src, dtype=np.uint8)
    magic, ver, dsize, hkey, start = struct.unpack_from('<5I', raw.tobytes()[:20])
    assert magic == 0x49504148 and ver == 0x10000
    k = hkey & 0xff
    if k:
        key = (~((k >> 6) | (k << 2))) & 0xff
        pos = (np.arange(len(raw), dtype=np.uint32) & 0xff).astype(np.uint8)
        dec = pos ^ (~raw) ^ np.uint8(key)
        dec[:20] = raw[:20]
    else:
        dec = raw
    d = dec.tobytes()

    def cstr(o):
        return d[o:d.index(b'\\0', o)].decode('latin-1')

    def readfile(o):
        off, size, comp = struct.unpack_from('<IIB', d, o)
        if comp == 0:
            return d[off:off + size]
        nchunks = (size + 65535) // 65536
        p = off + 4 * nchunks
        out = bytearray()
        for c in range(nchunks):
            csz = struct.unpack_from('<I', d, off + 4 * c)[0]
            hdr = d[p:p + 19]
            _, _, ctype, enc, comp_sz, dec_sz, _ = struct.unpack('<IBBBIII', hdr)
            payload = bytearray(d[p + 19:p + 19 + comp_sz])
            if enc:
                a = np.frombuffer(bytes(payload), dtype=np.uint8)
                idx = (np.arange(len(a)) & 0xff).astype(np.uint8)
                payload = ((a - idx) ^ idx).astype(np.uint8).tobytes()
            if ctype == 1:
                out += lz77(bytes(payload), dec_sz)
            elif ctype == 2:
                out += zlib.decompress(bytes(payload))
            else:
                out += payload
            p += csz
        return bytes(out[:size])

    count = 0
    def walk(o, path):
        nonlocal count
        n, lst = struct.unpack_from('<II', d, o)
        for e in range(n):
            no, do, isdir = struct.unpack_from('<IIB', d, lst + 9 * e)
            name = cstr(no)
            p = os.path.join(path, name.lower())
            if isdir:
                os.makedirs(p, exist_ok=True)
                walk(do, p)
            else:
                with open(p, 'wb') as f:
                    f.write(readfile(do))
                count += 1
    os.makedirs(outdir, exist_ok=True)
    walk(start, outdir)
    print(count, 'files')

main()
`,n=`# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""Writes one resource of a Windows program file (PE) to a file.

usage: pe_resource.py installer.exe TYPE ID LANG out
"""
import struct, sys

def main():
    path, rtype, rid, lang, out = sys.argv[1:6]
    rid, lang = int(rid), int(lang)
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', d, opt)[0]
    ddir = opt + (96 if magic == 0x10b else 112)
    rsrc_rva = struct.unpack_from('<I', d, ddir + 2 * 8)[0]
    secs = []
    for i in range(nsec):
        s = opt + optsz + i * 40
        vsz, va, rawsz, rawp = struct.unpack_from('<IIII', d, s + 8)
        secs.append((va, max(vsz, rawsz), rawp))
    def off(rva):
        for va, sz, rp in secs:
            if va <= rva < va + sz:
                return rva - va + rp
        raise ValueError(rva)
    base = off(rsrc_rva)
    def entries(o):
        nn, ni = struct.unpack_from('<HH', d, o + 12)
        for i in range(nn + ni):
            name, tgt = struct.unpack_from('<II', d, o + 16 + i * 8)
            if name & 0x80000000:
                p = base + (name & 0x7fffffff)
                n = struct.unpack_from('<H', d, p)[0]
                key = d[p + 2:p + 2 + 2 * n].decode('utf-16le')
            else:
                key = name
            yield key, tgt
    def find(o, key):
        for k, t in entries(o):
            if k == key:
                return t
        raise KeyError(key)
    t = find(base, rtype)
    t = find(base + (t & 0x7fffffff), rid)
    t = find(base + (t & 0x7fffffff), lang)
    rva, size = struct.unpack_from('<II', d, base + t)
    o = off(rva)
    open(out, 'wb').write(d[o:o + size])
    print(out, size)

main()
`,r=`# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""Sums a Hatari profile.txt's call graph per function: exclusive and
inclusive cycles, largest first."""
import re, sys
tot = {}
for line in open(sys.argv[1], errors='replace'):
    if not line.startswith('0x'):
        continue
    name = line.rsplit(',', 1)[-1].strip()
    ex = inc = calls = 0
    for m in re.finditer(r'= (\\d+) \\w+ (\\d+)/(\\d+)/(\\d+)/\\d+/\\d+ (\\d+)/(\\d+)/(\\d+)/\\d+/\\d+', line):
        calls += int(m.group(1)); inc += int(m.group(4)); ex += int(m.group(7))
    a = tot.setdefault(name, [0, 0, 0])
    a[0] += ex; a[1] += inc; a[2] += calls
allc = sum(v[0] for v in tot.values()) or 1
for n, (ex, inc, calls) in sorted(tot.items(), key=lambda kv: -kv[1][0])[:int(sys.argv[2]) if len(sys.argv) > 2 else 25]:
    print('%-28s excl %5.1f%% %11d  incl %11d  calls %d' % (n, 100.0 * ex / allc, ex, inc, calls))
`,i=`# Copyright (C) 2026 Neil Rackett
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
        while c in pal:
            c = tuple(min(255, v + 17) for v in c)
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
`,a=`# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""Writes every SHOT*.RAW the TT program dumped (32-byte TT palette, then a
640x480 4-plane screen) in a folder as PNG files in another."""
import glob, os, sys
import numpy as np
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
for f in sorted(glob.glob(os.path.join(src, '*.RAW'))):
    d = open(f, 'rb').read()
    pal = np.frombuffer(d[:32], '>u2')
    rgb = np.stack([((pal >> 8) & 15) * 17, ((pal >> 4) & 15) * 17, (pal & 15) * 17], -1).astype(np.uint8)
    w = np.frombuffer(d[32:32 + 153600], '>u2').reshape(480, 40, 4)
    bits = (w[..., None] >> (15 - np.arange(16))) & 1          # 480, 40, 4, 16
    idx = (bits * (1 << np.arange(4))[None, None, :, None]).sum(2).reshape(480, 640)
    Image.fromarray(rgb[idx]).save(os.path.join(dst, os.path.basename(f)[:-4] + '.png'))
    print('wrote', os.path.basename(f)[:-4] + '.png')
`,o=`# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""Renders a 3DO model as the battlefield shows it, to an RGBA image.

The projection is the game's: screen x = x, screen y = -z - y/2, and what
is nearer the viewer along (0, 2, -1) hides what is behind. Faces are lit
from the game's light direction (-0.8, 1, 0.25). Rendering is done at
SS times the size and averaged down, so the edges stay smooth.
"""
import math
import numpy as np
import ta

SS = 3
LIGHT = np.array([-0.8, 1.0, 0.25]); LIGHT /= np.linalg.norm(LIGHT)
VIEW = np.array([0.0, 2.0, -1.0]); VIEW /= np.linalg.norm(VIEW)

_textures = None


def textures():
    """Every texture of textures/*.gaf: name -> list of frames as RGB arrays."""
    global _textures
    if _textures is None:
        import os
        pal = np.array(ta.palette(), dtype=np.float32)
        _textures = {}
        folder = ta.path('textures')
        for f in sorted(os.listdir(folder)):
            if f.lower().endswith('.gaf'):
                for name, frames in ta.read_gaf(os.path.join(folder, f)).items():
                    _textures[name] = [pal[np.array(fr[4], dtype=np.int32).reshape(fr[1], fr[0])] for fr in frames]
    return _textures


HIDDEN = ('fire', 'flare', 'flash', 'muzzle', 'emit', 'beam', 'sfx', 'smoke', 'exhaust', 'wake', 'dust')


def collect(ob, origin=(0, 0, 0), out=None):
    """Flattens the piece tree into world-space polygons, leaving out pieces
    whose names say they are effects (HIDDEN)."""
    if out is None:
        out = []
    o = (origin[0] + ob.off[0], origin[1] + ob.off[1], origin[2] + ob.off[2])
    if not any(h in ob.name for h in HIDDEN):
        for col, idx, tex, sel in ob.prims:
            if sel or len(idx) < 3:
                continue
            pts = [(ob.verts[i][0] + o[0], ob.verts[i][1] + o[1], ob.verts[i][2] + o[2]) for i in idx]
            # the ground plate: some wrecks (armham_dead, corak_dead,
            # corthud_dead) don't mark it as the selection shape, and it
            # would draw as a black square
            if not tex and (col & 255) == 0 and all(abs(p[1]) < 0.01 for p in pts):
                continue
            out.append((pts, col, tex))
    for c in ob.children:
        collect(c, o, out)
    return out


def render(polys, heading, team=0, pad=2, scale=1.0):
    """Draws polygons turned by heading (radians, 0 = facing down-screen).

    Returns (rgba uint8 array, ox, oy): the image and where the model's
    origin falls in it.
    """
    pal = ta.palette()
    tex = textures()
    c, s = math.cos(heading), math.sin(heading)
    tris = []
    for pts, col, tname in polys:
        P = np.array([(p[0] * c + p[2] * s, p[1], -p[0] * s + p[2] * c) for p in pts]) * scale
        n = np.cross(P[1] - P[0], P[2] - P[0])
        ln = np.linalg.norm(n)
        if ln < 1e-9:
            continue
        n /= ln
        if np.dot(n, VIEW) < 0:
            n = -n
        shade = 0.45 + 0.75 * max(0.0, float(np.dot(n, LIGHT)))
        t = tex.get(tname)
        if t:
            img = t[min(team, len(t) - 1)]
        else:
            img = None
            flat = np.array(pal[col & 255], dtype=np.float32)
        uv = [(0, 0), (1, 0), (1, 1), (0, 1)]
        for k in range(1, len(P) - 1):
            a, b, d = 0, k, k + 1
            tris.append((P[[a, b, d]], [uv[min(a, 3)], uv[min(b, 3)], uv[min(d, 3)]], img,
                         None if img is not None else flat, shade))
    if not tris:
        return np.zeros((1, 1, 4), np.uint8), 0, 0
    allp = np.concatenate([t[0] for t in tris])
    sx = allp[:, 0]
    sy = -allp[:, 2] - allp[:, 1] / 2
    x0 = math.floor(sx.min()) - pad; x1 = math.ceil(sx.max()) + pad
    y0 = math.floor(sy.min()) - pad; y1 = math.ceil(sy.max()) + pad
    W, H = (x1 - x0) * SS, (y1 - y0) * SS
    rgb = np.zeros((H, W, 3), np.float32)
    zb = np.full((H, W), -1e9, np.float32)
    cov = np.zeros((H, W), bool)
    for P, uv, img, flat, shade in tris:
        X = (P[:, 0] - x0) * SS
        Y = (-P[:, 2] - P[:, 1] / 2 - y0) * SS
        Z = P[:, 1] * 2 - P[:, 2]
        bx0 = max(int(math.floor(X.min())), 0); bx1 = min(int(math.ceil(X.max())), W - 1)
        by0 = max(int(math.floor(Y.min())), 0); by1 = min(int(math.ceil(Y.max())), H - 1)
        if bx1 < bx0 or by1 < by0:
            continue
        gx, gy = np.meshgrid(np.arange(bx0, bx1 + 1) + 0.5, np.arange(by0, by1 + 1) + 0.5)
        den = (Y[1] - Y[2]) * (X[0] - X[2]) + (X[2] - X[1]) * (Y[0] - Y[2])
        if abs(den) < 1e-9:
            continue
        l0 = ((Y[1] - Y[2]) * (gx - X[2]) + (X[2] - X[1]) * (gy - Y[2])) / den
        l1 = ((Y[2] - Y[0]) * (gx - X[2]) + (X[0] - X[2]) * (gy - Y[2])) / den
        l2 = 1 - l0 - l1
        e = -1e-4
        inside = (l0 >= e) & (l1 >= e) & (l2 >= e)
        if not inside.any():
            continue
        z = l0 * Z[0] + l1 * Z[1] + l2 * Z[2]
        sub_z = zb[by0:by1 + 1, bx0:bx1 + 1]
        win = inside & (z > sub_z)
        if not win.any():
            continue
        if img is not None:
            u = l0 * uv[0][0] + l1 * uv[1][0] + l2 * uv[2][0]
            v = l0 * uv[0][1] + l1 * uv[1][1] + l2 * uv[2][1]
            th, tw = img.shape[:2]
            tx = np.clip((u * tw).astype(int), 0, tw - 1)
            ty = np.clip((v * th).astype(int), 0, th - 1)
            col = img[ty, tx]
        else:
            col = np.broadcast_to(flat, win.shape + (3,))
        col = np.clip(col * shade, 0, 255)
        sub_rgb = rgb[by0:by1 + 1, bx0:bx1 + 1]
        sub_rgb[win] = col[win]
        sub_z[win] = z[win]
        cov[by0:by1 + 1, bx0:bx1 + 1] |= win
    # average SS x SS blocks
    h, w = H // SS, W // SS
    c4 = cov.reshape(h, SS, w, SS).astype(np.float32)
    a = c4.mean(axis=(1, 3))
    rs = (rgb * cov[..., None]).reshape(h, SS, w, SS, 3).sum(axis=(1, 3))
    cnt = c4.sum(axis=(1, 3))[..., None]
    out = np.zeros((h, w, 4), np.uint8)
    out[..., :3] = np.where(cnt > 0, rs / np.maximum(cnt, 1), 0).astype(np.uint8)
    out[..., 3] = (a * 255).astype(np.uint8)
    return out, -x0, -y0
`,s=`# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""Readers for the Total Annihilation demo's file formats, for the TT converter.

Each reader returns plain Python values: TDF sections as nested dicts with
lower-case keys, GAF frames as (width, height, x, y, pixels) with palette
indices, 3DO objects as trees of vertices and primitives.
"""
import functools, os, re, struct

ROOT = None  # folder the demo's archive was unpacked into


def path(*parts):
    """Finds a file under ROOT whatever the case of its name."""
    p = ROOT
    for part in '/'.join(parts).split('/'):
        want = part.lower()
        for name in os.listdir(p):
            if name.lower() == want:
                p = os.path.join(p, name)
                break
        else:
            return None
    return p


# ---------------------------------------------------------------- TDF text

def parse_tdf(text):
    """Parses TDF/FBI/OTA/GUI text into nested dicts; keys are lower case.

    Repeated section names at one level become numbered with '#n'.
    """
    text = re.sub(r'//[^\\n]*', '', text)
    text = re.sub(r'/\\*.*?\\*/', '', text, flags=re.S)
    root = {}
    stack = [root]
    pending = None
    i = 0
    toks = re.finditer(r'\\[([^\\]]*)\\]|\\{|\\}|([^;{}\\[\\]=]+)=([^;]*);', text)
    for m in toks:
        s = m.group(0)
        if m.group(1) is not None:
            pending = m.group(1).strip().lower()
        elif s == '{':
            d = {}
            name = pending or ''
            key = name
            n = 1
            while key in stack[-1]:
                n += 1
                key = '%s#%d' % (name, n)
            stack[-1][key] = d
            stack.append(d)
            pending = None
        elif s == '}':
            if len(stack) > 1:
                stack.pop()
        else:
            stack[-1][m.group(2).strip().lower()] = m.group(3).strip()
    return root


def read_tdf(p):
    with open(p, 'rb') as f:
        return parse_tdf(f.read().decode('latin-1'))


def num(d, key, default=0.0):
    try:
        return float(d.get(key, default))
    except ValueError:
        m = re.match(r'\\s*-?[0-9.]+', str(d.get(key, '')))
        return float(m.group(0)) if m else default


# ---------------------------------------------------------------- palette

@functools.lru_cache(maxsize=None)
def palette():
    """The game's 256 colours as (r, g, b); read once."""
    raw = open(path('palettes/palette.pal'), 'rb').read()
    return [(raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2]) for i in range(256)]


# ---------------------------------------------------------------- GAF

def _gaf_frame(d, p):
    w, h, x, y, trans, comp, sub, _, ptr, _ = struct.unpack_from('<HHhhBBHIII', d, p)
    if sub:
        # layered frame: composite the layers onto the first one's grid
        layers = [_gaf_frame(d, struct.unpack_from('<I', d, ptr + 4 * i)[0]) for i in range(sub)]
        pix = [trans] * (w * h)
        for lw, lh, lx, ly, lp, lt in layers:
            ox, oy = x - lx, y - ly
            for r in range(lh):
                for c in range(lw):
                    v = lp[r * lw + c]
                    if v != lt:
                        tx, ty = c + ox, r + oy
                        if 0 <= tx < w and 0 <= ty < h:
                            pix[ty * w + tx] = v
        return w, h, x, y, pix, trans
    if not comp:
        return w, h, x, y, list(d[ptr:ptr + w * h]), trans
    pix = []
    q = ptr
    for r in range(h):
        n = struct.unpack_from('<H', d, q)[0]
        q += 2
        row = []
        end = q + n
        while q < end and len(row) < w:
            m = d[q]; q += 1
            if m & 1:
                row += [trans] * (m >> 1)
            elif m & 2:
                row += [d[q]] * ((m >> 2) + 1); q += 1
            else:
                k = (m >> 2) + 1
                row += list(d[q:q + k]); q += k
        q = end
        row = (row + [trans] * w)[:w]
        pix += row
    return w, h, x, y, pix, trans


def read_gaf(p):
    """Returns {entry name lower: [frames]}; a frame is (w, h, x, y, pixels, transparent)."""
    d = open(p, 'rb').read()
    ver, n, _ = struct.unpack_from('<III', d, 0)
    out = {}
    for i in range(n):
        e = struct.unpack_from('<I', d, 12 + 4 * i)[0]
        nf, _, _ = struct.unpack_from('<HHI', d, e)
        name = d[e + 8:e + 40].split(b'\\0')[0].decode('latin-1').lower()
        frames = []
        for f in range(nf):
            fp = struct.unpack_from('<I', d, e + 40 + 8 * f)[0]
            frames.append(_gaf_frame(d, fp))
        out[name] = frames
    return out


# ---------------------------------------------------------------- 3DO

class Obj3do:
    pass


def read_3do(p):
    d = open(p, 'rb').read()

    def obj(o):
        (sig, nv, np_, sel, x, y, z, noff, _, voff, poff, sib, child) = struct.unpack_from('<13i', d, o)
        ob = Obj3do()
        ob.name = d[noff:d.index(b'\\0', noff)].decode('latin-1').lower()
        ob.off = (x / 65536.0, y / 65536.0, z / 65536.0)
        ob.verts = [tuple(v / 65536.0 for v in struct.unpack_from('<3i', d, voff + 12 * i)) for i in range(nv)]
        ob.prims = []
        for i in range(np_):
            col, nidx, _, ioff, toff, _, _, _ = struct.unpack_from('<8i', d, poff + 32 * i)
            idx = list(struct.unpack_from('<%dh' % nidx, d, ioff)) if nidx else []
            tex = d[toff:d.index(b'\\0', toff)].decode('latin-1').lower() if toff else ''
            ob.prims.append((col, idx, tex, i == sel))
        ob.children = []
        c = child
        while c:
            ch = obj(c)
            ob.children.append(ch)
            c = 0
        if child:
            # siblings hang off each child
            cur = ob.children[0]
            s = struct.unpack_from('<13i', d, child)[11]
            while s:
                nx = obj(s)
                ob.children.append(nx)
                s = struct.unpack_from('<13i', d, s)[11]
        return ob
    return obj(0)


# ---------------------------------------------------------------- TNT

class Tnt:
    pass


def read_tnt(p):
    d = open(p, 'rb').read()
    h = struct.unpack_from('<16I', d, 0)
    t = Tnt()
    t.w, t.h = h[1], h[2]          # in 16-pixel cells
    tw, th = t.w // 2, t.h // 2
    t.tilemap = list(struct.unpack_from('<%dH' % (tw * th), d, h[3]))
    attr = d[h[4]:h[4] + 4 * t.w * t.h]
    t.height = [attr[i * 4] for i in range(t.w * t.h)]
    t.feature = [struct.unpack_from('<H', attr, i * 4 + 1)[0] for i in range(t.w * t.h)]
    t.ntiles = h[6]
    t.tiles = [d[h[5] + 1024 * i:h[5] + 1024 * (i + 1)] for i in range(t.ntiles)]
    t.features = [d[h[8] + i * 132 + 4:h[8] + i * 132 + 132].split(b'\\0')[0].decode('latin-1')
                  for i in range(h[7])]
    t.sealevel = h[9]
    mm = h[10]
    mw, mh = struct.unpack_from('<II', d, mm)
    t.minimap = (mw, mh, d[mm + 8:mm + 8 + mw * mh])
    return t
`;const c=`https://cdn.jsdelivr.net/pyodide/v314.0.7/full/`,l=[`pe_resource.py`,`hpi_extract.py`,`convert.py`,`ta.py`,`quant.py`,`render3do.py`];async function u(e,t,n,r,i){let a=e.FS;a.mkdirTree(`/tools`);for(let e of l)a.writeFile(`/tools/${e}`,t[e]);a.mkdirTree(`/work`),a.writeFile(`/work/installer.exe`,n),e.setStdout({batched:i}),e.setStderr({batched:i});let o=(t,...n)=>{e.globals.set(`argv`,e.toPy([`/tools/${t}`,...n])),e.runPython(`import runpy, sys
sys.argv = list(argv)
runpy.run_path(sys.argv[0], run_name='__main__')`)};return r(`unpacking`),o(`pe_resource.py`,`/work/installer.exe`,`ADD`,`130`,`1033`,`/work/TADemo.hpi`),a.unlink(`/work/installer.exe`),o(`hpi_extract.py`,`/work/TADemo.hpi`,`/work/data`),a.unlink(`/work/TADemo.hpi`),r(`converting`),o(`convert.py`,`/work/data`,`/work/out`,`/work/previews`),a.readdir(`/work/out`).filter(e=>e!==`.`&&e!==`..`).sort().map(e=>({name:e,bytes:a.readFile(`/work/out/${e}`)}))}const d=Object.fromEntries(Object.entries(/* @__PURE__ */ Object.assign({"../../tools/convert.py":e,"../../tools/hpi_extract.py":t,"../../tools/pe_resource.py":n,"../../tools/profsum.py":r,"../../tools/quant.py":i,"../../tools/raw2png.py":a,"../../tools/render3do.py":o,"../../tools/ta.py":s})).map(([e,t])=>[e.slice(e.lastIndexOf(`/`)+1),t])),f=(e,t=[])=>self.postMessage(e,t);self.onmessage=async e=>{try{for(let e of l)if(!(e in d))throw Error(`${e} is missing from this page`);f({type:`stage`,stage:`loading`});let{loadPyodide:t}=await import(
/* @vite-ignore */
`${c}pyodide.mjs`),n=await t({indexURL:c});await n.loadPackage([`numpy`,`pillow`],{messageCallback:()=>{}});let r=await u(n,d,new Uint8Array(e.data.installer),e=>f({type:`stage`,stage:e}),e=>f({type:`log`,line:e}));f({type:`done`,files:r},r.map(e=>e.bytes.buffer))}catch(e){f({type:`error`,message:e.message})}};