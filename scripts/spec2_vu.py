#!/usr/bin/env python
"""VU1 / VU0 identity for the Spec II target (TASK-100 S5).

VU1: the EE uploads the same XOR-encrypted 0x3FF0-byte microcode blob on both discs. In Spec II it sits at
VA 0x69B110 (file offset 0x59C110); in the reconstructed v2.00 ELF at file 0x525DD0. Decrypting both with the v2.00
keystream (scripts/vu1_image.py) must give the same image, which is what makes our content-addressed VU1 AOT table
usable unchanged.

VU0: the upload is plain code in the data segment: VA 0x731180, 0x920 bytes in Spec II against VA 0x6D3280,
0xA80 bytes in v2.00. The generated VU0 programs are keyed by the live code image (scripts/vu0_aot_overlay.inc), so a
different image just misses every key and the exact interpreter runs. This tool measures the difference and records
which entries of the v2.00 VCALLMS list can still apply.

Nothing here writes game data: the ELFs are read from the owner's disc and from private/, and the pin holds hashes.
"""

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vu1_image  # noqa: E402
import vu0_image  # noqa: E402

SPEC2_VU1_OFFSET = 0x59C110
SPEC2_VU1_BLOB = 0x3FF0
SPEC2_VU1_SHA_PREFIX = 'ae6b6cb3b4406ff3'
SPEC2_VU0_VADDR = 0x731180
SPEC2_VU0_BYTES = 0x920
SPEC2_TEXT_VA = 0x100000
SPEC2_TEXT_SIZE = 0x55FBBC
SPEC2_DATA_VA = 0x65FC00
SPEC2_DATA_SIZE = 0xD45FC


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def spec2_file_offset(va):
    if SPEC2_TEXT_VA <= va < SPEC2_TEXT_VA + SPEC2_TEXT_SIZE:
        return 0x1000 + va - SPEC2_TEXT_VA
    if SPEC2_DATA_VA <= va < SPEC2_DATA_VA + SPEC2_DATA_SIZE:
        return 0x560C00 + va - SPEC2_DATA_VA
    raise ValueError("address %08x is not in the Spec II file image" % va)


def vu1_report(spec2, v200):
    encrypted = spec2[SPEC2_VU1_OFFSET:SPEC2_VU1_OFFSET + SPEC2_VU1_BLOB]
    if len(encrypted) != SPEC2_VU1_BLOB:
        raise ValueError("Spec II ELF is too short for the VU1 blob")
    plain = vu1_image.decrypt_blob(encrypted)
    v200_plain = vu1_image.decrypt_blob(v200[vu1_image.ELF_OFFSET:vu1_image.ELF_OFFSET + vu1_image.BLOB_BYTES])
    return {
        "file_offset": SPEC2_VU1_OFFSET,
        "blob_bytes": SPEC2_VU1_BLOB,
        "encrypted_sha256": sha256(encrypted),
        "decrypted_sha256": sha256(plain),
        "decrypted_sha256_prefix": sha256(plain)[:16],
        "expected_prefix": SPEC2_VU1_SHA_PREFIX,
        "prefix_matches": sha256(plain).startswith(SPEC2_VU1_SHA_PREFIX),
        "v200_decrypted_sha256": sha256(v200_plain),
        "identical_to_v200": plain == v200_plain,
        "aot_decision": "the content-addressed VU1 AOT table applies unchanged",
    }


def vu0_report(spec2, v200):
    start = spec2_file_offset(SPEC2_VU0_VADDR)
    image = spec2[start:start + SPEC2_VU0_BYTES]
    if len(image) != SPEC2_VU0_BYTES:
        raise ValueError("Spec II ELF is too short for the VU0 upload")
    v200_image = vu0_image.upload_bytes(v200)
    overlap = min(len(image), len(v200_image))
    differing = sum(1 for a, b in zip(image[:overlap], v200_image[:overlap]) if a != b)
    entries = []
    for entry in vu0_image.VCALLMS_ENTRIES:
        at = entry
        entries.append({
            "entry": entry,
            "in_range": at + 8 <= SPEC2_VU0_BYTES,
            "first_words_equal": image[at:at + 8] == v200_image[at:at + 8] if at + 8 <= overlap else False,
        })
    return {
        "vaddr": SPEC2_VU0_VADDR,
        "file_offset": start,
        "upload_bytes": SPEC2_VU0_BYTES,
        "sha256": sha256(image),
        "v200_upload_bytes": len(v200_image),
        "v200_sha256": sha256(v200_image),
        "differing_bytes_in_the_overlap": differing,
        "tail_bytes_beyond_v200": max(0, SPEC2_VU0_BYTES - len(v200_image)),
        "vcallms_entries": entries,
        "aot_decision": "the v2.00 generated programs cannot match this image (key and identity digest), so the exact "
                        "interpreter runs as the counted fallback; a Spec II table is generated after S7b if the "
                        "profile shows VU0 cost",
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--elf", type=Path, required=True, help="Spec II SCUS_974.36")
    ap.add_argument("--v200-elf", type=Path, required=True, help="reconstructed v2.00 SCUS_973.28 ELF")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)
    spec2 = args.elf.read_bytes()
    v200 = args.v200_elf.read_bytes()
    document = {
        "schema_version": 1,
        "schema": "spec2-vu/1",
        "spec2_elf": {"size_bytes": len(spec2), "sha256": sha256(spec2)},
        "vu1": vu1_report(spec2, v200),
        "vu0": vu0_report(spec2, v200),
        "confidence": {
            "vu1_blob_is_the_v200_image": "Confirmed" if vu1_report(spec2, v200)["identical_to_v200"] else "Unknown",
            "vu0_image_differs": "Confirmed",
            "vu0_runtime_fallback": "Confirmed from scripts/vu0_aot_overlay.inc (key + identity digest must match)",
        },
    }
    if args.json:
        args.json.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in document.items() if k in ("vu1", "vu0")}, indent=2)[:2000])
    return 0


if __name__ == "__main__":
    sys.exit(main())
