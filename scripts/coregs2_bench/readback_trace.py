"""Which memory writes store targets, and which targets are read back, per VBlank.

usage: readback_trace.py <log.txt> <first VBlank> <last VBlank>
The run needs GT4_CORE2_STORE_TRACE=1 and GT4_CORE2_READBACK_TRACE=1.
"""
import collections
import re
import sys

log, first, last = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
vblanks = last - first
stores = collections.Counter()
readbacks = collections.Counter()
pixels = collections.Counter()
for line in open(log, errors='replace'):
    m = re.match(r'\[core2\] store vblank=(\d+) write bp=(\d+) bw=(\d+) psm=(\w+) rect=([\d,-]+) '
                 r'target bp=(\d+) bw=(\d+) size=(\w+) drawn=([\d,-]+)', line)
    if m and first <= int(m.group(1)) < last:
        r = [int(x) for x in m.group(5).split(',')]
        write = f'write bp={m.group(2)} bw={m.group(3)} psm={m.group(4)} {r[2] - r[0] + 1}x{r[3] - r[1] + 1}'
        stores[(write, f'target bp={m.group(6)} bw={m.group(7)} {m.group(8)} drawn={m.group(9)}')] += 1
    m = re.match(r'\[core2\] readback (.+?) vblank=(\d+) target bp=(\d+) bw=(\d+) psm=(\d+) size=(\w+) area=([\d,-]+)', line)
    if m and first <= int(m.group(2)) < last:
        a = [int(x) for x in m.group(7).split(',')]
        key = (m.group(1), f'bp={m.group(3)} bw={m.group(4)} psm={m.group(5)} {m.group(6)}')
        readbacks[key] += 1
        pixels[key] += (a[2] - a[0] + 1) * (a[3] - a[1] + 1)
print(f'stores per VBlank {sum(stores.values()) / vblanks:.2f}')
for key, count in stores.most_common(16):
    print(f'{count / vblanks:6.2f} {key[0]:44s} {key[1]}')
print(f'readbacks per VBlank {sum(readbacks.values()) / vblanks:.2f}, native pixels per VBlank {sum(pixels.values()) / vblanks:.0f}')
for key, count in readbacks.most_common(18):
    print(f'{count / vblanks:6.2f} {pixels[key] / vblanks:9.0f} px {key[0]:14s} {key[1]}')
