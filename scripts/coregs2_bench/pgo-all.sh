#!/bin/bash
# TASK-134: profile-guided optimisation of the whole game: the EE library, the VU AOT code, the runtime, the VU1 worker and the GS
# libraries (IN-468), from one fresh profile per target. Run it as the last step of a candidate: it builds twice and trains on full
# scenes. The profile holds only hashes and counts, but of code translated from the game: it stays outside git.
# usage: pgo-all.sh <spec2|v200> <output dir> [ENV=VAL for the training runs ...]
# Needs: EE_GENERATED=<translated units of this target>, and for Spec II the VU0 programs (game/configure.cmd).
# Leaves: <output dir>/{gt4vulkan*.exe,.pdb,SDL3.dll,ps2game.lib,game.profdata,receipt.txt}.
set -e
here=$(cd "$(dirname "$0")" && pwd)
source "$here/common.sh"
target=$1; out=$2; shift 2
work=$GT4_BENCH_ROOT/build/pgo-all-$target
llvm="/c/Program Files/LLVM/bin"
tools=$here/game
win() { cygpath -m "$1"; }
mkdir -p "$work/raw" "$out"
rm -f "$work"/raw/*.profraw
exe_name=gt4vulkan.exe; [ "$target" = spec2 ] && exe_name=gt4vulkan-spec2.exe

library() {  # <build dir name> <extra compile flags>; prints the library hash
  cmd //c "$(cygpath -w "$tools/ee_configure.cmd")" "$1" "$(win "$EE_GENERATED")" "$2" > "$work/$1.configure.log" 2>&1
  cmd //c "$(cygpath -w "$tools/ee_build.cmd")" "$1" > "$work/$1.build.log" 2>&1
  sha256sum "$GT4_BENCH_ROOT/build/$1/ps2game.lib" | cut -d' ' -f1
}
game() {  # <build dir name> <library dir> <library hash> <cmake args...>
  local dir=$1 lib=$2 hash=$3; shift 3
  T134_ARGS="-DGT4_EE_LIBRARY=$(win "$GT4_BENCH_ROOT/build/$lib/ps2game.lib") -DGT4_EE_LIBRARY_SHA256=$hash $*" cmd //c "$(cygpath -w "$tools/configure.cmd")" "$dir" "$target" > "$work/$dir.configure.log" 2>&1
  cmd //c "$(cygpath -w "$tools/build.cmd")" "$dir" > "$work/$dir.build.log" 2>&1
}

echo "== instrumented build"
hash=$(library t134-ee-$target-gen "/clang:-fprofile-generate")
game t134-$target-gen t134-ee-$target-gen "$hash" -DGT4_HW_PGO=generate -DGT4_HW_THINLTO=OFF
exe=$GT4_BENCH_ROOT/build/t134-$target-gen/$exe_name

echo "== training"
raw=$(cygpath -w "$work/raw")
"$exe" --prepare-shaders > /dev/null 2>&1 || true
if [ "$target" = spec2 ]; then
  SCALE=1 bash "$here/replay-run.sh" george-volta2 pgo1 "$exe" "LLVM_PROFILE_FILE=$raw\\lap1.profraw" GT4_CORE2_PERFORMANCE=1 "$@"
  SCALE=2 bash "$here/replay-run.sh" george-volta2 pgo2 "$exe" "LLVM_PROFILE_FILE=$raw\\lap2.profraw" "$@"
  SCALE=2 EVERY=100000 FROM=999999 FRAMES=21500 bash "$here/spec2-run.sh" night pgo "$exe" "LLVM_PROFILE_FILE=$raw\\night.profraw" "$@"
else
  SCALE=2 EVERY=100000 FROM=999999 FRAMES=11500 bash "$here/night-run.sh" pgo "$exe" "LLVM_PROFILE_FILE=$raw\\night.profraw" "$@"
  SCALE=1 EVERY=100000 FROM=999999 bash "$here/arcade-run.sh" pgo "$exe" "LLVM_PROFILE_FILE=$raw\\arcade.profraw" "$@"
fi
"$llvm/llvm-profdata.exe" merge -o "$work/game.profdata" "$work"/raw/*.profraw
profile=$(win "$work/game.profdata")

echo "== optimised build"
# A build folder from an earlier profile would keep objects of the old profile, which ThinLTO refuses to link together.
rm -rf "$GT4_BENCH_ROOT/build/t134-ee-$target-use" "$GT4_BENCH_ROOT/build/t134-$target-use"
hash=$(library t134-ee-$target-use "/clang:-fprofile-use=$profile /clang:-Wno-profile-instr-out-of-date /clang:-Wno-profile-instr-unprofiled /clang:-Wno-backend-plugin")
game t134-$target-use t134-ee-$target-use "$hash" -DGT4_HW_PGO=use "-DGT4_HW_PGO_PROFILE=$profile" -DGT4_HW_THINLTO=ON
cp "$GT4_BENCH_ROOT/build/t134-$target-use/$exe_name" "$GT4_BENCH_ROOT/build/t134-$target-use/${exe_name%.exe}.pdb" \
   "$GT4_BENCH_ROOT/build/t134-$target-use/SDL3.dll" "$GT4_BENCH_ROOT/build/t134-ee-$target-use/ps2game.lib" "$work/game.profdata" "$out/"
( cd "$out" && sha256sum "$exe_name" ps2game.lib game.profdata > receipt.txt && cat receipt.txt )
