#!/bin/bash
# TASK-139: an optimised game build from an existing EE library and profile, without training again. For a candidate whose EE
# side is unchanged (GS fixes only): the profile's stale entries for changed functions are ignored.
# usage: use-build.sh <spec2|v200> <build dir name> <folder with ps2game.lib and game.profdata> <output dir>
# Spec II needs its VU0 programs: VU0GEN and VU0IMG (default the slice390 ones in the main checkout).
set -e
here=$(cd "$(dirname "$0")" && pwd)
source "$here/../common.sh"
target=$1; dir=$2; from=$3; out=$4
win() { cygpath -m "$1"; }
exe_name=gt4vulkan.exe; [ "$target" = spec2 ] && exe_name=gt4vulkan-spec2.exe
library=$from/ps2game.lib
hash=$(sha256sum "$library" | cut -d' ' -f1)
extra="-DGT4_EE_LIBRARY=$(win "$library") -DGT4_EE_LIBRARY_SHA256=$hash -DGT4_HW_PGO=use -DGT4_HW_PGO_PROFILE=$(win "$from/game.profdata") -DGT4_HW_THINLTO=ON -DGT4_FAST_HEADERS=ON -DGT4_VU_FAST_OPS=ON -DGT4_SSE41=ON"
if [ "$target" = spec2 ]; then
  extra="$extra -DGT4_VU0_GENERATED_DIR=$(win "${VU0GEN:-$GT4_ASSETS/private/slice390-spec2-vu0-gen}") -DGT4_VU0_IMAGE_FILE=$(win "${VU0IMG:-$GT4_ASSETS/private/slice390-spec2-vu0-image/vu0_image.bin}")"
fi
rm -rf "$GT4_BENCH_ROOT/build/$dir"
T134_ARGS="$extra" cmd //c "$(cygpath -w "$here/configure.cmd")" "$dir" "$target" > "$GT4_BENCH_ROOT/build/$dir.configure.log" 2>&1
cmd //c "$(cygpath -w "$here/build.cmd")" "$dir" > "$GT4_BENCH_ROOT/build/$dir.build.log" 2>&1
mkdir -p "$out"
cp "$GT4_BENCH_ROOT/build/$dir/$exe_name" "$GT4_BENCH_ROOT/build/$dir/${exe_name%.exe}.pdb" "$out/"
( cd "$out" && sha256sum "$exe_name" )
