#!/bin/bash
# TASK-082: the frame skip suite on v2.00, headless: the night race of night-run.sh (Hong Kong Reverse, 5 AI cars, B-Spec), driven by its
# --press list. As frame-skip-suite.sh, from a race with more cars on the other target.
# usage: frame-skip-suite-v200.sh <exe> [ENV=VAL ...]   env FROM=8000 LAST=8360 SCALE=1 PATTERNS="2 3" DUMP_AT=11400 FRAMES=11500 ALLOW_WORDS=80
here=$(cd "$(dirname "$0")" && pwd)
source "$here/common.sh"
exe=$1; shift
from=${FROM:-8000}; last=${LAST:-8360}; scale=${SCALE:-1}; dump_at=${DUMP_AT:-11400}; frames=${FRAMES:-11500}
out() { echo "$GT4_BENCH_OUT/night-$1"; }
run() {  # <run name> <pattern> <dump frames from> <frames> [ENV=VAL ...]
  local name=$1 pattern=$2 dump=$3 end=$4; shift 4
  rm -rf "$(out "$name")"
  mkdir -p "$(out "$name")"
  SCALE=$scale EVERY=1 FROM=$dump FRAMES=$end bash "$here/night-run.sh" "$name" "$exe" \
    GT4_FRAME_SKIP_FORCE=$pattern GT4_FRAME_SKIP_LOG="$(cygpath -w "$(out "$name")/skip.csv")" GT4_FRAME_SKIP_DUMP_AT=$dump_at \
    GT4_FRAME_SKIP_DUMP_FILE="$(cygpath -w "$(out "$name")/state.bin")" "$@" | tail -1
  grep -a "frame-skip\] rendered\|stopped after" "$(out "$name")/log.txt" | tail -2 | cut -c1-120
}
wrong=0
echo "== the whole race: the simulation"
run fs-sim-1 1 99999999 $frames "$@"
for pattern in ${PATTERNS:-2 3}; do
  run fs-sim-$pattern $pattern 99999999 $frames "$@"
  python "$here/frame_skip_check.py" "$(out fs-sim-1)/skip.csv" "$(out fs-sim-$pattern)/skip.csv" "$(out fs-sim-1)/state.bin" \
    "$(out fs-sim-$pattern)/state.bin" --dynamics-bytes 0x10000 --allow-words ${ALLOW_WORDS:-80} || wrong=1
done
echo "== every frame of VBlanks $from-$last"
run fs-frames-1 1 $from $last "$@"
run fs-frames-1b 1 $from $last "$@"
for pattern in ${PATTERNS:-2 3}; do
  run fs-frames-$pattern $pattern $from $last "$@"
  python "$here/frame_sheet.py" "$(out fs-frames-1)/frames" "$(out fs-frames-$pattern)/frames" "$(out fs-frames-$pattern)/skip.csv" \
    "$GT4_BENCH_OUT/sheet-v200-$pattern" --noise "$(out fs-frames-1b)/frames" || wrong=1
done
[ $wrong = 0 ] && echo SUITE-OK || echo SUITE-WRONG
exit $wrong
