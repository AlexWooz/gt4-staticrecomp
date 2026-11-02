"""Compare our upscaled frames with gt4-gs.exe final frames (black-box reference, same capture and frame).

usage: gsdiff.py <gsdx png> <ours ppm> [<ours2 ppm>] <out prefix>
Writes <out>_heat.png (block difference map) and <out>_crops.png (the top regions: GSdx | ours | ours2).
"""
import sys
from PIL import Image, ImageChops, ImageDraw

args = sys.argv[1:]
gsdx_path, ours_path = args[0], args[1]
ours2_path = args[2] if len(args) == 4 else None
out = args[-1]

ours = Image.open(ours_path).convert('RGB')
W, H = ours.size
gsdx = Image.open(gsdx_path).convert('RGB').resize((W, H), Image.BILINEAR)
ours2 = Image.open(ours2_path).convert('RGB') if ours2_path else None

diff = ImageChops.difference(gsdx, ours).convert('L')
B = 24
bw, bh = W // B, H // B
small = diff.resize((bw, bh), Image.BOX)
data = list(small.getdata())
heat = small.point(lambda v: min(255, v * 6)).resize((bw * 8, bh * 8), Image.NEAREST)
heat.save(out + '_heat.png')
mean = sum(data) / len(data)
print(f'mean block diff {mean:.2f}')
# Top blocks, merged greedily so crops do not repeat.
order = sorted(range(len(data)), key=lambda i: -data[i])
chosen = []
for i in order:
    bx, by = i % bw, i // bw
    if any(abs(bx - cx) < 6 and abs(by - cy) < 5 for cx, cy in chosen):
        continue
    chosen.append((bx, by))
    if len(chosen) == 6:
        break
cw, ch = 240, 160
cols = 3 if ours2 else 2
sheet = Image.new('RGB', (cw * cols, ch * len(chosen)))
for k, (bx, by) in enumerate(chosen):
    cx = min(max(bx * B + B // 2 - cw // 2, 0), W - cw)
    cy = min(max(by * B + B // 2 - ch // 2, 0), H - ch)
    box = (cx, cy, cx + cw, cy + ch)
    sheet.paste(gsdx.crop(box), (0, k * ch))
    sheet.paste(ours.crop(box), (cw, k * ch))
    if ours2:
        sheet.paste(ours2.crop(box), (2 * cw, k * ch))
    print(f'region {k}: x {cx}..{cx + cw} y {cy}..{cy + ch} (native {cx // 3},{cy // 3}) block diff {data[by * bw + bx]}')
sheet.save(out + '_crops.png')
