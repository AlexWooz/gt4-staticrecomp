#!/bin/bash
# Shared paths of the CoreGS 2.0 headless benches (git bash on Windows). Override by environment:
#   GT4_ASSETS       checkout holding private/ and the disc image (the main checkout)
#   GT4_BENCH_ROOT   this worktree
#   GT4_BENCH_OUT    where runs are written (ignored by git)
#   GT4_BENCH_CARD   memory card of the night race (a GT Mode save with the VW Lupo); copied per run, never written
#   GT4_ARCADE_CARD  memory card of the Arcade scene; copied per run, never written
GT4_BENCH_ROOT=${GT4_BENCH_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
GT4_ASSETS=${GT4_ASSETS:-/c/Antigravity/gt4-staticrecomp}
GT4_BENCH_OUT=${GT4_BENCH_OUT:-$GT4_BENCH_ROOT/build/evidence}
GT4_BENCH_CARD=${GT4_BENCH_CARD:-$GT4_ASSETS/private/coregs2-bench/night-card.ps2}
GT4_ARCADE_CARD=${GT4_ARCADE_CARD:-$GT4_ASSETS/private/slice110-menu-pilot/name-control4/card.ps2}

# KEEP="A-B C-D": a run that dumps every frame keeps only these windows of frame numbers (keep_frames.py). Takes the frames folder.
start_keeping_frames() {
  keep_stop=""
  if [ -z "$KEEP" ]; then return; fi
  keep_stop=$1/.keep-stop
  rm -f "$keep_stop"
  python "$(cygpath -w "$GT4_BENCH_ROOT/scripts/coregs2_bench/keep_frames.py")" "$(cygpath -w "$1")" "$(cygpath -w "$keep_stop")" $KEEP &
  keep_process=$!
}
stop_keeping_frames() {
  if [ -z "$keep_stop" ]; then return; fi
  touch "$keep_stop"
  wait "$keep_process"
  rm -f "$keep_stop"
}
