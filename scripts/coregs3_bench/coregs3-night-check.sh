#!/bin/bash
# TASK-090 CoreGS 3.0, phase 1: the identity gate on the scripted night race (the plan's P1-B car scene), at the scale the
# bench documents as deterministic (3x).
#
#   coregs3-night-check.sh <reference exe> <candidate exe> [ENV=VAL ...]
#
# Three headless runs of scripts/coregs2_bench/night-run.sh with the frame-skip log (race clock and simulation digest per
# VBlank), the state dump at DUMP_AT, and frames from FROM every EVERY:
#   ref     the reference build (plain units, no hooks);
#   off     the candidate with GT4_COREGS3=off;
#   shadow  the candidate with GT4_COREGS3=shadow (capturing, drawing nothing).
# Then: clock, digest and state identity for both, and frame identity at 3x. Exit code 1 on any difference.
#
# env: NAME=<run prefix, default c3n> SCALE=3 EVERY=200 FROM=6500 FRAMES=9000 DUMP_AT=7400
#      CAPTURE_VBLANKS=<a-b, default 7300-7310>
here=$(cd "$(dirname "$0")" && pwd)
bench=$(cd "$here/../coregs2_bench" && pwd)
source "$bench/common.sh"
ref=$1; cand=$2; shift 2
name=${NAME:-c3n}
scale=${SCALE:-3}
dump_at=${DUMP_AT:-7400}
frames=${FRAMES:-9000}
folder() { echo "$GT4_BENCH_OUT/night-$name-$1"; }
run() {  # <run> <exe> [ENV=VAL ...]
  local run=$1 exe=$2; shift 2
  rm -rf "$(folder "$run")"
  mkdir -p "$(folder "$run")"
  SCALE=$scale EVERY=${EVERY:-200} FROM=${FROM:-6500} FRAMES=$frames \
    GT4_FRAME_SKIP_FORCE=1 \
    GT4_FRAME_SKIP_LOG="$(cygpath -w "$(folder "$run")/skip.csv")" \
    GT4_FRAME_SKIP_DUMP_AT=$dump_at \
    GT4_FRAME_SKIP_DUMP_FILE="$(cygpath -w "$(folder "$run")/state.bin")" \
    bash "$bench/night-run.sh" "$name-$run" "$exe" "$@" | tail -1
}
run ref "$ref"
run off "$cand" GT4_COREGS3=off
run shadow "$cand" GT4_COREGS3=shadow \
  GT4_COREGS3_CAPTURE="$(cygpath -w "$(folder shadow)/capture.gt4scene")" \
  GT4_COREGS3_CAPTURE_VBLANKS=${CAPTURE_VBLANKS:-7300-7310}

wrong=0
compare() {  # <a> <b> <label>
  python - "$(cygpath -w "$(folder "$1")")" "$(cygpath -w "$(folder "$2")")" "$3" <<'EOF' || wrong=1
import csv, sys
import numpy as np
a_dir, b_dir, label = sys.argv[1], sys.argv[2], sys.argv[3]
def rows(folder):
    with open(folder + '\\skip.csv', newline='') as f:
        return {int(r['vblank']): (r['clock'], r['digest']) for r in csv.DictReader(f)}
a, b = rows(a_dir), rows(b_dir)
both = sorted(set(a) & set(b))
racing = [v for v in both if a[v][0] != '0' or b[v][0] != '0']
clock = [v for v in both if a[v][0] != b[v][0]]
digest = [v for v in both if a[v][1] != b[v][1]]
sa = np.fromfile(a_dir + '\\state.bin', dtype=np.uint32); sb = np.fromfile(b_dir + '\\state.bin', dtype=np.uint32)
words = np.nonzero(sa != sb)[0] * 4 if sa.shape == sb.shape else None
print(f'{label}: VBlanks {len(both)}, racing {len(racing)}, clock differs {len(clock)}, digest differs {len(digest)}, '
      f'state {"size differs" if words is None else str(len(words)) + " words differ"}')
if clock[:5]: print(f'  first clock differences at {clock[:5]}')
if digest[:5]: print(f'  first digest differences at {digest[:5]}')
sys.exit(0 if racing and not clock and not digest and words is not None and len(words) == 0 else 1)
EOF
}
compare ref off "off against ref"
compare ref shadow "shadow against ref"
diffs=$(python "$bench/frame_diff.py" "$(basename "$(folder ref)")" "$(basename "$(folder off)")" 2>&1)
echo "off frames: $(echo "$diffs" | tail -1)"
echo "$diffs" | grep -q '^frame_' && wrong=1
diffs=$(python "$bench/frame_diff.py" "$(basename "$(folder ref)")" "$(basename "$(folder shadow)")" 2>&1)
echo "shadow frames: $(echo "$diffs" | tail -1)"
echo "$diffs" | grep -q '^frame_' && wrong=1
capture=$(folder shadow)/capture.gt4scene
if [ -f "$capture" ]; then
  echo "capture: $(stat -c %s "$capture") bytes at $capture"
else
  echo "capture: MISSING (GT4_COREGS3=shadow wrote no file)"; wrong=1
fi
[ $wrong = 0 ] && echo COREGS3-NIGHT-CHECK-OK || echo COREGS3-NIGHT-CHECK-WRONG
exit $wrong
