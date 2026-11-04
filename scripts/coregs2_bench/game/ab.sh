#!/bin/bash
# Interleaved A/B of the George V Paris lap, headless. usage: ab.sh <scale> <rounds> <tag:exe[:ENV=VAL,...]> ...
# Prints the summary of the lap window at the end. Uses the worktree's build/evidence; runs named <tag><round>.
cd "$(dirname "$0")/../../.."
export GT4_BENCH_OUT=${GT4_BENCH_OUT:-$PWD/build/evidence} GT4_ASSETS=${GT4_ASSETS:-/c/Antigravity/gt4-staticrecomp}
scale=$1; rounds=$2; shift 2
while tasklist | grep -qiE "gt4vulkan|gt4\.exe|gt4-spec2"; do echo "a game is running"; exit 2; done
names=()
# A player's session reads its pipelines from the shader cache the launcher keeps (GT4_HW_SHADER_CACHE). Without one, every run
# compiled CoreGS 2.0's specialised pipelines again during the lap (93 of them, about 23 s of driver CPU on the 5700X, 2026-11-03).
# Each executable gets its own cache, filled by --prepare-shaders and one unmeasured lap the first time.
cache_of() { echo "$GT4_BENCH_OUT/shader-cache/$(sha256sum "$1" | cut -c1-16)"; }
for spec in "$@"; do
  IFS=: read -r tag exe envs <<< "$spec"
  cache=$(cache_of "$exe")
  if [ ! -f "$cache/variants.bin" ]; then
    mkdir -p "$cache"
    GT4_HW_SHADER_CACHE="$(cygpath -w "$cache")" "$exe" --prepare-shaders >/dev/null 2>&1
    SCALE=$scale bash scripts/coregs2_bench/replay-run.sh george-volta2 ${tag}-warm "$exe" GT4_HW_SHADER_CACHE="$(cygpath -w "$cache")" >/dev/null
  fi
done
for r in $(seq 1 $rounds); do
  for spec in "$@"; do
    IFS=: read -r tag exe envs <<< "$spec"
    extra=(); IFS=, read -ra extra <<< "$envs"
    SCALE=$scale bash scripts/coregs2_bench/replay-run.sh george-volta2 $tag$r "$exe" GT4_MTVU_REPORT_EVERY=600 GT4_HW_PROFILE_EVERY=600 \
      GT4_HW_SHADER_CACHE="$(cygpath -w "$(cache_of "$exe")")" "${extra[@]}" >/dev/null
  done
done
for spec in "$@"; do IFS=: read -r tag exe envs <<< "$spec"; for r in $(seq 1 $rounds); do names+=(replay-george-volta2-$tag$r); done; done
# Pipelines compiled during each run: near 0 with a warm cache.
for name in "${names[@]}"; do echo "$name $(grep -o 'compile_ms=[0-9.]*' "$GT4_BENCH_OUT/$name/log.txt" | tail -1)"; done
SUMMARY_LO=2400 SUMMARY_HI=8032 python scripts/coregs2_bench/cpu_summary.py "${names[@]}" | grep -E "^(metric|gs_cpu|process_cpu|wall|ee_critical_path|ee_thread_cpu|xe_[a-z0-9_]*|mtvu_worker_busy|mtvu_worker_cpu|mtvu_vu1_program|mtvu_output|frame_p50|frame_p95) "
