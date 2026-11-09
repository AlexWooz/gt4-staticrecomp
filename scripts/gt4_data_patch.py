"""Build a GT4.VOL data patch (same-length file substitution) for the runtime overlay.

The game reads GT4.VOL through the donor runtime's sector reader. `native/data_patch.cpp` (installed by the
`slice421-data-runtime` capsule) rewrites the sectors that hold one or more GT4.VOL files with replacement bytes of
exactly the same stored length, so the volume's TOC and every file size stay untouched. The replacement itself is a
patch recipe derived from the user's own disc: nothing derived from game data is ever committed or shipped.

Two edit kinds are supported:

  * `text_replacements` on a text file (for example `.mproject`), optionally minified (whitespace outside string
    literals is collapsed) so the new text still fits the original stored extent;
  * `byte_replacements` with an exact offset and length, for data whose text form is not edited.

The output is one binary container (format documented in the script) plus a JSON report with every hash. The report
contains no game data, only hashes and names.

This tool parses the disc with scripts/gt4_volume.py (validated RoFS reader). It does not write a new TOC: a file
whose new payload does not fit its original stored extent is refused.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

import gt4_volume

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"GT4DPAT1"
VERSION = 1
HEADER = struct.Struct("<8sIIII")
RECORD = struct.Struct("<QII32s32s")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def minify_text(text: str) -> str:
    """Collapse whitespace outside string literals, keeping the token stream identical.

    The widget/project text format separates tokens with whitespace; indentation is not semantic. A quoted string
    keeps every byte, including spaces and backslash escapes.
    """
    out = []
    pending_space = False
    in_string = False
    escaped = False
    for char in text:
        if in_string:
            out.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
            if pending_space and out:
                out.append(" ")
            pending_space = False
            in_string = True
            out.append(char)
        elif char.isspace():
            pending_space = bool(out)
        else:
            if pending_space and out:
                out.append(" ")
            pending_space = False
            out.append(char)
    return "".join(out)


def apply_text_replacements(text: str, replacements: list[dict]) -> str:
    for item in replacements:
        old, new = item["old"], item["new"]
        count = text.count(old)
        if count != 1:
            raise ValueError(f"text anchor must occur exactly once, found {count}: {old[:60]!r}")
        text = text.replace(old, new)
    return text


def ps2zip(payload: bytes) -> bytes:
    """A PS2ZIP (kind 2) wrapper with a raw DEFLATE body, as gt4_volume.decode_payload expects."""
    deflater = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
    body = deflater.compress(payload) + deflater.flush()
    return struct.pack("<II", gt4_volume.PS2ZIP_MAGIC, (-len(payload)) & 0xFFFFFFFF) + body


def decode_record(stream, record) -> tuple[bytes, bytes]:
    packed = gt4_volume.read_at(stream, gt4_volume.VOLUME_OFFSET + record.offset, record.stored_size,
                                gt4_volume.VOLUME_OFFSET + gt4_volume.VOLUME_SIZE)
    return packed, gt4_volume.decode_payload(packed, record.kind, record.size)


def build_replacement(record, decoded: bytes, edit: dict) -> bytes:
    if edit.get("byte_replacements"):
        if edit.get("text_replacements") or edit.get("minify"):
            raise ValueError("byte_replacements cannot be combined with text edits")
        result = bytearray(decoded)
        for item in edit["byte_replacements"]:
            offset, length = int(item["offset"]), int(item["length"])
            replacement = bytes.fromhex(item["hex"])
            if len(replacement) != length or offset < 0 or offset + length > len(result):
                raise ValueError("byte replacement is outside the file or has the wrong length")
            if item.get("expect_hex") is not None and bytes.fromhex(item["expect_hex"]) != result[offset:offset + length]:
                raise ValueError("byte replacement anchor differs from the disc")
            result[offset:offset + length] = replacement
        return bytes(result)

    text = decoded.decode("utf-8")
    text = apply_text_replacements(text, edit.get("text_replacements", []))
    if edit.get("minify", True):
        text = minify_text(text)
    result = text.encode("utf-8")
    if len(result) > record.size:
        raise ValueError(f"edited file grew past its unpacked size ({len(result)} > {record.size})")
    if len(result) < record.size:
        result += b"\n" * (record.size - len(result))
    return result


def encode_same_extent(record, decoded: bytes) -> bytes:
    if record.kind == 1:
        if len(decoded) != record.size:
            raise ValueError("raw record length changed")
        return decoded
    encoded = ps2zip(decoded)
    if len(encoded) > record.stored_size:
        raise ValueError(f"compressed payload does not fit the stored extent ({len(encoded)} > {record.stored_size})")
    return encoded + b"\x00" * (record.stored_size - len(encoded))


def verify_round_trip(record, container: bytes, decoded: bytes) -> None:
    if len(container) != record.stored_size:
        raise ValueError("replacement length differs from the stored extent")
    if record.kind == 2:
        inflater = zlib.decompressobj(-zlib.MAX_WBITS)
        out = inflater.decompress(container[8:], record.size + 1)
        if len(out) != record.size or out != decoded:
            raise ValueError("replacement PS2ZIP does not inflate to the edited file")
        if not inflater.eof:
            raise ValueError("replacement PS2ZIP has an incomplete DEFLATE stream")


def build(iso: Path, recipe: dict, output: Path) -> dict:
    entries, report_files = [], []
    with gt4_volume.verified_volume(iso) as stream:
        catalogue = gt4_volume.read_catalogue(stream)
        for edit in recipe["files"]:
            name = edit["catalogue_path"]
            record = catalogue.files.get(name)
            if record is None:
                raise ValueError(f"no such file in the volume catalogue: {name}")
            packed, decoded = decode_record(stream, record)
            replacement_decoded = build_replacement(record, decoded, edit)
            container = encode_same_extent(record, replacement_decoded)
            verify_round_trip(record, container, replacement_decoded)
            entries.append((gt4_volume.VOLUME_OFFSET + record.offset, container,
                            hashlib.sha256(packed).digest(), hashlib.sha256(container).digest()))
            report_files.append({
                "catalogue_path": name,
                "kind": record.kind,
                "volume_offset": record.offset,
                "absolute_offset": gt4_volume.VOLUME_OFFSET + record.offset,
                "stored_bytes": record.stored_size,
                "decoded_bytes": record.size,
                "original_stored_sha256": digest(packed),
                "replacement_stored_sha256": digest(container),
                "edit": {"text_replacements": len(edit.get("text_replacements", [])),
                         "byte_replacements": len(edit.get("byte_replacements", [])),
                         "minified": bool(edit.get("minify", True)) and not edit.get("byte_replacements")},
            })
    # Records and payload share the same ascending order; the runtime applies overlapping ranges in this order.
    entries.sort()
    payload = bytearray()
    blob = bytearray(HEADER.pack(MAGIC, VERSION, len(entries), sum(len(c) for _, c, _, _ in entries), 0))
    for offset, container, original_hash, replacement_hash in entries:
        payload_offset = len(payload)
        payload.extend(container)
        blob += RECORD.pack(offset, len(container), payload_offset, original_hash, replacement_hash)
    blob += payload

    output.mkdir(parents=True, exist_ok=False)
    patch_path = output / "gt4-data.patch"
    patch_path.write_bytes(blob)
    report = {
        "classification": "Derived patch container; game data replacement is generated from the user's own disc",
        "container": "GT4DPAT1",
        "container_sha256": digest(blob),
        "container_bytes": len(blob),
        "files": report_files,
    }
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--recipe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    recipe = json.loads(arguments.recipe.read_text(encoding="utf-8"))
    report = build(arguments.iso, recipe, arguments.output)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
