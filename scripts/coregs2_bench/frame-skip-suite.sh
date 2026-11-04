#!/bin/bash
# TASK-082: the frame skip suite, headless, on a recorded drive (Spec II) from replay-run.sh: the simulation over the whole drive, then
# every frame of a stretch against the same stretch with nothing left out.
# usage: frame-skip-suite.sh <exe> [ENV=VAL ...]
#   env RECORDING=george-volta2 FROM=3000 LAST=3360 SCALE=1 PATTERNS="2 3" DUMP_AT=8000 DYNAMICS=0x24000 ALLOW_WORDS=24
# Writes $GT4_BENCH_OUT/replay-<recording>-fs-*/ and sheet-<pattern>/ (frames.csv, sheet.png, summary.txt). Exit code 1 on a wrong result.
here=$(cd "$(dirname "$0")" && pwd)
source "$here/common.sh"
exe=$1; shift
recording=${RECORDING:-george-volta2}
from=${FROM:-3000}; last=${LAST:-3360}; scale=${SCALE:-1}; dump_at=${DUMP_AT:-8000}; dynamics=${DYNAMICS:-0x24000}
out() { echo "$GT4_BENCH_OUT/replay-$recording-$1"; }
run() {  # <run name> <pattern> <dump frames from> <last or empty> [ENV=VAL ...]
  local name=$1 pattern=$2 dump=$3 end=$4; shift 4
  rm -rf "$(out "$name")"
  mkdir -p "$(out "$name")"
  SCALE=$scale EVERY=1 FROM=$dump LAST=$end bash "$here/replay-run.sh" "$recording" "$name" "$exe" \
    GT4_FRAME_SKIP_FORCE=$pattern GT4_FRAME_SKIP_LOG="$(cygpath -w "$(out "$name")/skip.csv")" GT4_FRAME_SKIP_DUMP_AT=$dump_at \
    GT4_FRAME_SKIP_DUMP_FILE="$(cygpath -w "$(out "$name")/state.bin")" "$@" | tail -1
  grep -a "frame-skip\] rendered\|stopped after" "$(out "$name")/log.txt" | tail -2 | cut -c1-120
}
wrong=0
echo "== the whole drive: the simulation"
run fs-sim-1 1 99999999 "" "$@"
for pattern in ${PATTERNS:-2 3}; do
  run fs-sim-$pattern $pattern 99999999 "" "$@"
  python "$here/frame_skip_check.py" "$(out fs-sim-1)/skip.csv" "$(out fs-sim-$pattern)/skip.csv" "$(out fs-sim-1)/state.bin" \
    "$(out fs-sim-$pattern)/state.bin" --dynamics-bytes $dynamics --allow-words ${ALLOW_WORDS:-24} || wrong=1
done
echo "== every frame of VBlanks $from-$last"
run fs-frames-1 1 $from $last "$@"
run fs-frames-1b 1 $from $last "$@"
for pattern in ${PATTERNS:-2 3}; do
  run fs-frames-$pattern $pattern $from $last "$@"
  python "$here/frame_sheet.py" "$(out fs-frames-1)/frames" "$(out fs-frames-$pattern)/frames" "$(out fs-frames-$pattern)/skip.csv" \
    "$GT4_BENCH_OUT/sheet-$pattern" --noise "$(out fs-frames-1b)/frames" || wrong=1
done
[ $wrong = 0 ] && echo SUITE-OK || echo SUITE-WRONG
exit $wrong
