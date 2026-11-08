#!/usr/bin/env python3
"""Map SyncTest mismatch addresses (Rollback: lines in a game log) to symbols.
usage: synctest-symbols.py <stdout log> <binary>"""
import sys, re, bisect, subprocess, collections
log, binary = sys.argv[1], sys.argv[2]
syms = []
for l in subprocess.run(['nm', '-n', binary], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if len(f) == 3:
        syms.append((int(f[0], 16), f[2]))
addrs = [a for a, _ in syms]
slide = 0
hits = collections.Counter(); frames = []
for l in open(log, errors='replace'):
    m = re.search(r'slide 0x([0-9a-f]+)', l)
    if m: slide = int(m.group(1), 16)
    m = re.search(r'MISMATCH after frame (\d+)', l)
    if m: frames.append(int(m.group(1)))
    m = re.search(r'\(addr 0x([0-9a-f]+)\) (\d+) bytes', l)
    if m:
        a = int(m.group(1), 16) - slide
        i = bisect.bisect_right(addrs, a) - 1
        hits['%s+0x%x' % (syms[i][1], a - syms[i][0]) if i >= 0 else hex(a)] += int(m.group(2))
print('mismatch frames:', frames[:30])
for k, v in hits.most_common(60):
    print(v, k)
