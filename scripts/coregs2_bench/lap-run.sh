#!/bin/bash
# IN-520: one windowed bench of a recorded drive: menus without waiting, the drive in real time, graph and figures at the end.
# usage: lap-run.sh <run name> [ENV=VAL ...]   (env RECORDING=george-volta2 PACED_FROM=1990 SCALE=6 EXE=<exe> FROM_FIGURES=2040)
# The default recording is the owner's time trial on George V Paris, Spec II: the race draws from VBlank 2034, a lap of 1'23.394.
source "$(dirname "$0")/common.sh"
run=$1; shift
recording=${RECORDING:-george-volta2}
exe=${EXE:-$(cygpath -m "$GT4_BENCH_ROOT/build/spec2-core2-build/gt4vulkan-spec2.exe")}
if tasklist | grep -qi -E "gt4\.exe|gt4-spec2|gt4vulkan"; then echo "a game is running: close it first"; exit 2; fi
WINDOW=1 PACED_FROM=${PACED_FROM:-1990} SCALE=${SCALE:-6} bash "$(dirname "$0")/replay-run.sh" "$recording" "$run" "$exe" GT4_FULLSCREEN=1 \
  GT4_HW_PROFILE_EVERY=600 GT4_CORE2_SPIKE_MS=0.01 GT4_CORE2_SPIKE_LINES=9000 "$@"
out=$GT4_BENCH_OUT/replay-$recording-$run
python "$(dirname "$0")/frametime_plot.py" "$(cygpath -w "$out/frametime.png")" "$run=$(cygpath -w "$out/frametime.csv"):${FROM_FIGURES:-2040}:99999999"
grep -E "\[hw\] flare table" "$out/log.txt" | tail -1 | cut -c1-130
echo "graph: $out/frametime.png"
