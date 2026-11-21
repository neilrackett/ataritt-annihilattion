# Copyright (C) 2026 Neil Rackett
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
        return d[o:d.index(b'\0', o)].decode('latin-1')

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
