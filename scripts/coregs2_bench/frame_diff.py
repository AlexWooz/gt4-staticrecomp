"""Frame identity between two bench runs: differing pixels, largest channel difference and box per dumped frame.

usage: frame_diff.py <run A> <run B> [--delete-identical]   (folders under GT4_BENCH_OUT, e.g. night-a arcade-b)
--delete-identical removes run B's identical frames (a 12x frame is 124 MB). Since candidate 335 two runs of one build
are identical at 3x and 12x, so a difference between candidates is real. Exit code 1 when a frame differs.
"""
import os
import sys
from pathlib import Path

import numpy as np
from PIL import Image

root = Path(os.environ.get('GT4_BENCH_OUT', Path(__file__).resolve().parents[2] / 'build' / 'evidence'))
args = [a for a in sys.argv[1:] if not a.startswith('--')]
delete = '--delete-identical' in sys.argv
a_dir, b_dir = root / args[0] / 'frames', root / args[1] / 'frames'
same = differ = 0
for frame in sorted(a_dir.glob('*.ppm')):
    other = b_dir / frame.name
    if not other.exists():
        print(f'{frame.name} missing in {args[1]}')
        differ += 1
        continue
    a = np.asarray(Image.open(frame).convert('RGB')).astype(np.int16)
    b = np.asarray(Image.open(other).convert('RGB')).astype(np.int16)
    if a.shape != b.shape:
        print(f'{frame.name} size differs {a.shape} {b.shape}')
        differ += 1
        continue
    d = np.abs(a - b).max(axis=2)
    count = int((d > 0).sum())
    if count == 0:
        same += 1
        if delete:
            other.unlink()
        continue
    differ += 1
    ys, xs = np.nonzero(d)
    print(f'{frame.name} {count} px max {int(d.max())} box x {xs.min()}..{xs.max()} y {ys.min()}..{ys.max()}')
print(f'{args[1]} against {args[0]}: {same} identical, {differ} differ')
sys.exit(1 if differ else 0)
