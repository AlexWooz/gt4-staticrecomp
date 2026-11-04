#!/bin/bash
# IN-468: builds gt4vulkan with profile-guided optimisation and ThinLTO of the GS libraries from a fresh profile of the
# current code. usage: pgo-build.sh <configured build dir> <output dir> (run it from a VS x64 environment, see
# native/build.cmd). It leaves the build dir in PGO mode; reset it for fixtures and A/B work with
#   cmake -B <build dir> -DGT4_HW_PGO= -DGT4_HW_THINLTO=OFF
# The profile only holds hashes and counts of MIT GS code, no game data. Retrain as the last step of a candidate: a stale
# profile is not wrong, but functions changed since it was taken lose their optimisation.
set -e
here=$(cd "$(dirname "$0")" && pwd)
source "$here/common.sh"
build=$1; out=$2
pgo=$GT4_BENCH_ROOT/build/pgo
mkdir -p "$pgo" "$out"
rm -f "$pgo"/*.profraw
llvm="/c/Program Files/LLVM/bin"
cmake -B "$build" -DGT4_HW_PGO=generate -DGT4_HW_THINLTO=OFF
cmake --build "$build" --target gt4vulkan
pw=$(cygpath -w "$pgo")
SCALE=3 EVERY=100000 FROM=999999 FRAMES=11500 bash "$here/night-run.sh" pgotrain "$build/gt4vulkan.exe" "LLVM_PROFILE_FILE=$pw\\night.profraw"
SCALE=3 EVERY=100000 FROM=99999 bash "$here/arcade-run.sh" pgotrain "$build/gt4vulkan.exe" "LLVM_PROFILE_FILE=$pw\\arcade.profraw"
"$llvm/llvm-profdata.exe" merge -o "$pgo/gs.profdata" "$pgo/night.profraw" "$pgo/arcade.profraw"
cmake -B "$build" -DGT4_HW_PGO=use "-DGT4_HW_PGO_PROFILE=$(cygpath -m "$pgo/gs.profdata")" -DGT4_HW_THINLTO=ON
cmake --build "$build" --target gt4vulkan
cp "$build/gt4vulkan.exe" "$build/gt4vulkan.pdb" "$build/SDL3.dll" "$out/"
sha256sum "$out/gt4vulkan.exe" "$pgo/gs.profdata"
