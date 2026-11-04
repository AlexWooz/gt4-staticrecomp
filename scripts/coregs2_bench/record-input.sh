#!/bin/bash
# IN-520: plays a game with a window, starting from a memory card copy, and records the pad state of every VBlank. F9 ends
# the recording and closes the game; Alt+F4 also leaves a complete recording.
# usage: record-input.sh <name> [gt4|spec2] [card]   (env SCALE=6, EXE, GT4_CONTROLS_FILE: your controls file; default the launcher's)
#   game: spec2 by default. card: the name of a card in $GT4_ASSETS/private/coregs2-bench/cards (without .ps2, never written;
#   bench_card builds one from a save library); default the Arcade card without game data (Spec II only).
# Writes $GT4_ASSETS/private/coregs2-bench/inputs/<name>.txt and <name>.setup (the game and the card, with its hash, that a
# replay needs); an existing recording is never overwritten.
source "$(dirname "$0")/common.sh"
name=${1:?usage: record-input.sh <name> [gt4|spec2] [card]}
game=${2:-spec2}
game_files "$game" || exit 2
if [ -n "$3" ]; then
  card=$GT4_ASSETS/private/coregs2-bench/cards/$3.ps2
  if [ ! -e "$card" ]; then echo "no card $card"; exit 2; fi
elif [ "$game" = spec2 ]; then
  card=$GT4_ARCADE_CARD
else
  echo "gt4 needs a card: record-input.sh <name> gt4 <card>"; exit 2
fi
inputs=$GT4_ASSETS/private/coregs2-bench/inputs
recording=$inputs/$name.txt
if [ -e "$recording" ]; then echo "$recording exists: choose another name"; exit 2; fi
if tasklist | grep -qi -E "gt4\.exe|gt4-spec2|gt4vulkan"; then echo "a game is running: close it first"; exit 2; fi
mkdir -p "$inputs"
OUT=$GT4_BENCH_OUT/record-$name
mkdir -p "$OUT"
cp "$card" "$OUT/card.ps2"
chmod u+w "$OUT/card.ps2"  # the cards in cards/ are read-only; the game writes its copy
# The launcher writes this file only after a change of controls; empty means the game's default controls.
if [ -z "$GT4_CONTROLS_FILE" ]; then
  for controls in "$APPDATA/GT4Recomp/controls.toml" "$APPDATA/GT4Recomp.before-onboarding-20261102/controls.toml"; do
    if [ -e "$controls" ]; then GT4_CONTROLS_FILE=$controls; break; fi
  done
fi
echo "Controls: ${GT4_CONTROLS_FILE:-game defaults}"
if [ "$game" = gt4 ]; then exe=${EXE:-$GT4_ASSETS/private/slice401-dist-130-rc12-test/gt4.exe}
else exe=${EXE:-$GT4_ASSETS/private/slice401-dist-130-rc12-test/gt4-spec2.exe}; fi
printf 'game=%s\ncard=%s\ncard_sha256=%s\nrecorded_with=%s\n' "$game" "${card#"$GT4_ASSETS"/}" \
  "$(sha256sum "$card" | cut -d' ' -f1)" "${exe#"$GT4_ASSETS"/}" > "$inputs/$name.setup"
echo "Game: $game, card: $card"
echo "Recording to $recording. Press F9 at the end: it stops the recording and closes the game."
env GT4_RENDERER=hw GT4_HW_CORE=2 GT4_FORCE_480P=1 GT4_FORCE_WIDESCREEN=1 GT4_DISPLAY_ASPECT=16:9 GT4_HW_SCALE=${SCALE:-6} GT4_FULLSCREEN=1 \
  GT4_CONTROLS_FILE="$GT4_CONTROLS_FILE" GT4_INPUT_RECORD="$(cygpath -w "$recording")" "$exe" \
  "$(cygpath -w "$GAME_ELF")" --ntsc --disc "$(cygpath -w "$GAME_ISO")" \
  --frames 0 --memcard "$(cygpath -w "$OUT/card.ps2")" --window > "$OUT/log.txt" 2>&1
echo "exit $?"
rm -f "$OUT/card.ps2"
if grep -q '^end ' "$recording"; then
  echo "Recorded: $(grep -c '^v ' "$recording") changes, $(grep '^end ' "$recording") VBlanks."
else
  echo "The recording has no end line: something closed the game abruptly. See $OUT/log.txt"
  exit 1
fi
