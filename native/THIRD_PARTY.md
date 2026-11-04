# Third-party components and license boundaries

Status: release 1.0.0 (TASK-044, 2026-10-17). This file lists what the game executable and the installed folder
contain, what the separate renderer program contains, what is used only to build or research, and what is kept out.
Revisions are exact where the repository records them. Entries marked **not verified** were not checked against the
component's own license text in this repository and need a check before the release.

## 1. Shipped with the game (executable and installed folder)

| Component | Revision or source | License | Where it is | Notes |
|---|---|---|---|---|
| GT4Recomp runtime and tools (this repository) | `main` | MIT ([LICENSE](../LICENSE)) | `gt4vulkan.exe`, `licenses\LICENSE-GT4Recomp.txt` | Own code: `native/`, `tools/` (except the donor-derived files below), `scripts/`. |
| ps2-web-recomp (donor runtime) | commit `1b4808793a12c45125262fd2d5f50a9ec9f7c020` | MIT, Copyright (c) 2026 Samir Nuri (donor `LICENSE`) | `gt4vulkan.exe`, `licenses\LICENSE-ps2-web-recomp.txt` | EE-to-C++ runtime, PS2 service and device models, IOP interpreter, CoreGS 1.1 (WGSL compute renderer). Derived files: the private port made by `scripts/prepare_external_vulkan.py`, the capsules from `scripts/prepare_memcard_flush.py` and `scripts/prepare_license_guard.py`, and `tools/vu_recomp/src/vurecomp.cpp` (its header lists the changes; the MIT text is `tools/vu_recomp/MIT-LICENSE.txt`). |
| Donor WGSL shaders, compiled to SPIR-V | donor shaders at the revision above | MIT (donor) | `shaders\*.spv` (nine files) | Compiled at build time by `tools/wgsl_spirv` (naga, see the next row). |
| naga 30.0.0 (in `tools/wgsl_spirv`) | `tools/wgsl_spirv/Cargo.lock` | MIT OR Apache-2.0 (as stated by naga) | build time only | Not linked into the executable. Its output, the SPIR-V shaders, is shipped. |
| Microsoft Visual C++ runtime: `vcruntime140.dll`, `msvcp140.dll`, `msvcp140_atomic_wait.dll` | the local Visual Studio CRT redistributable folder (`scripts/make_distribution.py --crt-dir`) | Microsoft redistributable terms. **Not verified**: the terms were not read in this repository | installed folder root | Imported by `gt4vulkan.exe` (checked with `dumpbin /dependents`, slice200). |
| SDL 3.4.18 (`SDL3.dll`, import library `SDL3.lib`, headers) | release-3.4.18 (`https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18`), commit `829a65d769d935c4852f8159e964312c0957260a`; pinned in `docs/inputs/slice210-sdl3.json` | zlib (`licenses\LICENSE-SDL3.txt`) | game executable (gamepad input, TASK-075): `SDL3.dll` and `licenses\LICENSE-SDL3.txt` next to `gt4vulkan.exe` | Gamepads only (`SDL_INIT_GAMEPAD`); the game window stays Win32. `make_distribution.py` ships the DLL and the licence. |
| GT4Recomp GS stream protocol (1.2) | `native/gs_stream/gs_stream_protocol.h` | MIT | game executable (client) and a vendored copy in the renderer package | The same header is in `3rdparty/gt4recomp-gs-stream/` of the fork, checked by hash. |
| rcheevos | `https://github.com/RetroAchievements/rcheevos`, commit `bbc79b58a5a2a0b98dfbf2be6ca9e42c764661ce` | MIT (`native/third_party/rcheevos/LICENSE`) | `gt4vulkan.exe` (`native/third_party/rcheevos/`) | RetroAchievements client, trigger evaluator, and PS2 disc/memory hashing. |
| Translated game code | generated from Gran Turismo 4 (NTSC-U, SCUS-97328) | **not MIT**: the game's code belongs to Sony Interactive Entertainment and Polyphony Digital | `gt4vulkan.exe` and the private game library | Distribution follows decision 0069 (prebuilt executable; the user supplies the disc). No game data is shipped. |

Windows system DLLs (`winmm.dll`, `xinput`, `user32.dll`, `gdi32.dll`, `gdiplus.dll`, `winhttp.dll`, the Vulkan loader `vulkan-1.dll`) are used at run time from
the operating system. They are not shipped.

## 2. Separate renderer program (GPL-3.0-or-later)

| Component | Revision or source | License | Where it is | Notes |
|---|---|---|---|---|
| `gt4-gs.exe` (PCSX2 GS in a separate process) | fork [`moesuito/gt4recomp-gs`](https://github.com/moesuito/gt4recomp-gs), tag `gt4gs-v1.0.1`, commit `e4b3d6c88bf7210664321beb5bddff2b61cd5159` | GPL-3.0-or-later | `<exe folder>\gs\gt4-gs.exe`, downloaded and pinned by the launcher (TASK-043). It is not in the game executable and not in the folder written by `make_distribution.py`. | Based on PCSX2 `v2.8.2` (upstream commit `fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3`), with our patches listed in the release notes. Its own zip carries `SOURCE.txt` and `gt4gs/NOTICE`. |
| DLLs bundled in the gt4-gs zip (SDL3, shaderc, FFmpeg, x264, SVT-AV1, Opus, libvpl, zlib, libpng, libjpeg-turbo, libwebp, FreeType, HarfBuzz and others) | versions in the fork's dependency script | per component (FFmpeg and x264 GPL-2.0-or-later; the others permissive, see `gt4gs/NOTICE`) | inside the gt4-gs zip only | The list and the licenses are in the fork's `gt4gs/NOTICE`. This file does not repeat them. |

The game executable never contains or links GPL code. The two processes exchange only the GS stream over shared memory
(decision 0070; [GS stream protocol](../docs/GS-STREAM-PROTOCOL.md)). Each gt4-gs release has its source attached at its tag.

## 3. Used to build the game, not shipped

| Component | Revision or source | License | Notes |
|---|---|---|---|
| Khronos Vulkan headers | the copy in the `3rdparty/vulkan/include` folder of a pinned PCSX2 checkout (`native/CMakeLists.txt`, `VULKAN_HEADERS`) | Apache-2.0 OR MIT (Khronos) | Compile-time headers only. The copy comes from a PCSX2 checkout: a release build should use an upstream Khronos copy (**open item**). |
| Visual Studio 2022 Build Tools, clang-cl, CMake, Ninja, Rust toolchain | local installs | their own licenses | Toolchains, not distributed. |
| `pycdlib` 1.20.0 | `scripts/requirements.txt` | LGPL-2.1-only (from the installed package metadata) | Offline ISO tools: `scripts/ee_producer.py`, `gt4disc.py`, `gt4_volume.py`, `compare_fsv_trace.py`. Not in the executable or the folder. |

## 4. Research references (not linked, not shipped)

| Source | Revision | License | Use |
|---|---|---|---|
| Nenkai's repositories: PDTools, GT-File-Specifications-Documentation, GTPS2ModelTool, GTSpecDB (MIT roots, as recorded there), gt-modding-hub (no root license found) | see [NENKAI-REFERENCES](../docs/NENKAI-REFERENCES.md) | per repository, as recorded in that document | File-format facts only. No code is copied or linked. |
| GT4FS (`Razer2015/GT4FS`) | see [NENKAI-REFERENCES](../docs/NENKAI-REFERENCES.md) | no root license found in the inspected checkout | RoFS format reference only; no implementation adopted. |
| gt4-decomp (`gustavocanepa/gt4-decomp`) | see [DECOMP-IDENTITY](../docs/DECOMP-IDENTITY.md) | MIT | Names, addresses and layouts used as a reference. Nothing is imported; [DECOMP-INTEGRATION](../docs/DECOMP-INTEGRATION.md) is a study. If its source is ever copied in, it is listed here. |
| PCSX2 source checkout | private, pinned in the research notes | GPL-3.0-or-later | Source of the header copy in section 3 and of research notes ([PCSX2-LESSONS](../docs/PCSX2-LESSONS.md)). No PCSX2 implementation is linked into the game. |

## 5. Dropped or planned

- **paraLLEl-GS** (LGPL-3.0-or-later) and **Granite** (MIT): dropped from the release (decision 0070). Nothing from them ships.
  The MIT renderer seam and capture code kept from that branch are in our own tree (TASK-035).
- **ARMSX2** (GPL-3.0): the mobile GS pipeline planned for Android and iOS. No code is in this repository. Its JIT is not used.
- **CoreGS 2.0** (TASK-026, TASK-027): our own MIT renderer, written clean room from a behavioral specification; no
  PCSX2 implementation may be read by its implementers (decision 0070).

## 6. Open items before the release

- Replace the Khronos header copy from the PCSX2 checkout with an upstream copy (section 3).
- Read the Microsoft redistributable terms that apply to the copied CRT DLLs (section 1).
- Keep the release notes of `gt4gs-v1.0.1` and the fork's `gt4gs/NOTICE` in step with each gt4-gs release.

## 7. CoreGS 2.0 development branch (TASK-027, `GT4_HW_GS=ON` only)

Nothing below is in the release executable: the HWR libraries are built only with `GT4_HW_GS=ON` on
`task/027-coregs2`.

### HWR texture replacement core (TASK-027, M16/M18)

Vendored under `native/hw_gs/replace/third_party/`, unmodified:

- **xxHash 0.8.3** (`xxhash.h`), XXH3-64 with seed 0 for the replacement keys
  (chapters 11.2 and 11.3 of the TASK-026 specification). Upstream
  <https://github.com/Cyan4973/xxHash> tag `v0.8.3`, file SHA-256
  `17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b`.
  Licence: BSD-2-Clause, Copyright (C) 2012-2023 Yann Collet (notice in the header).
- **stb_image v2.30** (`stb_image.h`), PNG decoder only (`STBI_ONLY_PNG`, no stdio).
  Upstream <https://github.com/nothings/stb> master, file SHA-256
  `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`.
  Licence: public domain (header statement "public domain image loader").

Original code under `native/hw_gs/replace/` (MIT) is written from the TASK-026
specification only (decision 0069). The test fixtures under
`native/hw_gs/replace/tests/` (reference GS memory, PNG writer) are original
code for CTest and are not linked into the game.

### HWR shader compiler (TASK-027, M4)

- **glslangValidator 16.2.0** (glslang project, Khronos), taken from the Vulkan SDK 1.4.341.1 at
  `$VULKAN_SDK/Bin/glslangValidator.exe`. File SHA-256
  `db832826d73049193882d5572ad76e14fcb7b1cdfc1a6b892ac25217b850b6bc`, pinned in
  `native/hw_gs/shaders/CMakeLists.txt` (`GT4_HW_GLSLANG_SHA256`). Build tool only: it is not
  linked or shipped, and the SPIR-V it emits from our own GLSL is embedded in the build output.
  The glslang licence (BSD-style) is not vendored here; check the upstream licence before
  redistributing the binary itself. Implementation note IN-201.
- Our own HWR GLSL (`native/hw_gs/shaders/*.glsl`, `.vert`, `.frag`, `.comp`) is MIT, written from
  `docs/hw-renderer-spec` only (decision 0069). No third-party shader source is vendored.
