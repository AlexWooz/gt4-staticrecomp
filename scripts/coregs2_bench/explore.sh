#!/bin/bash
# Tries a list of button presses and leaves a contact sheet of dumped frames: for scripting a new bench scene.
# usage: explore.sh <name> <frames> <every> "<vblank:button:length ...>"   -> $GT4_BENCH_OUT/explore-<name>/sheet.png
source "$(dirname "$0")/common.sh"
name=$1; frames=$2; every=$3; presses=$4
OUT=$GT4_BENCH_OUT/explore-$name
rm -rf "$OUT"; mkdir -p "$OUT/frames"
cp "$GT4_ARCADE_CARD" "$OUT/card.ps2"
args=(); for p in $presses; do args+=(--press "$p"); done
env GT4_RENDERER=hw GT4_HW_CORE=2 GT4_FORCE_480P=1 GT4_FORCE_WIDESCREEN=1 GT4_HW_SCALE=${EXPLORE_SCALE:-1} GT4_HW_DUMP_SOURCE=upscaled \
  GT4_HW_DUMP_DIR="$(cygpath -w "$OUT/frames")" GT4_HW_DUMP_EVERY=$every GT4_HW_DUMP_FROM=$every "${EXPLORE_EXE:-build/core2-build/gt4vulkan.exe}" \
  "$(cygpath -w "$GT4_ASSETS/private/reconstructed/SCUS_973.28.elf")" --ntsc --disc "$(cygpath -w "$GT4_ASSETS/Gran Turismo 4 (USA) (v2.00).iso")" \
  --frames "$frames" --memcard "$(cygpath -w "$OUT/card.ps2")" "${args[@]}" > "$OUT/log.txt" 2>&1
python - "$OUT" <<'EOF'
import glob, os, sys
from PIL import Image, ImageDraw
out = sys.argv[1]
files = sorted(glob.glob(os.path.join(out, 'frames', '*.ppm')))
columns, cell = 6, (256, 180)
sheet = Image.new('RGB', (columns * cell[0], ((len(files) + columns - 1) // columns) * cell[1]))
for i, path in enumerate(files):
    image = Image.open(path).convert('RGB').resize(cell)
    ImageDraw.Draw(image).text((3, 3), os.path.basename(path)[6:11], fill=(255, 255, 0))
    sheet.paste(image, ((i % columns) * cell[0], (i // columns) * cell[1]))
sheet.save(os.path.join(out, 'sheet.png'))
EOF
