#!/usr/bin/env python
"""Build the Spec II derived game ELF: every injector store applied ahead of translation (TASK-100 S2).

Why: the plugin injector writes 35 words into guest RAM at boot (docs/SPEC2-PATCHES.md). Statically recompiled code
never executes those stores, so the words must be in the image the translator reads.

What it does:
  1. reads SCUS_974.36 from the owner's Spec II ISO (plain ELF, no CORE/ELF reconstruction step) and verifies its pin;
  2. applies every entry of docs/inputs/spec2-patches.json that has source "injector", checking the old word first;
  3. writes the result to a fresh directory.

Decision (documented in SPEC2-PATCHES.md): the trampoline at 0x001001E8 is **kept**. The plugin's init calls the
function it replaced (0x005E3AA8) and one more game function (0x004E2940) whose side effects are not part of the store
table, so reproducing the stores natively while dropping the call would change behaviour. The runtime stores become
redundant (they write the same words again) and harmless: translated code never re-reads them.

The plugin segment stays in the image as code, so its 2,642 instructions are translated like the rest of the ELF and
its five jal hooks keep working. No JIT anywhere.

Usage:
  private/tooling-venv/Scripts/python.exe scripts/spec2_elf_patch.py --iso "<Spec II ISO>" \
      --output C:/Antigravity/gt4-staticrecomp/private/slice272-spec2-elf \
      --json docs/inputs/spec2-elf.json
"""

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ISO_PIN = ROOT / "docs/inputs/spec2-iso.json"
PATCH_PIN = ROOT / "docs/inputs/spec2-patches.json"
TEXT_VA, TEXT_SIZE = 0x100000, 0x55FBBC
DATA_VA, DATA_SIZE = 0x65FC00, 0xD45FC
PLUGIN_VA, PLUGIN_SIZE = 0x68A480, 0x2CCC
ENTRY_CALL = 0x1001E8


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_elf(data):
    """(segments, sections) of a little-endian ELF32 file, minimal."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("not a little-endian ELF32 image")
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    segments = []
    for index in range(phnum):
        p_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_flags, _align = struct.unpack_from(
            "<8I", data, phoff + index * phentsize)
        segments.append({"type": p_type, "offset": p_offset, "vaddr": p_vaddr, "filesz": p_filesz,
                         "memsz": p_memsz, "flags": p_flags})
    return segments


def file_offset(segments, address, size=4):
    for segment in segments:
        if segment["type"] != 1:
            continue
        if segment["vaddr"] <= address and address + size <= segment["vaddr"] + segment["filesz"]:
            return segment["offset"] + address - segment["vaddr"]
    raise ValueError("address %08x is not file-backed" % address)


def split_data_around_plugin(data):
    """Rewrite the program headers so no segment overlaps the .plugin PT_LOAD.

    The Spec II file stores the plugin as its own RWX PT_LOAD at 0x0068A480, *after* a data PT_LOAD that also covers
    those addresses (the beta's data image is zero there). The runtime loads the segments in order, so the plugin wins;
    the donor translator instead reads the *first* segment that contains an address (recomp/elf.cpp, Image::word), so
    it read the zeros and emitted empty code for every plugin entry. Splitting the data segment around the plugin makes
    the loaded image identical and every lookup unambiguous.
    """
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    headers = [list(struct.unpack_from("<8I", data, phoff + i * phentsize)) for i in range(phnum)]
    plugin = next((h for h in headers if h[0] == 1 and h[2] == PLUGIN_VA), None)
    if plugin is None:
        raise ValueError("no PT_LOAD at the plugin address")
    data_header = next((h for h in headers if h[0] == 1 and h[2] != PLUGIN_VA
                        and h[2] < PLUGIN_VA < h[2] + h[5]), None)
    if data_header is None:
        raise ValueError("no data PT_LOAD covers the plugin address")
    if data_header[5] < plugin[5]:
        raise ValueError("the covering segment is smaller than the plugin")
    low_size = PLUGIN_VA - data_header[2]
    high_address = PLUGIN_VA + plugin[4]
    high_offset = data_header[1] + (high_address - data_header[2])
    high_size = data_header[4] - (high_address - data_header[2])
    low = [1, data_header[1], data_header[2], data_header[3], low_size, low_size, data_header[6], data_header[7]]
    bss = data_header[5] - data_header[4]
    high = [1, high_offset, high_address, data_header[3] + (high_address - data_header[2]), high_size,
            high_size + bss, data_header[6], data_header[7]]
    rewritten = []
    for header in headers:
        if header is data_header:
            rewritten.extend([low, plugin, high])
        elif header is plugin:
            continue
        else:
            rewritten.append(header)
    if 52 + 32 * len(rewritten) > 0x1000:
        raise ValueError("program header table no longer fits before the text")
    out = bytearray(data)
    for index, header in enumerate(rewritten):
        struct.pack_into("<8I", out, phoff + index * phentsize, *header)
    struct.pack_into("<H", out, 44, len(rewritten))
    # The old table had phnum entries; zero the leftovers so the file is deterministic.
    for index in range(len(rewritten), phnum):
        struct.pack_into("<8I", out, phoff + index * phentsize, *([0] * 8))
    return bytes(out)


def apply_patches(data, patches, reverse=False):
    """Applies (or reverts) every injector patch; returns the new bytes."""
    segments = load_elf(data)
    out = bytearray(data)
    for patch in patches:
        address, size = patch["addr"], patch["size"]
        old = patch["old"] & ((1 << (8 * size)) - 1)
        new = patch["new"] & ((1 << (8 * size)) - 1)
        at = file_offset(segments, address, size)
        current = int.from_bytes(out[at:at + size], "little")
        expected, value = (new, old) if reverse else (old, new)
        if current != expected:
            raise ValueError("%s: word at %08x is %08x, expected %08x"
                             % ("revert" if reverse else "apply", address, current, expected))
        out[at:at + size] = value.to_bytes(size, "little")
    return bytes(out)


def read_iso(iso_path):
    """Verifies the ISO identity and returns (SCUS_974.36 bytes, identity)."""
    pin = json.loads(ISO_PIN.read_text(encoding="utf-8"))
    digest = hashlib.sha256()
    size = 0
    with iso_path.open("rb") as stream:
        while chunk := stream.read(1 << 24):
            digest.update(chunk)
            size += len(chunk)
    identity = {"size_bytes": size, "sha256": digest.hexdigest()}
    if identity != pin["disc"]:
        raise ValueError("unexpected Spec II ISO identity: %s" % identity)
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.open(str(iso_path))
    try:
        with iso.open_file_from_iso(iso_path="/SCUS_974.36;1") as stream:
            data = stream.read()
    finally:
        iso.close()
    if sha256(data) != pin["files"]["/SCUS_974.36;1"]["sha256"]:
        raise ValueError("SCUS_974.36 differs from the pin")
    return data, identity


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--iso", type=Path, help="the owner's Spec II ISO")
    parser.add_argument("--elf", type=Path, help="an already extracted SCUS_974.36 (skips the ISO check)")
    parser.add_argument("--output", type=Path, help="fresh output directory")
    parser.add_argument("--json", type=Path, help="write the pin/summary here")
    parser.add_argument("--revert", action="store_true", help="remove the patches (control run)")
    args = parser.parse_args(argv)

    pin = json.loads(PATCH_PIN.read_text(encoding="utf-8"))
    patches = [p for p in pin["patches"] if p.get("source", "injector") == "injector"]
    if args.elf:
        original = args.elf.read_bytes()
        identity = None
    else:
        if not args.iso:
            parser.error("--iso or --elf is required")
        original, identity = read_iso(args.iso)
    derived = apply_patches(original, patches, reverse=args.revert)
    if not args.revert:
        derived = split_data_around_plugin(derived)

    summary = {
        "schema_version": 1,
        "schema": "spec2-elf/1",
        "iso": identity,
        "original": {"size_bytes": len(original), "sha256": sha256(original)},
        "derived": {"size_bytes": len(derived), "sha256": sha256(derived)},
        "patches_applied": 0 if args.revert else len(patches),
        "patches_reverted": len(patches) if args.revert else 0,
        "layout": {
            "text": {"va": TEXT_VA, "size_bytes": TEXT_SIZE},
            "data": {"va": DATA_VA, "size_bytes": DATA_SIZE},
            "plugin": {"va": PLUGIN_VA, "size_bytes": PLUGIN_SIZE,
                       "note": "kept as translatable code; its five jal hooks and its init are translated"},
        },
        "entry_call": {"address": ENTRY_CALL, "kept": True,
                       "reason": "the beta's own boot initialiser (0x005E3AA8) and one further game call are made "
                                 "from the plugin init, so the call is kept; the runtime stores are redundant"},
        "tool": "scripts/spec2_elf_patch.py",
    }
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / "SCUS_974.36").write_bytes(derived)
    if args.json:
        args.json.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: summary[k] for k in ("original", "derived", "patches_applied", "patches_reverted")}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
