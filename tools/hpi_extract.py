# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unpacks a Total Annihilation HPI archive into a folder.

usage: hpi_extract.py archive.hpi outdir

Hpi reads an archive held in memory, for other tools: its files by path,
lower case.
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


class Hpi:
    """An HPI archive's bytes: files lists its paths, read() unpacks one."""

    def __init__(self, data):
        raw = np.frombuffer(data, dtype=np.uint8)
        magic, ver, dsize, hkey, start = struct.unpack_from('<5I', data[:20])
        assert magic == 0x49504148 and ver == 0x10000, 'not an HPI archive'
        k = hkey & 0xff
        if k:
            key = (~((k >> 6) | (k << 2))) & 0xff
            pos = (np.arange(len(raw), dtype=np.uint32) & 0xff).astype(np.uint8)
            dec = pos ^ (~raw) ^ np.uint8(key)
            dec[:20] = raw[:20]
            self.d = dec.tobytes()
        else:
            self.d = bytes(data)
        self.files = {}         # path -> its entry's offset, in the archive's order
        self.dirs = []          # every folder's path, empty ones too
        self._walk(start, '')

    def _cstr(self, o):
        return self.d[o:self.d.index(b'\0', o)].decode('latin-1')

    def _walk(self, o, path):
        n, lst = struct.unpack_from('<II', self.d, o)
        for e in range(n):
            no, do, isdir = struct.unpack_from('<IIB', self.d, lst + 9 * e)
            p = path + self._cstr(no).lower()
            if isdir:
                self.dirs.append(p)
                self._walk(do, p + '/')
            else:
                self.files[p] = do

    def read(self, p):
        d = self.d
        off, size, comp = struct.unpack_from('<IIB', d, self.files[p])
        if comp == 0:
            return d[off:off + size]
        nchunks = (size + 65535) // 65536
        q = off + 4 * nchunks
        out = bytearray()
        for c in range(nchunks):
            csz = struct.unpack_from('<I', d, off + 4 * c)[0]
            _, _, ctype, enc, comp_sz, dec_sz, _ = struct.unpack('<IBBBIII', d[q:q + 19])
            payload = d[q + 19:q + 19 + comp_sz]
            if enc:
                a = np.frombuffer(payload, dtype=np.uint8)
                idx = (np.arange(len(a)) & 0xff).astype(np.uint8)
                payload = ((a - idx) ^ idx).astype(np.uint8).tobytes()
            if ctype == 1:
                out += lz77(payload, dec_sz)
            elif ctype == 2:
                out += zlib.decompress(payload)
            else:
                out += payload
            q += csz
        return bytes(out[:size])

    def extract(self, outdir):
        """Writes every file under outdir; returns how many."""
        for p in self.dirs:
            os.makedirs(os.path.join(outdir, *p.split('/')), exist_ok=True)
        for p in self.files:
            dst = os.path.join(outdir, *p.split('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as f:
                f.write(self.read(p))
        return len(self.files)


def main():
    src, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    with open(src, 'rb') as f:
        n = Hpi(f.read()).extract(outdir)
    print(n, 'files')


if __name__ == '__main__':
    main()
