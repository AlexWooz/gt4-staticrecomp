#!/bin/bash
# Headless Arcade night race (scripts/frame_cost_headless.py scene "night": Arcade > Single Race > Clubman Stage Route 5,
# Ford Ka): the Arcade menu, the car showroom, the pre-race screens and the race start.
# usage: arcade-run.sh <name> <exe> [ENV=VAL ...]   (env SCALE=3 FRAMES=9800 EVERY=100000 FROM=99999 WINDOW=1)
# WINDOW=1 opens the game's window and paces it at 60 VBlanks/s, so 9800 VBlanks take 2 min 43 s: only with the owner's word.
# Writes $GT4_BENCH_OUT/arcade-<name>/. EVERY=700 FROM=700 dumps the 14 frames of the identity check.
# Timeline (VBlanks): 600 title, 900-2450 Arcade menu, 3500-4100 track, ~4100-6500 car select (showroom),
#  6650-7800 pre-race screens, 8700-9200 race start.
source "$(dirname "$0")/common.sh"
name=$1; exe=$2; shift 2
OUT=$GT4_BENCH_OUT/arcade-$name
mkdir -p "$OUT/frames"
cp "$GT4_ARCADE_CARD" "$OUT/card.ps2"
P="600:start:30 900:cross:30 1500:cross:30 2300:down:10 2450:cross:10 3500:right:10 3700:right:10 3900:cross:10 4100:cross:10 6500:down:8 6650:cross:10 7400:cross:10 7600:cross:10 7800:cross:10 8700:cross:10 9200:cross:7000"
args=(); for p in $P; do args+=(--press "$p"); done
env GT4_RENDERER=hw GT4_HW_CORE=2 GT4_FORCE_480P=1 GT4_FORCE_WIDESCREEN=1 GT4_HW_SCALE=${SCALE:-3} GT4_HW_DUMP_SOURCE=upscaled \
  GT4_HW_DUMP_DIR="$(cygpath -w "$OUT/frames")" GT4_HW_DUMP_EVERY=${EVERY:-100000} GT4_HW_DUMP_FROM=${FROM:-99999} "$@" "$exe" \
  "$(cygpath -w "$GT4_ASSETS/private/reconstructed/SCUS_973.28.elf")" --ntsc --disc "$(cygpath -w "$GT4_ASSETS/Gran Turismo 4 (USA) (v2.00).iso")" \
  --frames ${FRAMES:-9800} --memcard "$(cygpath -w "$OUT/card.ps2")" ${WINDOW:+--window} "${args[@]}" > "$OUT/log.txt" 2>&1
echo "exit $?"
