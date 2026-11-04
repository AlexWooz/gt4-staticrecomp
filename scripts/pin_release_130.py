"""Pin the release 1.3.0 game builds (TASK-100/TASK-101): docs/inputs/slice<N>-pcsx2.json or slice<N>-spec2.json.

The manifest starts from the release 1.2.3 pin (slice268-pcsx2.json) and replaces what release 1.3.0 changes: the
executable, the runtime/guard capsules (v2.00: slice262; Spec II: slice276 IOP + slice277 license guard), the
frozen EE library (v2.00: slice217 aspect; Spec II: slice278 spec2) and the gt4-gs package (1.2.1). Everything else
(port, shaders, VU1/VU0 tables, SDL3, card seeds, environment, toolchain) is copied from the base pin and verified by
the Python tests against the files on disk, so a copy can only be correct if the files really match.

The output file must not exist: a new slice always gets a new pin file (docs/inputs are history, never edited).
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import external_runtime_probe as probe  # noqa: E402

BASE = "docs/inputs/slice268-pcsx2.json"
GS_121 = {
    "directory": "private/slice281-gt4gs-121",
    "zip_name": "gt4-gs-1.2.1-windows-x64.zip",
    "zip_size": 13025171,
    "zip_sha256": "7eb7e4a3bb42f7b742fe3ac26bf81790e6fa01b0d82c6fcef26eabdc415ad71c",
    "release": "https://github.com/moesuito/gt4recomp-gs/releases/tag/gt4gs-v1.2.1",
    "source": "the published stable release gt4gs-v1.2.1 (fork moesuito/gt4recomp-gs), extracted and checked against "
              "its SHA256SUMS: the pinned package is the published one",
    "no_jit": "GT4GS_ONLY build: no recompilers, no GS software JIT, no executable memory; the no-JIT audit at the end "
              "of the stream and at exit reports 0 executable non-image regions",
}


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def pin(slice_number, target, executable, date="2026-10-23"):
    assets = Path(r"C:\Antigravity\gt4-staticrecomp")
    exe = Path(executable)
    if not exe.is_file():
        raise ValueError(f"executable not found: {exe}")
    base = json.loads((ROOT / BASE).read_text(encoding="utf-8"))
    manifest = json.loads(json.dumps(base))
    runtime_rel = exe.relative_to(assets).as_posix() if exe.is_relative_to(assets) else exe.as_posix()
    package_dir = assets / GS_121["directory"]
    if not package_dir.is_dir():
        raise ValueError(f"gt4-gs package not found: {package_dir}")
    tree_sha, tree_files = probe.package_tree_digest(package_dir)
    gs_exe = package_dir / "gt4-gs.exe"
    gs_entry = {"path": f'{GS_121["directory"]}/gt4-gs.exe', "size_bytes": gs_exe.stat().st_size,
                "sha256": sha256(gs_exe)}
    dependencies = [dict(item) for item in base["runtime_dependencies"] if not item["path"].endswith("gt4-gs.exe")]
    dependencies.append(gs_entry)

    manifest.update({
        "slice": slice_number,
        "date": date,
        "baseline_commit": "slice268 (release 1.2.3, main db5c559)",
        "branch": "task/101-multi-game (release 1.3.0 integration)",
        "measurement_mode": "pcsx2",
        "runtime": {"path": runtime_rel, "size_bytes": exe.stat().st_size, "sha256": sha256(exe)},
        "runtime_dependencies": dependencies,
        "gs_stream": dict(base["gs_stream"], gs_process=f'{GS_121["directory"]}/gt4-gs.exe', gs_package={
            "directory": GS_121["directory"],
            "tree_sha256": tree_sha,
            "files": tree_files,
            "data_root": True,
            "zip": {"name": GS_121["zip_name"], "size_bytes": GS_121["zip_size"], "sha256": GS_121["zip_sha256"]},
            "release": GS_121["release"],
            "source": GS_121["source"],
            "no_jit": GS_121["no_jit"],
        }),
        "status": "release 1.3.0 candidate; owner test pending",
    })
    if target == "gt4":
        guard_pin = json.loads((ROOT / "docs/inputs/slice262-license-guard-runtime.json").read_text(encoding="utf-8"))
        manifest.update({
            "purpose": "Release 1.3.0 Gran Turismo 4 (v2.00) game build: the 1.2.3 recipe plus TASK-097 (standalone "
                       "start, game_main), TASK-101 (target-aware hooks) and the release 1.3.0 source state.",
            "classification": "The slice268 1.2.3 recipe on the task/101-multi-game source: same capsules "
                              "(slice262-frame-cost-runtime, slice262-license-guard-runtime), port (slice200), shaders "
                              "(slice153), EE library profile aspect (slice217) and VU1/VU0 tables; the executable "
                              "adds the standalone start rules, the host main and the target-aware hook table, and "
                              "the release ships gt4-gs 1.2.1.",
            "preserved_inputs": "Port (slice200), memcard capsule, shaders (slice153), VU1/VU0 tables and EE library "
                                "profile aspect (slice217) as slice268. Changed: the executable, gt4-gs 1.2.1.",
            "source_capsule": "private/slice262-frame-cost-runtime",
            "source_manifest": "slice262-frame-cost-runtime.json",
            "license_guard_capsule": "private/slice262-license-guard-runtime",
            "license_guard_receipt_sha256": guard_pin["receipt_sha256"],
            "license_guard_source_sha256": guard_pin["source_sha256"],
        })
        suffix = "pcsx2"
    elif target == "spec2":
        guard_pin = json.loads((ROOT / "docs/inputs/slice277-license-guard-runtime.json").read_text(encoding="utf-8"))
        iop_pin = json.loads((ROOT / "docs/inputs/slice276-spec2-iop-runtime.json").read_text(encoding="utf-8"))
        elf_pin = json.loads((ROOT / "docs/inputs/spec2-elf.json").read_text(encoding="utf-8"))
        lib_pin = json.loads((ROOT / "docs/inputs/spec2-ee-libraries.json").read_text(encoding="utf-8"))
        lib = lib_pin["profiles"]["spec2"]
        manifest.update({
            "target": "spec2",
            "purpose": "Release 1.3.0 Gran Turismo 4 Spec II 1.10 game build (TASK-100/TASK-101): the Spec II target "
                       "with the S8c EE library, the S7b IOP reboot rule and the S8b license demo guard.",
            "classification": "Spec II target build from the task/101-multi-game source: -DGT4_TARGET=spec2 "
                              "-DGT4_EE_PROFILE=spec2, runtime capsule slice276-spec2-iop-runtime (DNAS310 async "
                              "FILEIO), license guard slice277-license-guard-runtime, EE library slice278 "
                              "(spec2-ee-libraries.json), release distribution ON. Same port, shaders and VU1/VU0 "
                              "tables as the v2.00 release; gt4-gs 1.2.1.",
            "preserved_inputs": "Port (slice200), shaders (slice153), VU1 table (slice157) and VU0 image/table "
                                "(slice160) as the v2.00 release. Changed: target/profile, EE library, runtime "
                                "capsule, license guard, executable; gt4-gs 1.2.1.",
            "source_capsule": "private/slice276-spec2-iop-runtime",
            "source_manifest": "slice276-spec2-iop-runtime.json",
            "license_guard_capsule": "private/slice277-license-guard-runtime",
            "license_guard_receipt_sha256": guard_pin["receipt_sha256"],
            "license_guard_source_sha256": guard_pin["source_sha256"],
            "frozen_game_library": {"path": lib["path"], "size_bytes": lib["size_bytes"], "sha256": lib["sha256"]},
            "spec2_derived_elf": {"path": "private/slice272-spec2-elf/SCUS_974.36",
                                  "size_bytes": elf_pin["derived"]["size_bytes"],
                                  "sha256": elf_pin["derived"]["sha256"]},
            "spec2_iop_runtime": {"capsule": "private/slice276-spec2-iop-runtime",
                                  "receipt_sha256": iop_pin.get("receipt_sha256"),
                                  "source_sha256": iop_pin.get("source_sha256")},
            "ee_library": {"manifest": "docs/inputs/spec2-ee-libraries.json", "profile": "spec2",
                           "path": lib["path"], "size_bytes": lib["size_bytes"], "sha256": lib["sha256"],
                           "generated_units": lib["generated_units"]},
            "owner_launch": {"default": "the launcher's Spec II Play (gt4vulkan-spec2.exe with the derived ELF and "
                                       "the Spec II disc copy)"},
        })
        suffix = "spec2"
    else:
        raise ValueError("target must be gt4 or spec2")

    output = ROOT / "docs/inputs" / f"slice{slice_number}-{suffix}.json"
    if output.exists():
        raise ValueError(f"preserve the existing pin: {output}")
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    return manifest, output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("slice", type=int)
    parser.add_argument("--target", choices=("gt4", "spec2"), required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--date", default="2026-10-23")
    options = parser.parse_args()
    manifest, output = pin(options.slice, options.target, options.exe, options.date)
    print(json.dumps({"pin": str(output), "runtime": manifest["runtime"]}, indent=2))
