# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""Readers for the Total Annihilation demo's file formats, for the TT converter.

Each reader returns plain Python values: TDF sections as nested dicts with
lower-case keys, GAF frames as (width, height, x, y, pixels) with palette
indices, 3DO objects as trees of vertices and primitives.
"""
import functools, os, re, struct

ROOT = None  # folder the game's archives were unpacked into
OVER = []    # folders unpacked over it, as the expansions' are: the last one wins


def _roots():
    """The folders to look in, as they lie: ROOT, then each of OVER over it."""
    return [ROOT] + OVER


@functools.lru_cache(maxsize=None)
def _names(folder):
    """A folder's names, by their lower case."""
    return {n.lower(): n for n in os.listdir(folder)}


def _find(root, parts):
    p = root
    for part in parts:
        name = _names(p).get(part.lower()) if os.path.isdir(p) else None
        if name is None:
            return None
        p = os.path.join(p, name)
    return p


def path(*parts):
    """Finds a file under ROOT, or a folder over it, whatever the case of its
    name; the last of OVER that has it wins."""
    parts = '/'.join(parts).split('/')
    for r in reversed(_roots()):
        p = _find(r, parts)
        if p:
            return p
    return None


def lowest(*parts):
    """The first folder, ROOT or else the first of OVER, that has a file:
    where it came from, before anything over it replaced it."""
    parts = '/'.join(parts).split('/')
    for r in _roots():
        if _find(r, parts):
            return r
    return None


def _merged(folder):
    """{lower-case name: full path} in folder, every root's, the winner's
    where two have the same name."""
    parts = folder.split('/')
    out = {}
    for r in _roots():
        p = _find(r, parts)
        if p and os.path.isdir(p):
            out.update((low, os.path.join(p, n)) for low, n in _names(p).items())
    return out


def listdir(folder):
    """The names in folder in every root, sorted."""
    return sorted(os.path.basename(p) for p in _merged(folder).values())


def walk(folder):
    """Every file under folder in every root: [full path], each folder's files
    before its folders', all in name order, as os.walk with sorted lists."""
    entries = sorted(_merged(folder).values(), key=os.path.basename)
    return ([p for p in entries if not os.path.isdir(p)] +
            [f for p in entries if os.path.isdir(p) for f in walk(folder + '/' + os.path.basename(p))])


# ---------------------------------------------------------------- TDF text

def parse_tdf(text):
    """Parses TDF/FBI/OTA/GUI text into nested dicts; keys are lower case.

    Repeated section names at one level become numbered with '#n'.
    """
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    root = {}
    stack = [root]
    pending = None
    i = 0
    toks = re.finditer(r'\[([^\]]*)\]|\{|\}|([^;{}\[\]=]+)=([^;]*);', text)
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


def number(v, default=0.0):
    """A number as TA's files write it, forgivingly."""
    try:
        return float(v)
    except ValueError:
        # a number followed by something else, such as 3.6. (a full game's typo)
        m = re.match(r'\s*-?(\d+\.?\d*|\.\d+)', str(v))
        return float(m.group(0)) if m else default


def num(d, key, default=0.0):
    return number(d.get(key, default), default)


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
        name = d[e + 8:e + 40].split(b'\0')[0].decode('latin-1').lower()
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
        ob.name = d[noff:d.index(b'\0', noff)].decode('latin-1').lower()
        ob.off = (x / 65536.0, y / 65536.0, z / 65536.0)
        ob.verts = [tuple(v / 65536.0 for v in struct.unpack_from('<3i', d, voff + 12 * i)) for i in range(nv)]
        ob.prims = []
        for i in range(np_):
            col, nidx, _, ioff, toff, _, _, _ = struct.unpack_from('<8i', d, poff + 32 * i)
            idx = list(struct.unpack_from('<%dh' % nidx, d, ioff)) if nidx else []
            tex = d[toff:d.index(b'\0', toff)].decode('latin-1').lower() if toff else ''
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


def tnt_size(p):
    """A TNT map's width and height in 16-pixel cells, from its header alone."""
    with open(p, 'rb') as f:
        return struct.unpack_from('<2I', f.read(12), 4)


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
    t.features = [d[h[8] + i * 132 + 4:h[8] + i * 132 + 132].split(b'\0')[0].decode('latin-1')
                  for i in range(h[7])]
    t.sealevel = h[9]
    mm = h[10]
    mw, mh = struct.unpack_from('<II', d, mm)
    t.minimap = (mw, mh, d[mm + 8:mm + 8 + mw * mh])
    return t
