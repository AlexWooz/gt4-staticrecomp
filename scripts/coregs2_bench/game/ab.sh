#!/bin/bash
# Interleaved A/B of the George V Paris lap, headless. usage: ab.sh <scale> <rounds> <tag:exe[:ENV=VAL,...]> ...
# Prints the summary of the lap window at the end. Uses the worktree's build/evidence; runs named <tag><round>.
cd "$(dirname "$0")/../../.."
export GT4_BENCH_OUT=${GT4_BENCH_OUT:-$PWD/build/evidence} GT4_ASSETS=${GT4_ASSETS:-/c/Antigravity/gt4-staticrecomp}
scale=$1; rounds=$2; shift 2
while tasklist | grep -qiE "gt4vulkan|gt4\.exe|gt4-spec2"; do echo "a game is running"; exit 2; done
names=()
# A new executable compiles its shader cache in the first session, which would load the first run: prepare them first.
for spec in "$@"; do IFS=: read -r tag exe envs <<< "$spec"; "$exe" --prepare-shaders >/dev/null 2>&1; done
for r in $(seq 1 $rounds); do
  for spec in "$@"; do
    IFS=: read -r tag exe envs <<< "$spec"
    extra=(); IFS=, read -ra extra <<< "$envs"
    SCALE=$scale bash scripts/coregs2_bench/replay-run.sh george-volta2 $tag$r "$exe" GT4_MTVU_REPORT_EVERY=600 GT4_HW_PROFILE_EVERY=600 "${extra[@]}" >/dev/null
    [ $r = 1 ] && true
  done
done
for spec in "$@"; do IFS=: read -r tag exe envs <<< "$spec"; for r in $(seq 1 $rounds); do names+=(replay-george-volta2-$tag$r); done; done
SUMMARY_LO=2400 SUMMARY_HI=8032 python scripts/coregs2_bench/cpu_summary.py "${names[@]}" | grep -E "^(metric|gs_cpu|process_cpu|wall|ee_critical_path|ee_thread_cpu|xe_[a-z0-9_]*|mtvu_worker_busy|mtvu_worker_cpu|mtvu_vu1_program|mtvu_output|frame_p50|frame_p95) "
