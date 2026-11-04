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

# Recorded drives (IN-520). A recording `inputs/<name>.txt` may have `inputs/<name>.setup` beside it: `game=gt4|spec2`,
# `card=<path under GT4_ASSETS>` and `card_sha256=<hash>`, written by record-input.sh. Without it the recording is a Spec II
# drive from the Arcade card (the first recordings).
game_files() {  # <gt4|spec2>: sets GAME_ELF and GAME_ISO
  case $1 in
    gt4) GAME_ELF=$GT4_ASSETS/private/reconstructed/SCUS_973.28.elf
         GAME_ISO=${GT4_V200_ISO:-$GT4_ASSETS/Gran Turismo 4 (USA) (v2.00).iso} ;;
    spec2) GAME_ELF=$GT4_ASSETS/private/slice272-spec2-elf/SCUS_974.36
           GAME_ISO=${GT4_SPEC2_ISO:-$GT4_ASSETS/Gran Turismo 4 Spec II v1.10 (PTBR).iso} ;;
    *) echo "unknown game '$1': gt4 or spec2"; return 2 ;;
  esac
}
recording_setup() {  # <recording name>: sets REC_GAME, REC_CARD, GAME_ELF and GAME_ISO; fails when the card changed since
  local setup=$GT4_ASSETS/private/coregs2-bench/inputs/$1.setup want have
  REC_GAME=spec2
  REC_CARD=$GT4_ARCADE_CARD
  if [ -e "$setup" ]; then
    REC_GAME=$(sed -n 's/^game=//p' "$setup")
    REC_CARD=$GT4_ASSETS/$(sed -n 's/^card=//p' "$setup")
    want=$(sed -n 's/^card_sha256=//p' "$setup")
    have=$(sha256sum "$REC_CARD" | cut -d' ' -f1)
    if [ "$want" != "$have" ]; then echo "the card $REC_CARD is not the one recorded with (sha256 $have, want $want)"; return 2; fi
  fi
  game_files "$REC_GAME"
}
