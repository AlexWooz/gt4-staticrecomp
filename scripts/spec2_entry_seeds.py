#!/usr/bin/env python
"""Collect the EE entry seeds the pinned translator cannot discover on its own (TASK-100 S8c).

The donor translator (`eerecomp.exe`) discovers entries from `fallthrough`, `dataword` and `luipair` sources, but
its `luipair` window is 3 instructions and its dataword scan only walks the data segments. Two classes of real code
entries are therefore missed:

  * text addresses built by `lui` + `addiu/ori` far apart (menu descriptor and vtable initialisers that set many
    registers first): the owner's Options-menu halt at `0x003CA690` is one (`lui s0,0x003D` at `0x003CC8EC`, the
    `addiu s0,s0,-22896` 53 instructions later);
  * literal text pointers inside executable segments (the plugin's own pointer tables, S7b).

This tool runs the same read-only ledger scans (`scripts/ee_entry_ledger.py`) on the pinned derived ELF and writes a
seed file in the translator's `--entries` format (a count line, then `0x%08X` lines). By default it subtracts the
entries already present in a generated directory, so the file is the delta a rebuild needs; `--all` writes every
candidate for a from-scratch run. The seed list is game-derived: it stays under private/ or O: and is never
committed; its sha256 goes into the library pin.
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import ee_entry_ledger as ledger  # noqa: E402

ELF_PIN = ROOT / "docs/inputs/spec2-elf.json"
LUIPAIR_WINDOW = 64


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def executable_literals(image):
    """Literal text pointers stored in executable segments that name a code entry.

    The plugin segment is executable but carries data, strings and its own pointer tables. Only an entry that starts
    with a stack frame or a direct jump is code (the S7b plugin thunks are `j`); format strings and data words are not.
    """
    hits = {}
    for segment in image.text_segments():
        for offset in range(0, len(segment.body) - 3, 4):
            value = int.from_bytes(segment.body[offset:offset + 4], "little")
            if not image.is_text(value):
                continue
            word = image.word(value)
            if word is None or ledger.is_placeholder_word(word):
                continue
            op = (word >> 26) & 0x3F
            if not (ledger.is_stack_frame(word) or op in (0x02, 0x03)):  # j / jal thunks
                continue
            hits.setdefault(value, f"word at 0x{segment.address + offset:08X}")
    return hits


def looks_like_code(image, address):
    """Reject text addresses whose first word is not code.

    The plugin segment is executable but carries data and strings; a `lui`/`addiu`-shaped word there can name an
    ASCII string (the Options scan saw `unkB`, `over`, `argv`, `Test`). Placeholders are already excluded.
    """
    word = image.word(address)
    if word is None or ledger.is_placeholder_word(word):
        return False
    raw = word.to_bytes(4, "little")
    return not all(0x20 <= byte < 0x7F for byte in raw)


def collect(elf, generated=None):
    image = ledger.Image(ledger.load_segments(elf.read_bytes()))
    flow = ledger.scan_control_flow(image)
    luipairs = ledger.detect_luipairs(image, window=LUIPAIR_WINDOW)
    fallthrough, _ = ledger.detect_fallthrough(image, flow)
    literals = executable_literals(image)
    datawords = ledger.detect_datawords(image)
    sources = {"luipair": luipairs, "fallthrough": fallthrough, "executable-literal": literals, "dataword": datawords}
    translated = ledger.translated_entries(generated) if generated else set()
    candidates = {}
    for name, hits in sources.items():
        for address in hits:
            if address not in translated and looks_like_code(image, address):
                candidates.setdefault(address, []).append(name)
    return {
        "image": image,
        "sources": sources,
        "translated": translated,
        "candidates": candidates,
    }


def write_seed_file(path, addresses):
    addresses = sorted(set(addresses))
    lines = [str(len(addresses))] + [f"0x{address:08X}" for address in addresses]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return addresses


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--elf", type=Path, required=True, help="derived SCUS_974.36 (pinned by spec2-elf.json)")
    parser.add_argument("--generated", type=Path, help="generated units of the previous translation (delta mode)")
    parser.add_argument("--output", type=Path, required=True, help="seed file for the translator --entries")
    parser.add_argument("--json", type=Path, help="summary written next to the seed file")
    parser.add_argument("--all", action="store_true", help="write every candidate, not only the not-translated delta")
    parser.add_argument("--allow-unpinned", action="store_true", help="accept an ELF that differs from the pin")
    options = parser.parse_args(argv)

    pin = json.loads(ELF_PIN.read_text(encoding="utf-8"))
    elf_hash = sha256(options.elf)
    if not options.allow_unpinned and elf_hash != pin["derived"]["sha256"]:
        raise SystemExit("input ELF is not the pinned derived image")

    result = collect(options.elf, None if options.all else options.generated)
    addresses = write_seed_file(options.output, result["candidates"])
    summary = {
        "schema_version": 1,
        "input_elf": {"path": options.elf.name, "size_bytes": options.elf.stat().st_size, "sha256": elf_hash},
        "luipair_window": LUIPAIR_WINDOW,
        "generated": str(options.generated) if options.generated and not options.all else None,
        "translated_entries": len(result["translated"]),
        "source_candidates": {name: len(hits) for name, hits in result["sources"].items()},
        "seeds": len(addresses),
        "seeds_by_source": {
            name: sum(1 for address in addresses if name in result["candidates"][address])
            for name in result["sources"]
        },
        "seed_file": {"path": options.output.name, "sha256": sha256(options.output)},
    }
    if options.json:
        options.json.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
