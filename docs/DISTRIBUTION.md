# Distribution folder (TASK-058, TASK-059)

The installed game runs from a folder that holds no repository path, no build-machine path and no game data.
`scripts/make_distribution.py` assembles that folder from a candidate build. This page is the reference for what the
folder contains, what the executable resolves on its own, and what the owner or the launcher must supply.

## What the folder holds

Built by `scripts/make_distribution.py` (slice200: `private/slice200-dist`, 16 files, about 93 MB):

| Path | Content | Source and check |
|---|---|---|
| `gt4.exe` | the game executable (built as `gt4vulkan.exe`, renamed at distribution in release 1.3.0, TASK-108) | the candidate build, checked against the runtime manifest pin (size and SHA-256) |
| `shaders/*.spv` | the nine compute shaders (`upload`, `copy_read`, `copy_write`, `load_clut`, `display`, `raster`, `resolve`, `snapshot`, `decode`) | the pinned `slice153-pacing-shaders`, each checked against the manifest |
| `vcruntime140.dll`, `msvcp140.dll`, `msvcp140_atomic_wait.dll` | the Visual C++ runtime the executable imports (`dumpbin /dependents`) | copied from the local Visual Studio CRT redistributable folder (`--crt-dir`, found by `vswhere`) |
| `licenses/LICENSE-GT4Recomp.txt` | the repository MIT license | `LICENSE` |
| `licenses/LICENSE-ps2-web-recomp.txt` | the donor MIT license (ps2-web-recomp, pinned revision) | `private/references/ps2-web-recomp/LICENSE` |
| `licenses/THIRD_PARTY.md` | third-party notes of the native runtime | `native/THIRD_PARTY.md` |
| `MANIFEST.json` | every file with size and SHA-256, the executable hash, the runtime manifest name, the excluded items | written by the script |

## What the folder does not hold, by decision

- **The gt4-gs package** (PCSX2 GS in a separate process, GPL-3.0). It is not shipped in this folder. The launcher
  downloads it into `<folder>\gs\` and pins it (TASK-043, decision 0070). Until then `--mode pcsx2` needs
  `GT4_GS_PROCESS` (see below).
- **Game data.** No disc image, no `CORE.GT4`, no reconstructed ELF, no memory card, no caches, no captures. The owner
  supplies the disc; the launcher keeps saves in the user data folder (TASK-025).
- **Development outputs.** No `private/` material, no build directories.

The script refuses an output folder outside the assets checkout's `private/` folder and refuses a non-empty folder.

## How the executable finds its paths

| Need | Resolution (TASK-059) | Notes |
|---|---|---|
| Shaders | `<exe folder>\shaders` when that folder exists, else the build-time folder `GT4_VULKAN_SHADER_DIR` | The build-time fallback is the development behaviour (`native/install_paths.cpp`, `native/native_vulkan.cpp`). A release build (`-DGT4_RELEASE_DISTRIBUTION=ON`, TASK-081, slice213) has no fallback: only `<exe folder>\shaders`, and a missing folder stops the start with "The game's shader folder is missing (...). Reinstall GT4Recomp." The build tree of a release build gets its own `shaders\` copy. |
| GS process (`--mode pcsx2`) | `GT4_GS_PROCESS` when set; else `<exe folder>\gs\gt4-gs.exe` when that file exists; else refused with a message | The launcher sets `GT4_GS_PROCESS` and `GT4_GS_PROCESS_SHA256` from its pin. `gt4-gs.ini` comes from `GT4_GS_CONFIG`. |
| Memory card | `--memcard <file>` only; no default path is built in | The launcher passes the active slot's card. |
| Disc, ELF | command-line arguments | Unchanged. |

### `--portable-check`

```
gt4vulkan.exe --portable-check
```

Prints the executable folder, the shader folder and whether it is installed or the build-time fallback, the nine
shader files (`complete` or `INCOMPLETE`), the GS process in use, and `result: ok` (exit 0) or `result: shaders
missing` (exit 1). It loads nothing and opens no window. The port entry point is `private/slice200-port/main.cpp`.

### Release build (TASK-081, slice213)

`-DGT4_RELEASE_DISTRIBUTION=ON` sets `GT4_VULKAN_SHADER_DIR` to an empty string and copies the nine shaders from the
pinned capsule into `<build>\shaders` (CMake POST_BUILD), so CTest and headless runs from the build tree still work.
`--portable-check` prints `build-time shader fallback: none (release build)`.

Scan of the slice213 `gt4vulkan.exe` (97,188,352 bytes, sha256 `c503ee9c...`), ASCII and UTF-16 runs of 5 or more
characters, for `X:\`, `X:/`, `Antigravity`, `Alano`, `gt4-wt`, `Users\`, `staticrecomp`, `private\slice`, `.cargo` and
`AppData`: **no match**. `tests/python/test_portable_install.py` repeats the scan for every release build.

### Remaining absolute strings in the executable (audit, slice200)

A scan of `gt4vulkan.exe` (ASCII and UTF-16 `X:\` or `X:/` strings, `Antigravity`, `gt4-staticrecomp`, `gt4-wt-`, user
names) finds exactly one build-machine path:

- `C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-shaders`: the build-time shader folder (`GT4_VULKAN_SHADER_DIR`).
  It is kept on purpose. It is the fallback for development builds and is never used when `shaders\` sits next to the
  executable. Removing it would make a development build fail when run outside the installed layout, so it stays until
  the release build is made with an empty fallback (open item, see below).

No `pdb` path, no `__FILE__` path and no user name is present (`Alano` is not in the executable).

## Building and checking a distribution folder

```
python scripts\make_distribution.py private\slice200-dist --assets-root C:\Antigravity\gt4-staticrecomp ^
  --exe private\slice200-build\build\gt4vulkan.exe --runtime-manifest docs\inputs\slice200-pcsx2.json
```

The tests (`tests/python/test_make_distribution.py`, `tests/python/test_portable_install.py`) check the pins, the
folder content, that the output stays under `private/`, that a changed executable is refused, and the four
`--portable-check` cases (installed shaders, missing shader, GS override, build-time fallback).

## Smoke results (slice200, headless, from the folder)

The executable ran with its own folder as the working directory (`private/slice200-dist`), the guest ELF, the disc, a
copy of the seed card and the frames passed by absolute path, so the shaders came from `shaders\` next to it
(`[vulkan] shader folder: ...\private\slice200-dist\shaders`).

- donor (no GS process): 42/42 frames byte-identical to `private/slice157-smoke/headless-157/frames` and to the
  slice199 donor run; 4,200 frames, frame limit reached.
- pcsx2 (`GT4_RENDERER=pcsx2`, `GT4_GS_PROCESS` = the slice197 gt4-gs 0.4.0 package, SHA-256 matches its pin):
  41/41 PNG dumps byte-identical to the slice199 pcsx2 run; Arbitrary Code Guard on.
- The memory card of both runs equals the seed card byte for byte: the menus schedule never saves (see BUILD slice200).

## Owner command

From a checkout with the pinned `private/` files (the worktree after the slice200 copy), the candidate runs as before:

```
python scripts\play_performance.py --assets-root C:\Antigravity\gt4-wt-058-harden --mode pcsx2 --force-480p
```

The disc `Gran Turismo 4 (USA) (v2.00).iso` must be in the checkout root (the probe verifies it on every launch; it is
not copied into the worktree). The installed folder is run with `GT4_GS_PROCESS` set, or with the package at
`gs\gt4-gs.exe` next to the executable, and with `--memcard` pointing at a copy of the card.

## Open items

- Done in TASK-081 (slice213): the release build has an empty build-time shader fallback, so no build path remains.
  Development builds keep the fallback.
- The installer (`installer/gt4recomp.nsi`, TASK-081) packs this folder as `game\`; the launcher downloads the GS
  package (TASK-043). Neither is part of this folder.
- The crash-safe flush on exit is verified by the fixture, the kill test and the build, not by a headless save (no
  headless schedule saves yet, see BUILD slice200).
