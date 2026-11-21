# Copyright (C) 2026 Neil Rackett
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
