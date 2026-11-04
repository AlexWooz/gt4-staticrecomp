# Gran Turismo 4 Spec II v1.10 — patch set and plugin injector map (TASK-100 S1)

2026-10-22. Read-only analysis of the owner's two discs (nothing extracted is committed).
Task plan: [SPEC2-PLAN](SPEC2-PLAN.md). Decision: [0072](decisions/0072-spec2-second-target.md).

This page answers one question: **what does Spec II change in the game?** Two sources, and both are mapped here:

1. **Static**: bytes already different in the shipped `SCUS_974.36` file. Measured by reconstructing the unmodified
   Public Beta's game image and diffing it against the Spec II ELF, section by section.
2. **Injector**: words the plugin writes into guest RAM at boot. Recovered by executing the plugin's init routine on
   an EE-RAM image and recording every store (no game code runs, no JIT).

Machine-readable table: [`inputs/spec2-patches.json`](inputs/spec2-patches.json). Beta image pin:
[`inputs/spec2-beta.json`](inputs/spec2-beta.json).

## Identity [Confirmed]

| Image | Size | SHA-256 |
|---|---|---|
| GT4 Online Public Beta ISO (`Gran Turismo 4 (USA) (Beta).iso`) | 2,654,109,696 | `1f49b9d8077801188860b62c3efdbc631ba8af7df979373811ce8c60dcc48ebe` |
| Beta boot loader `/SCUS_974.36;1` | 491,900 | `8a5fe10a43128f0d362d8e6e62d026f6f1ee30fb9b508167a8a3a775458425d1` |
| Beta game image `/CORE.GT4;1` (encrypted) | 2,269,619 | `0cf52c3ad8e9c1ddba55e7538a35dfed1c02cd0e38e5c348bdf80975046617ff` |
| Beta game ELF, rebuilt (see below) | 6,510,076 | `2e5621ede797ff064eef4c51eff7d4ad01e0cefb82bc9895107ee818192aa039` |
| Spec II ISO | 3,683,418,112 | `6a3484b9672c10a92154ecee6faa5bbcfd4ed8cd538d8283e429c3fe6e1704b2` |
| Spec II `/SCUS_974.36;1` (plain ELF) | 6,520,944 | `db08421bc26bdc0298e80cb02a8f2067ddf640f0575af5dfe0548f1fd73ac39d` |

**The beta is the exact base of Spec II 1.10.** The community patch
`Gran Turismo 4 Spec II (1.10 Full).xdelta` (689,527,318 bytes) decoded with the beta ISO as source produces a file
whose SHA-256 is exactly the owner's Spec II ISO (`6a3484b9…04b2`). The decoded scratch ISO was deleted after the
check; the `.xdelta` is not used for anything else. [Confirmed]

## How the beta image was recovered

The beta disc is protected differently from the retail v2.00 disc: it boots a loader and keeps the game in an
encrypted `CORE.GT4`. The loader (`SCUS_974.36`, plain ELF, VA base `0x01000000`) contains the routine.

- **Container** [Confirmed]: `[u64 LE nonce][Salsa20/20 ciphertext][u32 LE CRC-32C of everything before it]`.
  Decrypt, then inflate `[u16 tag 0x0101][u32 size][raw deflate]`, then read the same record container as retail
  v2.00: two `u16`-length 128-byte header blobs, `i32` record count, `u32` entry, then
  `{u32 address, u32 size, body}`.
- **Cipher** [Confirmed]: Salsa20/20, 64-byte blocks, 16-byte (tau) key variant, 64-bit nonce in words 6/7, 64-bit
  counter in words 8/9 starting at 0.
- **Key** [Confirmed]: `PolyphonyDigital`, stored XOR `0x55` at loader VA `0x10724C8` (file `0x734C8`).
- **Validation** [Confirmed]: CRC-32C `0x07B4C92D` matches the trailer, the declared inflated size
  (6,505,204 bytes) is reproduced exactly with no trailing bytes, and a one-byte key change makes the inflate fail.
- **Records** [Confirmed]: 24 bytes at `0x0065FBA4` (`.reginfo`: 20 zero bytes then gp `0x0073C1F0`), text at
  `0x00100000` (0x55FBBC bytes, hash `2394249b…`), data at `0x0065FC00` (0xD45FC bytes, hash `9b505c4f…`).
  Entry `0x00100008`.
- The `gencrypt`, `desx`, `des_ede`, `rc5_64`, `cbc_interleaved` and `ofb_pipelined` strings in the loader belong to
  the DNAS/anti-piracy module (`DNAS310.IMG`), not to the game image path. [Confirmed]

Tool: `scripts/spec2_beta_elf.py` (writes the rebuilt ELF and the plain container payload into a fresh
`private/slice271-spec2-beta-elf/`). Layout follows the retail v2.00 recipe (`scripts/ee_producer.py`): text at file
`0x1000`, `.reginfo` after it, data page-aligned, `+8 MiB` zero-fill for the data segment (Hypothesis: the same BSS
extent as Spec II and v2.00; the container does not carry it).

The container's `.reginfo` record sits at `0x65FBA4`, inside the last 24 bytes of the text record, while Spec II keeps
those bytes as text and puts `.reginfo` at `0x65FBBC`. The text record is applied after the reginfo, so the loaded
image is the same; the rebuilt file uses Spec II's non-overlapping layout. [High confidence]

## Static patch set: one word and one segment

The rebuilt beta image and Spec II's `SCUS_974.36` have the same layout (text 0x55FBBC at `0x00100000`, data 0xD45FC
at `0x0065FC00`). Comparing every file-backed word:

| What | Detail |
|---|---|
| Words different outside the plugin segment | **1**: `0x001001E8` |
| Plugin segment | `0x0068A480`, 0x2CCC bytes, RWX, over zero-filled data |

- `0x001001E8`: beta `0x0C178EAA` (`jal 0x005E3AA8`) → Spec II `0x0C1A29DA` (`jal 0x0068A768`, a thunk that jumps to
  the plugin's init at `0x0068A480`). The plugin's first action (`0x0068A490`) is to call `0x005E3AA8`, i.e. the
  original function, so the trampoline loses nothing. [Confirmed]
- `0x0068A480..0x0068D14C`: in the beta this is 0x2CCC zero bytes inside the data image; Spec II fills it with the
  plugin (code and data, including a table of game function addresses at `0x0068D020..0x0068D14C`). [Confirmed]
- Every other byte of the beta's own code and data is **identical** in Spec II. Spec II makes no other static code
  change; all remaining behaviour differences are written at boot by the injector below. [Confirmed]

Consequence for S2: the derived ELF is the Spec II ELF with the injector's stores applied ahead of translation. No
beta-based re-derivation is needed, and the plugin segment stays translatable code (it contains real R5900 code).

## Injector map

The init at `0x0068A480` runs once from `0x001001E8`. It executes 356 of the plugin's 2,642 instructions in a
straight line: 2 calls out of the plugin and 35 stores. It reads **no settings and no arguments**, so this patch set has
no variants. [Confirmed]

Evidence for "no variants": the same table is produced with three different stack pointers and with the external
call's return value forced to 0 or garbage (`--perturb`), and the executed path contains no branch on loaded data
before the stores.

Two calls leave the plugin during init:

| Call site | Target | What it is |
|---|---|---|
| `0x0068A498` | `0x005E3AA8` | the beta's own boot initialiser, the function the trampoline replaced |
| `0x0068C808` | `0x004E2940` | called from `0x0068C7C4`, reached from the hook-install path at `0x0068B140` |

Plugin helpers used by the init (all small, Confirmed):

| Helper | Effect |
|---|---|
| `0x0068A7E4` | `*(u32*)a0 = a1` (store word) |
| `0x0068A89C` | store 0 at `a0` |
| `0x0068A794` | store `jal target` at `a0` (encodes `(target >> 2) & 0x03FFFFFF \| 0x0C000000`) |
| `0x0068A84C` | store `j target` and zero the delay slot |
| `0x0068A950` | store `jal target` and zero the delay slot |
| `0x0068A8DC` | store 16-bit halfword |
| `0x0068A814` | store a float (`swc1`) |

### The 35 stores

Table generated from `scripts/spec2_patches.py`; `old` is the beta word (Spec II file word), `new` the injected word.

| Address | Region | Old | New | Kind | Note |
|---|---|---|---|---|---|
| `0x00121D58` | text | `0x0C13BF00` | `0x0C1A2C77` | jal_into_payload | hook: jal 0x004EFC00 -> payload 0x68B1DC (calls the original, then a two-entry callback table at 0x68D090) |
| `0x00132E60` | text | `0x0C13C194` | `0x0C1A2CCD` | jal_into_payload | hook: jal 0x004F0650 -> payload 0x68B334 |
| `0x0014EC70` | text | `0x0C13C2C4` | `0x0C1A2DD1` | jal_into_payload | hook: jal 0x004F0B10 -> payload 0x68B744 |
| `0x0020B594` | text | `0x27BDFFD0` | `0x03E00008` | jr_ra | prologue rewrite: function at 0x0020B590 returns immediately (addiu sp -> jr ra); the delay slot stays lbu v0,0x1E60(v1) |
| `0x0020B744` | text | `0x54400006` | `0x14000006` | branch | bnel -> bnez: the delay slot now always runs |
| `0x00212D68` | text | `0x50400009` | `0x00000000` | zero | remove `beql v0,zero` (early-out) -> nop |
| `0x00212D70` | text | `0xC600051C` | `0x460017C6` | float op | `lwc1 f0,0x51C(s0)` -> `mov.s f31,f2` (f31 becomes a spare across the block) |
| `0x00212D74` | text | `0x46140034` | `0x46011034` | float cmp | `c.olt.s f0,f20` -> `c.olt.s f2,f1` |
| `0x00212D7C` | text | `0xC6020510` | `0xC61F0510` | float load | `lwc1 f2,0x510(s0)` -> `lwc1 f31,0x510(s0)` |
| `0x00212D80` | text | `0xA2000620` | `0x46006FC6` | float op | `sb zero,0x620(s0)` -> `mov.s f31,f13` |
| `0x00212D84` | text | `0x10000068` | `0x00000000` | zero | `b 0x212F28` -> nop (fall through instead of jumping) |
| `0x00212DA0` | text | `0x4600B886` | `0x4600BFC6` | float op | `mov.s f2,f23` -> `mov.s f31,f23` |
| `0x00212DC0` | text | `0x46161501` | `0x4616FD01` | float op | `sub.s f20,f11,f22` -> `sub.s f20,f31,f22` |
| `0x0023F438` | text | `0x0C08FCDC` | `0x0C1A2AF1` | jal_into_payload | hook: jal 0x0023F370 -> payload 0x68ABC4 (6-entry table lookup at 0x68D020, returns the index or -1) |
| `0x0023F444` | text | `0x0C08FCEC` | `0x0C1A2B6D` | jal_into_payload | hook: jal 0x0023F3B0 -> payload 0x68ADB4 |
| `0x00240254` | text | `0x12400007` | `0x00000000` | zero | `beq s2,zero,0x240274` -> nop |
| `0x0027EAD8` | text | `0x24050004` | `0x2405000B` | immediate | `addiu a1,zero,4` -> `addiu a1,zero,11` |
| `0x005225F0` | text | `0x0C1757A6` | `0x00000000` | zero | `jal 0x005D5E98` -> nop |
| `0x006605F4` | data | `0x00000003` | `0x00000001` | data word | flag 3 -> 1 (no reader found in a text scan) |
| `0x006F02B0` | data | `0x6D726F6E` | `0x30687865` | string | sound bank table: `normal0` -> `exh0` |
| `0x006F02B4` | data | `0x00306C61` | `0x00000000` | data word | entry tail `al0\0` -> 0 (8-byte entries) |
| `0x006F02B8` | data | `0x6D726F6E` | `0x31687865` | string | `normal1` -> `exh1` |
| `0x006F02BC` | data | `0x00316C61` | `0x00000000` | data word | entry tail `al1\0` -> 0 |
| `0x006F02C0` | data | `0x6D726F6E` | `0x32687865` | string | `normal2` -> `exh2` |
| `0x006F02C4` | data | `0x00326C61` | `0x00000000` | data word | entry tail `al2\0` -> 0 |
| `0x006F02C8` | data | `0x6D726F6E` | `0x33687865` | string | `normal3` -> `exh3` |
| `0x006F02CC` | data | `0x00336C61` | `0x00000000` | data word | entry tail `al3\0` -> 0 |
| `0x006F02D0` | data | `0x62727574` | `0x30687865` | string | `turbo0` -> `exh0` |
| `0x006F02D4` | data | `0x0000306F` | `0x00000000` | data word | entry tail `o0\0` -> 0 |
| `0x006F02D8` | data | `0x62727574` | `0x31687865` | string | `turbo1` -> `exh1` |
| `0x006F02DC` | data | `0x0000316F` | `0x00000000` | data word | entry tail `o1\0` -> 0 |
| `0x006F02E0` | data | `0x62727574` | `0x32687865` | string | `turbo2` -> `exh2` |
| `0x006F02E4` | data | `0x0000326F` | `0x00000000` | data word | entry tail `o2\0` -> 0 |
| `0x006F02E8` | data | `0x62727574` | `0x33687865` | string | `turbo3` -> `exh3` |
| `0x006F02EC` | data | `0x0000336F` | `0x00000000` | data word | entry tail `o3\0` -> 0 |

Counts: 18 text, 17 data, 0 inside the plugin.

### What the groups do [Hypothesis / Confirmed as stated]

- **Sound bank names** [Confirmed]: the guest table at `0x006F02B0` is `normal0..normal3`, `turbo0..turbo3`,
  `motosound`, `carsound` (8-byte entries). Spec II rewrites both four-entry groups to `exh0..exh3`, matching the
  `carsound/exh0..3` sample banks it adds to the volume (decision 0072). This is why copying retail audio into Spec II
  plays nothing.
- **Five `jal` hooks** into the plugin [Confirmed targets, Hypothesis purpose]: `0x00121D58` (calls 0x4EFC00 then a
  two-entry callback table), `0x00132E60` and `0x0014EC70` (race/sound updates at 0x4F0650 and 0x4F0B10),
  `0x0023F438` and `0x0023F444` (a six-entry lookup and a second handler in the AI/race path).
- **Gear/ratio block `0x00212D68..0x00212DC0`** [Confirmed instructions, Hypothesis purpose]: the plugin frees f31
  as a scratch register, removes an early-out and a jump, and changes which operand the final `sub.s` uses. It sits in
  the block that computes `0x50C / (0x50C + 0x508)`, i.e. a ratio (transmission), one instruction before the value is
  used for a shift decision.
- **`0x0020B594`** [Confirmed]: `0x0020B590` starts with `lui v1,0x66; lbu v0,0x1E60(v1)` and then a long body; the
  injector turns its prologue into `jr ra`, disabling the function while keeping its first two instructions.
- **`0x005225F0`** [Confirmed]: a `jal` replaced by a nop.
- **`0x0027EAD8`** [Confirmed]: an immediate 4 becomes 11 (`addiu a1,zero`). The word is used two instructions before
  a `swc1` of a scaled value; the plugin's own strings mention "Odometer test" and "Engine life test".

## The plugin's own code

- 2,642 instructions in 0x2CCC bytes; three sections of interest: helpers at `0x68A7E4..0x68A9A8`, boot-time installers
  at `0x68A9A8..0x68B744`, and runtime handlers at `0x68AA04..0x68C82C` plus an import/data table at
  `0x68D020..0x68D14C`.
- The data table holds absolute game addresses (`0x00205278`, `0x0050F1D8`, `0x005DF338`, `0x004E2940`, `0x004F0650`,
  `0x004F0B10`, `0x004EFC00`, `0x0010D598`, …), i.e. the plugin was linked against the beta's symbol addresses. There
  is no runtime symbol binding to reproduce: the awk addresses are already final. [Confirmed]
- Runtime handlers read function pointers from guest RAM (`0x68D09C` and neighbours) that the table above fills.
  Since the table is part of the segment, the handlers need no loader cooperation in a recompiled target. [High
  confidence]
- Strings: camera toggles (`toggleBonnetCam`, `toggleInteriorCam`, `toggleRaisedCam`, `toggleRoofCam`, `toggleGT3Cam`),
  `toggleGaugeMode`, `toggleTriggerSensitivity`, `setTransmissionVolume`, `overrideLaps`, `toggleMultiCategoryAI`,
  `ProcessTransmissionMT`, `getPerformanceIndex`, `Odometer test`, `Engine life test`, `argv size/str cast`,
  `mMyModule`, `MMyModule`, `GetHookSTDCVersion`. [Confirmed, they are literals in the segment]

## Open points

- `0x006605F4` 3 -> 1: no reader was found by a linear scan of the text; the meaning stays Unknown. It is harmless to
  apply (it is the beta's own data word being changed).
- The two 128-byte header blobs after the inflate (`987d146d…`, `725a0bc2…`) are opaque; the loader's in-path validator
  is a stub that always passes, so their meaning does not affect the image. Unknown.
- The plugin's runtime handlers write further guest state while the game runs. That code is translated like the rest
  of the segment, so it is not part of the build-time patch set; S7b must confirm that none of it depends on being
  loaded at a particular address or on the EE's instruction cache.
- Purpose labels marked Hypothesis above are documentation only; the words themselves are Confirmed by the
  interpreter run and by the beta diff.

## Reproduction

```
# beta game image (writes only into a fresh private/ directory)
private/tooling-venv/Scripts/python.exe scripts/spec2_beta_elf.py ^
  --iso "C:\Antigravity\gt4-staticrecomp\Gran Turismo 4 (USA) (Beta).iso" ^
  --output C:\Antigravity\gt4-staticrecomp\private\slice271-spec2-beta-elf ^
  --json docs/inputs/spec2-beta.json --check-spec2 <extracted Spec II SCUS_974.36>

# injector patch table + static diff
private/tooling-venv/Scripts/python.exe scripts/spec2_patches.py --iso "<Spec II ISO>" ^
  --beta-payload C:\Antigravity\gt4-staticrecomp\private\slice271-spec2-beta-elf\beta-core-payload.bin ^
  --perturb --emit-json docs/inputs/spec2-patches.json
```

Both tools read the owner's discs and write only hashes, addresses and words into the repository.
