<div align="center">

# GT4Recomp

**Gran Turismo 4, recompiled to run natively on PC.**<br>
No JIT, no emulator window, full speed.

![60 FPS](https://img.shields.io/badge/60%20FPS-100%25%20game%20speed-2ea44f?style=for-the-badge)
![Windows](https://img.shields.io/badge/Windows%20x64-Vulkan-2ea44f?style=for-the-badge)
![MIT](https://img.shields.io/badge/license-MIT-1f6feb?style=for-the-badge)
![Release](https://img.shields.io/badge/release-1.2.5-2ea44f?style=for-the-badge)
![Beta](https://img.shields.io/badge/beta-1.3.0--rc14-d29922?style=for-the-badge)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-support%20the%20project-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/moesuito)
[![Discord](https://img.shields.io/badge/Discord-join%20the%20server-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/kHfGk4qJt)

![Code](https://img.shields.io/badge/code-EE%20%C2%B7%20VU0%20%C2%B7%20VU1%20100%25%20AOT%20%C2%B7%20IOP%20libraries%20native-2ea44f)
![GS](https://img.shields.io/badge/GS-CoreGS%202.0%20%2B%20GSdx-1f6feb)

<br>

<img src="docs/images/night-race-hires.webp" alt="Gran Turismo 4 night race at high resolution, 60 FPS" width="100%">

</div>

GT4Recomp is a static recompilation of **Gran Turismo 4 (NTSC-U, SCUS-97328)** for **Windows x86-64**. The game's own
code is translated ahead of time into C++ and compiled to native machine code; the PS2 hardware it talks to is
reproduced in software, and graphics run on **Vulkan**. Nothing generates code while the game runs.

> [!IMPORTANT]
> **You need your own original copy of the game.** GT4Recomp does not include or distribute Gran Turismo 4. We only
> distribute the launcher and the natively recompiled system that runs the game. To play, make a disc image of your own
> **Gran Turismo 4 (USA, v2.00, SCUS-97328)** disc as a plain `.iso`, and for the mod, your own **Spec II 1.10** image.

## Highlights

- **60 FPS at 100% game speed**, with all game code precompiled: the main CPU (EE) and every VU0 and VU1 microprogram
  run as native code, with no interpreter on the normal path.
- **CoreGS 2.0, our own renderer, is the default from 1.3.0:** high internal resolutions, sharpening, custom texture
  packs from the PCSX2 community and a Performance Mode for weaker PCs. **GSdx** (the PCSX2 GS, in its own process) stays
  available as the fallback.
- **Two games:** Gran Turismo 4 v2.00 and **Gran Turismo 4 Spec II 1.10** (1.3.0), including images built on Spec II
  1.10 such as translations and car packs.
- **Native ultrawide and any aspect ratio:** 21:9, 32:9, 16:10 or a phone shape; the 3D view widens, the HUD and the race
  menus keep their 16:9 shape.
- **A launcher that sets everything up from your disc image:** installer or portable, save library, controller
  remapping, update channels (Stable and Beta), English and Brazilian Portuguese.
- **Any controller through SDL3** (Xbox, DualShock 4, DualSense, Switch Pro...), configurable racing wheels, the full
  trigger range for throttle and brake, a 360-degree chase camera and camera zoom in races.
- **Crash-safe memory card:** saves survive a crash or a forced close.

## Where we are

| Channel | Version | Renderer | |
|---|---|---|---|
| **Stable** | 1.2.5 | GSdx | The latest final release |
| **Beta** | 1.3.0-rc14 | **CoreGS 2.0** (default) · GSdx | Release candidate: Spec II, CoreGS 2.0, new launcher |

Choose the channel in the launcher (Settings > General > Channel). Every piece of work is a numbered task in
[TASKS.md](TASKS.md), tested in game before it is released.

## Known issues

- **Busy driving views on laptop processors (CoreGS 2.0):** the player's view with the HUD and the rear-view mirror can
  drop below 60 FPS on mid-range laptops (the first license, some city tracks). The cause is known (palette changes split
  the drawing into too many batches) and the fix is planned for the next release candidate.
- **Photo mode on slower graphics cards (CoreGS 2.0):** frame times can be uneven.
- **Some AMD Radeon RX 5000/6000 drivers closed the game while CoreGS 2.0 prepared its shaders.** 1.3.0-rc14 guards
  against it; GSdx works on those PCs.
- **Some license demos and menu backgrounds do not play**; every license test is playable
  ([why](docs/LICENSE-DEMO-CIPHER.md)).
- **Spec II:** only images with Spec II 1.10's game code are supported; earlier Spec II releases are refused.
- Menus, movies and photo mode stay 16:9 (pillarboxed) at other aspect ratios.
- The installer is not code-signed: Windows SmartScreen may warn on the first start ("More info", "Run anyway").
- Most testing was on an AMD graphics card; NVIDIA and Intel cards have had less testing.

## Roadmap

| Phase | Release | Platform | Renderer | Status |
|---|---|---|---|---|
| **1** | 1.0.0 - 1.2.5 | Windows PC | GSdx (PCSX2 GS, separate process) | Done |
| **2** | 1.3.0 - 2.0.0 | Windows PC | **CoreGS 2.0** (our own) as the default · GSdx as the fallback | In progress (1.3.0 beta) |
| **3** | after 2.0.0 | Linux (native) | CoreGS 2.0 | Planned |
| **4** | after 2.0.0 | Android | CoreGS 2.0 (Vulkan) | Planned |

## How it works

The EE game code is translated ahead of time into C++ and compiled for x86-64. The VU1 microcode, stored encrypted
inside the executable, is recovered and compiled offline and verified against a reference interpreter; the VU0
microprograms are compiled the same way. IOP system services are native, and frames are paced by one high-resolution
clock. This is a **hybrid recompilation**: PS2 hardware is still modelled in software, so we do not claim zero emulation
or full PS2 fidelity. Details: [Architecture](docs/ARCHITECTURE.md).

## Requirements

- **Windows 10 or 11, 64-bit**, and a **processor with SSE4.1** (Intel Core i3/i5/i7, AMD FX or newer).
- **A graphics card with Vulkan 1.2** and current drivers for CoreGS 2.0 (GSdx can also use Direct3D 11 on older cards).
- **Your own disc image** of Gran Turismo 4 USA v2.00 (SCUS-97328), and of Spec II 1.10 for the mod, as a plain `.iso`.
  The launcher reads the image where you keep it; it is not copied.

The launcher checks the processor and the graphics card before setup and before Play, and says what is missing.

## Install

1. Download the installer (**`GT4Recomp-Setup-<version>.exe`**) or the portable zip from
   [Releases](https://github.com/moesuito/gt4-staticrecomp/releases): the latest release for Stable, or the newest
   pre-release for Beta.
2. Start **GT4Recomp** and choose your disc image. The launcher checks it, prepares the game and its shaders.
3. Press **Play**.

Updates install from the launcher. Your settings, saves and replays are kept per Windows user in `%APPDATA%\GT4Recomp`
(the portable build keeps them in its own folder). Building from source needs your own game files and a development
environment: see the [Build guide](docs/BUILD.md).

### Controls

Controllers work out of the box with the DualShock 2 layout (analog triggers with pressure for throttle and brake) and
can be remapped in the launcher (Settings > Controls). The keyboard has a fixed layout:

| Key | Action (PS2 button) |
|---|---|
| W · Enter | Accelerate, confirm (Cross) |
| S | Brake (Square) |
| Space | Handbrake (Circle) |
| A · D · arrow keys | Steer, navigate (D-pad) |
| Backspace · Esc | Back (Triangle) |
| Shift | Nitro (R1) |
| R · F | Gear up · gear down (R2 · L2) |
| Q | Look back (L1) |
| C · Tab | Camera (Select) |
| P | Start / Pause |
| Alt+Enter, F11 | Borderless fullscreen |

## Support and contributing

- **Donate:** [ko-fi.com/moesuito](https://ko-fi.com/moesuito). Donations pay for the AI development tools behind
  GT4Recomp.
- **Contribute:** read [CONTRIBUTING.md](CONTRIBUTING.md), then pick a task from [TASKS.md](TASKS.md) or an
  [issue](https://github.com/moesuito/gt4-staticrecomp/issues).
- **Report bugs:** open an issue with the log bundle (Settings > About > Export logs).
- **Talk with us:** join the [GT4Recomp Discord server](https://discord.gg/kHfGk4qJt).

## Credits

**Contributors on GitHub:** Wise ([gustavocanepa](https://github.com/gustavocanepa)), the author of gt4-decomp, for the
game's function names and a check on GS settings; Julio Vitor Rufino ([JulioHeroZ](https://github.com/JulioHeroZ)), for
racing wheel support, RetroAchievements and the renderer choice kept on updates; Luan Antony
([luanantony](https://github.com/luanantony)), for the racing wheel integration, DualSense trigger curves and many fixes
and studies. Thank you!

| Project | Author | What it gives this project |
|---|---|---|
| gt4-staticrecomp, the original project | [Kaezzey](https://github.com/Kaezzey) (Niket Shah) | Where GT4Recomp started: this repository is a fork of his project. |
| [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp) (MIT) | snuri00 | The runtime foundation: EE-to-C++ translator, PS2 hardware models and the original VU1 compiler. |
| [gt4-decomp](https://github.com/gustavocanepa/gt4-decomp) (MIT) | gustavocanepa | GT4's decompilation: function names, class layouts and engine notes ([identity](docs/DECOMP-IDENTITY.md)). |
| [Nenkai's GT research](https://github.com/Nenkai) | Nenkai | File formats and the VU1 microcode encryption scheme ([details](docs/VU1-MICROCODE-SOURCE.md)). |
| [GT4FS](https://github.com/Razer2015/GT4FS) | Razer2015 | GT4 volume file-system research. |
| [Gran Turismo 4 Spec II](https://www.theadmiester.co.uk/specii/) | TheAdmiester and contributors | The Spec II mod (the second game). GT4Recomp ships none of its files. |
| [PCSX2](https://github.com/PCSX2/pcsx2) (GPL-3.0) | PCSX2 team | Decades of PS2 knowledge; its GS runs only in the separate `gt4-gs.exe`. |
| [naga](https://github.com/gfx-rs/wgpu/tree/trunk/naga) · [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | gfx-rs · Khronos | Shader compilation at build time · Vulkan API headers. |

**Gran Turismo 4 Spec II** by TheAdmiester and contributors, with credits to Nenkai, ddm, ermaccer, zinklesmesh, pez2k,
xfileFIN, Silentwarrior112, Xenn and Submaniac, Silent, Alconautica, hydnhntr, JeyM and pioneerspine; special thanks to
DustinEden and the testers and translators listed on the mod's credits page.

Exact revisions and licenses: [THIRD_PARTY.md](native/THIRD_PARTY.md).

## License and legal

- GT4Recomp's own code is [MIT](LICENSE). The game executable does not contain or link GPL code; the PCSX2 GS
  (GPL-3.0-or-later) runs only in the separate `gt4-gs.exe`, built from
  [`moesuito/gt4recomp-gs`](https://github.com/moesuito/gt4recomp-gs) with its source published on each release.
- **You bring your own game.** The project does not include Gran Turismo 4, Spec II files, disc images, BIOS files, keys
  or saves; we distribute only the launcher and the recompiled runtime.
- "Gran Turismo" and "PlayStation" are trademarks of Sony Interactive Entertainment. Gran Turismo 4, its logos and
  artwork belong to Sony Interactive Entertainment and Polyphony Digital. GT4Recomp is an unofficial fan project, not
  affiliated with or endorsed by either.

## Documentation

[TASKS](TASKS.md) · [CONTRIBUTING](CONTRIBUTING.md) · [STATUS](docs/STATUS.md) · [BUILD](docs/BUILD.md) ·
[ARCHITECTURE](docs/ARCHITECTURE.md) · [Renderers](docs/GS-RENDERER-OPTIONS.md) · [Decisions](docs/decisions/) ·
[Journal](docs/journal/)
