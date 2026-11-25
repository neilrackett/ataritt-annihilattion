# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""Converts Total Annihilation's game data, the demo's or the full game's, for
the Atari TT.

usage: convert.py DATA_DIR OUT_DIR [PREVIEW_DIR]

DATA_DIR is the demo's TADemo.hpi unpacked by hpi_extract.py, or the full
game's data unpacked from its CDs by cd_extract.py, whose music/ and video/
folders beside it are converted too. The missions are those the campaigns
in camps/ list and whose maps are there. Each world (an OTA's planet) has a
palette of its own. OUT_DIR receives the files the TT program reads, all
big-endian:

  GAME.DAT      the campaigns, their missions and the music
  UNITS.DAT     the weapon and unit tables, and the units' wrecks
  COMMON.DAT    the font and the fog's edges
  SOUNDS.DAT    sound effects, signed 8-bit at 12517 or 6258 Hz
  WORLDn.DAT    world n's palette, effects, cursors and titles
  WORLDn.SPR    every unit's sprites, wreck and build picture in world n's
                colours, which the game reads as it needs them
  PANELnA.DAT, PANELnC.DAT  the panels round the battlefield and the order
                buttons in world n's colours, Arm's and Core's
  AC01.DAT ..   each mission's map, features, objectives, scripts and units
  AC01.TXT, AC01.SND, AC01.IMG  its briefing's text, narration (signed
                8-bit at 12517 Hz) and loading picture
  TITLE.IMG, SINGLE.IMG, BRIEFARM.IMG, BRIEFCOR.IMG  full-screen pictures
                with their own palettes
  MUSIC02.SND ..  the CD's music, signed 8-bit at 12517 Hz (the full game)
  FILM1.DAT, FILM1.SND ..  the films, for TT low resolution, and their
                sound (the full game, with ffmpeg to decode them): 1 the
                Cavedog logo, 2 the introduction, 3 Arm's victory, 4
                Core's, 5 the credits

Pictures of the converted art for checking go to PREVIEW_DIR, or previews/
beside OUT_DIR.

For trying things out, OA_ONLY=ac05,cc12 converts only those missions and
their worlds, and leaves the music and the films out.

The layouts are mirrored in ../src/data.h; change both together.
"""
import math, os, shutil, struct, subprocess, sys, wave
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ta, quant, render3do

TICKS = 15                  # simulation ticks a second on the TT
TA_FRAMES = 30              # the original's ticks a second
DIRS = 16                   # directions a mobile unit is drawn in
MAX_UNITTYPES = 320         # data.h's limit, and the bits of a mission's allowed list
NO_TYPE = 0xFFFF            # data.h's: no unit type
MAX_BUILD = 32              # build options a unit's record holds
MAX_WEAPONS = 255           # a shot's weapon is a byte
ENGINE_ANIMS = ['explode5']  # effects the engine shows by number (data.h, ANIM_*)
# The memory a mission's map may take on the TT: its tiles, its cells, and
# what the engine keeps for each cell (map_memory())
MISSION_BUDGET = 1900 * 1024
# and what its whole file and the engine's arrays may: as much as the full
# game's largest (a skirmish on Pincushion) takes, so that the expansions'
# missions leave the sprites as much room
MISSION_TOTAL = 2240 * 1024
SKIRMISH_TILES = 1000       # the fewest tiles a skirmish map may be cut to
MIN_TILES = 300             # and a campaign mission's
FEAT_NONE, FEAT_WRECK = 0xFFFFFFFF, 0xFFFFFFFE      # data.h's: a feature with no sprite, a unit's wreck
MAX_TILES = 2600

# ------------------------------------------------------------------ helpers


def be(fmt, *v):
    return struct.pack('>' + fmt, *v)


def cstr(s, n):
    b = s.encode('latin-1', 'replace')[:n - 1]
    return b + b'\0' * (n - len(b))


def pad4(b):
    b = bytearray(b)
    while len(b) & 3:
        b.append(0)
    return bytes(b)


class Bank:
    """A blob that sprites and other records are appended to; offsets are 4-aligned."""

    def __init__(self, start=0):
        self.data = bytearray()
        self.start = start

    def add(self, b):
        while len(self.data) & 3:
            self.data.append(0)
        o = len(self.data)
        self.data += b
        return o + self.start


BITS = 1 << (15 - np.arange(16))     # a 16-pixel group's pixels as a word's bits


def plane_words(idx, opaque=None, planes=4):
    """Palette indices (h, w; w a multiple of 16) as words (h, w / 16, n): for
    each 16-pixel group, its planes, after its mask when opaque is given."""
    h, w = idx.shape
    g = w // 16
    ix = idx.reshape(h, g, 16).astype(np.int32)
    words = [((ix >> p) & 1) @ BITS for p in range(planes)]   # (h, g) each
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


def gaf_rgba(frame, pal):
    w, h, x, y, pix, tr = frame
    a = np.array(pix, np.uint8).reshape(h, w)
    rgb = np.array(pal, np.uint8)[a]
    alpha = (a != tr).astype(np.uint8) * 255
    return np.dstack([rgb, alpha]), x, y


def rgba_sprite(img, ox, oy, mapper):
    opaque = img[..., 3] > 110
    idx = mapper.dither(img[..., :3].astype(np.float32), 0, 0) if opaque.any() else np.zeros(opaque.shape, np.uint8)
    return sprite(idx, opaque, ox, oy)


# ------------------------------------------------------------------ the game: campaigns and missions


class Mission:
    pass


class Campaign:
    def __init__(self, name, side, game, missions):
        self.name, self.side, self.game, self.missions = name, side, game, missions


# The expansions, as cd_extract.py unpacks them beside the data: their
# folders, each over the last, which also start their missions' files'
# names, and their games' names; their games' numbers (data.h's GAME_*) are
# their places here, after the original's 0
EXPANSIONS = [('cc', 'core contingency'), ('bt', 'battle tactics')]
LENGTHS = ['very short', 'short', 'medium', 'long']     # Battle Tactics' campaigns, in turn


def find_missions():
    """The campaigns in camps/, each game's in turn (the original's, then
    the expansions'), Arm's first, and each one's missions whose maps are
    there, then the skirmishes: [Campaign]."""
    camps = []
    for f in ta.listdir('camps'):
        if not f.lower().endswith('.tdf'):
            continue
        c = ta.read_tdf(ta.path('camps', f))
        side = c.get('header', {}).get('campaignside', 'ARM').upper()
        sd = 0 if side == 'ARM' else 1
        folder = os.path.basename(ta.lowest('camps', f))
        game = next((i + 1 for i, (e, _) in enumerate(EXPANSIONS) if e == folder), 0)
        ms = []
        for k in sorted((k for k in c if k.startswith('mission')), key=lambda k: int(k[7:])):
            base = c[k].get('missionfile', '').lower().replace('.ota', '')
            if not ta.path('maps/%s.ota' % base) or not ta.path('maps/%s.tnt' % base):
                continue
            if not fits(base, MIN_TILES):
                print('  mission left out, its map too large for the TT:', base)
                continue
            m = Mission()
            m.base = base
            m.map = base
            m.side = sd
            m.ota = ta.read_tdf(ta.path('maps/%s.ota' % base))['globalheader']
            m.name = c[k].get('missionname') or m.ota.get('missionname', base)
            m.planet = m.ota.get('planet', '').strip().lower() or 'green planet'
            ms.append(m)
        if not ms:
            continue
        # the original's are named by side; the expansions' by their files,
        # less the game's name: KROGOTH ENCOUNTER, ARM VERY SHORT
        own = EXPANSIONS[game - 1][1] if game else None
        stem = ' - '.join(w for w in (w.strip() for w in f[:-4].lower().split(' - ')) if w != own)
        name = '%s CAMPAIGN' % side if game == 0 else stem.upper()
        length = next((i for i, w in enumerate(LENGTHS) if stem.endswith(w)), 0)
        bonus = game and not stem.startswith(('arm', 'core'))
        camps.append(((game, 2 if bonus else sd, length), Campaign(name, sd, game, ms)))
    camps = [c for _, c in sorted(camps, key=lambda e: e[0])]
    # the files' names: the original's maps' (AC01), the expansions' numbered
    # (CC001, BT001), as their maps' names are long or could be any file's
    files = [0] * (len(EXPANSIONS) + 1)
    for c in camps:
        for m in c.missions:
            files[c.game] += 1
            m.file = '%s%03d' % (EXPANSIONS[c.game - 1][0], files[c.game]) if c.game else m.base
    sk = find_skirmish(set(m.map for c in camps for m in c.missions))
    if sk:
        camps.append(Campaign('SKIRMISH', 2, 0, sk))
    return camps


def map_memory(w, h, nclasses):
    """What a map of w x h cells takes on the TT beside its tiles: its
    cells' heights, flags and metal, its tile map, and the engine's arrays
    for it (sim.c's map_alloc(): 6 bytes a 32-pixel path cell, its rows a
    power of two long, and one a move class; game.c's ex_alloc(): about 2
    more for the explored map). Returns (bytes, path cells)."""
    stride = 1
    while stride < (w + 1) // 2:
        stride *= 2
    pcells = stride * ((h + 1) // 2)
    return w * h * 3 + pcells * (8 + nclasses) + (w // 2) * (h // 2) * 2, pcells


def tile_room(mem):
    """The tiles (512 bytes each) that MISSION_BUDGET leaves beside a map
    that takes mem bytes (map_memory())."""
    return (MISSION_BUDGET - mem) // 512


def fits(stem, tiles):
    """Whether a map fits the TT: room beside it for so many tiles, and path
    cells the engine's 16-bit indices reach."""
    mem, pcells = map_memory(*ta.tnt_size(ta.path('maps/%s.tnt' % stem)), 6)
    return tile_room(mem) >= tiles and pcells <= 65535


def find_skirmish(campaign_maps):
    """The multiplayer maps that fit the TT, for skirmishes: an Arm Commander
    at the map's first start and a Core one at its second, 1000 metal and
    energy each, and the enemy's Commander to kill. [Mission]."""
    out = []
    for f in ta.listdir('maps'):
        stem = f[:-4].lower()
        if not f.lower().endswith('.ota') or stem in campaign_maps or not ta.path('maps/%s.tnt' % stem):
            continue
        gh = ta.read_tdf(ta.path('maps', f)).get('globalheader', {})
        s0 = gh.get('schema 0', {})
        starts = {}
        for sp in s0.get('specials', {}).values():
            what = sp.get('specialwhat', '').lower()
            if what.startswith('startpos'):
                starts[what] = (int(ta.num(sp, 'xpos')), int(ta.num(sp, 'zpos')))
        if 'startpos1' not in starts or 'startpos2' not in starts:
            continue
        if not fits(stem, SKIRMISH_TILES):
            print('  skirmish map left out, too large for the TT:', stem)
            continue
        m = Mission()
        m.base = m.file = 'sk%02d' % (len(out) + 1)
        m.map = stem
        m.side = 0
        m.name = gh.get('missionname', stem)
        m.planet = gh.get('planet', '').strip().lower() or 'green planet'
        ota = {k: v for k, v in gh.items() if k not in COND}
        (x1, z1), (x2, z2) = starts['startpos1'], starts['startpos2']
        ota.update({'schemacount': '1', 'killenemycommander': '1', 'commanderkilled': '1',
                    'schema 0': {'surfacemetal': s0.get('surfacemetal', '3'), 'humanmetal': '1000',
                                 'humanenergy': '1000', 'computermetal': '1000', 'computerenergy': '1000',
                                 'units': {'unit0': {'unitname': 'ARMCOM', 'player': '1', 'xpos': str(x1),
                                                     'zpos': str(z1)},
                                           'unit1': {'unitname': 'CORCOM', 'player': '2', 'xpos': str(x2),
                                                     'zpos': str(z2)}}}})
        m.ota = ota
        m.brief = ('A skirmish on %s: one Commander against another, from nothing.\n\n%s\n\n'
                   '&GDestroy the enemy Commander&. Lose yours and the battle is lost.'
                   % (m.name, gh.get('missiondescription', '')))
        out.append(m)
    return out


# ------------------------------------------------------------------ palette

def gather_palette(maps):
    pal = np.array(ta.palette(), np.uint8)
    samples = []
    for t in maps:
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
    for f in ta.listdir('units'):
        if f.endswith('.fbi'):
            d = ta.read_tdf(ta.path('units', f))['unitinfo']
            units[d['unitname'].lower()] = d
    weapons = {}
    for f in ta.listdir('weapons'):
        for k, v in ta.read_tdf(ta.path('weapons', f)).items():
            weapons[k] = v
    return units, weapons


DOWNLOADS = None


def downloads():
    """{builder: [unit]}: what download/*.tdf adds to builders' menus, as the
    expansions add their units to the game's builders, by menu and button."""
    global DOWNLOADS
    if DOWNLOADS is None:
        entries = []
        for f in ta.listdir('download'):
            for v in ta.read_tdf(ta.path('download', f)).values():
                if isinstance(v, dict) and v.get('unitmenu') and v.get('unitname'):
                    entries.append((v['unitmenu'].lower(), int(ta.num(v, 'menu')), int(ta.num(v, 'button')),
                                    v['unitname'].lower()))
        DOWNLOADS = {}
        for b, _, _, n in sorted(entries):
            DOWNLOADS.setdefault(b, []).append(n)
    return DOWNLOADS


def build_options(name, units):
    """The units on a builder's menu pages (guis/<name>1.gui, <name>2.gui...),
    then those download/ adds."""
    names = []
    for page in range(1, 6):
        p = ta.path('guis/%s%d.gui' % (name, page))
        if not p:
            break
        for v in ta.read_tdf(p).values():
            names += [vv.get('name', '').lower() for vv in v.values() if isinstance(vv, dict)]
    out = []
    for nm in names + downloads().get(name, []):
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


def resample(a, fr, rate, scale):
    """Integer samples a (int64, mono) at fr as signed 8-bit at rate: filtered
    first when rate is lower than fr, so that what rate can't hold doesn't
    fold back as hiss, then resampled linearly; scale brings a to 8 bits."""
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


def wav_samples(p):
    """A WAV file's samples as 16-bit-scaled integers, its channels summed:
    (samples, rate, channels). Integers until after the filter, so that
    every platform sums alike."""
    w = wave.open(p)
    n, sw, ch, fr = w.getnframes(), w.getsampwidth(), w.getnchannels(), w.getframerate()
    raw = w.readframes(n)
    if sw == 1:
        a = (np.frombuffer(raw, np.uint8).astype(np.int64) - 128) * 256
    else:
        a = np.frombuffer(raw, '<i2').astype(np.int64)
    return a.reshape(-1, ch).sum(1), fr, ch


def read_wav(p, rate):
    """Signed 8-bit mono at rate (see resample())."""
    a, fr, ch = wav_samples(p)
    return resample(a, fr, rate, 256 * ch)


WEAPON_KIND = {'beam': 0, 'cannon': 1, 'rocket': 2, 'shot': 3, 'dgun': 4, 'bomb': 5, 'torpedo': 6}
# WeaponType.flags and flags2 (data.h's WF_*, WF2_*)
WF_PARALYZE, WF_MANUAL, WF_BALLISTIC, WF_TRACKS, WF_AIR, WF_WATER, WF_STOCKPILE, WF_INTERCEPT = \
    1, 2, 4, 8, 16, 32, 64, 128
WF2_NUKE, WF2_FLAME = 1, 2
ANIMS = []          # explosion animation names, in the worlds' order


def anim_ref(name):
    name = (name or '').lower()
    if not name:
        return -1
    if name not in ANIMS:
        ANIMS.append(name)
    return ANIMS.index(name)


def weapon_kind(w):
    rt = int(ta.num(w, 'rendertype'))
    if w.get('name', '').lower().startswith('disintegrator') or 'disintegrator' in w.get('_key', ''):
        return 'dgun'
    if int(ta.num(w, 'dropped')):
        return 'bomb'
    if int(ta.num(w, 'waterweapon')):
        return 'torpedo'
    if int(ta.num(w, 'paralyzer')) or int(ta.num(w, 'beamweapon')) or (rt == 0 and int(ta.num(w, 'lineofsight'))):
        return 'beam'
    if int(ta.num(w, 'ballistic')):
        return 'cannon'
    if rt == 1 or w.get('model'):
        return 'rocket'
    return 'shot'


def weapon_record(w, sounds, units):
    """A weapon: kind is how it reaches its target, the flags what else it does
    (a mapper's colour is filled in per world: the beams' colours are kept as
    the game palette's index, 0-255)."""
    kind = weapon_kind(w)
    paralyzer = int(ta.num(w, 'paralyzer'))
    col = 255 if paralyzer else int(ta.num(w, 'color', 208)) & 255
    dmg = w.get('damage', {})
    damage = int(ta.num(dmg, 'default', 10)) if isinstance(dmg, dict) else 10
    # what it does to aircraft, where the table says
    air = [int(ta.num(dmg, k)) for k in (dmg if isinstance(dmg, dict) else {})
           if k in units and int(ta.num(units[k], 'canfly'))]
    damage_air = max(air) if air else damage
    flags = 0
    if paralyzer:
        flags |= WF_PARALYZE
    if int(ta.num(w, 'commandfire')):
        flags |= WF_MANUAL
    if int(ta.num(w, 'ballistic')):
        flags |= WF_BALLISTIC
    if int(ta.num(w, 'tracks')):
        flags |= WF_TRACKS
    # what can reach an aircraft: shots that go in a straight line or steer
    if kind in ('beam', 'rocket', 'shot') and not int(ta.num(w, 'ballistic')) and \
            (int(ta.num(w, 'lineofsight')) or int(ta.num(w, 'tracks'))):
        flags |= WF_AIR
    if kind == 'torpedo':
        flags |= WF_WATER
    if int(ta.num(w, 'stockpile')):
        flags |= WF_STOCKPILE
    if int(ta.num(w, 'interceptor')):
        flags |= WF_INTERCEPT
    flags2 = 0
    if int(ta.num(w, 'targetable')):
        flags2 |= WF2_NUKE
    if int(ta.num(w, 'rendertype')) == 5:
        flags2 |= WF2_FLAME
    reload = max(1, int(round(ta.num(w, 'reloadtime', 1) * TICKS)))
    speed = int(round(ta.num(w, 'weaponvelocity', 300) / TICKS * 256))
    burst = max(1, int(ta.num(w, 'burst', 1)))
    gap = max(1, int(round(ta.num(w, 'burstrate', 0.1) * TICKS)))
    rng = int(ta.num(w, 'range', 200))
    if kind == 'bomb':
        rng = 48                # a bomb falls where it is let go
    return be('HHHHHHBBBBhhhBBHH',
              min(rng, 65535), min(reload, 65535), min(damage, 65535), min(damage_air, 65535),
              min(speed, 65535), int(ta.num(w, 'areaofeffect', 16)),
              WEAPON_KIND[kind], col, min(burst, 255), min(gap, 255),
              sounds.ref(w.get('soundstart')), sounds.ref(w.get('soundhit')),
              anim_ref(w.get('explosionart')), flags, flags2,
              min(65535, int(ta.num(w, 'energypershot'))), min(65535, int(ta.num(w, 'metalpershot')))) + \
        cstr(w.get('_key', ''), 12)


MOVEINFO = None


def move_class(u):
    """(max slope, max water depth, min water depth) of a unit's movement
    class, one of six, as a map cell has six bits for them: tanks, KBots
    (which climb more), amphibious units, spiders (which climb anything),
    and ships for shallow and for deep water. Aircraft have none (None)."""
    global MOVEINFO
    if int(ta.num(u, 'canfly')):
        return None
    if MOVEINFO is None:
        MOVEINFO = {v.get('name', '').lower(): v for v in ta.read_tdf(ta.path('gamedata/moveinfo.tdf')).values()}
    c = MOVEINFO.get(u.get('movementclass', '').lower(), u)
    slope, maxd, mind = ta.num(c, 'maxslope', 255), ta.num(c, 'maxwaterdepth', 255), ta.num(c, 'minwaterdepth', 0)
    if mind > 0:
        return (255, 255, 15 if mind >= 10 else 3)
    if maxd >= 100:
        return (32, 100, 0)
    if slope >= 200:
        return (255, 16, 0)
    return (32, 12, 0) if slope >= 25 else (15, 12, 0)


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


# UnitType.flags (data.h's UF_*)
UF = dict(BUILDER=1, COMMANDER=2, ARMED=4, WIND=8, PATROL=16, MEX=32, FLY=64, SHIP=128, SUB=256,
          CAPTURE=512, TIDAL=1024, MAKER=2048, GEO=4096, KAMIKAZE=8192, TRANSPORT=16384,
          AIRPREF=32768, NOAIR=65536, BOMBER=1 << 17, ANTINUKE=1 << 18, STOCKPILE=1 << 19,
          PHIB=1 << 20, ONOFF=1 << 21, WATERBLD=1 << 22, RADAR=1 << 23, SONAR=1 << 24,
          JAMMER=1 << 25, REPAIR=1 << 26, ACTIVE=1 << 27, HOVER=1 << 28)


def unit_flags(u, opts, weapons):
    cat = u.get('category', '').upper().split()
    builder = int(ta.num(u, 'builder'))
    f = 0
    if builder and opts:
        f |= UF['BUILDER']
    if 'commander' in u.get('category', '').lower():
        f |= UF['COMMANDER']
    wl = [u.get(k, '').lower() for k in ('weapon1', 'weapon2', 'weapon3')]
    wl = [w for w in wl if w in weapons]
    if int(ta.num(u, 'canattack', 1)) and wl:
        f |= UF['ARMED']
    if int(ta.num(u, 'windgenerator')):
        f |= UF['WIND']
    if int(ta.num(u, 'canpatrol', 0)):
        f |= UF['PATROL']
    if float(ta.num(u, 'extractsmetal')) > 0:
        f |= UF['MEX']
    if int(ta.num(u, 'canfly')):
        f |= UF['FLY']
    if 'SHIP' in cat or int(ta.num(u, 'floater')) or (int(ta.num(u, 'bmcode')) == 0 and int(ta.num(u, 'minwaterdepth')) > 0):
        f |= UF['SHIP']
    if 'UNDERWATER' in cat:
        f |= UF['SUB']
    if int(ta.num(u, 'cancapture')) or 'commander' in u.get('category', '').lower():
        f |= UF['CAPTURE']
    if int(ta.num(u, 'tidalgenerator')):
        f |= UF['TIDAL']
    if int(ta.num(u, 'makesmetal')):
        f |= UF['MAKER']
    if 'G' in u.get('yardmap', ''):
        f |= UF['GEO']
    if int(ta.num(u, 'kamikaze')):
        f |= UF['KAMIKAZE']
    if int(ta.num(u, 'transportcapacity')) or int(ta.num(u, 'canload')) or 'TPORT' in cat:
        f |= UF['TRANSPORT']
    bad = u.get('badtargetcategory', '').upper()
    if bad == 'NOTAIR' or u.get('wpri_badtargetcategory', '').upper() == 'NOTAIR':
        f |= UF['AIRPREF']
    if 'VTOL' in bad:
        f |= UF['NOAIR']
    if any(int(ta.num(weapons[w], 'dropped')) for w in wl):
        f |= UF['BOMBER']
    if int(ta.num(u, 'antiweapons')) or any(int(ta.num(weapons[w], 'interceptor')) for w in wl):
        f |= UF['ANTINUKE']
    if any(int(ta.num(weapons[w], 'stockpile')) for w in wl):
        f |= UF['STOCKPILE']
    if 'PHIB' in cat:
        f |= UF['PHIB']
    if int(ta.num(u, 'onoffable')):
        f |= UF['ONOFF']
    if int(ta.num(u, 'bmcode')) == 0 and int(ta.num(u, 'minwaterdepth')) > 0:
        f |= UF['WATERBLD']
    if int(ta.num(u, 'radardistance')) > 0:
        f |= UF['RADAR']
    if int(ta.num(u, 'sonardistance')) > 0:
        f |= UF['SONAR']
    if int(ta.num(u, 'radardistancejam')) > 0 or int(ta.num(u, 'sonardistancejam')) > 0:
        f |= UF['JAMMER']
    if builder and int(ta.num(u, 'bmcode')) == 1:
        f |= UF['REPAIR']
    if int(ta.num(u, 'activatewhenbuilt')):
        f |= UF['ACTIVE']
    if int(ta.num(u, 'canhover')):
        f |= UF['HOVER']
    return f


SOUND_TABLE = None


def sounds_for(u):
    global SOUND_TABLE
    if SOUND_TABLE is None:
        SOUND_TABLE = ta.read_tdf(ta.path('gamedata/sound.tdf'))
    cat = SOUND_TABLE.get(u.get('soundcategory', '').lower(), {})
    return SOUNDS.ref(cat.get('select1')), SOUNDS.ref(cat.get('ok1'))


FF_RECLAIM, FF_INDESTRUCT, FF_BAKED, FF_GEO = 1, 4, 8, 16      # data.h's FF_*


def baked(name, d):
    """Rocks and dragon's teeth are drawn into the map's tiles, not on their own."""
    return (d.get('category', '').lower() in ('rocks', 'dragonteeth') or 'rock' in name or
            name == 'dragonsteeth')


def feature_record(n, d, sprite_ofs):
    flags = ((FF_RECLAIM if int(ta.num(d, 'reclaimable', 0)) else 0) |
             (FF_INDESTRUCT if int(ta.num(d, 'indestructible', 0)) else 0) |
             (FF_BAKED if baked(n, d) else 0) |
             (FF_GEO if int(ta.num(d, 'geothermal', 0)) else 0))
    return cstr(n, 16) + be('HIIIBBBBI', flags, int(ta.num(d, 'metal')), int(ta.num(d, 'energy')),
                            int(ta.num(d, 'damage', 100)), int(ta.num(d, 'footprintx', 1)),
                            int(ta.num(d, 'footprintz', 1)), int(ta.num(d, 'height', 0)) & 255, 0,
                            sprite_ofs)


class UnitArt:
    """A unit's art drawn once, as RGBA, for every world to map onto its colours."""
    pass


def render_unit(name, u, team, dirs, feat_defs):
    ob = ta.read_3do(ta.path('objects3d/%s.3do' % name))
    polys = render3do.collect(ob)
    art = UnitArt()
    art.frames = []
    for d in range(dirs):
        art.frames.append(render3do.render(polys, 2 * math.pi * d / dirs, team=team))
    # the unit's size on screen facing down it, for picking and selection brackets
    img, ox, oy = art.frames[0]
    ys, xs = np.nonzero(img[..., 3] > 128)
    art.radius = int(max(abs(xs - ox).max(), 4)) if len(xs) else 8
    art.top = int(max(oy - ys.min(), 4)) if len(ys) else 8
    # its wreck
    art.wreck = None
    c = u.get('corpse', '').lower()
    d = feat_defs.get(c)
    if d and d.get('object') and ta.path('objects3d/%s.3do' % d['object'].lower()):
        p = render3do.collect(ta.read_3do(ta.path('objects3d/%s.3do' % d['object'].lower())))
        art.wreck = render3do.render(p, 0.3, team=team)
    # its build picture
    p = ta.path('unitpics/%s.pcx' % u['unitname'].lower())
    art.pic = np.array(Image.open(p).convert('RGB').resize((64, 64), Image.LANCZOS), np.float32) if p else None
    return art


def write_units(out, names, units, weapons, sounds, feat_defs, arts):
    """UNITS.DAT: the tables, without sprites (WORLDn.SPR holds those)."""
    assert len(names) <= MAX_UNITTYPES, len(names)
    wnames = []
    for n in names:
        for k in ('weapon1', 'weapon2', 'weapon3', 'explodeas', 'selfdestructas'):
            w = units[n].get(k, '').lower()
            if w and w in weapons and w not in wnames:
                wnames.append(w)
    assert len(wnames) <= MAX_WEAPONS, len(wnames)
    for w in wnames:
        weapons[w]['_key'] = w
    classes = []
    for n in names:
        if int(ta.num(units[n], 'bmcode')):
            c = move_class(units[n])
            if c and c not in classes:
                classes.append(c)
    assert len(classes) <= 6, classes
    recs = []
    wrecks = []
    for i, n in enumerate(names):
        u = units[n]
        art = arts[n]
        side = 0 if u.get('side', '').upper() == 'ARM' else 1
        mobile = int(ta.num(u, 'bmcode')) == 1
        builder = int(ta.num(u, 'builder'))
        opts = build_options(n, units)
        opts = [o for o in opts if o in names]
        kind = 0 if mobile else (2 if builder and opts else 1)
        flags = unit_flags(u, opts, weapons)
        wl = [wnames.index(u[k].lower()) if u.get(k, '').lower() in wnames else -1
              for k in ('weapon1', 'weapon2', 'weapon3')]
        dirs = DIRS if mobile else 1
        mc = classes.index(move_class(u)) if mobile and move_class(u) else 255
        cat = sounds_for(u)
        explode = wnames.index(u['explodeas'].lower()) if u.get('explodeas', '').lower() in wnames else -1
        selfd = wnames.index(u['selfdestructas'].lower()) if u.get('selfdestructas', '').lower() in wnames else -1
        opts_idx = [names.index(o) for o in opts][:MAX_BUILD]
        c = u.get('corpse', '').lower()
        has_wreck = c in feat_defs and art.wreck is not None
        wrecks.append(feature_record(c, feat_defs[c], FEAT_WRECK) if has_wreck else feature_record('', {}, FEAT_NONE))
        rec = (cstr(n.upper(), 10) + cstr(u.get('name', n), 20) + cstr(u.get('description', ''), 32) +
               be('BBBBBB', side, kind, int(ta.num(u, 'footprintx', 1)), int(ta.num(u, 'footprintz', 1)),
                  mc, len(opts_idx)) +
               be('hhhh', wl[0], wl[1], wl[2], explode) +
               be('I', flags) +
               be('HHHHHHHHH',
                  min(65535, int(ta.num(u, 'maxdamage', 100))),
                  int(round(ta.num(u, 'maxvelocity') * TA_FRAMES / TICKS * 256)),
                  int(round(ta.num(u, 'acceleration', 0.1) * (TA_FRAMES / TICKS) ** 2 * 256)),
                  min(65535, int(ta.num(u, 'turnrate', 500) * TA_FRAMES / TICKS)),
                  int(ta.num(u, 'sightdistance', 200)),
                  int(ta.num(u, 'builddistance', 60) or 60),
                  int(ta.num(u, 'workertime')),
                  int(ta.num(u, 'cruisealt')),
                  min(65535, int(ta.num(u, 'radardistance')) or int(ta.num(u, 'sonardistance')))) +
               be('III', int(ta.num(u, 'buildcostmetal')), int(ta.num(u, 'buildcostenergy')),
                  int(ta.num(u, 'buildtime', 100))) +
               be('iiiiiii',
                  per_tick(ta.num(u, 'energymake')), per_tick(ta.num(u, 'metalmake')),
                  per_tick(ta.num(u, 'energyuse')),
                  int(round(ta.num(u, 'extractsmetal') * 1e6)),
                  int(ta.num(u, 'windgenerator')),
                  int(ta.num(u, 'metalstorage')), int(ta.num(u, 'energystorage'))) +
               be('BB', dirs, min(255, art.radius)) +
               be('hhhh', art.top, cat[0], cat[1], i if has_wreck else -1) +
               be('hBBBB', selfd, min(255, int(ta.num(u, 'minwaterdepth'))),
                  min(255, int(ta.num(u, 'maxwaterdepth', 255))), min(255, int(ta.num(u, 'maxslope', 255))), 0) +
               be('%dH' % MAX_BUILD, *(opts_idx + [NO_TYPE] * MAX_BUILD)[:MAX_BUILD]) + yard_bits(u) +
               be('H', 0))
        assert len(rec) == 228, len(rec)
        recs.append(rec)
    wrecs = [weapon_record(weapons[w], sounds, units) for w in wnames]
    body = (b'OAU4' + be('HHH', len(wrecs), len(recs), len(classes)) +
            b''.join(be('BBB', *c) for c in classes))
    if len(body) & 1:
        body += b'\0'
    body += b''.join(wrecs) + b''.join(recs) + b''.join(wrecks)
    open(os.path.join(out, 'UNITS.DAT'), 'wb').write(body)
    print('UNITS.DAT', len(body), 'bytes;', len(wrecs), 'weapons,', len(recs), 'units, classes', classes)
    return classes


def write_sprites(out, num, names, arts, mapper):
    """WORLDn.SPR: for each unit type its build picture, and its sprites with
    its wreck's, in the world's colours."""
    index = bytearray()
    data = Bank(8 + 12 * len(names))       # offsets from the file's start
    for n in names:
        art = arts[n]
        pic_ofs = data.add(image_planar(mapper.dither(art.pic))) if art.pic is not None else 0
        blob = Bank()
        frames = [rgba_sprite(img, ox, oy, mapper) for img, ox, oy in art.frames]
        table = bytearray(4 * (len(frames) + 1))
        blob.add(bytes(table))
        offs = [blob.add(f) for f in frames]
        wreck = blob.add(rgba_sprite(*art.wreck, mapper)) if art.wreck is not None else 0
        blob.data[0:4 * (len(frames) + 1)] = b''.join(be('I', o) for o in offs + [wreck])
        index += be('III', pic_ofs, data.add(bytes(blob.data)), len(blob.data))
    body = b'OAS3' + be('HH', len(names), 0) + bytes(index) + bytes(data.data)
    open(os.path.join(out, 'WORLD%d.SPR' % num), 'wb').write(body)
    print('WORLD%d.SPR' % num, len(body), 'bytes')


# ------------------------------------------------------------------ a world's art

def load_features():
    feats = {}
    # in name order, so that the result is the same whatever order the
    # filesystem lists them in (the web converter's is not the Mac's)
    for f in ta.walk('features'):
        if f.endswith('.tdf'):
            for k, v in ta.read_tdf(f).items():
                feats[k] = v
    return feats


BUTTONS = ['move', 'attack', 'stop', 'patrol', 'defend', 'blast', 'reclaim', 'repair', 'orders', 'build',
           'prev', 'next', 'fireord', 'moveord', 'capture', 'onoff']     # data.h's BTN_*


def write_panel(out, num, side, game_pal, mapper):
    """PANELnA.DAT or PANELnC.DAT: the panels round the battlefield, as TA lays
    them out at 640x480, and that side's order buttons."""
    pal = ta.palette()
    panel = np.zeros((480, 640, 3), np.uint8)
    pre = 'arm' if side == 0 else 'cor'
    gui = gaf('armint' if side == 0 else 'corint') or gaf('armint')
    common = gaf('commongui')
    for fr, x, y in ((gui['panelside'][0], 0, 0), (gui['paneltop'][0], 128, 0),
                     (gui['panelbot'][0], 128, 447), ((common.get(pre + 'pan') or common['armpan'])[0], 0, 128)):
        blit_rgba(panel, gaf_rgba(fr, pal)[0], x, y)
    pidx = quant.Mapper(game_pal, spread=20).dither(panel.astype(np.float32))
    Image.fromarray(np.array(game_pal, np.uint8)[pidx]).save(
        os.path.join(PREVIEWS, 'panel%d%s.png' % (num, 'ac'[side])))
    # -- order buttons (each with its frames: up, down, lit)
    bank = Bank()
    brecs = []
    for b in BUTTONS:
        frs = common.get(pre + b) or common.get('arm' + b, [])
        offs = []
        for fr in frs[:6]:
            img, ox, oy = gaf_rgba(fr, pal)
            offs.append(bank.add(rgba_sprite(img, 0, 0, mapper)))
        brecs.append(be('H', len(offs)) + b''.join(be('I', o) for o in (offs + [0] * 6)[:6]))
    body = pad4(image_planar(pidx) + be('H', len(BUTTONS)) + b''.join(brecs))
    body += be('I', len(bank.data)) + bytes(bank.data)
    open(os.path.join(out, 'PANEL%d%s.DAT' % (num, 'AC'[side])), 'wb').write(body)


def write_world(out, num, game_pal, mapper):
    """WORLDn.DAT: the palette, effects, cursors and titles, and the beams'
    colours."""
    pal = ta.palette()
    bank = Bank()
    # -- explosion and other effect animations
    fx = dict(gaf('fx'))
    fx.update(gaf('exp1'))
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

    # -- cursors: normal, select, move, attack, build, patrol, cant, reclaim, guard, repair, capture
    cur = gaf('cursors')
    cnames = ['cursornormal', 'cursorselect', 'cursormove', 'cursorattack', 'cursorfindsite',
              'cursorpatrol', 'cursortoofar', 'cursorreclamate', 'cursordefend', 'cursorrepair',
              'cursorcapture']
    crecs = []
    for c in cnames:
        fr = (cur.get(c) or cur['cursornormal'])[0]
        img, ox, oy = gaf_rgba(fr, pal)
        crecs.append(be('I', bank.add(rgba_sprite(img, ox, oy, mapper))))

    # -- victory, defeat and paused titles
    ig = gaf('igtitles')
    trecs = []
    for t in ('igvictory', 'igdefeat', 'igpaused'):
        img, ox, oy = gaf_rgba(ig[t][0], pal)
        trecs.append(be('I', bank.add(rgba_sprite(img, 0, 0, mapper))))

    # -- beams' colours: the game palette's 256 mapped onto this world's 16
    beams = mapper.nearest(np.array([pal], np.float32))[0]
    beams[255] = quant.WHITE        # the paralyzer's, whatever the colour there
    head = (b'OAW3' + b''.join(be('H', tt_word(c)) for c in game_pal) +
            be('HHH', len(arecs), len(crecs), len(trecs)) + bytes(beams.astype(np.uint8)))
    body = head + b''.join(arecs) + b''.join(crecs) + b''.join(trecs)
    body = pad4(body)
    data = body + be('I', len(bank.data)) + bytes(bank.data)
    open(os.path.join(out, 'WORLD%d.DAT' % num), 'wb').write(data)
    print('WORLD%d.DAT' % num, len(data), 'bytes')


def write_common(out):
    """COMMON.DAT: the font and the fog's edges, the same in every world."""
    pal = ta.palette()
    bank = Bank()
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
    data = b'OAC3' + be('II', font_ofs, fog_ofs) + be('I', len(bank.data)) + bytes(bank.data)
    open(os.path.join(out, 'COMMON.DAT'), 'wb').write(data)
    print('COMMON.DAT', len(data), 'bytes')


# ------------------------------------------------------------------ missions

# data.h's COND_*: what wins, and what loses
COND = {'moveunittoradius': 1, 'killunittype': 2, 'destroyallunits': 3, 'captureunittype': 4,
        'buildunittype': 5, 'killenemycommander': 6, 'killalloftype': 7, 'unittypepassesz': 8,
        'commanderkilled': 10, 'allunitskilled': 11, 'allunitskilledoftype': 12,
        'unittypekilled': 13, 'anyunitpassesz': 14, 'deathtimerrunsout': 15,
        # the expansions': every enemy mobile unit destroyed, and holding out for a time
        'killallmobileunits': 9, 'victorytimerrunsout': 16}


def compile_script(text, unit_names, idents, size):
    """InitialMission text -> ops: (op, a, b, c) as i16s.

    1 move x z, 2 patrol x z, 3 wait ticks, 4 wait-for-attack [ident], 5
    attack-type t, 6 orders move fire, 7 build t count, 8 guard ident, 9
    attack x z, 10 self-destruct. 's' (make selectable) and 'u' (unload)
    give none: every unit is selectable, and transports carry nothing.
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
                    who = tok[2:].split()
                    ops.append((4, idents.get(who[0], -1) if who else -1, 0, 0))
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
            elif c == 'd' and not args:
                ops.append((10, 0, 0, 0))
        except (ValueError, IndexError):
            print('  script token skipped:', tok)
    # places kept on the map (a full game's mission has a move to x 81200),
    # and every value to 16 bits
    w, h = size
    out = []
    for op in ops:
        a, b, c = op[1], op[2], op[3]
        if op[0] in (1, 2, 9):
            a, b = min(max(a, 0), w - 1), min(max(b, 0), h - 1)
        out.append((op[0],) + tuple(min(max(v, -32768), 32767) for v in (a, b, c)))
    return out


def merge_tiles(thumbs, uses, tilemap, tile_bytes, limit):
    """Fits a map into limit tiles by drawing tiles that look nearly alike
    with one of them: each tile's 8x8 colour thumbnail is compared, the most
    used tiles are kept first, and the threshold rises until the rest fit."""
    n = len(thumbs)
    order = np.argsort(-np.array(uses), kind='stable')
    rep_idx = np.zeros(limit + 1, np.int64)                     # the representatives so far
    rep_thumbs = np.zeros((limit + 1, thumbs.shape[1]), np.float32)
    if n <= 4000:
        # every pair of thumbnails' distance, worked out once for all the passes
        dist = np.empty((n, n), np.float32)
        for s in range(0, n, 16):
            dist[s:s + 16] = np.sqrt(((thumbs[s:s + 16, None, :] - thumbs[None, :, :]) ** 2).mean(2))

        def dist_to(i, nreps):
            return dist[i, rep_idx[:nreps]]
    else:
        def dist_to(i, nreps):
            return np.sqrt(((rep_thumbs[:nreps] - thumbs[i]) ** 2).mean(1))
    lo, hi = 0.0, 64.0
    best = None
    for _ in range(14 if n <= 4000 else 9):
        t = (lo + hi) / 2
        nreps = 0
        assign = np.zeros(n, np.int32)
        for i in order:
            if nreps:
                d = dist_to(i, nreps)
                k = int(d.argmin())
                if d[k] < t:
                    assign[i] = k
                    continue
            if nreps == limit:
                nreps += 1          # one more than fits: a higher threshold, then
                break
            assign[i] = nreps
            rep_idx[nreps] = i
            rep_thumbs[nreps] = thumbs[i]
            nreps += 1
        if nreps <= limit:
            best = (t, [int(r) for r in rep_idx[:nreps]], assign.copy())
            hi = t
        else:
            lo = t
    t, reps, assign = best
    print('  merged %d tiles into %d (threshold %.1f)' % (n, len(reps), t))
    return [int(assign[v]) for v in tilemap], [tile_bytes[i] for i in reps]


BAND = 256          # map rows drawn at a time


def write_mission(out, m, t, mapper, game_pal, unit_names, classes, feat_defs):
    gh = m.ota
    pal = ta.palette()
    npal = np.array(pal, np.uint8)
    W, H = t.w, t.h
    tw, th = W // 2, H // 2
    # features: rocks and every shadow baked into the picture; the rest placed
    bakes = []          # (rgba, x, y, shade)
    placed = []
    ftypes = []         # names of the features placed, the mission's own
    metal = np.zeros((H, W), np.int32)
    blocked_feat = np.zeros((H, W), bool)
    geo = []
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
                bakes.append((s, px - sx, py - sy, 0.55))
            if baked(name, d):
                seq = g.get(d.get('seqname', '').lower())
                if seq:
                    s, sx, sy = gaf_rgba(seq[0], pal)
                    bakes.append((s, px - sx, py - sy, None))
                # metal deposits are built on, not walked round
                if int(ta.num(d, 'blocking', 1)) and not int(ta.num(d, 'metal')):
                    blocked_feat[cz:cz + fz, cx:cx + fx] = True
            else:
                if name not in ftypes:
                    ftypes.append(name)
                placed.append((ftypes.index(name), px, pz))
    # the picture a band at a time: tiles, bakes, dither, then its tiles
    tiles_src = [npal[np.frombuffer(b, np.uint8).reshape(32, 32)] for b in t.tiles]
    tiles = {}
    tilemap = []
    tile_bytes = []
    thumbs = []
    uses = []
    preview = np.zeros((th * 32 // 4, tw * 32 // 4), np.uint8)
    for b0 in range(0, th * 32, BAND):
        rows = min(BAND, th * 32 - b0)
        img = np.zeros((rows, tw * 32, 3), np.float32)
        for ty in range(b0 // 32, (b0 + rows) // 32):
            for tx in range(tw):
                img[ty * 32 - b0:ty * 32 - b0 + 32, tx * 32:tx * 32 + 32] = tiles_src[t.tilemap[ty * tw + tx]]
        for s, x, y, shade in bakes:
            if y < b0 + rows and y + s.shape[0] > b0:
                blit_rgba(img, s, x, y - b0, shade)
        q = mapper.dither(img, 0, b0)
        preview[b0 // 4:(b0 + rows) // 4] = q[::4, ::4]
        for ty in range(b0 // 32, (b0 + rows) // 32):
            for tx in range(tw):
                tq = q[ty * 32 - b0:ty * 32 - b0 + 32, tx * 32:tx * 32 + 32]
                key = tq.tobytes()
                if key not in tiles:
                    tiles[key] = len(tile_bytes)
                    tile_bytes.append(planar_rows(tq))
                    sub = img[ty * 32 - b0:ty * 32 - b0 + 32, tx * 32:tx * 32 + 32]
                    thumbs.append(sub.reshape(8, 4, 8, 4, 3).mean((1, 3)).ravel())
                    uses.append(0)
                uses[tiles[key]] += 1
                tilemap.append(tiles[key])
    Image.fromarray(np.array(game_pal, np.uint8)[preview]).save(os.path.join(PREVIEWS, m.file + '.png'))
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
    # the minimap: TA's own, fitted into 128x128
    mw0, mh0, mpx = t.minimap
    mimg = npal[np.frombuffer(mpx, np.uint8).reshape(mh0, mw0)]
    # TA's minimaps are padded past the map's shape: keep the part it covers
    fit = min(mw0 / float(W), mh0 / float(H))
    keep_w, keep_h = max(1, min(mw0, int(round(W * fit)))), max(1, min(mh0, int(round(H * fit))))
    mimg = mimg[:keep_h, :keep_w]
    sc = 128.0 / max(W * 16, H * 16)
    mw, mh = max(16, int(W * 16 * sc) // 16 * 16), max(1, int(H * 16 * sc))
    mm = Image.fromarray(mimg).resize((mw, mh), Image.BOX)
    mmi = mapper.dither(np.array(mm, np.float32))
    # objectives
    conds = []
    for k, v in gh.items():
        if k not in COND or isinstance(v, dict):
            continue
        parts = [p.strip() for p in v.split(',')]
        typ = -1
        args = [0, 0, 0]

        def typeof(p):
            return unit_names.index(p.lower()) if p.lower() in unit_names else -1

        def number(i):
            return int(ta.number(parts[i]))
        if k == 'moveunittoradius':
            typ = typeof(parts[0])
            args = [number(1), number(2), number(3)]
        elif k in ('killunittype', 'unittypekilled'):
            typ = typeof(parts[0])
            args = [number(1) if len(parts) > 1 and parts[1] else 1, 0, 0]
        elif k in ('allunitskilledoftype', 'captureunittype', 'buildunittype', 'killalloftype'):
            typ = typeof(parts[0])
        elif k == 'unittypepassesz':
            typ = typeof(parts[0])
            args = [number(1), 0, 0]
        elif k == 'anyunitpassesz':
            args = [number(0), 0, 0]
        elif k in ('deathtimerrunsout', 'victorytimerrunsout'):
            # seconds, as the briefings say: 1202 is "twenty minutes", 3600 "60 minutes"
            args = [number(0), 0, 0]
        elif v.strip() in ('0', ''):
            continue
        conds.append(be('hhhhh', COND[k], typ, *args))
    # schemas: units and their scripts
    schemas = []
    scripts = bytearray()
    need = []
    for si in range(int(ta.num(gh, 'schemacount', 1))):
        sch = gh.get('schema %d' % si)
        if not sch:
            continue
        ulist = []
        idents = {}
        entries = [v for k, v in sorted(sch.get('units', {}).items(), key=lambda kv: int(kv[0][4:].split('#')[0]))]
        # idents name a unit by its place among those placed: the known ones
        entries = [u for u in entries if u.get('unitname', '').lower() in unit_names]
        for i, u in enumerate(entries):
            if u.get('ident'):
                idents.setdefault(u['ident'], i)
        for u in entries:
            n = u.get('unitname', '').lower()
            if n not in unit_names:
                print('  unknown unit', n)
                continue
            if unit_names.index(n) not in need:
                need.append(unit_names.index(n))
            ops = compile_script(u.get('initialmission', ''), unit_names, idents, (W * 16, H * 16))
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
    assert len(conds) <= 8 and len(schemas) <= 3, (len(conds), len(schemas))   # data.h's Mission
    # the features' sprites, in the world's colours
    bank = Bank()
    frecs = []
    for n in ftypes:
        d = feat_defs[n]
        spr = b''
        if d.get('object'):
            p = ta.path('objects3d/%s.3do' % d['object'].lower())
            if p:
                polys = render3do.collect(ta.read_3do(p))
                img, ox, oy = render3do.render(polys, 0.3, team=1 if n.startswith('cor') else 0)
                spr = rgba_sprite(img, ox, oy, mapper)
        elif d.get('filename') and d.get('seqname'):
            g = gaf(d['filename'].lower())
            seq = g.get(d['seqname'].lower())
            if seq:
                img, ox, oy = gaf_rgba(seq[0], pal)
                spr = rgba_sprite(img, ox, oy, mapper)
        frecs.append(feature_record(n, d, bank.add(spr) if spr else FEAT_NONE))
    # base metal everywhere else
    sm = int(ta.num(gh.get('schema 0', {}), 'surfacemetal', 3))
    metal[metal == 0] = sm
    # what the player may build: the mission's useonly list, or everything
    uo = ta.path('camps/useonly/%s.tdf' % m.base) if m.base == m.map else None
    allowed = set(k.lower().split('#')[0] for k in ta.read_tdf(uo)) if uo else set(unit_names)
    bits = np.zeros(MAX_UNITTYPES, np.uint8)
    for i, n in enumerate(unit_names):
        bits[i] = n in allowed

    def assemble(tilemap, tile_bytes):
        head = (b'OAM4' + cstr(m.name, 40) +
                be('HHHHHHHHHHHHHHHHHHHH', W, H, tw, th, len(tile_bytes), t.sealevel, mw, mh, len(ftypes),
                   len(placed), len(conds), int(ta.num(gh, 'solarstrength')), int(ta.num(gh, 'minwindspeed')),
                   int(ta.num(gh, 'maxwindspeed')), len(schemas), int(ta.num(gh, 'tidalstrength')), m.world,
                   m.side, int(ta.num(gh, 'lavaworld')), len(need)))
        body = head + b''.join(conds) + b''.join(be('H', n) for n in need) + np.packbits(bits).tobytes()
        body += b''.join(frecs)
        body += b''.join(be('HHH', *p) for p in placed)
        body += be('I', len(scripts) // 8) + bytes(scripts)
        for s in schemas:
            body += s
        body += b''.join(be('H', v) for v in tilemap)
        body += hgt.astype(np.uint8).tobytes() + flags.tobytes() + np.clip(metal, 0, 255).astype(np.uint8).tobytes()
        body += image_planar(mmi)
        body = pad4(body)
        body += b''.join(tile_bytes)
        return body + be('I', len(bank.data)) + bytes(bank.data)
    # the tiles: what MISSION_BUDGET leaves beside the map and what the engine
    # keeps for it, and what MISSION_TOTAL leaves beside the whole file and
    # those arrays (the expansions' features can take a megabyte)
    mem = map_memory(W, H, len(classes))[0]
    rest = len(assemble(tilemap, [])) + mem - W * H * 3
    limit = min(MAX_TILES, tile_room(mem), (MISSION_TOTAL - rest) // 512)
    if limit < MIN_TILES:
        print('  mission left out, too little room for its tiles:', m.base, limit)
        return False
    if len(tile_bytes) > limit:
        tilemap, tile_bytes = merge_tiles(np.array(thumbs, np.float32), uses, tilemap, tile_bytes, limit)
    body = assemble(tilemap, tile_bytes)
    open(os.path.join(out, m.file.upper() + '.DAT'), 'wb').write(body)
    print(m.file.upper() + '.DAT', len(body), 'bytes,', len(tile_bytes), 'tiles,', len(placed), 'features,',
          len(ftypes), 'kinds')
    return True


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
    """Short sounds at 12517 Hz, longer ones at 6258 Hz; none past MAX_SOUND
    seconds. They live in ST-RAM: past SOUND_BUDGET, as with the expansions'
    too, the long ones are cut shorter, and then more of them slower."""
    paths = [ta.path('sounds/%s.wav' % n) for n in SOUNDS.names]
    lens = []
    for p in paths:
        w = wave.open(p)
        lens.append(w.getnframes() / float(w.getframerate()))

    def rate(secs):
        return 12517 if secs <= short else 6258
    # the head (below): OAS2, a count and 12 bytes a sound; each sound's
    # samples, a byte each, padded to an even length
    for short, longest in SOUND_STEPS:
        if 6 + 12 * len(lens) + sum(min(secs, longest) * rate(secs) + 1 for secs in lens) <= SOUND_BUDGET:
            break
    else:
        sys.exit('The sounds take more than SOUND_BUDGET however short they are cut (SOUND_STEPS)')
    if (short, longest) != SOUND_STEPS[0]:
        print('  sounds over %d bytes: 12517 Hz up to %.1f s, none past %.1f s' % (SOUND_BUDGET, short, longest))
    data = bytearray()
    offs = []
    for p, secs in zip(paths, lens):
        code = 1 if rate(secs) == 12517 else 0
        smp = np.frombuffer(read_wav(p, rate(secs)), np.int8).astype(np.float32)
        cap = int(longest * rate(secs))
        if len(smp) > cap:
            fade = int(0.25 * rate(secs))
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


def music_tracks(out, folder):
    """The CD's tracks as signed 8-bit mono at 12517 Hz, MUSICnn.SND; each one
    as calm (0) or loud (1), for the game to play the loud ones in battle:
    the busier half, by how much the sound changes from one moment to the
    next (drums and brass against strings and pads). Returns [(number,
    kind, samples)]."""
    tracks = []
    for f in sorted(os.listdir(folder)):
        if not (f.startswith('track') and f.endswith('.wav')):
            continue
        num = int(f[5:7])
        a, fr, ch = wav_samples(os.path.join(folder, f))
        busy = int(np.abs(np.diff(a[::4])).mean())
        s = resample(a, fr, 12517, 256 * ch)
        open(os.path.join(out, 'MUSIC%02d.SND' % num), 'wb').write(s)
        tracks.append([num, busy, len(s)])
        print('MUSIC%02d.SND' % num, len(s), 'bytes, busy', busy)
    order = sorted(tracks, key=lambda t: (t[1], t[0]))
    for i, t in enumerate(order):
        t[1] = 1 if i >= len(order) // 2 else 0
    return [tuple(t) for t in tracks]


FILM_LINES, FILM_GROUPS = 240, 20     # a film's picture: 320x240, in 16-pixel groups
FILM_SAME = 8       # a group whose colours change less than this on average stays as it is
# A film's budget: a frame's share, a little less than a TT reads from
# GemDrive and draws in a frame's time (the splash film asks for more); what
# the player reads before the first frame (FILM_LEAD in film.c); and what
# quieter frames may save for busier ones, which a TT with 4 MB of TT-RAM
# has room to read ahead
FILM_FRAME_BYTES = 26 * 1024
FILM_LEAD = 1024 * 1024
FILM_BANK = 2048 * 1024


def film_planes(idx):
    """A 320-wide picture's palette indices as TT low's 8 interleaved planes:
    (rows, 20, 16) bytes, a group's 8 plane words."""
    words = plane_words(idx, planes=8)
    return words.astype('>u2').view(np.uint8).reshape(idx.shape[0], FILM_GROUPS, 16)


def write_films(out, folder):
    """FILMn.DAT: a film at 15 frames a second for TT low resolution: each
    frame its palette when it changed (256 TT colours), then a bit for each
    16-pixel group of its 320x240 picture that looks different from what is
    shown (by FILM_SAME), then those groups. A frame over its budget (by
    FILM_FRAME_BYTES and FILM_BANK) is left out, changing nothing, and the
    next one shows what it would have. FILMn.SND:
    its sound. ffmpeg decodes the films' Smacker; without it there are
    none."""
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        print('no ffmpeg: the films are left out')
        return
    for num in range(1, 6):
        src = os.path.join(folder, '%d.smk' % num)
        if not os.path.exists(src):
            continue
        head = open(src, 'rb').read(12)
        w, h = struct.unpack_from('<II', head, 4)
        size = w * h + 1024
        proc = subprocess.Popen([ffmpeg, '-v', 'error', '-i', src, '-f', 'rawvideo', '-pix_fmt', 'pal8', '-'],
                                stdout=subprocess.PIPE)
        rows = np.arange(FILM_LINES) * h // FILM_LINES
        recs = []
        prev_pal = None
        shown = np.zeros((FILM_LINES, FILM_GROUPS * 16), np.uint8)
        bank = FILM_LEAD
        k = 0
        while True:
            b = proc.stdout.read(size)
            if len(b) < size:
                break
            k += 1
            if (k - 1) & 1:
                continue            # 30 frames a second, shown at 15
            idx = np.frombuffer(b, np.uint8, w * h).reshape(h, w)[rows, ::2]
            argb = np.frombuffer(b, '<u4', 256, w * h).astype(np.int64)
            rgb = np.stack([(argb >> 16) & 255, (argb >> 8) & 255, argb & 255], axis=1)
            tt = (rgb * 15 + 127) // 255
            pal = ((tt[:, 0] << 8) | (tt[:, 1] << 4) | tt[:, 2]).astype('>u2').tobytes()
            # what is shown, through this frame's palette, against the frame
            diff = np.abs(rgb[idx] - rgb[shown]).reshape(FILM_LINES, FILM_GROUPS, 16 * 3).sum(axis=2)
            changed = diff >= FILM_SAME * 16 * 3 if recs else np.ones((FILM_LINES, FILM_GROUPS), bool)
            body = b''
            flags = 0
            bank = min(bank + FILM_FRAME_BYTES, FILM_BANK)
            if recs and 6 + (pal != prev_pal) * len(pal) + FILM_LINES * FILM_GROUPS // 8 + changed.sum() * 16 > bank:
                changed[:] = False      # over the budget: the frame is left out
            elif pal != prev_pal:
                body += pal
                flags |= 1
                prev_pal = pal
            shown[np.repeat(changed, 16, axis=1)] = idx[np.repeat(changed, 16, axis=1)]
            groups = film_planes(shown)
            body += np.packbits(changed.ravel()).tobytes() + groups[changed].tobytes()
            bank -= 6 + len(body)
            recs.append(be('IH', len(body), flags) + body)
        proc.wait()
        data = b'OAF3' + be('HHH', len(recs), 15, FILM_LINES) + b''.join(recs)
        open(os.path.join(out, 'FILM%d.DAT' % num), 'wb').write(data)
        # its sound, mono 22050 Hz from ffmpeg, then the converter's own resampling
        a = subprocess.run([ffmpeg, '-v', 'error', '-i', src, '-vn', '-f', 's16le', '-ac', '1', '-ar', '22050', '-'],
                           stdout=subprocess.PIPE, check=True).stdout
        snd = resample(np.frombuffer(a, '<i2').astype(np.int64), 22050, 12517, 256)
        open(os.path.join(out, 'FILM%d.SND' % num), 'wb').write(snd)
        print('FILM%d.DAT' % num, len(data), 'bytes,', len(recs), 'frames;', 'FILM%d.SND' % num, len(snd), 'bytes')


def write_game(out, camps, worlds, tracks):
    """GAME.DAT: the campaigns, their missions, and the music."""
    body = b'OAG3'
    missions = [m for c in camps for m in c.missions]
    body += be('HHHHHH', len(camps), len(missions), len(worlds), len(tracks), 0, 0)
    first = 0
    for c in camps:
        body += cstr(c.name, 24) + be('BBHH', c.side, c.game, first, len(c.missions))
        first += len(c.missions)
    for m in missions:
        body += cstr(m.name, 40) + cstr(m.file.upper(), 10) + be('BBH', m.world, m.side, 0)
    for num, kind, n in tracks:
        body += be('BBI', num, kind, n)
    open(os.path.join(out, 'GAME.DAT'), 'wb').write(body)
    print('GAME.DAT', len(body), 'bytes:', len(camps), 'campaigns,', len(missions), 'missions,', len(tracks), 'tracks')


MAX_SOUND = 1.6
SOUND_BUDGET = 1100 * 1024      # SOUNDS.DAT's bytes, to leave ST-RAM free as the full game does
# (the longest at 12517 Hz, the longest at all), in turn until the sounds fit
SOUND_STEPS = [(0.5, MAX_SOUND), (0.5, 1.4), (0.5, 1.2), (0.5, 1.0), (0.4, 1.0), (0.3, 1.0), (0.2, 1.0),
               (0.0, 1.0), (0.0, 0.8)]
SOUNDS = Sounds()

PREVIEWS = None     # where pictures of the converted art go


def main():
    global PREVIEWS
    ta.ROOT = sys.argv[1]
    # the expansions beside the data, those OA_EXPANSIONS names (both unless it says)
    here = os.path.dirname(os.path.abspath(ta.ROOT))
    want = os.environ.get('OA_EXPANSIONS', 'cc,bt').lower().split(',')
    ta.OVER = [os.path.join(here, e) for e, _ in EXPANSIONS if e in want and os.path.isdir(os.path.join(here, e))]
    print('expansions', [os.path.basename(e) for e in ta.OVER])
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    PREVIEWS = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(out)), 'previews')
    os.makedirs(PREVIEWS, exist_ok=True)
    camps = find_missions()
    missions = [m for c in camps for m in c.missions]
    only = [b for b in os.environ.get('OA_ONLY', '').lower().split(',') if b]
    for m in missions:
        if m.file in only and m.base not in only:
            only.append(m.base)
    # the worlds, in the order their first missions come
    worlds = []
    for m in missions:
        if m.planet not in worlds:
            worlds.append(m.planet)
        m.world = worlds.index(m.planet)
    print('worlds', worlds)
    units, weapons = load_defs()
    feat_defs = load_features()
    names = sorted(units)
    for i, n in enumerate(ENGINE_ANIMS):
        assert anim_ref(n) == i, n
    # interface sounds the engine plays by number (see data.h, SND_*)
    for i, n in enumerate(('beep2', 'beep4', 'button9', 'cantdo4', 'xplomed2', 'xplosml3', 'xplolrg3',
                           'twractv1', 'unitdone', 'build1', 'metalno1', 'warning1', 'nanlath1',
                           'victory2', 'xplobld1')):
        assert SOUNDS.ref(n) == i, n
    # every unit drawn once
    arts = {}
    for n in names:
        u = units[n]
        side = 0 if u.get('side', '').upper() == 'ARM' else 1
        dirs = DIRS if int(ta.num(u, 'bmcode')) == 1 else 1
        arts[n] = render_unit(u.get('objectname', n).lower(), u, side, dirs, feat_defs)
    print('units drawn:', len(arts))
    classes = write_units(out, names, units, weapons, SOUNDS, feat_defs, arts)
    for extra in ('explode2', 'explode3', 'explode4', 'explode5', 'explosion', 'smoke 1', 'nuke1'):
        anim_ref(extra)
    write_common(out)
    left_out = []
    for num, planet in enumerate(worlds):
        wm = [m for m in missions if m.world == num]
        if only and not any(m.base in only for m in wm):
            continue
        # a world's maps at a time: all of them at once fill a computer's memory
        maps = {m.base: ta.read_tnt(ta.path('maps/%s.tnt' % m.map)) for m in wm}
        game_pal = gather_palette([maps[m.base] for m in wm])
        print('world', num + 1, planet, 'palette', game_pal)
        mapper = quant.Mapper(game_pal, spread=30)
        write_world(out, num + 1, game_pal, mapper)
        for side in (0, 1):
            write_panel(out, num + 1, side, game_pal, mapper)
        write_sprites(out, num + 1, names, arts, mapper)
        for m in wm:
            if (not only or m.base in only) and not write_mission(out, m, maps[m.base], mapper, game_pal,
                                                                  names, classes, feat_defs):
                left_out.append(m)
        # before the next world's are read
        del maps, game_pal, mapper
    write_sounds(out)
    for c in camps:
        c.missions = [m for m in c.missions if m not in left_out]
    camps = [c for c in camps if c.missions]
    missions = [m for m in missions if m not in left_out]
    # each mission's briefing, narration and loading picture
    last_glam = None
    for m in missions:
        if only and m.base not in only:
            continue
        brief = m.ota.get('brief', '')
        p = ta.path('camps/briefs/%s.txt' % brief.lower()) if brief else None
        txt = open(p, 'rb').read() if p else getattr(m, 'brief', '').encode('latin-1')
        # its first line ("MISSION n...") and the closing END left out; the
        # line break that ends the first line stays, a blank line under the
        # mission's name
        if txt.startswith(b'MISSION'):
            txt = txt[txt.index(b'\n'):] if b'\n' in txt else b''
        txt = txt.rstrip()
        if txt.endswith(b'END'):
            txt = txt[:-3]
        open(os.path.join(out, m.file.upper() + '.TXT'), 'wb').write(txt)
        nar = ta.path('camps/briefs/%s.wav' % m.ota.get('narration', '').lower())
        open(os.path.join(out, m.file.upper() + '.SND'), 'wb').write(read_wav(nar, 12517) if nar else b'')
        g = ta.path('bitmaps/glamour/%s.pcx' % m.ota.get('glamour', '').lower())
        dst = os.path.join(out, m.file.upper() + '.IMG')
        if g:
            write_picture(g, dst)
            last_glam = dst
        elif last_glam:
            shutil.copyfile(last_glam, dst)      # the demo has two of its three
    write_picture(ta.path('bitmaps/frontendx.pcx'), os.path.join(out, 'TITLE.IMG'))
    write_picture(ta.path('bitmaps/singlebg.pcx'), os.path.join(out, 'SINGLE.IMG'))
    write_picture(ta.path('bitmaps/mbriefarm.pcx'), os.path.join(out, 'BRIEFARM.IMG'))
    write_picture(ta.path('bitmaps/mbriefcor.pcx') or ta.path('bitmaps/mbriefarm.pcx'),
                  os.path.join(out, 'BRIEFCOR.IMG'))
    tracks = []
    music = os.path.join(here, 'music')
    if os.path.isdir(music) and not only:
        tracks = music_tracks(out, music)
    video = os.path.join(here, 'video')
    if os.path.isdir(video) and not only:
        write_films(out, video)
    write_game(out, camps, worlds, tracks)


if __name__ == '__main__':
    main()
