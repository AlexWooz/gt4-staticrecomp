#!/bin/bash
# A-Spec normal race, headless: the night-run scene (GT Mode, FF Challenge race 3 = Hong Kong Reverse, dusk/night, neon,
# VW Lupo 1.4, 5 AI cars) started in A-Spec instead of B-Spec. The only difference from night-run.sh is the press list:
# the 6300:right step selects the B-Spec (Director's Mode) icon on the pre-race screen, so dropping it leaves the cursor
# on Start and 6450:cross begins the normal A-Spec race, whose panel carries the tachometer and the full normal HUD.
# TASK-090 HUD A-Spec round (docs/COREGS3-GSSTREAM-CAPTURE.md, S3 status).
# usage: aspec-run.sh <name> <exe> [ENV=VAL ...]   (env SCALE=3 EVERY=600 FROM=6500 FRAMES=11500)
# Writes $GT4_BENCH_OUT/aspec-<name>/{log.txt,frames/,card.ps2}. EVERY=100000 FROM=999999 runs without frame dumps.
# Timeline (VBlanks): as night-run.sh but the race is A-Spec: ~6500 first race camera, ~7000-7300 green flag, the race
# runs past 11000.
source "$(dirname "$0")/common.sh"
name=$1; exe=$2; shift 2
OUT=$GT4_BENCH_OUT/aspec-$name
mkdir -p "$OUT/frames"
cp "$GT4_BENCH_CARD" "$OUT/card.ps2"
P="600:down:10 700:cross:30 1100:cross:10 1400:cross:10 1950:down:8 2050:down:8 2150:cross:10 2350:cross:10 2600:triangle:10 2900:triangle:10 3200:down:8 3350:right:8 3500:cross:10 3620:down:8 3700:cross:10 3900:cross:10 4900:up:8 5050:right:8 5200:right:8 5500:down:8 5650:cross:10 5850:cross:10 6450:cross:10"
args=(); for p in $P; do args+=(--press "$p"); done
env GT4_RENDERER=hw GT4_HW_CORE=2 GT4_FORCE_480P=1 GT4_FORCE_WIDESCREEN=1 GT4_HW_SCALE=${SCALE:-3} GT4_HW_DUMP_SOURCE=upscaled \
  GT4_HW_DUMP_DIR="$(cygpath -w "$OUT/frames")" GT4_HW_DUMP_EVERY=${EVERY:-600} GT4_HW_DUMP_FROM=${FROM:-6500} "$@" "$exe" \
  "$(cygpath -w "$GT4_ASSETS/private/reconstructed/SCUS_973.28.elf")" --ntsc --disc "$(cygpath -w "$GT4_ASSETS/Gran Turismo 4 (USA) (v2.00).iso")" \
  --frames ${FRAMES:-11500} --memcard "$(cygpath -w "$OUT/card.ps2")" "${args[@]}" > "$OUT/log.txt" 2>&1
echo "exit $?"
