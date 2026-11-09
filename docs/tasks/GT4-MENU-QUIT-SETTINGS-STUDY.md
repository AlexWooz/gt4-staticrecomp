# GT4 menu, quit path and in-game settings: the TASK-212/213 study

Read-only study, 2026-11-09. Sources: the v2.00 decomp (`private/references/gt4-decomp`, 30,233 of 30,875
functions matched), the older research clone, the decoded retail v2.00 game data
(`private/slice155-spec2-480p/retail_adc/`, 56 project scripts + widget trees), the Spec II volume catalogue
(`private/slice151-spec2-study/spec2_vol_records.json`), `docs/SPEC2-*.md`, `docs/INGAME-SETTINGS-STUDY.md`
(TASK-078), and the rc17 tree. Nenkai/OpenAdhoc v7 scripts were used for structure only, never copied.

Labels: **C** = Confirmed, **H** = High confidence, **Hy** = Hypothesis, **U** = Unknown.

## 1. Main menu (TASK-212)

- The GT4 "main menu" (the logo with Arcade Mode / Granturismo Mode / Replay Theater / Options / Save / Ending
  Movie / Credit) is the **`arcade` project, page `TopRoot`**. **C**
- Rows are **not a fixed table in the ELF**. `TopRoot::onInitialize` copies hidden `Composite` templates at
  runtime (`MWidget::doCopy` `0x00257808`, `MComposite::appendChild` `0x00205420`) and appends them to the
  `StartScroll::SelectBox`; each row has a script module under `PROJECT::ROOT::hidden::<Name>` with
  `onActivate`, `onFocusEnter`, `onFocusLeave`. **C**
- The row templates in `projects/arcade/arcade.mproject` (205,792 bytes) are `hidden::Arcade`, `GTmode`,
  `ReplayTheater`, `Option`, `Save`, `Ending`, `Credit`; the script is `arcade.adc` (380,083 bytes, Adhoc
  **v5**, `ADCH005`). There is no Online row and no `getPadButtonState` in retail. **C**
- The scene/mode change is script-driven: the row sets `main::game.next_menu_project` / `next_sequence`;
  `GranTurismo4::MenuGameObject::startProject` (`0x00109D00`) opens `/projects/%s/%s` and runs the project's
  frame loop. **C**
- The menu state is readable by the host through the same pointer chain `race_display.cpp` already uses:
  `[[0x00622F4C] + 0x3A39C]` = last menu project, `+0x3A3BC` = next menu project. **C**

### Can a Quit Game row be added?

| Route | What it takes | Verdict |
|---|---|---|
| a. Data + script patch | Add a `Composite "Quit"` child to `hidden` in `arcade.mproject` plus one `button_list` entry and a `PROJECT::ROOT::hidden::Quit` module in `arcade.adc` | The game's own way, but needs the **GT4.VOL overlay reader** (not built) and a **v5 script/widget edit**. Multi-slice, not rc17. **C/H** |
| b. Host-side widget injection | Patch only the `.mproject`; the host runs `doCopy`/`appendChild` on the guest and watches the `SelectBox` index | Research-grade; needs script-value boxing and layout work. **Hy** |
| c. Host overlay next to the menu | Host UI only, no game data | The rc17-safe route. **C** |

## 2. The quit path

- **The game has no quit-to-dashboard.** The `Save` row ends with the message "You can turn off the power". **C**
- The only "leave" primitive the scripts have is **`MSystem::Reboot` (`0x001ACD40`)**: it builds
  `cdrom0:\SCUS_973.28;1` and calls `LoadExecPS2`. Only `projects/network/network.adc` calls it among the 56
  decoded retail scripts; the other project scripts are not decoded. **C/U**
- Our runtime already halts on `Exit` and on `LoadExecPS2` (`runtime/src/kernel.cpp:475-476`), but the launcher
  classifies a non-`window closed` stop as a crash (`launcher/crates/gt4l-core/src/exit.rs`). **C**
- **Safe host exit:** post `WM_CLOSE` to the game window, exactly like Alt+F4/F9. `window.cpp` runs the normal
  path: input recorder finish, `ps2::halt("window closed")`, card `flush_cards_now()`, audio close, F8 probe
  finish, achievements/overlay shutdown. The launcher sees exit code 0 + `window closed` = normal close. **C**
- Do not `std::exit`/`TerminateProcess`. Do not close in the middle of a card save; the exit flush writes the
  card as it is. Guard with the card's `quiet >= ~50` VBlanks (needs a small accessor) or refuse while the
  save dialog is up. **C/H**

## 3. Controller quit with confirmation (the rc17-safe TASK-212)

- The host already has the full pad each VBlank (`PadState`, PS2 bits: Start 3, L3 1, R3 2, L2 8, R2 9, Cross
  14, Circle 13) in `window.cpp`'s `on_vblank`. **C**
- The **achievements overlay** (`native/achievements_overlay.cpp`) is an owned, layered, click-through,
  non-activating tool window over the game, ticked every VBlank, drawing GDI+ toasts; it survives both
  renderers. Reuse it for a "Quit to desktop? Cross = quit / Circle = cancel" prompt. **C/H**
- Proposed combo: hold **L3+R3+Start** (or L2+R2+Start) for 1.0-1.5 s, release cancels, then confirm with Cross.
  While the prompt is open, feed the game a neutral pad and close the prompt on timeout so the attract-demo
  timer does not fire. **H**
- The quit uses `PostMessageW(game_window, WM_CLOSE, 0, 0)`; the card/session flush is the existing exit path.
- Separate small fix: treat a guest `LoadExecPS2`/`Exit` as a normal quit (not a crash) in `exit.rs`; document
  that the recomp cannot restart GT4 in-process. **C**

## 4. Options menu and in-game settings (TASK-213)

- The Options project (`projects/option/option.adc`, Adhoc v5) builds pages from `hidden::Item`, `Item2`,
  `OptionItem` and `Group` templates: General (Race, Replay, Screen, Color, Sound), Arcade, Photo Mode,
  Controller, Misc. Rows write the game's option object (inside the save data) and `apply()`; values are
  persisted in the memory card save, not in a config file. **C**
- GT4's own Options already covers the game-side controls and audio; the **gap is the host settings**:
  renderer, render scale, resolution/monitor, vsync, fullscreen, effects, controller choice/deadzone, host
  audio. **C**
- **A row that changes a host setting must not write the option struct** (it would dirty the save with values a
  real PS2 cannot use); it writes host config only. **C**
- The launcher's setting ids (rc17): `controller`, `trigger_full_range`, `language`, `update_channel`,
  `renderer`, `render_scale`, `frame_rate`, `present_thread`, `coregs_sharpening`, `graphics_card`, `monitor`,
  `resolution`, `fullscreen`, `vsync`, `performance_hud`, `performance_report`, `check_vu_code`,
  `discord_rich_presence`. No audio setting yet; do not invent one. **C**

### Menu vs overlay

| | Extend GT4's Options | Native overlay |
|---|---|---|
| Look | native GT4 page | clearly ours, over the game |
| Requires | VOL overlay reader + v5 script/widget patch + dispatch bridge | host code only |
| Effort | TASK-078 P1-P2: 3-5 slices for the first row | ~1 slice for the first 2-3 rows |
| Risk | script exceptions are fatal; save contamination | none to the game; input/attract timer |
| Both games | v5 retail and v7 Spec II patches | one implementation |

**Proposal:** rc17 uses the overlay + a `game-changes.toml` feedback file. First rows: **Fullscreen** (live, no
restart) and, if time, **Renderer** (next start, with a clear notice). The game writes the file; the launcher
merges it into `settings.toml` on exit/next start. The native GT4 page comes after the VOL overlay reader and
the v5 script round trip (TASK-078 P1/P2); the first native row can use the empty `MSystem::SetDrawPerfMeter`
(`0x001AC238`) as a dispatch bridge, with the same capsule pattern as the license guard.

## 5. How Spec II adds its menus

- **The ELF is not where the menus are.** Spec II's ELF differs from its own base beta by one word
  (`0x001001E8`: `jal 0x005E3AA8` -> `jal 0x0068A768`) plus a 0x2CCC-byte plugin segment at `0x0068A480`;
  the plugin init runs 356 instructions and calls two functions. **C**
- **The game data is.** Comparing the beta's file sizes with Spec II 1.10: `arcade.adc` +7,509,
  `arcade.mproject` +10,380, `gtmode.adc` +325,902, `gtmode.mproject` +161,745, `option.adc` +10,023,
  `option.mproject` +106,791, `boot.adc` +14,771, `boot.mproject` -24,551, `setting.mproject` +3,152,
  `setting.adc` unchanged. **C**
- So Spec II's extra menus are **Adhoc script + widget-tree + text replacements inside GT4.VOL** (the beta is
  v7), with the plugin supplying the behaviour the new options switch on. **C/H**
- **Not reusable for v2.00:** retail v2.00 is Adhoc v5 with retail addresses/data; the Spec II files are v7 on
  the beta lineage. Copying them would not run and would drag beta content. The *approach* is the reference:
  data replacement + host behaviour + host settings out of the save. **C**

## 6. Decision and next steps

- **TASK-212 for rc17:** host-only controller quit (L3+R3+Start, confirmation overlay, `WM_CLOSE` exit) plus the
  `LoadExecPS2` normal-quit classification fix. No game-data change. Both games.
- **TASK-213 for rc17:** overlay first; first row Fullscreen, then Renderer with a restart notice; write-through
  through `game-changes.toml`. The native GT4 page waits for the VOL overlay reader and the v5 script round
  trip.
- **TASK-214 (later):** GT4.VOL overlay reader + v5 script/widget patch + a real Quit Game row (TASK-078 P1/P2),
  then more native rows. A real Quit Game row is the end state; the rc17 overlay is the safe first step.
- The exit classification fix (`LoadExecPS2`/`Exit` = normal quit) should ship with TASK-212.

## 7. Unknowns and next checks

1. Can GTAdhocToolchain compile retail v5 `arcade.adc`/`option.adc` back to identical bytes? (Gate for a real
   row.)
2. Can `scripts/gt4_volume.py` write a GT4.VOL TOC with an enlarged entry? (Gate for data replacement.)
3. Does the `.mproject` parser tolerate an extra child and a literal `text string` label?
4. What is the default attract-demo timeout (`option.timeout_count_to_autodemo`)?
5. Can the host read the card's save `quiet` counter? (Exit guard.)
6. Do any of the seven undecoded project scripts call `MSystem::Reboot`?
7. Can the host build and append a `MWidget` row via the script-value boxing helpers?
8. Which Spec II menu differences come from the beta vs the mod? (Needs the beta's `option.adc`.)
9. Does the achievements overlay prompt place correctly over the PCSX2 GS child window?
10. Does Steam see the hidden launcher parent as the game for the whole session? (TASK-211 test.)

## 8. Address quick reference

| Thing | Value | Conf. |
|---|---|---|
| Project runner | `GranTurismo4::MenuGameObject::startProject` `0x00109D00` | C |
| Project path | `"/projects/%s/%s"` at `D_0068CED0` | C |
| Top menu | `projects/arcade/arcade.mproject` -> `TopRoot` -> `StartScroll/SelectBox` | C |
| Row copy/append | `doCopy` `0x00257808`, `appendChild` `0x00205420` | C |
| Selection | `MSelectBox get/set_index` `0x002D8E98`/`0x002D8F30` | C |
| Scene switch | `MGame set_next_menu_project` `0x00163158`, `next_sequence` `0x00162E20` | C |
| Menu state | `[[0x00622F4C]+0x3A39C]` last, `+0x3A3BC` next | C |
| Guest restart | `MSystem::Reboot` `0x001ACD40` -> `LoadExecPS2("cdrom0:\\SCUS_973.28;1")` | C |
| Host exit | `window.cpp` `WM_CLOSE` 422, `WM_DESTROY` 445, `halt` 585/1028, flush 1036 | C |
| Card flush | `sio2.cpp` `dirty`/`quiet>=50`, `flush_cards_now()` on exit | C |
| Overlay | `achievements_overlay.cpp` 219/316/342; ticked at `window.cpp:712` | C |
| Free bridge native | `MSystem::SetDrawPerfMeter` `0x001AC238` (empty) | C |
| Per-target hooks | `native/game_target.hpp` `HookAddresses` (v200 / spec2) | C |

## Resumo para o dono (pt-BR)

- O menu principal do GT4 é o projeto `arcade` (`TopRoot`), e as linhas são copiadas em tempo de execução pelo
  próprio script. Dá para acrescentar um "Quit Game", mas isso é mexer nos dados do jogo (widgets + script v5) e
  o nosso leitor de sobreposição do `GT4.VOL` ainda não existe. Para o rc17 o caminho seguro é o atalho no
  controle com uma confirmação desenhada por nós sobre o jogo.
- O jogo não tem sair para o desktop; o `Save` termina com "você pode desligar o console". O único pedido de
  saída é o `MSystem::Reboot` (recarrega o ELF); o nosso runtime já encerra nesse ponto, mas o launcher hoje
  relata isso como crash. Vale corrigir.
- Fechar pelo atalho usa o mesmo caminho do Alt+F4: grava o cartão, fecha o áudio, grava o F8 e encerra com
  "window closed", que o launcher entende como fechamento normal.
- O Spec II não tem código de menu no executável: ele troca os arquivos de menu dentro do `GT4.VOL` (scripts e
  widgets maiores que os da base beta) e usa o plugin só para o comportamento. Serve de referência de
  abordagem, não de código; e não dá para reaproveitar os arquivos porque o v2.00 é Adhoc v5 e o Spec II é v7.
- Para o TASK-213 a proposta é começar pelo overlay: Tela cheia ao vivo e depois Renderizador com aviso de "vale
  no próximo início", gravando num arquivo que o launcher junta no `settings.toml`; a página nativa do GT4 fica
  para depois do leitor do `GT4.VOL`.
