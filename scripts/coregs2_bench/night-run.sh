#!/bin/bash
# Headless night race, the reference scene of the CoreGS 2.0 ledger: GT Mode, B-Spec, FF Challenge race 3 = Hong Kong
# Reverse (dusk/night, neon), VW Lupo 1.4, 5 AI cars.
# usage: night-run.sh <name> <exe> [ENV=VAL ...]   (env SCALE=3 EVERY=600 FROM=6500 FRAMES=11500)
# Writes $GT4_BENCH_OUT/night-<name>/{log.txt,frames/,card.ps2}. EVERY=100000 FROM=999999 runs without frame dumps.
# Timeline (VBlanks): 600 title, ~1000 GT Mode world map, 2150-2350 garage "Get In" Lupo, 3500-3700 Beginner Events,
#  3900 event OK, ~4000 event screen (races), 5650 Entry, ~6100 pre-race screen, 6450 B-spec (Director's Mode) icon,
#  ~6500 first race camera, ~7300 green flag (race clock 0:00), the race runs past 11000.
source "$(dirname "$0")/common.sh"
name=$1; exe=$2; shift 2
OUT=$GT4_BENCH_OUT/night-$name
mkdir -p "$OUT/frames"
cp "$GT4_BENCH_CARD" "$OUT/card.ps2"
P="600:down:10 700:cross:30 1100:cross:10 1400:cross:10 1950:down:8 2050:down:8 2150:cross:10 2350:cross:10 2600:triangle:10 2900:triangle:10 3200:down:8 3350:right:8 3500:cross:10 3620:down:8 3700:cross:10 3900:cross:10 4900:up:8 5050:right:8 5200:right:8 5500:down:8 5650:cross:10 5850:cross:10 6300:right:8 6450:cross:10"
args=(); for p in $P; do args+=(--press "$p"); done
env GT4_RENDERER=hw GT4_HW_CORE=2 GT4_FORCE_480P=1 GT4_FORCE_WIDESCREEN=1 GT4_HW_SCALE=${SCALE:-3} GT4_HW_DUMP_SOURCE=upscaled \
  GT4_HW_DUMP_DIR="$(cygpath -w "$OUT/frames")" GT4_HW_DUMP_EVERY=${EVERY:-600} GT4_HW_DUMP_FROM=${FROM:-6500} "$@" "$exe" \
  "$(cygpath -w "$GT4_ASSETS/private/reconstructed/SCUS_973.28.elf")" --ntsc --disc "$(cygpath -w "$GT4_ASSETS/Gran Turismo 4 (USA) (v2.00).iso")" \
  --frames ${FRAMES:-11500} --memcard "$(cygpath -w "$OUT/card.ps2")" "${args[@]}" > "$OUT/log.txt" 2>&1
echo "exit $?"
