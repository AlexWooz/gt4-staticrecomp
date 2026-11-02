#!/bin/bash
# Interleaved night race runs for A/B measurements (one variable per candidate, nothing else busy on the machine).
# usage: series.sh <tag:exe[:ENV=VAL,ENV=VAL]> ...   (env SCALE=3; exe relative to this worktree or absolute without a
# drive colon, e.g. /c/...: the colon separates the fields)
# Example, 2+2: series.sh a1:build/a/gt4vulkan.exe b1:build/b/gt4vulkan.exe a2:build/a/gt4vulkan.exe b2:build/b/gt4vulkan.exe
# Then: python scripts/coregs2_bench/cpu_summary.py a1 a2 b1 b2
here=$(cd "$(dirname "$0")" && pwd)
source "$here/common.sh"
cd "$GT4_BENCH_ROOT"
while tasklist //FI "IMAGENAME eq gt4vulkan.exe" | grep -q gt4vulkan; do sleep 10; done
for spec in "$@"; do
  IFS=: read -r tag exe envs <<< "$spec"
  extra=(); IFS=, read -ra extra <<< "$envs"
  EVERY=100000 FROM=999999 SCALE=${SCALE:-3} bash "$here/night-run.sh" "$tag" "$exe" GT4_MTVU_REPORT_EVERY=600 GT4_HW_PROFILE_EVERY=600 "${extra[@]}"
  echo "finished $tag"
done
