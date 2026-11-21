# Copyright (C) 2026 Neil Rackett
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
    for m in re.finditer(r'= (\d+) \w+ (\d+)/(\d+)/(\d+)/\d+/\d+ (\d+)/(\d+)/(\d+)/\d+/\d+', line):
        calls += int(m.group(1)); inc += int(m.group(4)); ex += int(m.group(7))
    a = tot.setdefault(name, [0, 0, 0])
    a[0] += ex; a[1] += inc; a[2] += calls
allc = sum(v[0] for v in tot.values()) or 1
for n, (ex, inc, calls) in sorted(tot.items(), key=lambda kv: -kv[1][0])[:int(sys.argv[2]) if len(sys.argv) > 2 else 25]:
    print('%-28s excl %5.1f%% %11d  incl %11d  calls %d' % (n, 100.0 * ex / allc, ex, inc, calls))
