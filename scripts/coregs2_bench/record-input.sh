#!/bin/bash
# IN-520: plays Spec II with a window, starting from the bench's card without game data, and records the pad state of every
# VBlank. F9 ends the recording and closes the game; Alt+F4 also leaves a complete recording.
# usage: record-input.sh <name>   (env SCALE=6, GT4_CONTROLS_FILE: your controls file; default the launcher's)
# Writes $GT4_ASSETS/private/coregs2-bench/inputs/<name>.txt; an existing recording is never overwritten.
source "$(dirname "$0")/common.sh"
name=${1:?usage: record-input.sh <name>}
inputs=$GT4_ASSETS/private/coregs2-bench/inputs
recording=$inputs/$name.txt
if [ -e "$recording" ]; then echo "$recording exists: choose another name"; exit 2; fi
if tasklist | grep -qi -E "gt4\.exe|gt4-spec2|gt4vulkan"; then echo "a game is running: close it first"; exit 2; fi
mkdir -p "$inputs"
OUT=$GT4_BENCH_OUT/record-$name
mkdir -p "$OUT"
cp "$GT4_ARCADE_CARD" "$OUT/card.ps2"
GT4_SPEC2_ISO=${GT4_SPEC2_ISO:-$GT4_ASSETS/Gran Turismo 4 Spec II v1.10 (PTBR).iso}
GT4_CONTROLS_FILE=${GT4_CONTROLS_FILE:-$APPDATA/GT4Recomp/controls.toml}
exe=${EXE:-$GT4_BENCH_ROOT/build/spec2-core2-build/gt4vulkan-spec2.exe}
echo "Recording to $recording. Press F9 at the end: it stops the recording and closes the game."
env GT4_RENDERER=hw GT4_HW_CORE=2 GT4_FORCE_480P=1 GT4_FORCE_WIDESCREEN=1 GT4_DISPLAY_ASPECT=16:9 GT4_HW_SCALE=${SCALE:-6} GT4_FULLSCREEN=1 \
  GT4_CONTROLS_FILE="$GT4_CONTROLS_FILE" GT4_INPUT_RECORD="$(cygpath -w "$recording")" "$exe" \
  "$(cygpath -w "$GT4_ASSETS/private/slice272-spec2-elf/SCUS_974.36")" --ntsc --disc "$(cygpath -w "$GT4_SPEC2_ISO")" \
  --frames 0 --memcard "$(cygpath -w "$OUT/card.ps2")" --window > "$OUT/log.txt" 2>&1
echo "exit $?"
rm -f "$OUT/card.ps2"
if grep -q '^end ' "$recording"; then
  echo "Recorded: $(grep -c '^v ' "$recording") changes, $(grep '^end ' "$recording") VBlanks."
else
  echo "The recording has no end line: something closed the game abruptly. See $OUT/log.txt"
  exit 1
fi
