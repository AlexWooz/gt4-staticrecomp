#!/bin/bash
# IN-520: replays a recording on the reference renderer (gt4-gs.exe, black box), headless, from the same card, up to its last VBlank.
# usage: replay-gsdx.sh <recording name> <run name> <exe> [ENV=VAL ...]   (env KEEP="A-B C-D" SCALE=6 EVERY=1000 GS_EXE=<gt4-gs.exe> LAST=<VBlank>)
# Writes $GT4_BENCH_OUT/replay-<recording>-<run>/: gs-frames/ with the final frames every EVERY frames, and log.txt.
# Clean room: TASK-027 agents use the final frames only. log.txt also holds the GS process's own output, so read only the game's lines.
source "$(dirname "$0")/common.sh"
name=$1; run=$2; exe=$3; shift 3
recording=$GT4_ASSETS/private/coregs2-bench/inputs/$name.txt
last=$(grep '^end ' "$recording" | cut -d' ' -f2)
if [ -z "$last" ]; then echo "no finished recording at $recording"; exit 2; fi
if tasklist | grep -qi -E "gt4\.exe|gt4-spec2|gt4vulkan|gt4-gs"; then echo "a game is running: close it first"; exit 2; fi
GT4_SPEC2_ISO=${GT4_SPEC2_ISO:-$GT4_ASSETS/Gran Turismo 4 Spec II v1.10 (PTBR).iso}
gs_exe=${GS_EXE:-$GT4_ASSETS/private/slice281-gt4gs-121/gt4-gs.exe}
OUT=$GT4_BENCH_OUT/replay-$name-$run
mkdir -p "$OUT/gs-frames"
cp "$GT4_ARCADE_CARD" "$OUT/card.ps2"
cat > "$OUT/gt4-gs.ini" <<EOF
[EmuCore/GS]
Renderer = 14
upscale_multiplier = ${SCALE:-6}
accurate_blending_unit = 3
deinterlace_mode = 0
AspectRatio = 16:9
VsyncEnable = false
DisableMailboxPresentation = true
HWDownloadMode = 0

[Folders]
Cache = $(cygpath -w "$GT4_ASSETS/private/slice193-gt4gs-cache")

[GT4GS]
DataRoot = $(cygpath -w "$GT4_ASSETS/private/slice193-gt4gs-data")
ProhibitDynamicCode = false
Surfaceless = true
DumpDir = $(cygpath -w "$OUT/gs-frames")
DumpEvery = ${EVERY:-1000}
EOF
start_keeping_frames "$OUT/gs-frames"
env GT4_RENDERER=pcsx2 PS2_GS=gpu GT4_GS_PROCESS="$(cygpath -w "$gs_exe")" GT4_GS_CONFIG="$(cygpath -w "$OUT/gt4-gs.ini")" \
  GT4_FORCE_480P=1 GT4_FORCE_WIDESCREEN=1 GT4_DISPLAY_ASPECT=16:9 GT4_INPUT_REPLAY="$(cygpath -w "$recording")" "$@" "$exe" \
  "$(cygpath -w "$GT4_ASSETS/private/slice272-spec2-elf/SCUS_974.36")" --ntsc --disc "$(cygpath -w "$GT4_SPEC2_ISO")" \
  --frames "${LAST:-$last}" --memcard "$(cygpath -w "$OUT/card.ps2")" > "$OUT/log.txt" 2>&1
echo "exit $?"
stop_keeping_frames
rm -f "$OUT/card.ps2"
