# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""Converts the Total Annihilation demo's game data for the Atari TT demake.

usage: convert.py DEMO_DATA_DIR OUT_DIR

DEMO_DATA_DIR is the demo's TADemo.hpi unpacked by hpi_extract.py. OUT_DIR
receives the files the TT program reads, all big-endian:

  UNITS.DAT   weapon and unit tables, and every unit's sprites
  COMMON.DAT  the battle palette, the panel picture, features, effects,
              cursors, unit pictures, buttons and the font
  MISS1.DAT.. each mission's map, metal, objectives and placed units
  SOUNDS.DAT  sound effects, signed 8-bit at 12517 Hz
  BRIEF1.SND  each mission's spoken briefing, signed 8-bit at 6258 Hz
  BRIEF1.TXT..  each mission's briefing text
  TITLE.IMG, SINGLE.IMG, BRIEF.IMG, GLAM1.IMG..  full-screen pictures with
              their own palettes; GLAMn.IMG shows while mission n loads

Pictures of the converted art for checking go to previews/ beside OUT_DIR.

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
    return b + b'\0' * (n - len(b))


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


def read_wav(p, rate):
    """Signed 8-bit mono at rate, resampled linearly."""
    w = wave.open(p)
    n, sw, ch, fr = w.getnframes(), w.getsampwidth(), w.getnchannels(), w.getframerate()
    raw = w.readframes(n)
    if sw == 1:
        a = np.frombuffer(raw, np.uint8).astype(np.float32) - 128
    else:
        a = np.frombuffer(raw, '<i2').astype(np.float32) / 256
    a = a.reshape(-1, ch).mean(1)
    m = int(len(a) * rate / fr)
    a = np.interp(np.arange(m) * fr / rate, np.arange(len(a)), a)
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
        body += b'\0'
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
    for root, _, files in os.walk(ta.path('features')):
        for f in files:
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
    glyphs = bytearray()
    gofs = []
    for c in range(256):
        w, h, x, y, pix, tr = seq[c] if c < len(seq) else seq[0]
        px = np.array(pix, np.uint8).reshape(h, w)
        body = (px != tr) & (px != 93)          # 93 is the glyphs' dark outline
        line = px == 93
        w2 = min(w, 16)
        # each row: the letter's word, then its outline's
        rows = np.stack([body[:, :w2] @ BITS[:w2], line[:, :w2] @ BITS[:w2]], axis=1)
        gofs.append(len(glyphs))
        glyphs += be('bbbb', w, h, x, y - base) + rows.astype('>u2').tobytes()
    font_ofs = bank.add(be('HH', base + 2, 0) + b''.join(be('H', o) for o in gofs) + bytes(glyphs))

    # -- victory, defeat and paused titles
    ig = gaf('igtitles')
    trecs = []
    for t in ('igvictory', 'igdefeat', 'igpaused'):
        img, ox, oy = gaf_rgba(ig[t][0], pal)
        trecs.append(be('I', bank.add(rgba_sprite(img, 0, 0, mapper))))

    head = (b'OAC1' + b''.join(be('H', tt_word(c)) for c in game_pal) +
            be('IHHHHHHI', panel_ofs, len(fnames), len(arecs), len(crecs), len(prec), len(brecs), len(trecs), font_ofs))
    body = head + b''.join(frecs) + b''.join(arecs) + b''.join(crecs) + b''.join(prec) + b''.join(brecs) + b''.join(trecs)
    while len(body) & 3:
        body += b'\0'
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
        body += b'\0'
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
            b += b'\0'
        offs.append((len(data), len(b), code))
        data += b
    head = b'OAS2' + be('H', len(offs)) + b''.join(be('IIHH', o, l, c, 0) for o, l, c in offs)
    open(os.path.join(out, 'SOUNDS.DAT'), 'wb').write(head + bytes(data))
    print('SOUNDS.DAT', len(head) + len(data), 'bytes,', len(offs), 'sounds')


MAX_SOUND = 1.6
SOUNDS = Sounds()


PREVIEWS = None     # where pictures of the converted art go, beside OUT_DIR


def main():
    global PREVIEWS
    ta.ROOT = sys.argv[1]
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    PREVIEWS = os.path.join(os.path.dirname(os.path.abspath(out)), 'previews')
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
            txt = txt[txt.index(b'\n'):]
        if txt.endswith(b'END'):
            txt = txt[:-3]
        open(os.path.join(out, 'BRIEF%d.TXT' % (i + 1)), 'wb').write(txt)
        s = read_wav(ta.path('camps/briefs/arm%02d.wav' % (i + 1)), 6258)
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
