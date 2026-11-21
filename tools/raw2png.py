# Copyright (C) 2026 Neil Rackett
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
