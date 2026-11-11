#!/bin/bash
# Headless Spec II 1.10 run on CoreGS 2.0, Arcade from a card without game data (the boot order of docs/BUILD.md, S7b).
# usage: spec2-run.sh <night|day|george> <name> <exe> [ENV=VAL ...]   (env SCALE=3 EVERY=1000 FROM=1000 FRAMES=22000 WINDOW=1)
# WINDOW=1 opens the game's window and paces it at 60 VBlanks/s, so 21400 VBlanks take 6 min. Never over an owner session.
# Writes $GT4_BENCH_OUT/spec2-<scene>-<name>/. Needs the derived private/slice272-spec2-elf/SCUS_974.36 and a Spec II disc
# image (GT4_SPEC2_ISO; the PT-BR image has the same program). The card is copied per run, never written.
# night: C.S. Route 5 (Clubman, night), the race starts near 20000. day: Costa di Amalfi, the race starts near 21000 (FRAMES=24000).
# george: George V Paris, the heaviest track, a rally against one car with no B-Spec icon, so the presses steer: the race starts near 20300 (FRAMES=27000).
source "$(dirname "$0")/common.sh"
GT4_SPEC2_ISO=${GT4_SPEC2_ISO:-$GT4_ASSETS/Gran Turismo 4 Spec II v1.10 (PTBR).iso}
scene=$1; name=$2; exe=$3; shift 3
OUT=$GT4_BENCH_OUT/spec2-$scene-$name
mkdir -p "$OUT/frames"
export GT4_BENCH_SCENE=$(record_scene "$OUT" "spec2-$scene" "$@")
cp "$GT4_ARCADE_CARD" "$OUT/card.ps2"
boot="600:start:30 900:cross:30 1500:left:10 1600:cross:30 2200:right:10 2300:cross:30 6200:start:30 8000:start:30 9200:cross:30 10000:cross:30"
case "$scene" in
  night)
    P="$boot 10600:down:10 10800:cross:30"
    for frame in 11200 11400 11600 11800 12000 12200 12400 12600 12800; do P="$P $frame:right:10"; done
    P="$P 13000:cross:30 13800:cross:30 14400:cross:30 15000:cross:30"
    for frame in 15600 15800 16000 16200 16400; do P="$P $frame:right:10"; done
    P="$P 17000:cross:30 17800:cross:30 18600:cross:30 19400:cross:2600"
    ;;
  day)
    P="$boot 11000:down:10 11200:down:10 11400:cross:30 12200:cross:30 13000:cross:30 14400:cross:30"
    for frame in 15000 15200 15400 15600 15800; do P="$P $frame:right:10"; done
    P="$P 16400:cross:30 17200:cross:30 18000:cross:30 18800:cross:30 19600:cross:30 20400:cross:4000"
    ;;
  george)
    P="$boot 10600:down:10 10800:cross:30 11200:right:10 13000:cross:30 13800:cross:30 14400:cross:30 15000:cross:30"
    for frame in 15600 15800 16000 16200 16400; do P="$P $frame:right:10"; done
    P="$P 17000:cross:30 17800:cross:30 18600:cross:30 19400:cross:30 20200:cross:30 21000:cross:7000 21050:left:200"
    ;;
  # TASK-215: the streamer's scenes, mapped on 2026-11-11 from the Spec II Arcade screens (the 2x2 category
  # selector at VBlank 10,600 with its four quadrants, then the track strip of that category):
  #   City Courses   (down once, cross): Amalfi 0, George V 1, Paris Opera 2, Hong Kong 3, Citta di Aria 4,
  #                                      New York 5, Tokyo R246 6, Cote d'Azur 7, S.S. Route 5 8, C.S. Route 5 9 ...
  #   World Circuits (cross only):       Nurburgring 0, Laguna Seca 1, Tsukuba 2, Tsukuba Molhado 3, FISCO '90s 4 ...
  # George V is the Paris rally track (its preview reads "RALLY GEORGE V PARIS"), so "George Paris Rally" and the
  # George V reference replay are the same track.
  opera|paris-opera)  # Paris Opera (city, index 2)
    P="$boot 10600:down:10 10800:cross:30"
    for frame in 11200 11400; do P="$P $frame:right:10"; done
    P="$P 13000:cross:30 13800:cross:30 14400:cross:30 15000:cross:30"
    for frame in 15600 15800 16000 16200 16400; do P="$P $frame:right:10"; done
    P="$P 17000:cross:30 17800:cross:30 18600:cross:30 19400:cross:2600"
    ;;
  ssr5)  # S.S. Route 5 (city, index 8)
    P="$boot 10600:down:10 10800:cross:30"
    for frame in 11200 11400 11600 11800 12000 12200 12400 12600; do P="$P $frame:right:10"; done
    P="$P 13000:cross:30 13800:cross:30 14400:cross:30 15000:cross:30"
    for frame in 15600 15800 16000 16200 16400; do P="$P $frame:right:10"; done
    P="$P 17000:cross:30 17800:cross:30 18600:cross:30 19400:cross:2600"
    ;;
  tsukuba-wet)  # Tsukuba Molhado (world circuits, index 3; the default quadrant, no down)
    P="$boot 10800:cross:30"
    for frame in 11200 11400 11600; do P="$P $frame:right:10"; done
    # The World path reaches the car screens ~2,000 VBlanks earlier than the city path: the track cross at 13,000 lands
    # on the manufacturer grid (~13,400), 14,400 enters the highlighted maker (Ford) and 14,800 moves off its first model
    # (the Ford Model T Tourer '15, which Single Race refuses); 15,200 confirms the second model.
    P="$P 13000:cross:30 14400:cross:30 14800:right:10 15200:cross:30 16200:cross:30 17000:cross:30 17800:cross:30 18600:cross:30 19400:cross:2600"
    ;;
  george-rally)  # George V Paris (the Paris rally track, city index 1; the reference replay's track)
    P="$boot 10600:down:10 10800:cross:30 11200:right:10"
    P="$P 13000:cross:30 13800:cross:30 14400:cross:30 15000:cross:30"
    for frame in 15600 15800 16000 16200 16400; do P="$P $frame:right:10"; done
    P="$P 17000:cross:30 17800:cross:30 18600:cross:30 19400:cross:2600"
    ;;
  *) echo "unknown scene $scene"; exit 2 ;;
esac
args=(); for p in $P; do args+=(--press "$p"); done
env GT4_RENDERER=hw GT4_HW_CORE=2 GT4_FORCE_480P=1 GT4_FORCE_WIDESCREEN=1 GT4_HW_SCALE=${SCALE:-3} GT4_HW_DUMP_SOURCE=upscaled \
  GT4_HW_DUMP_DIR="$(cygpath -w "$OUT/frames")" GT4_HW_DUMP_EVERY=${EVERY:-1000} GT4_HW_DUMP_FROM=${FROM:-1000} "$@" "$exe" \
  "$(cygpath -w "$GT4_ASSETS/private/slice272-spec2-elf/SCUS_974.36")" --ntsc --disc "$(cygpath -w "$GT4_SPEC2_ISO")" \
  --frames ${FRAMES:-22000} --memcard "$(cygpath -w "$OUT/card.ps2")" ${WINDOW:+--window} "${args[@]}" > "$OUT/log.txt" 2>&1
echo "exit $?"
