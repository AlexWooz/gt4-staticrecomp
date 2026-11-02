"""Scan frame pairs (gt4-gs live PNG vs our replay PPM) for localized differences.

usage: scan.py <gsdx dir> <ours dir> <out dir> [first last step]
Both are compared at 640 x 448 (box-filtered) so resampling noise mostly cancels; the score of a frame is its
largest 8 x 8 block difference after subtracting the frame's median block difference.
"""
import os
import sys
import numpy as np
from PIL import Image

gdir, odir, out = sys.argv[1:4]
first, last, step = (int(v) for v in sys.argv[4:7]) if len(sys.argv) > 4 else (9610, 10790, 10)
os.makedirs(out, exist_ok=True)
rows = []
for n in range(first, last + 1, step):
    gp = os.path.join(gdir, f'live_frame{n:05d}.png')
    op = os.path.join(odir, f'frame_{n:05d}.ppm')
    if not (os.path.exists(gp) and os.path.exists(op)):
        continue
    o = Image.open(op).convert('RGB').resize((640, 448), Image.BOX)
    g = Image.open(gp).convert('RGB').resize((640, 448), Image.BOX)
    d = np.abs(np.asarray(o, np.float32) - np.asarray(g, np.float32)).mean(axis=2)
    blocks = d.reshape(56, 8, 80, 8).mean(axis=(1, 3))
    score = float(blocks.max() - np.median(blocks))
    by, bx = np.unravel_index(int(blocks.argmax()), blocks.shape)
    rows.append((score, n, int(bx) * 8, int(by) * 8, float(np.median(blocks))))
rows.sort(reverse=True)
for score, n, x, y, med in rows[:15]:
    print(f'frame {n}: score {score:.1f} at native {x},{y} (median {med:.1f})')
# Side-by-side crops of the worst frames, around the worst block, at full resolution.
for score, n, x, y, med in rows[:8]:
    o = Image.open(os.path.join(odir, f'frame_{n:05d}.ppm')).convert('RGB')
    W, H = o.size
    g = Image.open(os.path.join(gdir, f'live_frame{n:05d}.png')).convert('RGB').resize((W, H), Image.LANCZOS)
    s = W // 640
    cx, cy = x * s + 4 * s, y * s + 4 * s
    box = (max(0, cx - 200), max(0, cy - 130), max(0, cx - 200) + 400, max(0, cy - 130) + 260)
    sheet = Image.new('RGB', (800, 260))
    sheet.paste(g.crop(box), (0, 0))
    sheet.paste(o.crop(box), (400, 0))
    sheet.save(os.path.join(out, f'worst_{n}.png'))
