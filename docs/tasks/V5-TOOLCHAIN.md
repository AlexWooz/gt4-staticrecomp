# TASK-214 - the Adhoc v5 page table and the GT4.VOL ToC writer (Phase 1)

Read-only research plus two new tools. Branch `task/214-v5-toolchain` (from `task/213-native-settings` at `3c4cbc77`),
2026-11-10. No game data, no build, no in-game run, no ISO write. Every private byte used here came from a bounded,
read-only extraction of the owner's disc into `private/slice437-option/`.

Tools: `scripts/adhoc_v5.py` (v5 page-table reader and same-length patcher) and `scripts/gt4_vol_toc.py` (RoFS
catalogue reader and leaf-extent writer). Tests: `tests/python/test_adhoc_v5.py` and
`tests/python/test_gt4_vol_toc.py` (synthetic fixtures only).

## 1. The third-party toolchain is not usable (blocking fact)

The brief assumed that GTAdhocToolchain is MIT and therefore allowed. The license is still MIT, but the repository
now forbids AI use:

- upstream `master` = `a15f748d6445ae8515c0b0c3c44df569672b750a` (2026-11-10, "Revise SPDX. Update PDTools.");
- every source header now carries
  `SPDX-FileNotice: AI usage and consumption is prohibited. See AGENTS.md for policy.`;
- `AGENTS.md` (1,949 bytes) is titled "AI / LLM Use Is Not Permitted" and prohibits use of the repository "including
  source code, documentation, commit history, issues, pull requests, assets, and metadata" for training, inference,
  retrieval, analysis, modification/automation and **reading and processing**, and instructs AI agents not to act,
  not to treat a repeated request as authorization, and warns against deleting the file to bypass it. The older
  snapshot in `private/slice151-research/nenkai/GTAdhocToolchain` (`3fecc2e`, 2026-06-26) has no such notice, but the
  policy explicitly covers commit history, so the older snapshot is not a workaround.

A fresh shallow clone (with submodules `esprima-dotnet` `55fe5f2` and `PDTools` `3356dfa`) was made before the notice
was seen, and one build was attempted. It fails at the current pins:

```
dotnet build GTAdhocToolchain.CLI -c Release   # net10.0 CLI, net9.0 libraries
GTAdhocToolchain.MenuResources/GpbBase.cs(90,51): error CS0117:
  'TextureSet3' does not contain a definition for 'TextureSetPlatformFormatType'
175 warning(s), 1 error(s)
```

The clone was then deleted (`private/slice437-toolchain/toolchain` removed; only an empty directory remains) and no
third-party code is used, built, or copied. **Consequence:** there is no third-party v5 compiler, disassembler or
project builder available to us, and the work below is our own clean-room tooling. The MIT license permits use in
general; the author's explicit request is what we follow.

## 2. What the v2.00 `option.adc` page table is (Confirmed)

`option.adc`: 251,715 decoded bytes, `ADCH005\0`, source name `_tmp_option.ad`, stored 55,353 bytes as PS2ZIP
(kind 2) at volume offset 87,158,784. SHA-256 decoded `acd29dd0...484c`, stored `b1f61aa8...d9f0`.

Header: 8-byte magic, `u16` length + source name at `0x08`, then several unqualified words (`0x110`, `0x8E`,
`0x1D2F`... in file order) and the module/type tables. The remaining header fields are **Unknown**; nothing in this
tool depends on them.

Text is `u16` little-endian length + ASCII. A page-table entry is a two-string constructor call:

```
u16 name_len, name            e.g. 04 00 "Race"
u32 class_id                  e.g. 166 (0xA6)
24 02 00 00 00                two-argument tag + pad
u16 root_len, root            e.g. 08 00 "RaceRoot"
u16 root_len, root            same text again
u32 trailer                   e.g. 98   (meaning unqualified)
```

The same encoding is used outside the page list (menus, enum tables, key bindings), so the tool scopes the scan to the
Options region `0x8A40..0x99DD` and requires the two root strings to be equal. With those rules it finds exactly the
`options` list container plus the 24 pages of the mapping table, and the four `Dummy` entries:

| offset | name | class id | root | trailer |
|---|---|---|---|---|
| 0x8A4B | options | 164 | options | 96 |
| 0x8B03 | Race | 166 | RaceRoot | 98 |
| 0x8B71 | Replay | 167 | ReplayRoot | 99 |
| 0x8BE5 | Screen | 168 | ScreenRoot | 100 |
| 0x8C59 | Color | 169 | ColorRoot | 101 |
| 0x8CCA | Sound | 170 | SoundRoot | 102 |
| 0x8D96 | Arcade | 173 | ArcadeRoot | 103 |
| 0x8E0A | SplitBattle | 174 | SplitBattleRoot | 104 |
| 0x8E8D | LAN | 175 | LANPopup | 105 |
| 0x8EFA | Network | 176 | NetRoot | 106 |
| 0x8F69 | Course | 177 | CourseRoot | 107 |
| 0x903B | Album | 180 | AlbumRoot | 108 |
| 0x9243 | Controller | 187 | ControllerRoot | 109 |
| 0x92D7 | DS2_port0 | 188 | KeyconfigRoot | 110 |
| 0x938C | DS2_port1 | 189 | KeyconfigRoot | 110 |
| 0x9441 | Steering | 190 | SteeringRoot | 113 |
| 0x94BB | CTR_port0 | 191 | KeyconfigRoot | 110 |
| 0x955D | CTR_port1 | 193 | KeyconfigRoot | 110 |
| 0x95FF | CGR_port0 | 194 | KeyconfigRoot | 110 |
| 0x96A1 | CGR_port1 | 195 | KeyconfigRoot | 110 |
| 0x9743 | JGR_port0 | 196 | KeyconfigRoot | 110 |
| 0x97E5 | JGR_port1 | 197 | KeyconfigRoot | 110 |
| 0x98E0 | Misc | 200 | MiscRoot | 120 |
| 0x994E | Unit | 201 | UnitRoot | 121 |
| 0x99BC | Language | 202 | LanguageRoot | 122 |

Dummy entries (zero-argument constructor: name, class id, byte `0x16`, class id repeated, byte `0x16`, class id
repeated): `0x90AC` id 181, `0x90FA` id 182, `0x9148` id 183, `0x9196` id 184. Each is 16 bytes. They carry no root
window and there is no spare byte inside them.

`LANPopup` is not a dangling name: it is a `RootWindow` declared inside `OptionRoot`'s children (decoded
`option.mproject` line 335) with `has_script 1`. Whether a nested `RootWindow` is acceptable as a page root is
**Unknown**.

## 3. The reassembly gap, exactly

Adding a page means inserting a constructor call (name, class id, `0x0224` tag, one or two root strings, trailer) into
the page-list sequence. That shifts every following byte. The v5 image has no visible relocation table, cross
references use small symbol ids (for example class id 166) rather than file offsets, but the header also carries
count-like fields that are not qualified, and no consumer of this file has been observed to tolerate a shifted
stream. So insertion is **Unknown and unsafe** until the header fields and the serialization grammar are qualified.

The three routes from the brief:

1. **Repurpose a Dummy entry same-length.** Refused by the bytes: a Dummy has no root string and needs 5 + 2 + 8 + 2
   + 8 = 25 more bytes for a `RecRoot1`-sized root pair, and it has zero slack.
2. **Minimal v5 script rewrite** (page table + a page using the existing primitives). Blocked: it needs a v5
   assembler. We did not write one; that is a multi-week reverse-engineering task and far beyond a safe round.
3. **Same-size retarget of an existing registered page.** Feasible and proven at the byte level (section 4).

## 4. The smallest same-length registered-page edit (proven offline)

`scripts/adhoc_v5.py patch --page LAN --root RecRoot1` rewrites only the two root strings of the `LAN` entry:

```
decoded before 251,715 bytes  sha256 acd29dd0507bdb479b0a64e2d8603b3e40a2ad34ab270479772a3e40bf56448c
decoded after  251,715 bytes  sha256 f1c8328311638c1129eafde5febc118458a3f95e82c9615aee44f563f9b0b719
changed bytes  0x8E9D len 4, 0x8EA2 len 3, 0x8EA7 len 4, 0x8EAC len 3   (14 bytes, all inside the two root strings)
```

Every other byte, every offset and the total length are unchanged, so no jump target, string offset or class id moves.

Re-packing the edited file with the accepted same-length pipeline (`scripts/gt4_data_patch.py: ps2zip`, padded to the
record's stored extent):

```
PS2ZIP container 54,395 bytes <= stored extent 55,353 bytes (958 bytes spare)
inflate -> 251,715 bytes, byte-identical to the edited file, complete DEFLATE stream
container sha256 282978d0d70beba9043075f1c697461cdc2b79af87e0bd025bc9fcc7dcabc843
```

So this edit needs **no ToC change**: it fits the existing TASK-212 data-patch runtime unchanged.

The project side also fits. A new `RootWindow {"RecRoot1"}` with `has_script 0`, a `Composite "Pane"` and a
`TextFace "Title"` added to `Project.children` (count 20 -> 21) minifies to 462,708 bytes and packs to
14,244 bytes, against the `option.mproject` stored extent of 22,449 bytes (8,205 bytes spare).

**Trade-off (owner decision):** a same-length retarget replaces one existing page. `LAN` is the candidate used for the
proof; `Network` was excluded earlier. Nothing here is a free extra page.

**What is not proven:** that the game accepts a page whose root is the new scriptless `RootWindow`, renders it, and
lists it in the Options screen. That needs the TASK-213 build and an in-game or headless run; it was not done in this
round (no build, no window).

## 5. GT4.VOL ToC writer (working)

Format (all Confirmed against the v2.00 ISO): header `u32 magic 0xACB990AD | u32 version 0x00030001 | u32 stored |
u32 physical_pages | u16 2048 | u16 logical_pages`; `logical_pages + 1` u32 offsets at 64, each XORed with
`(index + 1) * 0x14AC327A`; each page is raw DEFLATE of 2,048 bytes, XORed with `0x55`; page header `u16 kind,
u16 count, i32 next, i32 prev`; 8-byte descriptors indexed from the end; leaf key `u32 BE parent + name`; leaf
metadata 5/13/17 bytes for kinds 0/1/2.

`scripts/gt4_vol_toc.py` reads the catalogue, changes an **existing** leaf record's `data_page`/`packed`/`size`,
re-deflates only that page, and rebuilds the offset table and stored size. Unchanged pages keep their original
compressed bytes, so an unmodified catalogue re-emits byte-identically.

Real ToC round trip (read-only from the ISO):

```
stored 365,622 bytes, 179 physical pages (366,592 reserved, 970 spare), 407 logical pages, 18,562 records
re-emitted 365,622 bytes, sha256 c308312caf6cb2e0fed42158c5463bdb353d35ac8e773ef55fa4f8634f397d9b  (identical)
```

`option.adc`'s record and its space:

```
leaf page 212, entry 13, parent id 9031, node kind 2
data_page 33842  -> volume offset 87,158,784  (data pages start at ToC offset 0x10AC800 + 179 * 2048)
packed 55,353    decoded 251,715    pages used 28
next file at page 33870 -> slack 1,991 bytes inside its own last page
```

Growth plan inside the same allocation (no move, no ISO change): set `packed = 57,344` (28 pages). The tool re-emits a
365,617-byte catalogue (5 bytes smaller than the original because the page re-compresses), it fits the 366,592-byte
reservation, all 18,562 records reparse, and the reader still resolves `option.adc`. So the script may grow by up to
1,991 **stored** bytes (roughly 9 KB decoded at the observed 4.5:1 ratio) without a ToC relocation.

What the writer does **not** do: add, remove or rename records, rebuild index pages, allocate volume space, or write
the ISO. Moving a file to a new data page is supported (tested on a fixture) but the caller must find free pages.

Fixture tests (`tests/python/test_gt4_vol_toc.py`): two leaf pages, one directory, two files, validated with the
reviewed reader `scripts/gt4_volume.py` before and after; a record moved from page 0 to page 4 with new lengths is
re-read at the new offset and the other records are unchanged; directory records, bad magic, truncated pages and
catalogue growth past the reservation are refused.

## 6. Tests and commands

```powershell
$env:GT4_OPTION_ADC="private\slice437-option\orig\asset-00.bin"
private\tooling-venv\Scripts\python.exe -m unittest discover -s tests/python -p test_adhoc_v5.py    # 10 OK
private\tooling-venv\Scripts\python.exe -m unittest discover -s tests/python -p test_gt4_vol_toc.py # 10 OK
private\tooling-venv\Scripts\python.exe -m unittest discover -s tests/python                        # 841 OK, 77 skipped
python scripts\adhoc_v5.py inspect private\slice437-option\orig\asset-00.bin
python scripts\adhoc_v5.py roundtrip private\slice437-option\orig\asset-00.bin
python scripts\gt4_vol_toc.py --iso "Gran Turismo 4 (USA) (v2.00).iso" inspect --file option.adc
python scripts\gt4_vol_toc.py --iso "Gran Turismo 4 (USA) (v2.00).iso" roundtrip
```

The two new modules use only the standard library. The test files import `gt4_volume.py` (which needs `pycdlib`) for
the independent reader, exactly like the existing volume tests.

## 7. Gates, risks and the next step

- **Gate A (owner):** choose the page to retarget. A same-length edit cannot add a page; it trades one. Options:
  `LAN` (the proof target), `Network` (excluded previously), or wait for a v5 assembler.
- **Gate B (in-game):** prove that a page whose root is a new scriptless `RootWindow` renders and appears in the
  Options screen. Needs the TASK-213 test build, a data patch with the two edited files and a headless run; not done
  here.
- **Gate C (growth):** anything beyond +1,991 stored bytes needs a new data extent. The writer can move the record,
  but the volume has no end slack (`GT4.VOL` is exactly 1,200,928 sectors) and free pages are not mapped yet, so a
  larger script would need a free-page scan and probably an ISO rebuild. No ISO copy was made or is needed for
  Phase 1 (0 bytes copied).
- **Spec II is v7** (`ADCH007`, 288,080 bytes): the same scanner works with its own region, but its page table
  offsets, page set and `GroupSpecII` entry are not mapped by this tool yet (**Unknown**).
- **Risk:** the `0x0224`/`0x16` tags and the trailer word are empirical. The tool refuses anything that does not match
  the exact validated shape rather than guessing.
- **Risk:** `LANPopup` is nested inside `OptionRoot`; a page root may have to be a top-level child of `Project`. The
  mproject experiment adds the new root at the end of `Project.children`, which covers both readings.

## 8. License and clean room

Our two modules and their tests are original MIT code written from the file bytes and from our own documents
(`docs/tasks/INGAME-SETTINGS-MAP.md`, `docs/VOLUME-READER.md`, `scripts/gt4_volume.py`). No third-party source was
read for them, no OpenAdhoc (GPL-3.0) code or Spec II script was copied, and the GTAdhocToolchain clone was deleted
unused. Nothing derived from the disc is committed: the tools read a private copy at run time and the fixtures are
synthetic.

---

# Phase 2 - the GTAdhocToolchain private build and a real inserted page (2026-11-10)

Owner decision (2026-11-10 10:52): **use GTAdhocToolchain for private use**, even though its upstream `AGENTS.md`
asks AI tools not to. The owner's rationale: it is a private local build tool, nothing from it is contributed,
committed or distributed, and the MIT license permits the use. This document records the authorization and the
author's request side by side; the shipped path remains our MIT `scripts/adhoc_v5.py` and `scripts/gt4_vol_toc.py`.
No GTAdhocToolchain source, binary or derived file is in the repository.

## 9. The private toolchain build (Confirmed)

- Repo: https://github.com/Nenkai/GTAdhocToolchain, tag **1.4.0** = `0e98a7dcad818647c22662ee2e350ce4800d5def`
  (the current `master` fails to compile at its own `PDTools` pin: `TextureSet3` has no
  `TextureSetPlatformFormatType`).
- Clone + submodules: `esprima-dotnet` `0e09beb`, `PDTools` `3356dfa`; `dotnet build GTAdhocToolchain.CLI -c Release`
  with SDK 10.0.303 -> **0 errors, 220 warnings**, `adhoc.exe` 1.4.0.
- One **local patch** in the private clone only: `GTAdhocToolchain.Core/AdhocCodeFrame.Write` wrote `LocalCount`
  before `MaxStackSize` for v5-v7 while its own reader (and the game) read `MaxStackSize` first. With the order
  swapped back, reading and writing the retail `option.adc` is **byte-identical** (251,715 bytes, SHA-256
  `acd29dd0...484c`).

So the toolchain is a genuine v5 **reassembler** for this file: `AdhocFile.ReadFromFile` -> `TopLevelFrame.Write`
reproduces the publisher image exactly.

## 10. The v5 module format (Confirmed against the retail file and the toolchain)

Header (all little endian):

```
0x00  "ADCH005\0"                  magic + 3-digit version
0x08  u16 len + source path        "_tmp_option.ad"
0x18  u32 function parameter count (0 for the module frame)
0x1C  u32 static slot count         272  (max static index 271 + 1)
0x20  u32 local count               142
0x24  u32 instruction count        3375
0x28  first instruction
```

Instruction: `u32 line_number | u8 opcode | payload`. Opcodes used by a page (all verified byte for byte):

| opcode | name | payload |
|---|---|---|
| 0x08 | EVAL | - |
| 0x06 | CALL | u32 argument count |
| 0x16 | NIL_CONST | - |
| 0x1C | STATIC_DEFINE | symbol |
| 0x1D | STRING_CONST | symbol |
| 0x24 | VARIABLE_PUSH | u32 path count (2) + name symbol + namespace symbol + u32 storage index |
| 0x1B | SET_STATE_OLD | u8 state (0 EXIT, 1 RETURN) |

Symbol = `u16 length + ASCII`. The module initializer ends with `u32 line, 0x1B, 0x00` (EXIT) and nothing after it.
A page is eight instructions:

```
VARIABLE_PUSH OptionPage.OptionPage Static:97     (the OptionPage class)
EVAL
STRING_CONST "<page label>"
VARIABLE_PUSH <Root>.<Root> Static:<slot>         (the layout root object)
EVAL
NIL_CONST
CALL 3
EVAL
```

The retail page names/roots in section 2 are exactly the `STRING_CONST` + first `VARIABLE_PUSH` of these blocks;
the "class id" column there was the **source line number** and the "trailer" is the **static slot**. The four
`Dummy` entries are pages with no root argument (`STRING_CONST "Dummy"` + zero-argument construction).

What insertion changes: the new block bytes, the header **instruction count** (+9) and the header **static slot
count** (one new slot). No string table, no offsets, no relocation: operands use symbol names and static slots, and
jump targets are instruction indices. Inserting **at the end of the module initializer** moves no existing index;
inserting in the middle moves every later root instruction index (see section 12).

## 11. The inserted page (proven at the file and loader level)

`scripts/adhoc_v5.py add-page` builds the nine instructions (STATIC_DEFINE + the eight above), inserts them before
the final EXIT and bumps both counts. Its output is **byte-identical** to the GTAdhocToolchain writer's output for
the same page (checked: `option.shipped.adc == option.newpage.adc`, 251,842 bytes each).

```
page label "Recomp", root "RecRoot1", static slot 272, line 204
decoded 251,715 -> 251,842 (+127)
statics 272 -> 273, instructions 3375 -> 3384
PS2ZIP 54,443 bytes <= 55,353 stored extent (910 spare)
GTAdhocToolchain disassembles the result and shows the block at root instructions 3374..3382, EXIT at 3383
```

`option.mproject`: a new top-level `RootWindow {"RecRoot1"}` with `has_script 0`, a background, a `Pane` with a
"Recomp settings" title and five label rows (Graphics, Controls, Display, Audio, System) minifies to 463,429 bytes
and packs to 14,312 against the 22,449 stored extent (8,137 spare).

## 12. Headless result (v2.00, `gt4vulkan.exe` TASK-213 build, private copy)

The patch container carries **three same-length ranges** (the ToC itself, `option.adc`, `option.mproject`):
`[data-patch] 3 range(s), 443424 payload bytes, verified against the image`. The ToC range is required because the
new script's decoded size (251,842) and stored size (54,443) differ from the record; the writer re-emits a
365,618-byte catalogue, padded to the original 365,622-byte range.

- **End insertion (our build):** the game boots, opens **Options**, renders the full page list (General, Arcade
  Mode, Photo Mode, Controllers, Miscellaneous) and runs 2,600/3,200 frames to the frame limit, **exit 0**. The
  reassembled script with the extra page, static and counts is accepted by the loader. **No tile for the new page
  appears** in the Options grid: placed after every group, the page is not attached to a group.
- **Mid-list insertion (after Race in `GroupGeneral`, and after Language in `GroupMisc`):** the game reaches
  Options and then stops at frame **1743** with `[ee] halted: missing code at 0x00000000 (ra 0x00319ECC)`. The same
  address in both cases: the Options tile builder walks the group page list, reaches the new page, and dereferences
  a null entry (a page with no tile definition). That is the **exact gate** for a visible page.

Evidence: `private/slice438-toolchain-run/` (`run-end.log` 2,600 frames exit 0, `run-end-right.log` 3,200 frames,
`run-after-race-crash.log` and the same crash after Language, `frames/`, `frames3/`, `state.bin`,
`gt4-data.patch` sha256 `089ffc89...f88a1` for the after-Language build).

## 13. Gate and next step

The page **entry, module code, static, PS2ZIP and ToC** are solved. The remaining gate is the **Options root tile**:
the script builds one tile per page inside each group (`0x00319ECC` is the failing call), and a page without a
matching layout tile/icon entry crashes it. Next step: read that builder (`0x00319ECC` and its caller), find what it
reads per page (label key, icon image, geometry) and whether an extra hidden `Item` in `option.mproject` plus one
more page in the group is enough; otherwise the group's tile list is sized by the script and needs one more
constructed tile, which is a further v5 code insertion at the end of the group (same instruction-level technique).
The fallback stays the accepted Arcade host-tree hub, which removes nothing.

## 14. Risks

- The inserted static slot (272) is beyond the retail maximum (271) but inside the header count; the loader accepted
  it, the layout binding to `RecRoot1` is **unproven** (the page never rendered far enough to test it).
- The three-range data patch replaces the ToC; a real release would need the release-time builder to emit it, and the
  launcher path only handles same-length files today.
- The private toolchain is not part of the shipped code and must not become a build dependency; the shipped tools
  reproduce the same bytes.
- Mid-list insertion is unsafe without instruction-index relocation; the shipped `add-page` only inserts at the end.
---

# Phase 3 - the tile gate: what the Options builder does with a 25th page (2026-11-10)

Owner constraint restated: no existing page is retargeted, swapped or removed. LAN, 2 Player Battle/SplitBattle,
Network and every other page stay. This phase only reads the tile path and probes it.

## 15. The crash site (Confirmed by disassembly)

`ra 0x00319ECC` is inside the function at `0x00319E68` (EE, reconstructed ELF). The relevant code:

```
00319E90  lw   $v1, 0x2c($s2)      ; v1 = container->list (an hArray-like: data +0x14, count +0x20)
00319E94  lw   $v0, 0x20($v1)      ; count
00319E98  lw   $s0, 0x14($v1)      ; data
00319E9C  addiu $v0, $v0, -1       ; count - 1
00319EA0  sll   $a1, $v0, 2
00319EA4  sw    $v0, 0x20($v1)     ; store the decremented count (pop)
00319EA8  jal   0x00323D08
00319EAC  addu  $s0, $s0, $a1
00319EB0  lw    $a1, ($s0)         ; a1 = element
00319EB4  lw    $v1, 4($a1)        ; v1 = element->class
00319EB8  addiu $v1, $v1, 0x50     ; the class method table at +0x50
00319EBC  lh    $a0, ($v1)         ; 16-bit method offset
00319EC0  lw    $v0, 4($v1)        ; function pointer
00319EC4  jalr  $v0                ; <-- v0 = 0: "missing code at 0x00000000"
00319EC8  addu  $a0, $a1, $a0
```

So the engine **pops an element from a list and dispatches a virtual method on it**; the element's class pointer
(or its method slot) is invalid, so the call lands at 0. It is not a tile-geometry read: the failing object is a
list element.

## 16. Probes (all headless, same 3-range patch, same navigation)

| probe | change | result |
|---|---|---|
| page at the end of the module initializer | `OptionPage("Recomp", RecRoot1, nil)` + `STATIC_DEFINE`, EXIT last | boots, Options renders, 2,600/3,200 frames, exit 0, **no tile** |
| page after `Race` (GroupGeneral) | new page + static | crash at frame **1743**, `ra 0x00319ECC` |
| page after `Language` (GroupMisc) | new page + static | crash at frame **1743**, same `ra` |
| page after `Language`, existing root | label `Recomp`, root `MiscRoot`, no new static | crash, same `ra` |
| page after `Language`, duplicate label | label `Misc`, root `MiscRoot` | crash, same `ra` |
| **byte-identical copy of the `Language` block** after itself | 7 instructions copied verbatim | crash at frame **1747**, same `ra` |
| 8 `NOP`s after `Language` (same instruction-count shift, no page) | control | **no crash**, 3,000 frames, exit 0 |

The NOP control rules out instruction-index relocation: shifting the later root instructions by 8 is harmless. The
verbatim copy of a retail block still crashes, so the block encoding, the label, the root and the static slot are
**not** the cause. Any additional page placed inside a group breaks the same engine structure.

## 17. The gate

The Options page list/group structures are sized for the **24 compiled pages**: the 25th page inside a group is
pushed into a list whose teardown dispatch (`0x00319ECC`) receives an element without a valid class. A page appended
after every group is accepted at load but belongs to no group, so it gets no tile.

The smallest next experiment (in order):

1. **Find the size.** The `Group` construction is `OptionPage(97) EVAL STRING_CONST "Group" STRING_CONST
   "GroupGeneral" NIL NIL CALL 4` - the two trailing `NIL`s are unqualified and one may be a capacity/count. Probe:
   replace the last `NIL` of `GroupMisc` with `INT_CONST 4` (or 1) and add the page inside that group; if the crash
   disappears the argument is the capacity and the tile count is script-declared.
2. **Or find the engine constant.** `0x00319E68`'s caller and the list owner (`[s2+0x2C]`) identify where 24 comes
   from; `state.bin` from a crashed run has the list count.
3. **Layout path:** if the count comes from `option.mproject`, the ScrollWindow/group `children [N]` is the place to
   add one slot (text edit, 8,137 bytes of slack available).

Until one of those is proven, the visible page stays blocked; the accepted Arcade host-tree hub remains the
fallback and nothing existing is touched.
---

# Phase 4 - the group capacity experiments (2026-11-10)

Owner constraint unchanged: nothing existing is retargeted, swapped or removed.

## 18. What `OptionPage` actually is (Confirmed from the disassembly)

`CLASS_DEFINE: OptionPage extends Object` at `0x7E9D`, four attributes (`title`, `root`, `func`, `path`) and one
method:

```
METHOD_DEFINE __init__(tt, rt, fc, pt)
  title = tt
  root  = rt
  func  = fc
  path  = pt != nil ? pt : tt        # the icon path defaults to the title
```

No validation, no nil return: the constructor cannot fail. Non-first groups call `OptionPage.Group("<Group>", nil)`
(3 arguments); the first group also passes the `options` list (4 arguments). Pages are
`OptionPage(label, root, nil[, "JP/<icon>"])` - the optional 4th argument is the icon path; a 3-argument page uses its
title. The tile builder we found earlier composes `image/option/` + `path` + `.png`.

## 19. Experiments

| probe (page after `Language` unless stated) | result |
|---|---|
| trailing group argument `NIL` -> `INT_CONST 4` (`GroupMisc`) | crash at 1743, same `ra` |
| trailing group argument `NIL` -> `INT_CONST 6` (`GroupGeneral`, page after `Race`) | crash at 1743, same `ra` |
| `MODULE_DEFINE OptionProject::RecRoot1` + page in group | crash at 1743 |
| 4th argument path `"JP/Misc"` + module + static | crash at 1743 |
| `MODULE_DEFINE` + page **at the end** (no group) | **no crash**, 3,000 frames, exit 0, no tile |
| new group `GroupRecomp` + page at the end | **no crash**, 3,000 frames, exit 0, **no new section** |

So: the trailing `NIL` of the group call is not a capacity; the module, the icon path, the label, the root and the
static are not the cause. Any page constructed **inside a group** crashes the load-time pass; a page (or a new group)
at the end is accepted but ignored by the Options root.

## 20. The gate, sharpened

The page is accepted by the loader (module, static, counts and PS2ZIP all fine) but the Options root does not take
runtime-added pages or groups into its tile list, and a page inserted in an existing group makes the load-time pass
dispatch on an element without a class (`0x00319E68` -> `0x00319ECC`). The two concrete next targets:

1. The `Group` static (`OptionPage.Group`) and the list it owns - `[object+0x2C]` in the crashing frame, an
   `hArray`-like with data `+0x14` and count `+0x20`. Its allocation policy (fixed vs growing) decides whether a
   runtime page can ever join.
2. The Options root's tile builder (the function that composes `image/option/<path>.png` and clones the hidden
   `Item`), i.e. how it enumerates pages. If it enumerates a **compiled list** rather than the runtime list, a visible
   page needs one more constructed tile in that enumeration, not another page in the group.

Neither is solved; the fallback remains the accepted Arcade host-tree hub and nothing existing changes.
---

# Phase 5 - the tile enumeration and the capacity field (2026-11-10)

## 21. Enumeration (Confirmed)

The Options root tiles come from `FUNCTION_DEFINE - onInitialize(context)` at `0xA047` (OptionRoot's module). It
walks **`options`**, a runtime array (`options[i].title`, `AppendItem(context, options[i], i, x, y)`), skips `Dummy`,
`JGR_port0/1` outside the US and `Language` when only one language is available, and clones `hidden::Item` per entry
(`image/option/<path>.png`).

`options` is created at the end of the page list as `ARRAY_CONST_OLD 33` (offset `0x9A0C` in the retail file, line
203). 33 = the five groups plus the 28 page entries (24 pages + 4 `Dummy`).

## 22. The capacity was the gate (Confirmed)

With the page inserted in `GroupMisc`, the 34th element overflowed the 33-slot array; the load-time pass then
dispatched a virtual on a classless element (`0x00319ECC`). Raising the constant from 33 to 34 at `0x9A0C` removes the
crash: the page loads, and the tile appears.

```
ARRAY_CONST_OLD payload at 0x9A0C: 33 -> 34
```

## 23. The tile (Confirmed, headless)

Final build: page title `Recomp settings`, root `RecRoot1`, static slot 272, icon path `JP/Misc`, inserted after the
`Language` page in `GroupMisc`, with `MODULE_DEFINE OptionProject::RecRoot1`, capacity 34.

- decoded 251,935 bytes (+220), instructions 3375 -> 3388, statics 272 -> 273, PS2ZIP **54,469 <= 55,353**;
- Options renders **Miscellaneous: Misc, Units, Recomp settings** with the cursor on it and the footer
  `Recomp settings` (`private/slice438-toolchain-run/frames4/frame_03000.png`); every existing tile is unchanged;
- the run reaches the frame limit (exit 0) with no crash.

`scripts/adhoc_v5.py add-page` now reproduces this exact file byte for byte (same 251,935 bytes) with
`--after-page Language --module --path JP/Misc --capacity-offset 0x9A0C --capacity 34`.

## 24. Opening the page is the next gate (Confirmed)

Cross on the tile at frame 3100 stops at `missing code at 0x00000000 (ra 0x0030B450)` - the same virtual-dispatch
pattern (`lw v1,4(s1); addiu 0x50; lh a0; lw v0,4(v1); jalr v0`). The page module is empty, while every retail page
module defines at least:

| function | body (retail) |
|---|---|
| `open(context)` | `start_page(context, <Root>)` |
| `close(context)` | `start_page(context, OptionRoot)` |
| `reflect(context)` | fills the page widgets from the option object |
| `apply(context)` | writes the option object back |

`OptionProject::RaceRoot` shows the exact shape (line 2079+: `VARIABLE_PUSH start_page Static:2 ... CALL 2`). The next
insertion is those functions in `OptionProject::RecRoot1` (same instruction technique, ~10 instructions each) plus the
static wiring for `start_page` and the root object. Until then the tile exists but the page cannot open.
---

# Phase 6 - the page lifecycle functions (2026-11-10)

## 25. What was inserted

Following `OptionProject::RaceRoot` (line 2079+), the private harness built `OptionProject::RecRoot1` with:

| function | body |
|---|---|
| `open(context)` | `start_page(context, RecRoot1)` |
| `close(context)` | `start_page(context, OptionRoot)` |
| `reflect(context)` | `NOP`, `RETURN` |
| `apply(context)` | `NOP`, `RETURN` |

each as a `FUNCTION_DEFINE` frame (source `_tmp_option.ad`, one `context` parameter, `LocalCount 3`,
`MaxStackSize 4`, `start_page` at static slot 2, the target root at slot 3), exactly the retail shape. The build:
`252,456` decoded bytes, instructions 3392, statics 273, PS2ZIP **54,513 <= 55,353** (840 spare).

## 26. Result and the exact gate

Cross on the tile still stops at `missing code at 0x00000000 (ra 0x0030B450)`; the same address with and without
`has_script 1` on the new `RootWindow`, and with the empty module. So the crash is not the module lookup.

`AppendItem` (in `OptionRoot.onInitialize`) copies four statics into each tile item:

```
item.defineStatic("page",   option.root)
item.defineStatic("func",   option.func)
item.defineStatic("number", number)
item.defineStatic("x", x)
```

`OptionPage.__init__` stores `func` as the **third constructor argument**, and every retail page passes `NIL` there.
The tile's Cross handler dispatches `item.func`, so the engine must fill `func` from somewhere else - either the
`OptionPage` factory resolves `OptionProject::<Root>.open` at construction time or the engine binds it when the root
window has a script.

Next experiments, smallest first:

1. Pass a function object as the third argument instead of `NIL`. In v5 a function reference is a `VARIABLE_PUSH` of
   a module member; emit `VARIABLE_PUSH OptionProject::RecRoot1::open` (path) and call `OptionPage(label, root, open)`.
2. Find the native/`OptionPage` factory that fills `func` when it is nil: search the disassembly for the call that
   turns `item.func` into a callable (the tile's Cross handler) and read how a nil func is resolved.
3. If (2) shows a *compiled* table of page functions (one per page), the 25th entry is the same kind of insertion as
   the capacity field.

Nothing existing changed: the tile stays, LAN / 2 Player Battle / SplitBattle / Network and every other page are
untouched, and the page does not open yet.
---

# Phase 7 - `func` is not the gate; the root binding is (2026-11-10)

## 27. What was proven

| probe (tile `Recomp settings`, group Misc, capacity 34, Cross at 3100) | result |
|---|---|
| root `RecRoot1` + module with `open/close/reflect/apply` | crash at 3101, `missing code at 0x00000000 (ra 0x0030B450)` |
| root `RecRoot1` + **empty** `open/close` | same crash - the failure is before `open` runs |
| root `RecRoot1` + `has_script 1`, window first or last in `Project.children` | same crash |
| **root `MiscRoot`** (existing, bound) + module `RecRoot1` unused | **page opens**: Options -> Cross -> *Miscellaneous Settings*, run continues to 3,600 frames, exit 0 (`frames4/frame_03400.png`) |

So `item.func` is not the problem: retail pages also pass `NIL` as the third `OptionPage` argument, and the tile
opens a page whose root is a bound layout object. The failing piece is the **root object**: with `RecRoot1` the
page's root static is nil, so the Cross path dispatches on nothing.

## 28. The gate

The layout loader binds a `RootWindow` to the page module's root static **by name** (every retail root, e.g.
`MiscRoot`, resolves; the hand-added `RecRoot1` does not, even with `has_script 1`, the module
`OptionProject::RecRoot1` present, and the window first or last in `Project.children`). The loader keeps its own
name -> object table from the compiled project, so a runtime-added window is not reachable through the data path.

Next experiments, smallest first:

1. Find the loader that instantiates the project roots and the module per `has_script` (search for the function that
   reads the `.mproject` `name string` and builds `OptionProject::<name>`), and check whether it walks the text
   children or a compiled name table.
2. If it walks the text, the window is created but the *static lookup* fails: find how `VARIABLE_PUSH <Root> Static:N`
   resolves a name (module table by name) and add the 25th entry there.
3. If it is a compiled table, the smallest insertion is one more name in it - the same technique as the capacity field.

Until then the page has a tile and a module, but no bound root: the Arcade host-tree hub remains the working
fallback and nothing existing changed.
---

# Phase 8 - the root-binding natives (2026-11-10)

## 29. What was found

The engine exposes two natives on the project object, next to the `RootWindow` string in `.rodata`:

| native | address |
|---|---|
| `MProject.findPage` | `0x00229608` |
| `MProject.exportRootWindow` | `0x002296E8` |

The same block holds `getDir`, `%s::%s` (the module path format used for `OptionProject::<name>`) and `.mproject`.
Neither native has a direct `jal` - both are registered script natives and are reached through the object model, so
the loader that walks the `.mproject` roots is not identified yet.

## 30. The binding gate

A page's root argument is a static that the retail project resolves **by name** to the layout window; a runtime-added
`RootWindow` (`RecRoot1`, static 272) stays nil even with `has_script 1`, the module `OptionProject::RecRoot1` present
and the window at either end of `Project.children`. `option.root` is then nil and Cross dispatches to 0
(`0x0030B450`). With an existing root (`MiscRoot`) the whole path works (Phase 7).

Next experiments:

1. Find the registration of `MProject.exportRootWindow`/`findPage` (`mProject__InitClass`) and the callers reached
   through the vtable - that is the loader that binds a window to a script value.
2. If the loader iterates a compiled name list, add `RecRoot1` at its end (same technique as the capacity field).
3. If it walks the text children, the failure is the name lookup; the fix is the missing table entry, not the window.

Until then the page has a tile and a module but no bound root; the Arcade host-tree hub remains the fallback.
---

# Phase 9 - where the project binds roots (2026-11-10)

## 31. The project is a widget tree; the reader is native

- `mProject` is an `mComposite` subclass (`mProject__structor_1`: `mComposite__structor_1` + a `RefCounter`),
  i.e. the project itself is the root of the widget tree.
- `mProject__virtual_63` walks the tree: `func_00206868(self)` gives the first child, `func_0025C300(child)` the
  next, and each child receives a vtable call (`vt[95]`). `mProject__virtual_64` does the same walk for the
  destructor path.
- `mProject__virtual_61` constructs `MWidgetReader` - the `.mproject` parser that owns the `RootWindow`/`has_script`
  keywords (the `RootWindow` string at file offset `0x599436`, `has_script` at `0x59AD00`, `.mproject` at `0x598B51`).

The binding natives live in the same `.rodata` block as those keywords: `MProject.findPage` (`0x00229608`),
`MProject.exportRootWindow` (`0x002296E8`), `getDir` and the module path format `%s::%s`. They are registered methods
(no direct `jal`): the reader exports each scriptable root through them.

## 32. The gate, precisely

A runtime-added `RootWindow` is parsed and instantiated (the tile exists) but **not exported** to the page's static:
`option.root` is nil, so Cross dispatches to 0. Nothing in the data path can add the export: it happens inside the
native reader when it walks the *compiled* project text it loaded at start.

Smallest next steps, in order:

1. Read `MWidgetReader`'s `RootWindow` branch (the function that references the `RootWindow` string) and see what it
   does with `has_script`; that is the exact export site.
2. If the export is a name -> value table inside the project, add `RecRoot1` there (data) or call
   `exportRootWindow` once for the new window (one native hook at project load).
3. Native patch shape: at `mProject` load completion, call `findPage("RecRoot1")`/`exportRootWindow` for the new
   window, or extend the reader's name check. Both are a single call in the existing walk, not a new subsystem.
---

# Phase 10 - the reader search (2026-11-10)

## 33. What the decomp has, and what it does not

| symbol | address | meaning |
|---|---|---|
| `MWidgetReader__structor_0` | `0x00207590` | reader construction |
| `MWidgetReader__structor_1` | `0x00218C00` | parse **one** widget (`func_00269290(reader, name)`) and register it (`func_00306638`) |
| `func_00269290` | `0x00269290` | the token/keyword parser (matched under `src/ui/unit_00265D00`) |
| `func_00306638` | `0x00306638` | builds a member name and inserts it (`func_003065F0`), the generic member-add helper |
| `MProject.findPage` / `exportRootWindow` | `0x00229608` / `0x002296E8` | registered project methods |

The `RootWindow`/`has_script` branch inside `func_00269290` is **not matched** in the decomp. There is also no
pointer to the `RootWindow` string anywhere in the image (searched the whole file for `0x00698436`): the reader
builds the keyword and compares it through `func_0057F260`/`memcmp`, so it cannot be found by a simple data search.

## 34. Route decision

- **Data route: exhausted.** The name -> value export table is not part of the title data we can edit; the reader
  builds it at load from the project it parses. Adding a `RootWindow` to the text is parsed and instantiated (the tile
  proves it) but not exported.
- **Native route (smallest):** one call to `MProject.exportRootWindow("RecRoot1")` after the project load, inside the
  existing walk (`mProject__virtual_63` already iterates every child and dispatches `vt[95]`). It changes nothing when
  the window is absent and does not touch any existing page.

## 35. Exact next step

Disassemble `func_00269290` (`0x00269290`) and follow the `RootWindow`/`has_script` comparisons to the point where the
reader calls `vt[95]`/`exportRootWindow`. Two outcomes:

1. it stores into a **name -> value table** owned by the project -> insert `RecRoot1` there (data, same technique as
   the capacity field);
2. it calls the export method directly -> add the single hook at that call site for the new window.

Until one of them is done, the page has a tile and a module but no bound root; the accepted Arcade host-tree hub
remains the fallback and nothing existing changes.
---

# Phase 11 - the parser dispatch (2026-11-10)

## 36. `func_00269290` and `func_0024DB78`

`func_00269290` (`0x00269290`, 30 lines, matched) is a generic line reader: it fills a 0x810-byte buffer, reads up to
0x81 bytes, then calls `func_0024DB78(self, reader)` - the **widget type dispatch** - followed by
`func_002688E8(*self->p, src)` and `func_00265D10(*self->p, reader.n)`. It does not compare `RootWindow` itself.

The keyword names are **not addressable** in the image: `RootWindow` (`0x698436`), `has_script` (`0x69AD00`) and
`.mproject` (`0x698B51`) have no `lui`/`addiu` or pointer reference anywhere (three independent searches). They are
interned at parse time and looked up through the VM's symbol table, so the dispatch in `func_0024DB78` works on
symbol ids, not string literals.

## 37. Route decision (final for this round)

- **Data: exhausted.** The export happens after the reader has interned the type and built the widget; a runtime
  `RootWindow` is instantiated (the tile proves it) but never exported to the page static. Nothing in the title data
  we can edit carries the name -> value table.
- **Native: not a one-line hook.** Calling `MProject.exportRootWindow("RecRoot1")` from the host means invoking guest
  code with a live script VM context (`MProject` object, `hString` argument, handle conversion) - a guest-call harness,
  not a hook. The smallest *data-side* alternative would be a compiled name table, which we have not found and
  cannot add without the reader's interned symbol ids.

## 38. Exact next step

Disassemble `func_0024DB78` (`0x0024DB78`) and the symbol interning (`func_003166B8`), and look for the moment the
reader binds a `has_script` widget to `OptionProject::<name>`: if it calls `func_00306638`/`exportRootWindow` with the
interned symbol, a single `exportRootWindow` call for `RecRoot1` (or a table entry) is the fix; if it walks a compiled
list, that list is the insertion point.

Until then: tile present, page root nil, accepted fallback unchanged, nothing existing modified.
---

# Phase 12 - the dispatch chain is string handling, not the binder (2026-11-10)

## 39. `func_0024DB78` disassembled

`func_0024DB78` runs `0x24DB78..0x24DD34` (a second helper follows to `0x24DE78`, a third to `0x24DF88`). Its calls
are string/reader helpers only: `func_005C2560` (string clone), `func_0020EEA0`, `func_005A2F88`, `func_0020FC58`,
`func_005C11A8` (allocator name), `func_00326798` (free), plus `jalr` through table slots - no call to
`func_00229608`/`func_002296E8` (`findPage`/`exportRootWindow`) and no symbol-interning call (`func_003166B8`).

So the chain `func_00269290 -> func_0024DB78 -> func_002688E8 -> func_00265D10` is text/string handling. The widget
type dispatch and the `has_script` -> `OptionProject::<name>` export happen deeper, in code the decomp has not matched
(the reader's parse-one-widget path is only partially recovered).

## 40. Final route decision for TASK-214

- **Data: closed.** A `RootWindow` added to `option.mproject` is parsed and instantiated (the tile proves it) but is
  never exported to the page's root static, and no name -> value table is reachable from the title data.
- **Native: needs a guest-call harness.** `MProject.exportRootWindow("RecRoot1")` must run with a live script VM
  context (the `MProject` object, an `hString`, handle conversion). That is a new native facility, not a one-line
  hook; no kit was built because no direct export call site was found to hook.
- **Everything else is proven:** page entry, module, static slot, PS2ZIP (54,469 / 55,353), ToC record, the tile in
  Options > Miscellaneous, and the full page-open path with an existing root (Misc page, 3,600 frames, exit 0).

## 41. Smallest next step

Find the reader's type-dispatch table (the function that maps an interned type symbol to a widget class and handles the
`has_script` property). Candidates to disassemble next: `func_002688E8`, `func_00265D10`, `func_0020FC58`,
`func_005A2F88` and the project load entry (`mProject__virtual_61` -> `MWidgetReader__structor_0` `0x00207590`). The
first one that calls `func_003166B8` (intern) and then reaches `findPage`/`exportRootWindow` is the binding site; from
there either a table entry (data) or the single export call (native) is the fix.

Until then the page has a tile, a module and no bound root; the Arcade host-tree hub remains the accepted fallback and
nothing existing was modified.
---

# Phase 13 - the parse chain is text-only; the binder is deeper (2026-11-10)

## 42. The five functions, read

| function | what it does | interning / export? |
|---|---|---|
| `func_002688E8` (`0x002688E8`) | copies a string handle into the widget (`+0x94`), refcount handling | no |
| `func_00265D10` (`0x00265D10`) | sets bit `0x1000` in `widget+0x98` from `(reader line count != 0)` | no |
| `func_0020FC58` (`0x0020FC58`) | skips a `{ ... }` block by brace depth | no |
| `func_005A2F88` | string/allocator helper (not decompiled) | no |
| `MWidgetReader__structor_0` (`0x00207590`) | string setup calls (`func_005C2560`, `func_0057F260`, `func_005C2630`, `func_005C11A8`) | no |

None calls `func_003166B8` (intern), `func_00229608` (`findPage`) or `func_002296E8` (`exportRootWindow`). The chain
`func_00269290 -> func_0024DB78 -> func_002688E8 -> func_00265D10` is the **text** path of one widget; the widget-type
dispatch and the `has_script` -> `OptionProject::<name>` export live in code the decomp has not recovered.

## 43. Route and harness scope (no subsystem started)

- **Data: closed.** A `RootWindow` added to the project text is parsed and instantiated (tile proven) but never
  exported to the page's root static.
- **Native: needs a guest-call harness.** A usable path would have to: (a) obtain the live `MProject` object after the
  project load, (b) build an `hString` for `"RecRoot1"` through the VM's string constructor, (c) call
  `MProject.exportRootWindow` (registered method, `0x002296E8`) with the correct `this`/argv and release the handle.
  That is a new native facility (guest call with a live script context), not a hook; per the owner's rule it was
  **not started**.
- **Everything else is proven:** page entry, module, static slot, PS2ZIP 54,469 / 55,353, ToC record, the tile in
  Options > Miscellaneous, and the complete page-open path with an existing root (Miscellaneous Settings, 3,600
  frames, exit 0).

## 44. Exact chain for the next RE round

`mProject__virtual_61` (`0x0022...`, constructs `MWidgetReader`) -> `MWidgetReader__structor_0` (`0x00207590`) ->
`func_00269290` (`0x00269290`) -> `func_0024DB78` (`0x0024DB78`) -> `func_002688E8`/`func_00265D10`.
The type dispatch is the branch inside `func_0024DB78` that reads the widget type line and selects a class; the
`has_script` handling is in the same reader but further in, and it is what calls the export method (or fills a name
table). Disassembling `func_0024DB78` in full (it is a small function followed by two helpers) and the widget-class
factory it reaches is the next concrete step.
---

# Phase 14 - the guest-call route tested in data first (2026-11-10, owner option A)

## 45. The natives that could replace a harness

`MRenderContext.getCursorProject` (`0x0022B438`) returns the live project, and `MProject.findPage`
(`0x00229608`) resolves a page/root **by name** (decomp: `func_00314920` turns the string into a symbol, `func_00229B48`
looks it up, the result is wrapped in a handle and written to the return register). So a script-side
`context.cursor_project.findPage("RecRoot1")` is the same call a harness would make.

## 46. Tested, and why it does not help

`open(context)` was rebuilt as:

```
start_page(context, context.cursor_project.findPage("RecRoot1"))
```

The run still stops at `missing code at 0x00000000 (ra 0x0030B450)`, at Cross, **before** any `open` code runs (proved in
Phase 6 with an empty `open`). The failing dispatch is the tile's own Cross handler on `item.page`, which `AppendItem`
copied from `option.root` (nil) at construction time.

So binding inside `open` is too late: the root must be valid when the page object is built.

## 47. The real insertion point

`AppendItem(context, option, number, x, y)` runs with a live `context` and sets
`item.defineStatic("page", option.root)` and `item.defineStatic("func", option.func)`. That is where a resolved
`context.cursor_project.findPage(option.title)` could replace a nil root - a data insertion inside AppendItem (its own
frame, no relocation of the caller), or the native equivalent: fill `item.page`/the page static at that point.

No native kit was built: a harness at `open` cannot run before the tile dispatch, and the only useful hook is the
construction site above. Nothing existing changed; the tile remains.
---

# Phase 15 - target moved to a new Options block (2026-11-10, owner 13:51)

## 48. Where the blocks come from

The Options screen blocks (General, Arcade Mode, Photo Mode, Controllers, Miscellaneous) are **not** layout nodes: the
`.mproject` has no `General`/`Miscellaneous` strings and no block containers in the Options root. The blocks are drawn
by the script from the page list, which is the `options` array created at the end of OptionRoot's module init as
`ARRAY_CONST_OLD 33` (`0x9A0C`) = **5 groups + 28 page entries** (24 pages + 4 dummies). The group construction is
`OptionPage.Group("<name>", nil)`; the block title comes from that name (rtext), not from the layout.

## 49. Phase 4's `GroupRecomp` revisited

Two probes:

| probe | capacity | result |
|---|---|---|
| new group `GroupRecomp` + page, at the end | 33 (retail) | runs, **no block appears** |
| new group + page, at the end | 35 (33 + group + page) | **crash at load**, frame 1727, `ra 0x003199D4` |

The crash is the same class of failure as Phase 4: the load pass pops elements from `[object+0x2C]` (data `+0x14`,
count `+0x20`) and dispatches virtual slots `+0x50` / `+0x90`; an element without the expected class calls null.

## 50. Gate

The block list is the runtime `options` array, and a group entry must be a **valid group object** with the block
metadata the loader expects - not just a 3-argument `OptionPage.Group(name, nil)` appended at the end. The retail
groups are created inside the compiled page-list sequence, between the previous block's pages and the next block's
pages, and the array capacity matches that exact count.

Smallest next steps:

1. Read the load pass `0x00319980..0x00319A24`: it pops from two lists (`[s0+0x2C]` and the same object's other list)
   and dispatches `+0x90`; that tells what a group entry must be.
2. Read the `Group` static of the `OptionPage` class (the callee of the `OptionPage.Group(...)` call) to see what it
   stores and returns; a group might need a title key/icon argument instead of the trailing `NIL`.
3. Insert the group **inside** the compiled sequence (before the final `EXIT` of the page list but after the last page
   of Misc), with the capacity raised to the exact new count, and check the block then renders.

No kit was built: the block does not render yet and the data path is still open only through steps 1-3. The Misc
fallback (TASK-213) covers the user-facing settings meanwhile; nothing existing was touched.
---

# Phase 16 - the group entry and the block template (2026-11-10)

## 51. A group entry is an OptionPage with the title "Group"

The five block calls are `OptionPage(...)` constructions with the literal title `"Group"`:

```
VARIABLE_PUSH OptionPage(97) / EVAL / STRING_CONST "Group" / STRING_CONST "<GroupName>" / NIL / CALL 3 / EVAL
```

(the first block also passes the `options` list first, `CALL 4`). The block branch of `onInitialize` (line 249) is
`if (options[i].title == "Group")`: it clones `hidden::Group` (`doCopy`), appends it to `ScrollWindow::clip::space`
and sets `label.text = context.translate(...)`. The template **`hidden::Group` exists** in the Options root layout
(the fourth child of `hidden`, line 295 of the decoded mproject), so no layout change is needed for the block chrome.

## 52. The capacity/array question

`options` is created at the end of the page list as `ARRAY_CONST_OLD 33` (`0x9A0C`) = 5 groups + 28 page entries
(24 pages + 4 dummies). Two probes with a new group + page appended at the end:

- capacity 33: runs, no block (the two extra entries exceed the built list);
- capacity 35: crash at load, frame 1727, `ra 0x003199D4` (the pop/teardown dispatch).

So the list is **sized and filled as a unit**: changing only the `ARRAY_CONST_OLD` count makes the builder iterate
slots that are not valid group/page objects. The entries must be created inside the sequence the constructor appends
to, not after it.

## 53. Gate and next step

A block needs: a group `OptionPage("Group", "<Name>", nil)` created **inside** the compiled sequence (so the `options`
list holds it between the previous block's pages and the next block's), the existing `hidden::Group` template, and the
`ARRAY_CONST_OLD` count matching the real number of entries (groups + pages).

The smallest next step is to find what fills `options`: the first block call passes the list explicitly
(`CALL 4`), later ones do not (`CALL 3`), so `OptionPage.Group` keeps a module-level cursor. Reading the callee of
that call (the class-object method) and the `ARRAY_CONST_OLD` site together is the remaining piece; once the fill is
understood, inserting one group + one page before the `EXIT` and raising the count by exactly 2 is the change.

Nothing existing was touched; the Misc host-tree fallback (TASK-213) covers the user-facing settings meanwhile.
---

# Phase 17 - the Recomp settings block renders at the bottom of Options (2026-11-10)

## 54. The missing rule: insert **before** the options array is built

The `options` array is created by `ARRAY_CONST_OLD 33` at `0x9A0C`, i.e. **after** the last page (`Language`,
`0x99BC..0x9A06`). Inserting the group and page at the very end of the module (after the array) is what failed in
Phase 15/16: at capacity 33 the entries are ignored, at 35 the load pass iterates slots that are not valid objects
(`0x003199D4`).

Inserting the same two constructions **right after the `Language` page, before `0x9A0B`**, with
`ARRAY_CONST_OLD` raised to **35** (5 groups + 28 page entries + 1 group + 1 page), is accepted:

```
newgroupin: OptionPage(97) EVAL "Group" "Recomp settings" NIL CALL 3 EVAL   <- block entry
            MODULE_DEFINE OptionProject::RecRoot1 NOP EXIT                  <- page module
            STATIC_DEFINE RecRoot1, OptionPage(...) "Recomp settings" RecRoot1 NIL CALL 3 EVAL
capacity:   0x9A0C 33 -> 35
```

- decoded 252,016 bytes, instructions 3394, statics 273, PS2ZIP **54,466 <= 55,353**;
- run reaches Options and scrolls to the end: the **"Recomp settings" block header and its tile render at the bottom,
  below Miscellaneous**, footer `Recomp settings`, cursor on the tile
  (`private/slice438-toolchain-run/frames4/frame_02800.png` and `frame_03000.png`); every existing block and tile is
  unchanged;
- the block label is the group name passed to `OptionPage("Group", <name>, nil)` - using the literal
  `"Recomp settings"` renders exactly that text (no rtext entry needed).

## 55. What remains

Cross on the tile still stops at `missing code at 0x00000000 (ra 0x0030B450)`: the page's `root` static (`RecRoot1`,
slot 272) is nil, the Phase 14 gate. The block and the tile are now in place; the page opens as soon as the root is
bound (proved with `MiscRoot` in Phase 7).

Smallest next step unchanged: resolve `option.root` at construction (`AppendItem(context, option, ...)`, which has a
live `context`, via `context.cursor_project.findPage(option.title)`), either as a data insertion inside that function
(with jump relocation) or as the native equivalent. No kit was built: the block renders, the page does not open yet.
---

# Phase 18 - the AppendItem root guard (2026-11-10)

## 56. Implemented

A guard was inserted at the start of `AppendItem` (its own frame, 29 instructions) and every jump target in that frame
relocated by +29:

```
if (option.root == nil)
    option.root = context.getCursorProject().findPage(option.title)
item.defineStatic("page", option.root)
```

`context.cursor_project` (attribute form) crashes at `0x00307A54` - `getCursorProject` is a **method** in the registered
list (`MRenderContext getCursorProject`), so the call is `context.getCursorProject()`.

## 57. Result

With the method form the run gets further but stops at the load pass again: `missing code at 0x00000000 (ra
0x003199D4)` at frame 1743 (the classless-element pop), i.e. the guard changes `AppendItem`'s behaviour in a way the
page list build does not accept. Two candidate causes, both untested:

1. **Jump relocation incomplete.** Only `InsJump`, `InsJumpIfFalse`, `InsJumpIfTrue` and `InsLogicalBase` were shifted;
   `InsJumpIfNil` and the logical-optional forms were not, so a branch inside `AppendItem` can land on the wrong
   instruction.
2. **`findPage` returns nil for the new title** and assigning nil back changes nothing, while the extra call itself
   runs before the project's cursor is set at load.

## 58. State

The block renders (Phase 17). The page still does not open; no kit was built. The smallest next step is to complete
the relocation (cover `InsJumpIfNil`/logical-optional) and only run the guard when the project is available (for
example guard on `context.getCursorProject() != nil`), then re-test.