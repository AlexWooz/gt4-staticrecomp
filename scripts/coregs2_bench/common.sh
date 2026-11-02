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
