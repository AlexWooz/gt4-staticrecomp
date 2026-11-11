# Tourist Trophy (USA) — Third-Target Study

2026-11-11. Read-only study of a contributor's ISO (not distributed), modelled on
[SPEC2-STUDY.md](SPEC2-STUDY.md). No repository code was changed; the existing scripts were
imported unmodified. [C] = Confirmed, [H] = Hypothesis, [U] = Unknown.

**Status:** study only. Whether Tourist Trophy is accepted as a third target is the owner's decision;
nothing here is scheduled.

## Identity [C]

- ISO SHA-256 `3d89e633ef4c6eb74d42e7a471a013c706c283fd0fff53733ab00803ddaeccdd`,
  3,004,628,992 bytes. Serial SCUS-97502, `SYSTEM.CNF`: `BOOT2 = cdrom0:\SCUS_975.02;1`,
  `VER = 1.00`, `VMODE = NTSC`.
- [U] The hash has not yet been compared against a public dump database; the image is a personal backup.

| Root file | Size | SHA-256 |
|---|---:|---|
| `SCUS_975.02` (loader ELF) | 281,724 | `d8ecfebe90a2de7ff7a620ed9324ef94af5e2b6fb8746f6dc1da4ddde51526fb` |
| `CORE.TT` | 1,849,178 | `52b32bef595cff8789d6898174e9a6420d239e2e4785255a929660828a18f789` |
| `IOPRP300.IMG` | 278,465 | `782690377b32380f4a44cac19a542fd34c4b071c910d4a02dd8124e5f07e2e5a` |
| `SYSTEM.CNF` | 57 | `be0f2ecc293683ee9537ec3cb1d668b42705f339a5a1120a0421ed3fc031b3da` |
| `TT.VOL` | 2,764,576,768 | not hashed |

Also present: directories `IRX/` and `EPSON/`.

## Game code packaging [C]

TT uses the same loader + `CORE.*` scheme as GT4, but in the **encrypted** form of the 2006 GT4
Online beta, not the plain form of retail v2.00:

- Retail v2.00 `CORE.GT4` is `[u16 0x0101][u32 size][raw deflate]` with no cipher
  (`scripts/ee_producer.py`). Running the beta decryptor on it fails the CRC-32C check, as expected.
- `CORE.TT` is exactly the container in `scripts/spec2_beta_elf.py`: u64 nonce
  `21785de852bdf035`, Salsa20/20 with key `PolyphonyDigital`, CRC-32C trailer `ea509de3` (matches),
  then the 0x0101 deflate container. `decrypt_core`, `inflate_container` and `parse_container` work
  **unchanged**.
- Inflated payload: 5,355,248 bytes, SHA-256
  `3c05ad44f0fb3484486ea816b51d35c4c53fe16c7b3196a9d77de558d54878a4`. Entry `0x00100008`
  (same as GT4). Three records, same shape as GT4:

| Record | Address | Size | SHA-256 |
|---|---|---:|---|
| `.reginfo` | `0x00578CA0` | 0x18 | `ad3bd1b5…847581a3` |
| text | `0x00100000` | 0x478CB8 | `b0685f5d…e415329d` |
| data | `0x00578D00` | 0xA28FC | `ed0024c2…ad54d61` |

- [C] `spec2_beta_elf.build_elf` produces a 5,359,868-byte ELF, SHA-256
  `97bf58d31538d542e86898e300ebed8a6309881c7c67327d026e8e6a1b11b614`.
  [H] As for the beta, the +8 MiB data zero-fill extent is assumed; the container does not carry it.

## Layout compared with GT4 v2.00 [C]

GT4 was rebuilt with `ee_producer.reconstruct_elf` and the pinned `usa-v2.00-native.json` policy
(ELF SHA-256 `10f82e22…6c7c935c`, which the function itself checks against the pin).

| | GT4 v2.00 | Tourist Trophy |
|---|---|---|
| Entry | 0x100008 | 0x100008 |
| Text | 0x100000, 0x517A14 bytes | 0x100000, 0x478CB8 bytes (−643 KB) |
| `.reginfo` | 0x617A14 | 0x578CB8 |
| Data | 0x617A80, 0xBE37C (+8 MiB BSS) | 0x578D00, 0xA28FC (+8 MiB BSS assumed) |
| `jr $ra` count | 30,330 | 30,730 |
| Source root string | `/projects/GT4Application` | `/projects-tt/GT4Application` |

- [C] TT is a branch of the GT4 application tree: the source root is literally
  `/projects-tt/GT4Application`, and the GT4 class names (`GT4_Motion`, `GT4PhotoMode`,
  `GT4GamePort`, the `GT4meter` HUD names) are all present.
- [C] TT has none of GT4's networking libraries (the Medius, DME and `rt_*` online stack). That
  accounts for much of the smaller text.
- [C] Code similarity, measured by splitting text at `jr $ra` and comparing function bodies:
  - 6,474 of 30,730 TT functions (21%) are byte-identical to a GT4 function;
  - 21,275 (69%) match once jump targets and 16-bit immediates are masked (address-only
    differences), covering 2.21 MB of 4.69 MB of text (47%);
  - among functions of 8+ words, 14,007 of 22,095 (63%) match after masking.
  - Strings: 3,746 of TT's 4,612 strings of 8+ characters also occur in GT4.
- Consequence: **every hard-coded guest address is different**, so no address in
  `native/game_target.hpp` carries over. But most host hooks target engine functions that exist in
  TT with the same body, so they can be found again by masked signature matching rather than by hand.

## Microcode [C]

- **VU1:** the encrypted VU1 image (0x3FF0 bytes, GT4 file 0x525DD0) occurs byte-for-byte in TT at
  file 0x488590 / VA 0x587290. The VU1 AOT table, which is keyed by content, should apply as is,
  just as it did for Spec II. [H] The upload routine's address differs and must be mapped.
- **VU0:** GT4's plain VU0 image (0xA80 bytes) matches TT's for its first 600 bytes (TT file
  0x519880), then differs in 1,323 of 2,688 bytes. It needs its own review, like Spec II's VU0.

## EE translation experiment [C]

2026-11-11, local and unpinned (`private/tt-ee-experiment/`). The toolchain was first checked by running the stock
`tools/ee_producer/build.cmd` on GT4 v2.00: generated sources were identical to the frozen 328-file inventory and the
327 objects matched `QUALIFIED_OBJECT_INVENTORY` after timestamp normalization.

The donor translator (rebuilt from the pinned commit with clang-cl 23.1.2) was then run on the TT ELF above with no
entry seeds: `eerecomp.exe TT.elf --out generated`.

| | Tourist Trophy | GT4 v2.00 (same translator) |
|---|---:|---:|
| Code | 1,082,980 instructions (4,230 KiB) | 1,240,374 instructions (4,845 KiB) |
| Functions | 14,499 | 14,660 |
| Entries | 125,650 | 135,093 |
| Units | 287 (+ `table.cpp`, `units.hpp`) | 326 |
| `stop()` fallbacks (unsupported op or delay slot) | 0 | 0 |

- All 287 units compile into `ps2game.lib` with the producer's CMake (`tools/ee_producer`, Release, no errors):
  65,100,410 bytes, SHA-256 `d600778e212200b2b828074b38de2ae6d5873890f03dd976fc48d2313e403c65`. Not a pin: the
  archive embeds paths and timestamps.
- Missed entries, from `scripts/spec2_entry_seeds.py --allow-unpinned` in delta mode: 992 candidates for TT (979
  fallthrough, 12 far `lui` pairs, 2 data words) against 1,076 for GT4 v2.00. The 12 far `lui` pairs are the class
  behind Spec II's Options-menu halt; seeding them from the start is cheap.
- [U] Nothing has run yet: booting needs a TT target in the native runtime (a library profile, a
  `game_target.hpp` entry with hooks disabled, VU0 handling).

## First boot and race [C]

2026-11-11, branch `task/tt-target`. `GT4_TARGET=tt` adds a hookless target (`kTTHooks = {}`; every host hook
disables itself and logs once) and requires its own library through `GT4_EE_LIBRARY`/`GT4_EE_LIBRARY_SHA256`.
Minimal local configuration: the default port from `scripts/prepare_external_vulkan.py` (its `gpu.cpp`, `gpu.hpp` and
stream-hooked `gs.cpp` match the release pins), the pinned slice153 shaders, Khronos Vulkan headers, SDL3 3.4.18, and no
performance, license guard, IPU, SPU2 or memory card capsule (VU microcode on the interpreter).

- The plain library boots TT: SCEA logo, the memory card prompt, the "Welcome to Tourist Trophy World" name entry. The IOP
  reboot with TT's own `IOPRP300.IMG`, its IRX set from disc, the IPU, VU1 and pad input all work without TT-specific code.
- Opening Options halted with `missing code at 0x00202FB0 (ra 0x002C20C8)`: one of the 12 far `lui` pair seeds above,
  the same class as the Spec II Options halt.
- Re-translating with all 992 seeds (`--entries`, 0 rejected) gives 15,641 functions, 1,112,246 instructions (4,344 KiB)
  in 287 units. With that library a player reached the main menu, opened Options and finished an Arcade race; a headless
  run with blind Cross presses reached course, bike, colour and rider equipment selection with no missing code.
- [U] Speed, audio and the license tests have not been assessed; widescreen and HUD hooks are unmapped (4:3).

## Release-style runtime, 16:9 hold and CoreGS 2.0 [C]

2026-11-11, branch `task/tt-target`.

- **16:9 hold (TASK-079) mapped** in `kTTHooks`: setter, preset copier, wide test and apply match v2.00 masked-identical
  (0x003FD938, 0x003FFA40, 0x003FFA08, 0x003FFB00); presets 0x585720..0x5857B0; owner pointer 0x5857DC and global
  0x579518 voted from the code that references the v2.00 words (43 and 11 sites). The option offset was confirmed in
  guest RAM (owner 0x008274B0 + 0x680 holds the wide record). TT stores the game-data pointer once (v2.00 moves it once
  at boot), so the hold settles through its 600-VBlank fallback, about 10 s after boot.
- **Frame-cost runtime:** `scripts/prepare_perf_runtime.py --mtvu --gs-stream --frame-cost` reproduces the slice262
  capsule exactly (7 sources and the receipt). TT pins it as `slice269-tt-frame-cost-runtime` with the slice191 port
  (`pin_frame_cost_runtime.py --base-manifest docs/inputs/slice191-gs-stream-runtime.json`), because `slice200-port`'s
  `main.cpp` was edited by hand and no script derives it; `prepare_external_vulkan.py --gs-stream-hooks` reproduces
  the slice191 port exactly.
- **VU1:** 47 snapshots recorded from TT (`PS2_VU_RECORD`) all equal the v2.00 decrypted blob; `vu1_image.py build`
  gives the pinned slice155 image (`9e5bc860...`). Full table: 2,048 entries, 0 refused.
- **VU0:** TT's upload routine (0x0046DE58, masked-identical to v2.00 `func_004A0398`) sends 0x920 bytes from 0x618580,
  byte-identical to Spec II's VU0 (`d0c85d37...`; padded image `e5ea672f...`). 14 entries (TT's 11 VCALLMS targets are a
  subset of the Spec II list), 0 refused.
- **CoreGS 2.0** (`GT4_HW_GS=ON`, Vulkan SDK 1.4.341.1, launched with `GT4_RENDERER=hw GT4_HW_CORE=2`): every 480i menu
  frame deinterlaced (`deinterlaced_frames` = presented frames), game fixes on, no visible regressions in owner play.
- Headless menus, 1,200 frames: 290 fps (minimal) against 365 fps (release-style). Race speed is the owner's check.

## License demos: guard and exact decrypt [C]

2026-11-11, branch `task/tt-target`.

- **Same cipher, Spec II-shaped loaders.** TT's cipher 0x004A4228 is v2.00 `func_00579E00` (masked-identical; FT at
  0x597DC0 and T8 at 0x6190A0 match the oracle formulas). Its wrapper 0x0042F7B0 has three callers: the two
  `license/%s` loaders 0x0010B6D0 / 0x0010B860 and the card path 0x0042F710. The loaders call vtable +0x510 / +0x1C8 (as
  Spec II) with a1 = buffer + header offset, returning to 0x0010B810 / 0x0010B9A0. The demo-input virtuals 0x00386D90 /
  0x00386DD0 add 0x1A24; `Manager::load` 0x002E8BD8 is masked-identical to v2.00.
- **Symptom without a fix:** starting License 1 crashed (`missing code at 0x00000000`, ra 0: a stack wiped by the
  record copy, the TASK-017 failure).
- **Guard (TASK-017) mapped for TT.** `kManagerOffset` is per target (TT 0x1A24, no body shift; v2.00 and Spec II keep
  0x1AA4). The guard capsule `slice269-tt-license-guard-runtime` is byte-identical to the release's slice376 derivation;
  only TT's table changes what it guards. With it, License 1 stopped crashing (`count=0x7F7F7F7F ... -> skipped`) but the
  demo did not play and the rider spawned off the course, as under PCSX2 without a patched disc: the demo file carries
  the license setup as well as the inputs.
- **Exact decrypt.** `native/tt_license_decrypt.*` ports the oracle's CRC-confirmed beam search (chop or chop + 1 ulp per
  div.s, the measured priors, saturation step <= 400, accepted only when the game's own check passes).
  `scripts/tt_units.py` adds one host call at TT's cipher entry (as `camera360_units.py` does for the camera);
  the call decrypts from the cipher text the game passes, sets v0 = 1 and returns, or leaves the game's code to run
  (guard behind it). `GT4_TT_LICENSE_EXACT=0` turns it off. GT4's units, capsules and pins are untouched.
- **Result (owner, License 1):** `exact (CRC confirmed, saturation step 241, 78 div.s one ulp up, ~850 ms)`; the guard
  then reports `crc=ok layout=ok count=0x00000000` at body + 0x1A24 (confirming TT's offset). The demo plays and the
  owner's own run starts on the course.
- Checks: `native_tt_license_decrypt` (six synthetic files encrypted with hardware-like ulp choices are recovered byte
  for byte; a wrong key is refused), `native_tt_hooks`, and the v2.00 `native_license_guard` fixture now compiles the
  guard itself, so it tests the v2.00 rule in any target's build.
- [U] Other licenses: GT4 recovered 528 of 592 files with this search; a TT file without a confirmed chain falls back to
  the guard (no crash, no demo).

## Unknowns

- [U] IOP side: whether `IOPRP300.IMG` and `IRX/` match GT4's set. GT4's IOP modules were not compared.
- [U] What `EPSON/` contains (probably printer support for photo mode, as in GT4).
- [U] Whether TT's license-test demos desync in the recomp the way they do under PCSX2
  (see [LICENSE-DEMO-CIPHER.md](LICENSE-DEMO-CIPHER.md) and `native/license_guard.*`).
- [U] `TT.VOL` format; [H] the same RoFS volume as `GT4.VOL`.

## What a third target needs

1. A TT input path: a `CORE.TT` reader that reuses the beta decryptor, plus new pins in
   `docs/inputs/` (a disc manifest and a native ELF policy, never edits to existing pins).
2. Its own EE translation and symbol map from the ELF above (translation and library compile already work
   locally; see the experiment section).
3. A TT entry in `game_target.hpp`, starting with every hook disabled (the existing "no entry, log
   once" behaviour), then hooks mapped one by one. A masked-signature tool that maps GT4 hook
   addresses to TT candidates would cover most of them.
4. VU0 review; VU1 reuse checked at runtime.
5. Launcher entry and disc identification by hash.

Recommendation: the first milestone (boot to title with every hook disabled) looks reachable with
existing tooling. The work is mostly the translation pipeline and pins, not new reverse-engineering
of the container or the microcode.

## Launcher integration and display holds [C]

2026-11-11, branch `task/tt-target`.

- **Third game in the launcher.** `gt4l_core::games::TT` (id `tt`, `game\gt4-tt.exe`, ELF `SCUS_975.02`, card
  `BASCUS-97502GAMEDATA` as read from a TT card). `gt4l-disc` identifies SCUS-97502 by its code (`docs/inputs/tt-iso.json`:
  `CORE.TT`, the loader, `SYSTEM.CNF`, `IOPRP300.IMG` and the 16 IRX modules) and prepares the ELF in Rust
  (`tt_elf.rs`: Salsa20/20 + CRC-32C + the v2.00 inflate + the `build_elf` layout). On the contributor's disc it derives
  the pinned ELF (`97bf58d3...`) in under half a second (`tests/tt_disc.rs`, ignored by default). Setup, the installed
  check, process names, the home selector ("Tourist Trophy", window sized to the longest name) and the Saves tags handle
  TT; a different SCUS-97502 code is refused with its own message (en-US, pt-BR). Owner test: install from the ISO,
  switch games and play.
- **16:9 hold settle.** TT publishes one game-data object and never replaces it, so the v2.00 rule (wait for one move,
  else 600 VBlanks) held 4:3 for about 10 s. `kOwnerMovesAtBoot` is false for TT: the hold settles after 30 stable
  VBlanks (fresh card: VBlank 37).
- **Race display 480p (`GT4_FORCE_480P`) mapped.** Option + 0x1718 (as Spec II); setter 0x003FDBA8 and race-start reader
  0x003FCE58 match v2.00. TT's names are "normal", "480p", "1080i", "480i_frame"; the hold rewrites 0 and 2 to 1 and keeps
  3. Before it, every TT frame under CoreGS 2.0 was 480i and deinterlaced (softer than GT4's 480p races); owner: races now
  as sharp as GT4. Menus stay 480i, as in GT4.
- Launcher tests: 668 pass; `gs_golden` and `launch_contract`'s session INI comparison fail on this checkout because the
  golden INI was checked out with doubled CRs (`\r\r\n`), before and after the TT changes.

## Controls: trigger range and sticks [C]

2026-11-11, branch `task/tt-target`.

- **Triggers (TASK-161).** TT's race input 0x00403810 has v2.00 `func_0043BBC0`'s pressure switch unchanged in behaviour:
  four cases with the same flags, stores and divisors (0x2 / +8 / 35 throttle, 0x4 / +10 / 65 brake, 0x10 / +14 / 20,
  0x8 / +12 / 35), only with the divisors in saved registers (s1, s6, s5). Without v2.00's statement replacements the host's
  full-range input met the stock `min(100 * 2p / d, 256)` scale, so full throttle came early in the trigger's travel.
  `scripts/tt_units.py` (formerly `tt_license_units.py`; it now applies every TT unit edit) replaces the doubling and the
  divisor of the throttle and brake cases (0x00403B6C / 0x00403BA0 and 0x00403BC8 / 0x00403BFC), exactly as v2.00's.
  `tests/python/test_tt_units.py` covers the edits and their refusals. Owner: the triggers feel like GT4's.
- **Sticks.** GT4Recomp skips GT4's built-in stick dead zone (bytes 90..165, knot table v2.00 0x0064C4D8). TT's table
  (0x00598438) is byte-identical and the code that reads it matched, so the host's stick rule is already right for TT.

## Release runtime capsules for TT [C]

2026-11-11. The TT build now takes the same three game-independent capsules as the rc16 release, each regenerated by its
script and matching the release pin `docs/inputs/slice408-pcsx2.json`: the crash-safe memory card
(`prepare_memcard_flush.py`, `sio2.cpp` hash in the pin), the SPU2 envelope fix (`prepare_spu2_runtime.py`, receipt in the
pin) and the fast IPU decoder (`prepare_ipu_fast.py`, receipt in the pin). `native_memcard_store`, `native_spu2_envelope`
and `native_ipu_fast` pass in the TT build; owner check: save, quit and reload, sound and video all normal. The headless exit
flush of the release lives in the hand-edited `slice200-port` `main.cpp`, which TT does not use (slice191 port); windowed
play flushes in `window.cpp`.

## Frame skipping (TASK-082) [C]

2026-11-11. Mapped in `kTTHooks` (sources in its comment): the main loop 0x00100FE8 and its update / render / idle functors
(0x001007D8 / 0x00100848 / 0x001008B8, as Spec II), the flip's page words 0x005795AC / 0x00579628, the race vtable cluster
0x005C7000..0x005D6000, DynamicsConductor at race + 0x70 with the step clock at + 0x23960 (as Spec II), flare offsets as
Spec II. The race clock and objects were found in RAM during a headless Arcade race (Laguna Seca): the only word that grew by
exactly 3000 over 60 frames sat at DynamicsConductor + 0x23960, and the race object's update / render / idle virtuals are
those of the four license race classes.

Check (the same race twice, `GT4_FRAME_SKIP_FORCE=1` and `=2`, CoreGS 2.0, 480p): the second run left out 800 frames, all in
the running race (never two in a row); the race clock is identical on all 3,597 frames (24.62 s at the end) and both runs end
on the same EE cycle count; the frames of both runs match. The per-frame digest of the 0x24000-byte block differs from the
first frame left out, but only in a few words that do not spread (6 at frame 2,400, 12 at 3,500, all within + 0x6B44..0x73xx
and + 0xB08x..0xB85x; the race object's render-side words differ as expected): values written while drawing, not the
simulation, which would diverge across the block within a few hundred frames.
