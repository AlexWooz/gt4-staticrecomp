# 0081 - Same-length GT4.VOL data patches for the native Quit Game row

Date: 2026-11-09. Status: accepted for TASK-212 (test build); the launcher integration is a follow-up.

## Context

The owner asked for a real **Quit Game** row at the bottom of GT4's main menu, after Credit (TASK-212), and rejected
the overlay-only quit for this item. The study (`docs/tasks/GT4-MENU-QUIT-SETTINGS-STUDY.md`) found that the row is
not an ELF table: `projects/arcade/arcade.mproject` holds the hidden row templates and `projects/arcade/arcade.adc`
(the Adhoc v5 script) copies a fixed `button_list` of them into `TopRoot/StartScroll/SelectBox`.

Two gates blocked the study's route (a): a GT4.VOL TOC writer and a v5 script round trip. The third-party Adhoc
toolchain is not usable here: its repository forbids AI/LLM use, so the v5 round trip cannot be built on it.

## Decision

1. **Do not rewrite the TOC.** A file whose replacement payload is **exactly the original stored extent** keeps every
   RoFS entry, size and offset unchanged; only the payload bytes differ. `scripts/gt4_data_patch.py` decodes the file
   with the reviewed reader, applies the edit, minifies widget text outside string literals, re-encodes the PS2ZIP
   payload and pads it to the stored extent; it refuses growth.
2. **Serve the replacement at the sector reader.** `native/data_patch.cpp`, installed by the
   `slice422-data-runtime` capsule, loads a container named by `GT4_DATA_PATCH` after the disc opens, verifies every
   original range's SHA-256 against the user's image, and rewrites overlapping bytes in `disc::read`. A missing,
   malformed or mismatched patch leaves the game unmodified and logs one line.
3. **The widget is data; the append is host-side.** The patch adds one `hidden::Quit` `Composite` to
   `arcade.mproject`. The script's `button_list` is fixed (adding a widget alone gave no row), so `native/menu_quit.*`
   appends the parsed widget to the arcade SelectBox in guest RAM after the script built the menu: last, after Credit,
   with the game's own font and scroll. No v5 script edit and no guest calls.
4. **The quit is host-side.** While the last row is focused the host hides Cross/Circle from the guest (the row has no
   script module) and a Cross rising edge posts `WM_CLOSE`; headless it halts with `quit game`. The window path keeps
   the card/audio/F8 flush. `exit.rs` treats `window closed`, `quit game`, `Exit` and the LoadExecPS2 reason as normal
   quits (exit 0 only).
5. **Nothing derived from the disc is committed or shipped.** The builder, the capsule and the fixtures are ours; the
   patch container and its private recipe are generated from the user's own disc at run time. A release must build the
   patch during the launcher's "Preparing game files" step.

## Consequences

- The data overlay is also the first half of TASK-078 P1 (GT4.VOL overlay reader): any same-length file replacement
  works, including future settings rows.
- A file that must grow still needs the TOC writer or a script patch; that remains open.
- Spec II needs its own widget vtables, SelectBox offsets and recipe; `menu_quit` is disabled for Spec II until then.
- The attract-demo timeout (~20 s with no input on the arcade top menu) is not held off while the Quit row is focused;
  the owner's controller test decides whether it needs a guard.
