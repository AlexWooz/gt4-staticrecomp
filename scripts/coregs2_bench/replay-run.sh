#!/bin/bash
# IN-520: replays a recording from record-input.sh on Spec II, from the same card, up to its last VBlank.
# usage: replay-run.sh <recording name> <run name> <exe> [ENV=VAL ...]
#   env SCALE=6 EVERY=100000 FROM=999999 WINDOW=1 (a window; fullscreen with GT4_FULLSCREEN=1) PACED_FROM=<VBlank> (no waiting before it)
# Writes $GT4_BENCH_OUT/replay-<recording>-<run>/: log.txt, frames/, and frametime.csv with a window.
source "$(dirname "$0")/common.sh"
name=$1; run=$2; exe=$3; shift 3
recording=$GT4_ASSETS/private/coregs2-bench/inputs/$name.txt
last=$(grep '^end ' "$recording" | cut -d' ' -f2)
if [ -z "$last" ]; then echo "no finished recording at $recording"; exit 2; fi
GT4_SPEC2_ISO=${GT4_SPEC2_ISO:-$GT4_ASSETS/Gran Turismo 4 Spec II v1.10 (PTBR).iso}
OUT=$GT4_BENCH_OUT/replay-$name-$run
mkdir -p "$OUT/frames"
cp "$GT4_ARCADE_CARD" "$OUT/card.ps2"
extra=()
if [ -n "$WINDOW" ]; then extra+=("GT4_HW_FRAMETIME_LOG=$(cygpath -w "$OUT/frametime.csv")"); fi
if [ -n "$PACED_FROM" ]; then extra+=("GT4_PACING_FROM_VBLANK=$PACED_FROM"); fi
env GT4_RENDERER=hw GT4_HW_CORE=2 GT4_FORCE_480P=1 GT4_FORCE_WIDESCREEN=1 GT4_DISPLAY_ASPECT=16:9 GT4_HW_SCALE=${SCALE:-6} GT4_HW_DUMP_SOURCE=upscaled \
  GT4_HW_DUMP_DIR="$(cygpath -w "$OUT/frames")" GT4_HW_DUMP_EVERY=${EVERY:-100000} GT4_HW_DUMP_FROM=${FROM:-999999} \
  GT4_INPUT_REPLAY="$(cygpath -w "$recording")" "${extra[@]}" "$@" "$exe" \
  "$(cygpath -w "$GT4_ASSETS/private/slice272-spec2-elf/SCUS_974.36")" --ntsc --disc "$(cygpath -w "$GT4_SPEC2_ISO")" \
  --frames "$last" --memcard "$(cygpath -w "$OUT/card.ps2")" ${WINDOW:+--window} > "$OUT/log.txt" 2>&1
echo "exit $?"
rm -f "$OUT/card.ps2"
