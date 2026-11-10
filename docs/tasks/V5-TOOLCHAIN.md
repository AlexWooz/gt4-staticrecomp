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
