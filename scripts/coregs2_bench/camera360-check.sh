#!/bin/bash
# TASK-159: the 360-degree chase camera against a build without it, on one recorded drive (docs/CAMERA-360.md).
# usage: camera360-check.sh <recording> <reference exe> <candidate exe> <dump VBlank> [ENV=VAL ...]
#   env NAME=<run prefix, default cam360> SCALE=1 EVERY=25 FROM=<first dumped frame> DYNAMICS=<0x10000 v2.00, 0x24000 Spec II>
# The reference exe is the same game built from the plain translated units; the candidate is built from the units of
# scripts/camera360_units.py. Three headless replays of the recording, each with the race clock of every VBlank (frame skip log,
# no frame left out), the simulation state and the race object at <dump VBlank>, and frames from FROM every EVERY:
#   ref  the reference;
#   off  the candidate with GT4_CAMERA_360 unset: frames, clock and state must equal the reference (the hook does nothing);
#   on   the candidate with GT4_CAMERA_360=1: the race clock must equal the reference at every VBlank; the state words that
#        differ are listed (the camera's own words while the stick is off rest), and so are the frames that differ.
# Exit code 1 when `off` differs from `ref` in anything, or the clock of `on` differs anywhere.
here=$(cd "$(dirname "$0")" && pwd)
source "$here/common.sh"
recording=$1; ref=$2; cand=$3; dump_at=$4; shift 4
name=${NAME:-cam360}; dynamics=${DYNAMICS:-0x10000}
folder() { echo "$GT4_BENCH_OUT/replay-$recording-$name-$1"; }
run() {  # <run> <exe> [ENV=VAL ...]
  local run=$1 exe=$2; shift 2
  rm -rf "$(folder "$run")"
  mkdir -p "$(folder "$run")"
  SCALE=${SCALE:-1} EVERY=${EVERY:-25} FROM=${FROM:-1} bash "$here/replay-run.sh" "$recording" "$name-$run" "$exe" GT4_FRAME_SKIP_FORCE=1 \
    GT4_FRAME_SKIP_LOG="$(cygpath -w "$(folder "$run")/skip.csv")" GT4_FRAME_SKIP_DUMP_AT=$dump_at \
    GT4_FRAME_SKIP_DUMP_FILE="$(cygpath -w "$(folder "$run")/state.bin")" "$@" | tail -1
}
run ref "$ref" "$@"
run off "$cand" "$@"
run on "$cand" GT4_CAMERA_360=1 "$@"
grep -a "\[camera360\]" "$(folder on)/log.txt" | head -3
wrong=0
echo "== off against ref (the hook present, the switch off)"
python - "$(cygpath -w "$(folder ref)")" "$(cygpath -w "$(folder off)")" "$dynamics" <<'EOF' || wrong=1
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
words = np.nonzero(sa != sb)[0] * 4 if sa.shape == sb.shape else None
print(f'VBlanks {len(both)}, racing {len(racing)}, clock differs at {len(differ)}; state {sa.nbytes} bytes, '
      f'{"size differs" if words is None else str(len(words)) + " words differ"}')
sys.exit(0 if racing and not differ and words is not None and len(words) == 0 else 1)
EOF
python "$here/frame_diff.py" "$(basename "$(folder ref)")" "$(basename "$(folder off)")" 2>&1 | tail -2 || wrong=1
echo "== on against ref (the camera on)"
python - "$(cygpath -w "$(folder ref)")" "$(cygpath -w "$(folder on)")" "$dynamics" <<'EOF' || wrong=1
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
print(f'VBlanks {len(both)}, racing {len(racing)}, race clock differs at {len(differ)}' + (f', first {differ[:5]}' if differ else ''))
sa = np.fromfile(a_dir + '\\state.bin', dtype=np.uint32); sb = np.fromfile(b_dir + '\\state.bin', dtype=np.uint32)
words = np.nonzero(sa != sb)[0] * 4
sim = [int(w) for w in words if w < dynamics]
obj = [int(w) - dynamics for w in words if w >= dynamics]
print(f'simulation state: {len(sim)} of {dynamics // 4} words differ: ' + ' '.join(hex(w) for w in sim[:40]))
print(f'race object: {len(obj)} words differ' + (': ' + ' '.join(hex(w) for w in obj[:24]) if obj else ''))
sys.exit(0 if racing and not differ else 1)
EOF
diffs=$(python "$here/frame_diff.py" "$(basename "$(folder ref)")" "$(basename "$(folder on)")" 2>&1)
echo "$diffs" | tail -1
echo "frames of on that differ from ref: $(echo "$diffs" | grep '^frame_' | awk '{print $1}' | sed 's/frame_0*//; s/.ppm//' | tr '\n' ' ')"
[ $wrong = 0 ] && echo CAMERA360-CHECK-OK || echo CAMERA360-CHECK-WRONG
exit $wrong
