# Copyright (C) 2026 Neil Rackett
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
        pal = np.array(ta.palette(), dtype=np.float32)
        _textures = {}
        for f in ta.listdir('textures'):
            if f.lower().endswith('.gaf'):
                for name, frames in ta.read_gaf(ta.path('textures', f)).items():
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
            # would draw as a black square; the Swatter's (armah) is
            # coloured, on its ground plate piece
            if not tex and ((col & 255) == 0 or ob.name == 'gp') and all(abs(p[1]) < 0.01 for p in pts):
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
