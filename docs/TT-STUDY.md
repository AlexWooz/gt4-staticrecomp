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
