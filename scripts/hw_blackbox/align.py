"""Best integer shift (output pixels) between a gt4-gs final frame (resized to ours) and our frame."""
import sys
import numpy as np
from PIL import Image

ours = np.asarray(Image.open(sys.argv[2]).convert('L'), dtype=np.float32)
H, W = ours.shape
g = np.asarray(Image.open(sys.argv[1]).convert('L').resize((W, H), Image.LANCZOS), dtype=np.float32)
best = None
m = 24
core = ours[m:H - m, m:W - m]
for dy in range(-m, m + 1, 1):
    for dx in range(-m, m + 1, 2):
        shifted = g[m + dy:H - m + dy, m + dx:W - m + dx]
        err = float(np.mean(np.abs(shifted - core)))
        if best is None or err < best[0]:
            best = (err, dx, dy)
print('coarse', best)
err0, bx, by = best
for dy in range(by - 2, by + 3):
    for dx in range(bx - 2, bx + 3):
        shifted = g[m + dy:H - m + dy, m + dx:W - m + dx]
        err = float(np.mean(np.abs(shifted - core)))
        if err < best[0]:
            best = (err, dx, dy)
print('best mean abs diff %.3f at dx %d dy %d (gsdx pixel = ours + shift)' % best)
