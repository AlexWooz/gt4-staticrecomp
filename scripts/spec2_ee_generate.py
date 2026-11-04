#!/usr/bin/env python
"""Translate the Spec II derived ELF with the pinned donor translator (TASK-100 S4, translation step only).

The donor producer (`tools/ee_producer`, scripts/ee_producer.py) rebuilds the v2.00 library from its CORE, and its
CMake profile is pinned to the v2.00 library hash. The second target needs a plain-ELF input instead:

    eerecomp.exe <derived SCUS_974.36> --out <fresh directory>

This tool runs exactly that, with the pinned translator, verifies the output inventory (one unit per 16 KiB window,
plus table.cpp), and writes a summary. It does not build a library or an executable; that is the rest of S4.

The generated C++ is game-derived: it stays under private/ or O: and is never committed.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRANSLATOR = Path("private/reference-acceleration-study/eerecomp.exe")
TRANSLATOR_SHA256 = "5aec705e31da349c941414fd54be264000459c3cb0d22727e7d810b5b24b03c0"
TRANSLATOR_SIZE = 692224
ELF_PIN = ROOT / "docs/inputs/spec2-elf.json"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def inventory(directory):
    records = []
    for path in sorted(directory.iterdir()):
        if path.is_file():
            records.append({"name": path.name, "size_bytes": path.stat().st_size, "sha256": sha256(path)})
    return records


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--assets-root", type=Path, default=Path("C:/Antigravity/gt4-staticrecomp"))
    parser.add_argument("--elf", type=Path, required=True, help="derived SCUS_974.36 (scripts/spec2_elf_patch.py)")
    parser.add_argument("--output", type=Path, required=True, help="fresh directory for the generated units")
    parser.add_argument("--entries", type=Path, help="entry seed file passed to the translator")
    parser.add_argument("--stats-line", help="translator stats line recorded in the summary")
    parser.add_argument("--inventory-only", action="store_true",
                        help="hash an existing generated directory instead of running the translator")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)

    pin = json.loads(ELF_PIN.read_text(encoding="utf-8"))
    if sha256(args.elf) != pin["derived"]["sha256"]:
        raise SystemExit("input ELF is not the pinned derived image")
    translator = args.assets_root / TRANSLATOR
    if translator.stat().st_size != TRANSLATOR_SIZE or sha256(translator) != TRANSLATOR_SHA256:
        raise SystemExit("translator differs from its pin")
    if args.inventory_only:
        if not (args.output / "table.cpp").exists():
            raise SystemExit("no generated units in %s" % args.output)
        records = inventory(args.output)
        result = None
        command = "inventory only"
    else:
        if args.output.exists() and any(args.output.iterdir()):
            raise SystemExit("refusing to reuse %s" % args.output)
        args.output.mkdir(parents=True, exist_ok=True)
        arguments = [str(translator), str(args.elf), "--out", str(args.output)]
        if args.entries:
            arguments += ["--entries", str(args.entries)]
        result = subprocess.run(arguments, capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit("translator failed: %s" % (result.stderr or result.stdout))
        records = inventory(args.output)
        command = result.stdout.strip()
    units = [r for r in records if r["name"].startswith("unit_")]
    summary = {
        "schema_version": 1,
        "schema": "spec2-ee-generated/1",
        "input_elf": {"path": args.elf.name, "size_bytes": args.elf.stat().st_size, "sha256": sha256(args.elf)},
        "translator": {"path": str(TRANSLATOR), "size_bytes": TRANSLATOR_SIZE, "sha256": TRANSLATOR_SHA256},
        "command": command,
        "stats": args.stats_line,
        "entry_seeds": None if not args.entries else {"path": args.entries.name, "sha256": sha256(args.entries)},
        "files": len(records),
        "units": len(units),
        "inventory_sha256": hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest(),
        "total_bytes": sum(r["size_bytes"] for r in records),
        "notes": "Generic sources only (the v2.00 decomp names and seeded entry list do not apply); the plugin segment "
                 "is translated as code because it is an executable PT_LOAD.",
    }
    if args.json:
        args.json.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: summary[k] for k in ("files", "units", "inventory_sha256", "total_bytes")}, indent=2))
    print(summary["command"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
