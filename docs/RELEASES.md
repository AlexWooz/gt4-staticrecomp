# Releases and the launcher's update rules

Status: launcher TASK-019 / TASK-043, 2026-10-20. Code: `launcher/crates/gt4l-core/src/{version,releases,update,versions,gs_release}.rs`.

## Two release streams

| Stream | What it is | Repository | Tag | Assets |
|---|---|---|---|---|
| **GT4Recomp** | launcher, game executable and installer | `moesuito/gt4-staticrecomp` | `vX.Y.Z` | `GT4Recomp-Setup-X.Y.Z.exe`, `SHA256SUMS` |
| **gt4-gs** | the PCSX2 GS renderer (`gt4-gs.exe`) | `moesuito/gt4recomp-gs` | `gt4gs-vX.Y.Z` | `gt4-gs-X.Y.Z-windows-x64.zip`, `gt4recomp-gs-X.Y.Z-source.zip`, `SHA256SUMS` |

- Both streams use SemVer `MAJOR.MINOR.PATCH`. Comparison is numeric: `1.10.0` is newer than `1.9.0`.
- Current versions: GT4Recomp **1.1.0** (1.0.0 before), gt4-gs **1.1.0** (`gh release view gt4gs-v1.1.0 --repo moesuito/gt4recomp-gs --json assets` lists the real asset names above).
- The launcher's minimum for gt4-gs is **1.1.0** since GT4Recomp 1.1.0 (it was 1.0.1): the ultrawide fill needs the
  protocol 1.3 consumer. For GT4Recomp it is **1.0.0** (`releases.rs`).
- A tag that does not match the stream's format is ignored. A renderer tag in the launcher repository (or the reverse) is never offered.

## The rule: stable only

- **Drafts** and **prereleases** are never offered, installed or checked. The launcher reads `draft` and `prerelease` from the GitHub release list.
- The newest stable release at or above the minimum is the candidate. Nothing else is.
- A release marked prerelease on GitHub stays out of the launcher even when its version is higher.

## Release notes

- The release **body** is the release notes. The update popup shows it under "What's new".
- The launcher shows the body as text. Supported: `#` headings, `-` or `*` bullets, `**bold**`. Any HTML in the body is shown as plain text.
- Notes are cut at 6000 characters.

## Checksums

- Every release has a `SHA256SUMS` asset: one line per file, `<sha256>  <file name>`.
- The launcher downloads the package and checks its SHA-256 against its line before anything is unpacked or started.
- For gt4-gs, `gt4-gs.exe` inside the zip must also match its own line.
- A missing line, a mismatch or a missing file is refused and the installed version stays as it was.

## How the launcher uses the streams

- **On every launch** (background, timeout 6 s, never blocks Play): the launcher reads both release lists. A failure is logged only.
- **gt4-gs not installed**: the launcher installs the newest stable one (setup does it too). No prompt. If the download fails, the launcher uses CoreGS 1.1 and says so.
- **gt4-gs below the minimum** (for example 1.0.1 left by GT4Recomp 1.0.0): the launcher installs the newest stable one at
  launch, the same path as a missing renderer (`gs_release::needs_install`), and records it in `version.json`. If that
  fails (offline), the old version stays in use (the game sends protocol 1.3 messages only to a 1.3 consumer, so there
  is no ultrawide fill) and the next launch tries again.
- **An update exists** (newer stable, not skipped): the popup shows the current and new version and the notes. Buttons: **Update**, **Not now**, **Skip this version** (remembered in `settings.toml` under `[updates]`, for exactly that version).
- **Updates are never automatic.** The launcher installs only when the player presses Update.
- **gt4-gs update**: only while the game is not running. The new version goes into its own folder (`%LOCALAPPDATA%\GT4Recomp\gs\<version>\`), `current.txt` switches to it, the previous folder is kept for rollback, and `version.json` records the version.
- **GT4Recomp update**: the setup program is downloaded to a temporary folder, its SHA-256 is checked, it is started, and the launcher exits. The installer upgrades the program files in place. It never touches the data folder.
- **Check for updates** (Settings > About) runs the same check. With nothing new it says "You're up to date."

## version.json

- Location: next to `GT4Recomp.exe` (the install folder). If that folder is not writable, the file is kept in the data folder and the launcher logs it.
- Written by the installer and updated by the launcher. Written atomically (temporary file, then rename).

```json
{
  "schema": 1,
  "gt4recomp": "1.1.0",
  "gt4gs": "1.1.0",
  "settings_schema": 1
}
```

- `gt4recomp` is the GT4Recomp version. The launcher's own Cargo version is authoritative when the file disagrees (a warning goes to the log).
- `gt4gs` is `null` until the renderer is installed.
- The installer writes `gt4gs: null` on every install and upgrade. At each start the launcher rewrites a file that
  disagrees with what is installed: `gt4recomp` becomes the Cargo version and `gt4gs` the checked gt4-gs install
  (`versions::reconciled`; a damaged gt4-gs folder leaves the recorded value).
- `settings_schema` is the schema of `settings.toml` that this launcher writes.
- A damaged or newer `version.json` is not rewritten; a damaged one is kept as `version.json.bad` and replaced with a fresh one.

## Publishing a release (checklist)

1. Bump the version in `launcher/Cargo.toml` (workspace), `launcher/crates/gt4l-tauri/tauri.conf.json` and the installer define in the release command.
2. **Game executable, release build** (TASK-081): a fresh `private/sliceNNN-*` build with `-DGT4_RELEASE_DISTRIBUTION=ON`
   (no build-time shader folder in the executable; slice214 is the 1.0.0 recipe, `private/slice214-scripts/build-game-214.bat`;
   slice218 is the 1.1.0 game, the same recipe plus the TASK-080 work, `docs/inputs/slice218-pcsx2.json`).
   The GS libraries are built with PGO and ThinLTO (IN-473): `scripts/coregs2_bench/pgo-build.sh <build> <out>` as the last
   step, on the final code. CTest, the Python suite with `GT4_NATIVE_BUILD` on it, the headless frame comparison, a new `docs/inputs/sliceNNN-pcsx2.json`
   pin, then the distribution folder: `python scripts\make_distribution.py private\sliceNNN-dist --assets-root <main checkout>
   --exe <build>\gt4vulkan.exe --runtime-manifest docs\inputs\sliceNNN-pcsx2.json`. Scan the executable's ASCII and UTF-16
   strings: no absolute path and no user name (`tests/python/test_portable_install.py` does this for release builds).
3. **Launcher**: `powershell -ExecutionPolicy Bypass -File launcher\tools\build-release.ps1` (sets `--remap-path-prefix`
   for the Cargo, rustup and checkout folders, runs `cargo tauri build --no-bundle`, restores any `Cargo.toml` the Tauri
   CLI rewrote, and fails if a user or build path is left in `GT4Recomp.exe`). Then `cargo test --workspace` and the UI
   pages `dev/nav-test.html` and `dev/focus-check.html` in headless Edge.
4. **WebView2 bootstrapper**: download `MicrosoftEdgeWebview2Setup.exe` from `https://go.microsoft.com/fwlink/p/?LinkId=2124703`
   into a fresh `private/release-NNN-*` folder, check `Get-AuthenticodeSignature` (Valid, CN=Microsoft Corporation) and
   record its SHA-256. It is never committed.
5. **Installer**: `makensis /DVERSION=X.Y.Z /DLAUNCHER_EXE=... /DGAME_DIR=<distribution folder> /DSDL3_DLL=...
   /DWEBVIEW2_BOOTSTRAPPER=... /DOUTFILE=<out>\GT4Recomp-Setup-X.Y.Z.exe installer\gt4recomp.nsi`. The default output name is
   already `GT4Recomp-Setup-X.Y.Z.exe`, the name the updater looks for. List the contents with `7z l` (it reads NSIS files).
6. Hash the setup program and write `SHA256SUMS` (`<sha256>  GT4Recomp-Setup-X.Y.Z.exe`).
7. Create the release `vX.Y.Z` as a **draft** (`gh release create vX.Y.Z --draft --target <commit> --title ... --notes-file
   docs/release-notes/X.Y.Z.md <setup> SHA256SUMS`). A draft has no tag yet and the launcher never offers it.
8. The owner installs from the draft's installer and tests end to end (fresh install, setup, Play, saves, controls,
   renderer fallback, uninstall), then approves. The branch is merged, and the owner publishes the draft as a stable
   release (not prerelease); publishing creates the tag on the target commit.

The release notes live in `docs/release-notes/X.Y.Z.md` and are the release body. Nothing in this document publishes a
release. The owner publishes releases.

## Requirement checks (TASK-081)

The launcher checks the PC before setup and before Play (`gt4l_core::system_check`, rules tested without hardware;
`gt4l_platform::system` reads the hardware). A failed check shows a message and starts nothing.

- **Processor**: SSE4.1 with SSE3 and SSSE3 (CPUID leaf 1; owner decision 2026-10-21). Message: "Your processor is
  not supported: GT4Recomp needs a CPU with SSE4.1 (Intel Core i3/i5/i7 / AMD FX or newer)." `gt4vulkan.exe` is built for baseline x86-64 (`GT4_X86_64_V3` is off; its only
  AVX code is CPU-dispatched Visual C++ library code) and the PCSX2 GS needs SSE4.1 (PCSX2's published minimum; the
  repository documents no other gt4-gs minimum), so the game has no CPU check of its own.
- **Vulkan**: `vulkan-1.dll` from System32, a bare Vulkan 1.0 instance, at least one GPU driver (not the
  "Microsoft Direct3D12 (...)" translation layer, not a CPU rasterizer) reporting Vulkan 1.2 or newer, and the two
  instance extensions the game requires (`VK_KHR_surface`, `VK_KHR_win32_surface`). Since slice214 the game uses
  swapchain maintenance 1 (present fences; KHR on 2025+ drivers, EXT on 2023+ drivers) only when the driver has it, and
  otherwise the plain `VK_KHR_swapchain` path; `GT4_VULKAN_FORCE_BASELINE=1` forces the plain path and the startup log
  says which path runs (`[vulkan] presentation path: ...`). The PCSX2 GS renders with Vulkan as well, so the check
  applies to both renderers. The
  probe runs once in the background at start; a success is kept for the session, a failure is checked again at the next
  Play. Messages name the driver update (NVIDIA, AMD or Intel).
- **WebView2**: checked by the installer, not the launcher (the launcher cannot open its window without it).

## What the tests cover

- SemVer parsing and numeric comparison, and refusal of pre-release tags (`version.rs`).
- Release lists of both repositories from fixtures with the real asset names, drafts and prereleases (`tests/release_lists.rs`).
- The update decision: newer stable, same version, prerelease, skipped version, first install, minimum (`update.rs`).
- `version.json` round trip, the installer form, the unwritable-folder fallback and the Cargo version rule (`versions.rs`).
- The data-path guard: installs and updates never write to saves, cards, library, replays or the runtime card folder, nor to card or save file types (`guard.rs`).
- The gt4-gs install against a fake folder source: verified install, corrupt download refused, prereleases ignored, upgrade keeps one previous version, an installed 1.0.1 below the 1.1.0 minimum replaced by 1.1.0 (and kept when the update fails) (`tests/gs_release.rs`).
- `version.json` brought in line after an upgrade (`versions.rs`).
- Migrations: the chain, backups, newer and corrupt files (`migrate.rs`, `tests/config_migration.rs`).
- The tray state machine (`gt4l-tauri/src/tray.rs`).

No test touches the network. The GitHub source is replaced by `FolderSource` (fixtures), and the live check is the owner's test.
