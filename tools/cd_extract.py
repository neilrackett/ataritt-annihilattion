# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unpacks the Total Annihilation CDs (1997) for the converter, and those of
its expansions, The Core Contingency and Battle Tactics (1998).

usage: cd_extract.py DISC1 DISC2 OUT_DIR
       cd_extract.py EXPANSION OUT_DIR

DISC1 and DISC2 are the two discs' images, in either order: a CUE sheet
with its BIN or IMG (raw 2352-byte sectors), the IMG alone (its CUE beside
it gives the audio tracks), or an ISO. Nothing on them runs. OUT_DIR
receives:

  data/     every file of the game's archives: totala1.hpi (which the
            installer copies from TOTALA3.HPI), then TOTALA2.HPI and
            TOTALA4.HPI over it; lower-case names, like the demo's
  music/    the first disc's audio tracks as WAV, track02.wav ..
  video/    the Smacker films, DATA/1.ZRB .. as 1.smk ..

An expansion's disc goes into a folder of its own beside data/, which the
converter reads over it: cc/ The Core Contingency's archives (its units,
maps and missions), bt/ Battle Tactics' (its units, maps and the TACTICS
missions), each in the order the game reads them.
"""
import os, re, struct, sys, wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hpi_extract import Hpi

SYNC = b'\x00' + b'\xff' * 10 + b'\x00'


class Disc:
    """A CD image: its ISO 9660 files, and its audio tracks when a CUE sheet
    gives them."""

    def __init__(self, path):
        cue = None
        if path.lower().endswith('.cue'):
            cue = path
            path = self._cue_file(cue)
        else:
            base = os.path.splitext(path)[0]
            for ext in ('.cue', '.CUE'):
                if os.path.exists(base + ext):
                    cue = base + ext
        self.h = open(path, 'rb')
        self.size = os.path.getsize(path)
        head = self.h.read(16)
        if head[:12] == SYNC:
            self.ss = 2352
            self.off = 24 if head[15] == 2 else 16   # mode 2 sectors have a subheader
        else:
            self.ss, self.off = 2048, 0
        self.tracks = self._tracks(cue) if cue else []
        self.files = {}
        pvd = self.sectors(16, 1)
        if pvd[1:6] != b'CD001':
            raise ValueError('%s holds no ISO 9660 file system' % path)
        root = pvd[156:190]
        self._walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '')

    @staticmethod
    def _cue_file(cue):
        for line in open(cue, encoding='latin-1'):
            m = re.match(r'\s*FILE\s+"([^"]+)"', line) or re.match(r'\s*FILE\s+(\S+)', line)
            if m:
                return os.path.join(os.path.dirname(cue), m.group(1))
        raise ValueError('%s names no file' % cue)

    def _tracks(self, cue):
        """[(number, audio, first sector, end sector)]: a track ends where the
        next one's pregap (INDEX 00) or start begins."""
        t = []
        for line in open(cue, encoding='latin-1'):
            m = re.match(r'\s*TRACK\s+(\d+)\s+(\S+)', line)
            if m:
                t.append([int(m.group(1)), m.group(2).upper() == 'AUDIO', None, None])
            m = re.match(r'\s*INDEX\s+(\d+)\s+(\d+):(\d+):(\d+)', line)
            if m and t:
                s = (int(m.group(2)) * 60 + int(m.group(3))) * 75 + int(m.group(4))
                t[-1][2 if int(m.group(1)) == 1 else 3] = s
        out = []
        for i, (n, audio, start, _) in enumerate(t):
            nxt = t[i + 1] if i + 1 < len(t) else None
            end = (nxt[3] if nxt[3] is not None else nxt[2]) if nxt else self.size // 2352
            out.append((n, audio, start, end))
        return out

    def sectors(self, n, count):
        out = bytearray()
        for i in range(count):
            self.h.seek((n + i) * self.ss + self.off)
            out += self.h.read(2048)
        return bytes(out)

    def _walk(self, extent, length, prefix):
        data = self.sectors(extent, (length + 2047) // 2048)
        i = 0
        while i < length:
            n = data[i]
            if n == 0:                  # records never cross a sector: on to the next
                i = (i // 2048 + 1) * 2048
                continue
            rec = data[i:i + n]
            i += n
            name = rec[33:33 + rec[32]]
            if name in (b'\x00', b'\x01'):
                continue
            name = name.decode('latin-1').split(';')[0]
            ext, ln = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
            if rec[25] & 2:
                self._walk(ext, ln, prefix + name + '/')
            else:
                self.files[(prefix + name).upper()] = (ext, ln)

    def read(self, name):
        ext, ln = self.files[name.upper()]
        if self.ss == 2048:
            self.h.seek(ext * 2048)
            return self.h.read(ln)
        return self.sectors(ext, (ln + 2047) // 2048)[:ln]

    def audio(self, number):
        """A track's samples: 44100 Hz, 16-bit little-endian stereo."""
        for n, audio, start, end in self.tracks:
            if n == number and audio:
                self.h.seek(start * 2352)
                return self.h.read((end - start) * 2352)
        return None


# An expansion's disc: the archive that tells which it is, its folder beside
# data/, and its archives, each over the last
EXPANSIONS = [
    ('CC/CCDATA.CCX', 'cc', ['CC/CCDATA.CCX', 'CC/CCMAPS.CCX', 'CC/CCMISS.CCX']),
    ('BT/BTDATA.CCX', 'bt', ['BT/BTDATA.CCX', 'BT/BTMAPS.CCX'] + ['BT/TACTICS%d.HPI' % i for i in range(1, 9)]),
]


def expansion(disc, out):
    d = Disc(disc)
    for mark, folder, archives in EXPANSIONS:
        if mark in d.files:
            break
    else:
        sys.exit('%s is neither The Core Contingency nor Battle Tactics' % disc)
    dst = os.path.join(out, folder)
    for a in archives:
        print(a, Hpi(d.read(a)).extract(dst), 'files')


def main():
    if len(sys.argv) == 3:
        expansion(sys.argv[1], sys.argv[2])
        return
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    discs = [Disc(sys.argv[1]), Disc(sys.argv[2])]
    if 'TOTALA4.HPI' in discs[0].files:
        discs.reverse()
    d1, d2 = discs
    for d, f, which in ((d1, 'TOTALA3.HPI', 'first'), (d2, 'TOTALA4.HPI', 'second')):
        if f not in d.files:
            sys.exit('The %s disc has no %s: are these the Total Annihilation CDs?' % (which, f))
    out = sys.argv[3]
    data = os.path.join(out, 'data')
    os.makedirs(data, exist_ok=True)
    # the installed archive first, then the discs' over it, as the game reads them
    archives = [('totala1.hpi', Hpi(Hpi(d1.read('TOTALA3.HPI')).read('install/totala1.hpi'))),
                ('TOTALA2.HPI', Hpi(d1.read('TOTALA2.HPI'))),
                ('TOTALA4.HPI', Hpi(d2.read('TOTALA4.HPI')))]
    for name, a in archives:
        print(name, a.extract(data), 'files')
    music = os.path.join(out, 'music')
    os.makedirs(music, exist_ok=True)
    n = 0
    for num, audio, _, _ in d1.tracks:
        if audio:
            w = wave.open(os.path.join(music, 'track%02d.wav' % num), 'wb')
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(44100)
            w.writeframes(d1.audio(num))
            w.close()
            n += 1
    print(n, 'music tracks' if n else 'music tracks: give the first disc as a CUE sheet, or its CUE beside it')
    video = os.path.join(out, 'video')
    os.makedirs(video, exist_ok=True)
    films = sorted(f for f in d1.files if f.startswith('DATA/') and f.endswith('.ZRB'))
    for f in films:
        with open(os.path.join(video, os.path.basename(f)[:-4].lower() + '.smk'), 'wb') as h:
            h.write(d1.read(f))
    print(len(films), 'films')


if __name__ == '__main__':
    main()
