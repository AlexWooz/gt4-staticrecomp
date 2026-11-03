<div align="center">

# GT4Recomp

**Gran Turismo 4, recompiled to run natively on PC.**<br>
No JIT, no emulator window, full speed.

![60 FPS](https://img.shields.io/badge/60%20FPS-100%25%20game%20speed-2ea44f?style=for-the-badge)
![Windows](https://img.shields.io/badge/Windows%20x64-Vulkan-2ea44f?style=for-the-badge)
![MIT](https://img.shields.io/badge/license-MIT-1f6feb?style=for-the-badge)
![Release](https://img.shields.io/badge/release-1.2.5-2ea44f?style=for-the-badge)
![Ultrawide](https://img.shields.io/badge/ultrawide-any%20aspect%20ratio-2ea44f?style=for-the-badge)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-support%20the%20project-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/moesuito)
[![Discord](https://img.shields.io/badge/Discord-join%20the%20server-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/kHfGk4qJt)

![Code](https://img.shields.io/badge/code-EE%20%C2%B7%20VU0%20%C2%B7%20VU1%20100%25%20AOT%20%C2%B7%20IOP%20libraries%20native-2ea44f)
![GS](https://img.shields.io/badge/GS-GSdx%20%2B%20CoreGS%201.1-1f6feb)

<br>

<img src="docs/images/night-race-hires.webp" alt="Gran Turismo 4 night race at high resolution, 60 FPS" width="100%">

</div>

GT4Recomp is a static recompilation of **Gran Turismo 4 (NTSC-U, SCUS-97328)** for **Windows x86-64**.
The game's own code is translated ahead of time into C++ and compiled to native machine code; the PS2
hardware it talks to is reproduced in software, and graphics run on **Vulkan**. Nothing generates code
while the game runs, which is also what makes future **Android and iOS** builds possible.

## Highlights

- **Locked 60 FPS at 100% game speed** in every scene we test, with a frame every **16.7 ms**.
- **All game code is precompiled:** the main CPU (EE) and every VU1 and VU0 microprogram run as native
  code, with no interpreter on the normal path.
- **Sharp, high-resolution image:** **1080p** out of the box, with **1440p, 4K and 8K** available. The game
  is rendered at that resolution, not just stretched, with borderless fullscreen.
- **Native ultrawide and any aspect ratio (1.1.0):** 21:9, 32:9, 16:10 or a phone shape, automatic from the output
  resolution. The 3D view widens Hor+/Vert+, the HUD stays centred 16:9 and menus are pillarboxed; the internal
  resolution follows automatically, with a Render scale of 50-200%.
- **GSdx renderer** (the PCSX2 GS) in its own process: crisp deinterlaced menus, every GSdx option in the launcher
  and a built-in performance overlay. **CoreGS 2.0**, our own renderer, is in development.
- **Custom texture packs:** drop a pack's files straight into the `data\Textures` folder of your
  GT4Recomp install (Settings > Graphics > **Open textures folder**); **Load custom textures** is on by default. HD textures and mods from the PCSX2
  community work without renaming folders.
- **Crash-safe memory card:** saves survive a crash or a forced close.
- **Release 1.2.1:** a one-click **installer** and a **launcher** that sets the game up from your own disc
  image, with settings, saves, controller remapping and update checks.
- **Any controller through SDL3** (Xbox, DualShock 4, DualSense, Switch Pro...) and a modern keyboard layout.

## Performance

Every tested scene holds **60 FPS at 100% game speed** on the GSdx renderer
(Ryzen 7 5700X + Radeon RX 9060 XT):

| Scene | FPS | Game speed | Frame pacing |
|---|:---:|:---:|:---:|
| Time Trial · Nordschleife · Toyota Supra | **60** | **100%** | 16.7 ms flat |
| Arcade race · Nordschleife · 5 AI cars | **60** | **100%** | 16.7 ms flat |
| Night race · 5 AI cars | **60** | **100%** | 16.7 ms flat |
| Rally · Grand Canyon | **60** | **100%** | 16.7 ms flat |
| Car showroom | **60** | **100%** | 16.7 ms flat |

**How we got here:** the first native build ran at **30 FPS and 51% speed**. Compiling the VU1 microcode
ahead of time, submitting GPU work asynchronously and pacing every frame from a single clock brought it
to a steady **60 FPS at full speed**. Release 1.2.2 batches the work handed to the VU1 thread and
the graphics process, so the night race needs about 2 ms less CPU time per frame.

## Known issues

- **Some license demos and menu backgrounds do not play**; every license test is playable. This matches
  PCSX2 with default settings ([why](docs/LICENSE-DEMO-CIPHER.md)).
- Racing wheels are not supported yet, and long sessions are not fully qualified.
- Menus, movies and photo mode stay 16:9 (pillarboxed) at other aspect ratios; split screen and photo mode in
  ultrawide have had little testing.
- The installer is not code-signed: Windows SmartScreen may warn on the first start ("More info", "Run anyway").
- Most testing was on an AMD GPU; NVIDIA and Intel GPUs have had less testing.

## Roadmap

| Phase | Release | Platform | Renderer | Status |
|---|---|---|---|---|
| **1** | 1.x | Windows PC | GSdx (PCSX2 GS, separate process) | Done (1.0.0; current 1.2.0) |
| **2** | 2.0.0 | Windows PC | **CoreGS 2.0** (our own renderer) as the default · GSdx as the fallback | In progress |
| **3** | after 2.0.0 | Linux (native) | CoreGS 2.0 | Planned |
| **4** | after 2.0.0 | Android | CoreGS 2.0 (Vulkan) | Planned |

Phases 3 and 4 start once CoreGS 2.0 is ready and GT4Recomp 2.0.0 is out.

Every piece of work is a numbered task in [TASKS.md](TASKS.md), tested in game before it is merged.

## How it works

The EE game code is translated ahead of time into C++ and compiled for x86-64. The VU1 microcode, stored
encrypted inside the executable, is recovered and compiled offline (all 2,048 entry points), verified
against a reference interpreter on real game state; the 14 VU0 microprograms are compiled the same way.
IOP system services are native. Frames are paced by one high-resolution clock with rate-controlled audio.
This is a **hybrid recompilation**: PS2 hardware is still modeled in software, so we do not claim zero
emulation or full PS2 fidelity. Details: [Architecture](docs/ARCHITECTURE.md).

## Requirements

- **Windows 10 or 11, 64-bit.**
- **A processor with SSE4.1** (Intel Core i3/i5/i7 / AMD FX or newer).
- **A graphics card with Vulkan 1.2** and current drivers (NVIDIA, AMD or Intel).
- **Your own disc image of Gran Turismo 4 USA v2.00 (SCUS-97328)** as a plain `.iso`.
- **About 6 GB free:** the launcher keeps a copy of your disc image (5.3 GB) next to the program.

The launcher checks the processor and Vulkan before setup and before Play, and says what is missing.

## Install

1. Download **`GT4Recomp-Setup-1.2.5.exe`** from [Releases](https://github.com/moesuito/gt4-staticrecomp/releases)
   and run it. It installs to `C:\Program Files\GT4Recomp` by default (Windows asks for administrator approval), or to
   any folder or drive you choose. If the Microsoft Edge WebView2 Runtime is missing (Windows 10), the installer
   downloads it from Microsoft.
2. Start **Gran Turismo 4** from the Start menu and choose your disc image. The launcher copies and checks it and
   downloads the GSdx renderer, which runs the game (the game needs GSdx and does not start without it).
3. Press **Play**.

Everything large stays together in the install folder, in `data` (the copy of your disc image, GSdx, custom textures
and logs), so the whole folder can be moved or copied to another drive and still works. Your settings, saves and
replays are per Windows user in `%APPDATA%\GT4Recomp`, and the uninstaller keeps them unless you tick the box.
Updates from the launcher install silently after one Windows approval and reopen the launcher. Coming from 1.2.0, the
first start moves your files to these places (copied and checked; the old Documents folder is renamed, never deleted).
This repository does not include game files, disc images, BIOS dumps or save files. Building from source needs your
own game files and a development environment: see the [Build guide](docs/BUILD.md).

### Controls

**Controllers** (Xbox, DualShock 4, DualSense, Switch Pro and others, through SDL3) work out of the box, mapped to the
DualShock 2 layout, and can be remapped in the launcher (Settings > Controls).

| Controller | PS2 | Notes |
|---|---|---|
| A · B · X · Y (Cross · Circle · Square · Triangle on PlayStation pads) | Cross · Circle · Square · Triangle | Face buttons |
| LB · RB | L1 · R1 | |
| **LT · RT** | **L2 · R2** | **Analog triggers with pressure**, for progressive throttle and brake |
| Left / right stick | Left / right analog stick | With dead zones; L3 · R3 on stick click |
| D-pad | D-pad | |
| Start · Back | Start · Select | |

The keyboard has a fixed layout:

| Key | Action (PS2 button) |
|---|---|
| W · Enter | Accelerate, confirm (Cross) |
| S | Brake (Square) |
| Space | Handbrake (Circle) |
| A · D · arrow keys | Steer, navigate (D-pad); E is also D-pad up |
| Backspace · Esc | Back (Triangle) |
| Shift | Nitro (R1) |
| R · F | Gear up · gear down (R2 · L2) |
| Q | Look back (L1) |
| C · Tab | Camera (Select) |
| P | Start / Pause |
| Alt+Enter, F11 | Borderless fullscreen |

## Support and contributing

**Want to help?** Contribute to the development or donate: tokens are expensive, and they are what make this project
possible.

- **Donate:** [ko-fi.com/moesuito](https://ko-fi.com/moesuito). Donations pay for the AI development tools behind
  GT4Recomp, starting with the plan that keeps CoreGS 2.0 and the next updates moving.
- **Contribute:** read [CONTRIBUTING.md](CONTRIBUTING.md) for the rules and the pull request template, then pick a
  task from [TASKS.md](TASKS.md) or an [issue](https://github.com/moesuito/gt4-staticrecomp/issues).
- **Report bugs:** open an issue with the launcher log (Settings > About > Open log folder).
- **Talk with us:** join the [GT4Recomp Discord server](https://discord.gg/kHfGk4qJt).

## Credits

**Contributors on GitHub:** Wise ([gustavocanepa](https://github.com/gustavocanepa)), the author of gt4-decomp, for the
game's function names and a check on GS settings; Julio Vitor Rufino ([JulioHeroZ](https://github.com/JulioHeroZ)), for
racing wheel support, RetroAchievements and the renderer choice kept on updates; Luan Antony
([luanantony](https://github.com/luanantony)), for the racing wheel integration, DualSense trigger curves and many fixes
and studies. Thank you!

| Project | Author | What it gives this project |
|---|---|---|
| gt4-staticrecomp, the original project | [Kaezzey](https://github.com/Kaezzey) (Niket Shah) | Where GT4Recomp started: this repository is a fork of his project, with its first setup and tools. |
| [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp) (MIT) | snuri00 | The runtime foundation: EE-to-C++ translator, PS2 hardware models, the GS compute renderer and the original VU1 compiler. |
| [gt4-decomp](https://github.com/gustavocanepa/gt4-decomp) (MIT) | gustavocanepa | GT4's decompilation: function names, class layouts and engine notes ([identity](docs/DECOMP-IDENTITY.md)). |
| [Nenkai's GT research](https://github.com/Nenkai) | Nenkai | File formats and the VU1 microcode encryption scheme ([details](docs/VU1-MICROCODE-SOURCE.md)). |
| [GT4FS](https://github.com/Razer2015/GT4FS) | Razer2015 | GT4 volume file-system research. |
| [PCSX2](https://github.com/PCSX2/pcsx2) (GPL-3.0) | PCSX2 team | Decades of PS2 knowledge; its GS runs only in the separate `gt4-gs.exe`. |
| [naga](https://github.com/gfx-rs/wgpu/tree/trunk/naga) · [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | gfx-rs · Khronos | Shader compilation at build time · Vulkan API headers. |

Exact revisions and licenses: [THIRD_PARTY.md](native/THIRD_PARTY.md).

## License and legal

- GT4Recomp's own code is [MIT](LICENSE). The game executable does not contain or link GPL code; the
  PCSX2 GS (GPL-3.0-or-later) runs only in the separate `gt4-gs.exe`, built from
  [`moesuito/gt4recomp-gs`](https://github.com/moesuito/gt4recomp-gs) with its source published on each release.
- You bring your own disc. The project does not include Gran Turismo 4, disc images, BIOS files, keys or saves.
- "Gran Turismo" and "PlayStation" are trademarks of Sony Interactive Entertainment. Gran Turismo 4, its
  logos and artwork belong to Sony Interactive Entertainment and Polyphony Digital. GT4Recomp is an
  unofficial fan project, not affiliated with or endorsed by either.

## Documentation

[TASKS](TASKS.md) · [CONTRIBUTING](CONTRIBUTING.md) · [STATUS](docs/STATUS.md) · [BUILD](docs/BUILD.md) · [ARCHITECTURE](docs/ARCHITECTURE.md) ·
[Renderers](docs/GSDX-PLAN.md) · [Release 1.0.0 plan](docs/RELEASE-1.0.0-PLAN.md) ·
[Decisions](docs/decisions/) · [Journal](docs/journal/)
