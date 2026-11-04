#!/bin/bash
# TASK-095: the 2D overlay canvas hooks against a build without them, on one recorded drive (docs/ULTRAWIDE-OVERLAYS.md).
# usage: overlay-aspect-check.sh <recording> <reference exe> <candidate exe> <dump VBlank> [ENV=VAL ...]
#   env NAME=<run prefix, default t095> SCALE=1 EVERY=25 FROM=<first dumped frame> DYNAMICS=<0x10000 v2.00, 0x24000 Spec II>
#       RATIOS="21:9 32:9"
# The reference exe is the same game built from the units without the overlay calls; the candidate from the units of
# scripts/camera360_units.py with them. Headless replays of the recording, each with the race clock of every VBlank (frame skip
# log, no frame left out), the simulation state and the race object at <dump VBlank>, and frames from FROM every EVERY:
#   ref-169, cand-169   at 16:9 (the replay default): clock, state and frames must be identical (the hooks change nothing);
#   ref-<R>, cand-<R>   at each ratio of RATIOS (GT4_DISPLAY_ASPECT): the race clock must be identical at every VBlank and so
#                       must the state (a canvas never reaches the race); the frames that differ are listed, and
#                       overlay_geometry.py checks that the candidate's overlay is the reference's narrowed by s = R / (16/9)
#                       around the frame's centre.
# Exit code 1 when the 16:9 runs differ in anything, or a clock or the state differs at another ratio.
here=$(cd "$(dirname "$0")" && pwd)
source "$here/common.sh"
recording=$1; ref=$2; cand=$3; dump_at=$4; shift 4
name=${NAME:-t095}; dynamics=${DYNAMICS:-0x10000}; ratios=${RATIOS:-21:9 32:9}
folder() { echo "$GT4_BENCH_OUT/replay-$recording-$name-$1"; }
run() {  # <run> <exe> [ENV=VAL ...]
  local run=$1 exe=$2; shift 2
  rm -rf "$(folder "$run")"
  mkdir -p "$(folder "$run")"
  SCALE=${SCALE:-1} EVERY=${EVERY:-25} FROM=${FROM:-1} bash "$here/replay-run.sh" "$recording" "$name-$run" "$exe" GT4_FRAME_SKIP_FORCE=1 \
    GT4_FRAME_SKIP_LOG="$(cygpath -w "$(folder "$run")/skip.csv")" GT4_FRAME_SKIP_DUMP_AT=$dump_at \
    GT4_FRAME_SKIP_DUMP_FILE="$(cygpath -w "$(folder "$run")/state.bin")" "$@" | tail -1
}
same_race() {  # <run A> <run B>: clock of every VBlank and the state at the dump VBlank
  python - "$(cygpath -w "$(folder "$1")")" "$(cygpath -w "$(folder "$2")")" "$dynamics" <<'EOF'
import csv, sys
import numpy as np
a_dir, b_dir, dynamics = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
def clocks(folder):
    with open(folder + '\\skip.csv', newline='') as f:
        return {int(r['vblank']): r['clock'] for r in csv.DictReader(f)}
a, b = clocks(a_dir), clocks(b_dir)
both = sorted(set(a) & set(b))
racing = [v for v in both if a[v] != '0' or b[v] != '0']
differ = [v for v in both if a[v] != b[v]]
sa = np.fromfile(a_dir + '\\state.bin', dtype=np.uint32); sb = np.fromfile(b_dir + '\\state.bin', dtype=np.uint32)
words = np.nonzero(sa != sb)[0] * 4 if sa.shape == sb.shape and sa.size else None
detail = 'no state dump' if words is None else f'{len(words)} state words differ' + (
    ' (' + ' '.join(hex(int(w)) for w in words[:12]) + ')' if len(words) else '')
print(f'VBlanks {len(both)}, racing {len(racing)}, clock differs at {len(differ)}; state {sa.nbytes} bytes, {detail}')
sys.exit(0 if both and not differ and words is not None and len(words) == 0 else 1)
EOF
}
wrong=0
run ref-169 "$ref" "$@"
run cand-169 "$cand" "$@"
echo "== 16:9: candidate against reference (must be identical)"
same_race ref-169 cand-169 || wrong=1
python "$here/frame_diff.py" "$(basename "$(folder ref-169)")" "$(basename "$(folder cand-169)")" 2>&1 | tail -1 || wrong=1
grep -a "\[aspect\] overlay" "$(folder cand-169)/log.txt" | head -3
for ratio in $ratios; do
  tag=${ratio/:/x}
  run ref-$tag "$ref" GT4_DISPLAY_ASPECT=$ratio "$@"
  run cand-$tag "$cand" GT4_DISPLAY_ASPECT=$ratio "$@"
  echo "== $ratio: candidate against reference (the race identical, the overlay frames narrowed)"
  same_race ref-$tag cand-$tag || wrong=1
  grep -a "\[aspect\] \(presentation\|overlay\)" "$(folder cand-$tag)/log.txt" | head -12
  diffs=$(python "$here/frame_diff.py" "$(basename "$(folder ref-$tag)")" "$(basename "$(folder cand-$tag)")" 2>&1)
  echo "$diffs" | tail -1
  echo "frames that differ: $(echo "$diffs" | grep '^frame_' | awk '{print $1}' | sed 's/frame_0*//; s/.ppm//' | tr '\n' ' ')"
  python "$here/overlay_geometry.py" "$(folder ref-$tag)/frames" "$(folder cand-$tag)/frames" "$ratio" || wrong=1
done
[ $wrong = 0 ] && echo OVERLAY-ASPECT-CHECK-OK || echo OVERLAY-ASPECT-CHECK-WRONG
exit $wrong
