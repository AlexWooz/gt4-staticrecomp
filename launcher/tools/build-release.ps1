# Builds the release GT4Recomp.exe (TASK-081) with no user or build-machine path inside.
#
# Rust embeds source paths in panic locations. Paths of the workspace are already relative (crates\...), but the
# crates from the registry are absolute (%USERPROFILE%\.cargo\registry\src\...). `--remap-path-prefix` rewrites them;
# Cargo's own trim-paths option is still unstable (Cargo 1.99). The prefixes depend on the machine, so they are set
# here at build time and not in a committed .cargo/config.toml.
#
#   powershell -ExecutionPolicy Bypass -File launcher\tools\build-release.ps1 [-Sdl3Root <folder>] [-Portable]
#
# -Portable (TASK-098) builds the portable launcher: the Cargo feature `portable` (user data in <exe dir>\user, large data in
# <exe dir>\data, no registry or system folders, update by zip swap). It builds into "<target dir>-portable", so the installer
# build's executable is never overwritten; the two builds are different files from one source. The zip is made by
# scripts\make_portable_zip.py.
#
# Steps: set CARGO_ENCODED_RUSTFLAGS (one flag per entry, so paths with spaces survive), run
# `cargo tauri build --no-bundle`, restore any Cargo.toml the Tauri CLI rewrote, then scan the executable (ASCII and
# UTF-16) for the user profile, the Cargo and rustup folders and the checkout path. Any hit fails the build.
# CARGO_ENCODED_RUSTFLAGS replaces RUSTFLAGS and target rustflags; the workspace sets none today.

param(
    [string]$Sdl3Root = "C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18",
    [switch]$Portable,
    [string]$Label = ""
)
$ErrorActionPreference = "Stop"
if ($Label -and $Label -notmatch '^rc[1-9][0-9]*$') { throw "-Label must be empty or rc<N> (for example rc5), got '$Label'" }
$savedLabel = $env:GT4_RELEASE_LABEL
$env:GT4_RELEASE_LABEL = $Label

$launcher = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$checkout = (Resolve-Path (Join-Path $launcher "..")).Path
$cargoHome = if ($env:CARGO_HOME) { $env:CARGO_HOME } else { Join-Path $env:USERPROFILE ".cargo" }
$rustupHome = if ($env:RUSTUP_HOME) { $env:RUSTUP_HOME } else { Join-Path $env:USERPROFILE ".rustup" }
# CARGO_TARGET_DIR may put the build on another drive (C: short of space, TASK-091); its path is remapped and scanned
# like the others.
$targetDir = if ($env:CARGO_TARGET_DIR) { $env:CARGO_TARGET_DIR } else { Join-Path $launcher "target" }
$savedTargetDir = $env:CARGO_TARGET_DIR
if ($Portable) {
    $targetDir = "$targetDir-portable"
    $env:CARGO_TARGET_DIR = $targetDir
}

# Broad prefix first: when several prefixes match a path, rustc uses the last one, so the user-profile entry only
# catches what the narrower Cargo and rustup entries do not.
$remaps = @(
    @($env:USERPROFILE, "/home"),
    @($checkout, "/gt4recomp"),
    @($targetDir, "/target"),
    @($rustupHome, "/rustup"),
    @($cargoHome, "/cargo")
)
$flags = foreach ($pair in $remaps) { "--remap-path-prefix=$($pair[0])=$($pair[1])" }
$env:CARGO_ENCODED_RUSTFLAGS = $flags -join [char]0x1f
$env:GT4_SDL3_ROOT = $Sdl3Root

# The Tauri CLI may rewrite Cargo.toml files (feature lists). Keep the exact bytes and put them back afterwards.
$manifests = Get-ChildItem -Path $launcher -Recurse -Filter Cargo.toml | Where-Object { $_.FullName -notmatch "\\target\\" }
$saved = @{}
foreach ($file in $manifests) { $saved[$file.FullName] = [System.IO.File]::ReadAllBytes($file.FullName) }

Push-Location (Join-Path $launcher "crates\gt4l-tauri")
try {
    if ($Portable) {
        cargo tauri build --no-bundle --features portable
    } else {
        cargo tauri build --no-bundle
    }
    if ($LASTEXITCODE -ne 0) { throw "cargo tauri build failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
    $env:CARGO_TARGET_DIR = $savedTargetDir
    $env:GT4_RELEASE_LABEL = $savedLabel
    foreach ($path in $saved.Keys) {
        $now = [System.IO.File]::ReadAllBytes($path)
        if ([Convert]::ToBase64String($now) -ne [Convert]::ToBase64String($saved[$path])) {
            [System.IO.File]::WriteAllBytes($path, $saved[$path])
            Write-Host "restored $path (rewritten by the Tauri CLI)"
        }
    }
}

$exe = Join-Path $targetDir "release\GT4Recomp.exe"
$bytes = [System.IO.File]::ReadAllBytes($exe)
$ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
$utf16 = [System.Text.Encoding]::Unicode.GetString($bytes)
$utf16Odd = [System.Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1)
$needles = @($env:USERPROFILE, $cargoHome, $rustupHome, $checkout, $targetDir, $env:USERNAME, "\Users\", "Antigravity") |
    Where-Object { $_ -and $_.Length -ge 4 } | Select-Object -Unique
$hits = 0
foreach ($needle in $needles) {
    foreach ($view in @($ascii, $utf16, $utf16Odd)) {
        if ($view.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            Write-Host "build path left in the executable: $needle"
            $hits++
            break
        }
    }
}
if ($hits -ne 0) { throw "$hits build path(s) left in $exe" }
$hash = (Get-FileHash -Algorithm SHA256 $exe).Hash.ToLower()
$kind = if ($Portable) { "portable" } else { "installer" }
$labelText = if ($Label) { ", label $Label" } else { "" }
Write-Host "GT4Recomp.exe ($kind build$labelText): $((Get-Item $exe).Length) bytes, sha256 $hash; no user or build path found"
