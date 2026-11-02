#!/bin/bash
# TASK-134: proves a candidate leaves the game itself unchanged. The owner's recorded lap (George V Paris, Spec II), headless,
# against a reference executable: the race clock at every VBlank, the whole simulation state at one VBlank (byte for byte; a
# wrong word spreads through the cars within seconds), the EE cycle count at the end, and frames.
# usage: game_identity.sh <reference exe> <candidate exe> [ENV=VAL ...]    (env SCALE=1 EVERY=250 FROM=2200 DUMP_AT=8000 NAME=c)
# A reference run is kept and reused while its folder exists (REFERENCE=<name>, default ref). Exit code 1 on any difference.
here=$(cd "$(dirname "$0")" && pwd)
source "$here/common.sh"
ref=$1; cand=$2; shift 2
recording=${RECORDING:-george-volta2}
reference=${REFERENCE:-ref}
name=${NAME:-cand}
scale=${SCALE:-1}; every=${EVERY:-250}; from=${FROM:-2200}; dump_at=${DUMP_AT:-8000}; dynamics=${DYNAMICS:-0x24000}
folder() { echo "$GT4_BENCH_OUT/replay-$recording-$1"; }
run() {  # <run name> <exe> [ENV=VAL ...]
  local run=$1 exe=$2; shift 2
  rm -rf "$(folder "$run")"
  mkdir -p "$(folder "$run")"
  SCALE=$scale EVERY=$every FROM=$from bash "$here/replay-run.sh" "$recording" "$run" "$exe" GT4_FRAME_SKIP_FORCE=1 \
    GT4_FRAME_SKIP_LOG="$(cygpath -w "$(folder "$run")/skip.csv")" GT4_FRAME_SKIP_DUMP_AT=$dump_at \
    GT4_FRAME_SKIP_DUMP_FILE="$(cygpath -w "$(folder "$run")/state.bin")" "$@" | tail -1
}
if [ ! -f "$(folder "$reference")/state.bin" ]; then run "$reference" "$ref"; fi
run "$name" "$cand" "$@"
python "$here/frame_skip_check.py" "$(folder "$reference")/skip.csv" "$(folder "$name")/skip.csv" "$(folder "$reference")/state.bin" \
  "$(folder "$name")/state.bin" --dynamics-bytes $dynamics --allow-words 0
wrong=$?
if ! cmp -s "$(folder "$reference")/state.bin" "$(folder "$name")/state.bin"; then echo "state dump differs"; wrong=1; else echo "state dump identical ($(wc -c < "$(folder "$name")/state.bin") bytes)"; fi
python "$here/frame_diff.py" "$(folder "$reference")" "$(folder "$name")" 2>&1 | tail -2 || wrong=1
[ $wrong = 0 ] && echo IDENTITY-OK || echo IDENTITY-WRONG
exit $wrong
