"""Bounded offline RoFS catalogue and explicitly selected GT4 file extraction.

Format facts: docs/NENKAI-REFERENCES.md. This enumerates validated leaf records;
it does not implement or qualify B-tree key routing. No game/runtime dependency.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import zlib

import pycdlib

from gt4disc import DEFAULT_MANIFEST, fingerprint, validate_manifest

ROOT = Path(__file__).resolve().parents[1]
# v2.00 (SCUS-97328, CORE.GT4): the reviewed slice119 layout.
TOC_OFFSET = 17_483_776
VOLUME_OFFSET = 216_840_192
VOLUME_SIZE = 2_459_502_592
# Spec II (SCUS-97436): the volume is larger and its TOC sits at page 0x22B7 (docs/SPEC2-PLAN.md, S7b). The GT4.VOL
# extent itself comes from the disc (the PT-BR and base v1.10 discs differ in size); the boot ELF hash identifies it.
SPEC2_TOC_OFFSET = 0x115B800
SPEC2_BOOT_PATH = "/SCUS_974.36;1"
PAGE_BYTES = 2048
CATALOGUE_CAP = 2 * 1024 * 1024
SELECTION_CAP = 8
FILE_CAP = 16 * 1024 * 1024
TOTAL_CAP = 32 * 1024 * 1024
PS2ZIP_MAGIC = 0xFFF7EEC5


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def extent(offset: int, length: int, limit: int, minimum: int = 0) -> None:
    require(offset >= minimum and length >= 0 and offset + length <= limit,
            "extent exceeds its container")


def read_at(stream, offset: int, length: int, limit: int) -> bytes:
    extent(offset, length, limit)
    stream.seek(offset)
    data = stream.read(length)
    require(len(data) == length, "short input read")
    return data


def inflate_exact(data: bytes, length: int, cap: int, *, page: bool = False) -> bytes:
    require(0 <= length <= cap, "decoded length exceeds cap")
    inflater = zlib.decompressobj(-zlib.MAX_WBITS)
    try:
        decoded = inflater.decompress(data, length + 1)
    except zlib.error as error:
        raise ValueError("invalid raw DEFLATE") from error
    require(len(decoded) == length, "decoded length mismatch")
    require(not inflater.unconsumed_tail and not inflater.unused_data,
            "trailing or unconsumed DEFLATE bytes")
    # Pinned ToC segments use non-final flush framing; file streams must finish.
    require(inflater.eof or (page and data.endswith(b"\x00\x00\xff\xff")),
            "incomplete DEFLATE stream")
    return decoded


def decode_payload(data: bytes, kind: int, size: int) -> bytes:
    require(len(data) <= FILE_CAP and 0 <= size <= FILE_CAP, "file cap exceeded")
    require(kind in (1, 2), "unsupported file kind")
    if kind == 1:
        require(len(data) == size, "raw file size mismatch")
        require(data[:4] != struct.pack("<I", PS2ZIP_MAGIC),
                "PS2ZIP under raw record kind is not qualified")
        return data
    require(len(data) >= 8, "truncated PS2ZIP header")
    magic, complement = struct.unpack_from("<II", data)
    require(magic == PS2ZIP_MAGIC, "unsupported compressed file wrapper")
    require((size + complement) & 0xFFFFFFFF == 0, "PS2ZIP size mismatch")
    return inflate_exact(data[8:], size, FILE_CAP)


@dataclass(frozen=True)
class Record:
    parent: int
    name: str
    kind: int
    node_id: int = 0
    offset: int = 0
    stored_size: int = 0
    size: int = 0


@dataclass(frozen=True)
class Catalogue:
    files: dict[str, Record]
    directories: int
    pages: int
    records: int
    stored_sha256: str
    decoded_sha256: str


def check_name(name: str, *, root: bool = False) -> None:
    require(bool(name) and not any(char in name for char in "/\\\x00:"),
            "unsafe record name")
    require(name not in (".", "..") or (root and name == "."),
            "relative record name")


def resolve_records(records: list[Record]) -> tuple[dict[str, Record], int]:
    roots = [record for record in records
             if record.parent == 0 and record.name == "." and record.kind == 0]
    require(len(roots) == 1, "expected exactly one root directory")
    root = roots[0]
    directories = {}
    siblings = set()
    for record in records:
        check_name(record.name, root=record is root)
        require(record.parent != 0 or record is root, "extra root record")
        key = (record.parent, record.name)
        require(key not in siblings, "duplicate sibling name")
        siblings.add(key)
        if record.kind == 0:
            require(record.node_id > 0 and record.node_id not in directories,
                    "invalid or duplicate directory ID")
            directories[record.node_id] = record
    paths = {root.node_id: ""}
    for directory_id in directories:
        chain = []
        current = directory_id
        while current not in paths:
            require(current in directories and current not in chain and len(chain) < 64,
                    "missing parent, directory cycle or excessive depth")
            chain.append(current)
            current = directories[current].parent
        for child in reversed(chain):
            node = directories[child]
            paths[child] = (paths[node.parent] + "/" + node.name).lstrip("/")
    files = {}
    for record in records:
        if record is root:
            continue
        require(record.parent in paths, "missing parent directory")
        if record.kind != 0:
            name = (paths[record.parent] + "/" + record.name).lstrip("/")
            files[name] = record
    return files, len(directories)


def parse_catalogue(stored: bytes, volume_size: int, toc_offset: int = TOC_OFFSET,
                    count_parity: int | None = None) -> Catalogue:
    require(64 <= len(stored) <= CATALOGUE_CAP, "catalogue input cap/header")
    magic, version, stored_size, physical_pages, page_bytes, logical_pages = struct.unpack_from(
        "<IIIIHH", stored)
    require(magic == 0xACB990AD and version == 0x00030001, "unsupported RoFS header")
    require(stored_size == len(stored) and page_bytes == PAGE_BYTES,
            "catalogue size/page size mismatch")
    require(logical_pages > 0 and logical_pages * PAGE_BYTES <= CATALOGUE_CAP,
            "catalogue output cap")
    data_start = toc_offset + physical_pages * PAGE_BYTES
    require(toc_offset + len(stored) <= data_start <= volume_size,
            "catalogue/data overlap or data outside volume")
    table_end = 64 + 4 * (logical_pages + 1)
    extent(64, table_end - 64, len(stored))
    offsets = [struct.unpack_from("<I", stored, 64 + index * 4)[0]
               ^ (((index + 1) * 0x14AC327A) & 0xFFFFFFFF)
               for index in range(logical_pages + 1)]
    require(offsets[0] == table_end and offsets[-1] == len(stored), "page offset framing")
    require(all(0 < right - left <= 4096 for left, right in zip(offsets, offsets[1:])),
            "unordered or oversized stored page")
    records = []
    decoded_hash = hashlib.sha256()
    for left, right in zip(offsets, offsets[1:]):
        encrypted = inflate_exact(stored[left:right], PAGE_BYTES, PAGE_BYTES, page=True)
        page = bytes(value ^ 0x55 for value in encrypted)
        decoded_hash.update(page)
        kind, count, next_page, previous_page = struct.unpack_from("<HHii", page)
        require(kind in (0, 1), "unsupported page kind")
        require(all(link == -1 or 0 <= link < logical_pages
                    for link in (next_page, previous_page)), "page link out of range")
        require(count % 2 == (kind if count_parity is None else count_parity),
                "page entry count parity")
        entries = count // 2
        require(entries <= (PAGE_BYTES - 16) // 8, "page table exceeds page")
        table_start = PAGE_BYTES - entries * 8 - 4
        # Delimiter interpretation/routing are deliberately not used for lookup.
        for index in range(entries):
            key_offset, key_size, meta_offset, meta_size = struct.unpack_from(
                "<HHHH", page, PAGE_BYTES - (index + 1) * 8)
            extent(key_offset, key_size, table_start, 12)
            require(key_size >= (1 if kind else 5), "short record key")
            if kind == 1:
                child = meta_offset | meta_size << 16
                require(child < logical_pages, "index child out of range")
                continue
            extent(meta_offset, meta_size, table_start, 12)
            require(meta_size > 0, "empty node metadata")
            require(key_offset + key_size <= meta_offset or meta_offset + meta_size <= key_offset,
                    "key/metadata overlap")
            key = page[key_offset:key_offset + key_size]
            try:
                name = key[4:].decode("utf-8")
            except UnicodeDecodeError as error:
                raise ValueError("invalid record name encoding") from error
            parent = int.from_bytes(key[:4], "big")
            meta = page[meta_offset:meta_offset + meta_size]
            node_kind = meta[0]
            require(node_kind in (0, 1, 2) and meta_size == (5, 13, 17)[node_kind],
                    "unsupported node metadata")
            if node_kind == 0:
                records.append(Record(parent, name, 0, struct.unpack_from("<I", meta, 1)[0]))
            else:
                data_page, modified, packed = struct.unpack_from("<III", meta, 1)
                size = packed if node_kind == 1 else struct.unpack_from("<I", meta, 13)[0]
                offset = data_start + data_page * PAGE_BYTES
                extent(offset, packed, volume_size, data_start)
                records.append(Record(parent, name, node_kind, offset=offset,
                                      stored_size=packed, size=size))
    files, directories = resolve_records(records)
    return Catalogue(files, directories, logical_pages, len(records), digest(stored),
                     decoded_hash.hexdigest())


@contextmanager
def verified_volume(iso_path: Path):
    manifest = json.loads(DEFAULT_MANIFEST.read_text(encoding="utf-8"))
    validate_manifest(manifest)
    with iso_path.open("rb") as stream:
        require(fingerprint(stream) == manifest["disc"], "ISO does not match pinned disc")
        stream.seek(0)
        iso = pycdlib.PyCdlib()
        try:
            iso.open_fp(stream)
            record = iso.get_record(iso_path="/GT4.VOL;1")
            require(record.extent_location() * 2048 == VOLUME_OFFSET
                    and record.data_length == VOLUME_SIZE, "unexpected GT4.VOL extent")
            yield stream
        finally:
            iso.close()


def read_catalogue(stream) -> Catalogue:
    header = read_at(stream, VOLUME_OFFSET + TOC_OFFSET, 64, VOLUME_OFFSET + VOLUME_SIZE)
    size = struct.unpack_from("<I", header, 8)[0]
    require(64 <= size <= CATALOGUE_CAP, "catalogue input cap")
    stored = read_at(stream, VOLUME_OFFSET + TOC_OFFSET, size, VOLUME_OFFSET + VOLUME_SIZE)
    pin = json.loads((ROOT / "docs/inputs/slice119-nenkai-assets.json").read_text(encoding="utf-8"))
    require(digest(stored) == pin["catalogue"]["catalogue_input_window_sha256"],
            "catalogue differs from reviewed slice119")
    catalogue = parse_catalogue(stored, VOLUME_SIZE)
    require(catalogue.decoded_sha256 == pin["catalogue"]["decoded_catalogue_sha256"],
            "decoded catalogue differs from reviewed slice119")
    return catalogue


@dataclass(frozen=True)
class DiscLayout:
    """One game disc's GT4.VOL geometry. Offsets are absolute within the ISO stream."""
    target: str
    volume_offset: int
    volume_size: int
    toc_offset: int
    count_parity: int | None = None  # None = the v2.00 rule (even leaf, odd index); 1 = every Spec II page is odd


class OpenedVolume:
    """An open ISO whose GT4.VOL was identified; records carry volume-relative offsets."""

    def __init__(self, stream, layout: DiscLayout, catalogue_pin: dict | None):
        self.stream = stream
        self.layout = layout
        self.catalogue_pin = catalogue_pin

    def read(self, offset: int, size: int) -> bytes:
        return read_at(self.stream, self.layout.volume_offset + offset, size,
                       self.layout.volume_offset + self.layout.volume_size)

    def read_catalogue(self) -> Catalogue:
        layout = self.layout
        header = self.read(layout.toc_offset, 64)
        size = struct.unpack_from("<I", header, 8)[0]
        require(64 <= size <= CATALOGUE_CAP, "catalogue input cap")
        stored = self.read(layout.toc_offset, size)
        if self.catalogue_pin is not None:
            require(digest(stored) == self.catalogue_pin["catalogue_input_window_sha256"],
                    "catalogue differs from its reviewed pin")
        catalogue = parse_catalogue(stored, layout.volume_size, layout.toc_offset, layout.count_parity)
        if self.catalogue_pin is not None:
            require(catalogue.decoded_sha256 == self.catalogue_pin["decoded_catalogue_sha256"],
                    "decoded catalogue differs from its reviewed pin")
        return catalogue


def detect_target(iso_path: Path) -> str:
    """The boot executable names the target: CORE.GT4 for v2.00, SCUS_974.36 for Spec II."""
    iso = pycdlib.PyCdlib()
    try:
        iso.open(str(iso_path))
        with iso.open_file_from_iso(iso_path="/SYSTEM.CNF;1") as stream:
            config = stream.read(4097)
    finally:
        iso.close()
    require(b"BOOT2" in config, "SYSTEM.CNF has no BOOT2")
    if b"SCUS_974.36" in config:
        return "spec2"
    if b"SCUS_973.28" in config:
        return "v200"
    raise ValueError("unknown GT4 disc target")


@contextmanager
def opened_volume(iso_path: Path, target: str | None = None):
    """Open a v2.00 or Spec II ISO and describe its GT4.VOL without modifying anything.

    v2.00 keeps the reviewed whole-disc pin. Spec II accepts any disc whose boot ELF is the pinned SCUS_974.36 image
    (the PT-BR and base v1.10 volumes differ in size); the patch builder still embeds every original range hash, and the
    runtime refuses an image that does not match.
    """
    target = target or detect_target(iso_path)
    if target == "v200":
        manifest = json.loads(DEFAULT_MANIFEST.read_text(encoding="utf-8"))
        validate_manifest(manifest)
        pin = json.loads((ROOT / "docs/inputs/slice119-nenkai-assets.json").read_text(encoding="utf-8"))
        with iso_path.open("rb") as stream:
            require(fingerprint(stream) == manifest["disc"], "ISO does not match pinned disc")
            stream.seek(0)
            iso = pycdlib.PyCdlib()
            try:
                iso.open_fp(stream)
                record = iso.get_record(iso_path="/GT4.VOL;1")
                require(record.extent_location() * 2048 == VOLUME_OFFSET
                        and record.data_length == VOLUME_SIZE, "unexpected GT4.VOL extent")
                yield OpenedVolume(stream, DiscLayout("v200", VOLUME_OFFSET, VOLUME_SIZE, TOC_OFFSET),
                                   pin["catalogue"])
            finally:
                iso.close()
        return
    if target != "spec2":
        raise ValueError(f"unknown disc target: {target}")
    elf_pin = json.loads((ROOT / "docs/inputs/spec2-elf.json").read_text(encoding="utf-8"))
    expected = elf_pin["original"]["sha256"]
    with iso_path.open("rb") as stream:
        iso = pycdlib.PyCdlib()
        try:
            iso.open_fp(stream)
            boot = iso.get_record(iso_path=SPEC2_BOOT_PATH)
            require(boot.data_length == elf_pin["original"]["size_bytes"], "unexpected Spec II boot size")
            with iso.open_file_from_iso(iso_path=SPEC2_BOOT_PATH) as handle:
                require(digest(handle.read()) == expected, "Spec II boot ELF differs from the pinned image")
            record = iso.get_record(iso_path="/GT4.VOL;1")
            require(record.extent_location() > 0 and record.data_length > 0, "unexpected Spec II GT4.VOL extent")
            yield OpenedVolume(stream, DiscLayout(
                "spec2", record.extent_location() * 2048, record.data_length, SPEC2_TOC_OFFSET, 1), None)
        finally:
            iso.close()


def selected_records(catalogue: Catalogue, names: list[str]) -> list[Record]:
    require(0 < len(names) <= SELECTION_CAP and len(names) == len(set(names)),
            "empty, duplicate or excessive selection")
    selected = []
    for name in names:
        require(name in catalogue.files, "selected catalogue path is absent: " + name)
        record = catalogue.files[name]
        require(0 <= record.stored_size <= FILE_CAP and 0 <= record.size <= FILE_CAP,
                "file cap exceeded")
        selected.append(record)
    require(sum(record.stored_size for record in selected) <= TOTAL_CAP
            and sum(record.size for record in selected) <= TOTAL_CAP, "total selection cap exceeded")
    return selected


def fresh_output_directory(output: Path, private_root: Path) -> Path:
    # Only a fresh ignored capsule is permitted. Do not follow junctions/symlinks.
    output = output.absolute()
    private_root = private_root.absolute()
    require(".." not in output.parts, "relative output traversal")
    require(output.is_relative_to(private_root) and output != private_root,
            "output must be a fresh directory beneath private/")
    for path in (output, *output.parents):
        require(not path.is_symlink() and not path.is_junction(), "linked output ancestor")
    require(output.resolve().is_relative_to(private_root.resolve()), "output escapes private/")
    require(not output.exists(), "output capsule already exists")
    output.mkdir(parents=True, exist_ok=False)
    return output


def extract_selected(stream, catalogue: Catalogue, names: list[str], output: Path,
                     private_root: Path) -> dict:
    selected = selected_records(catalogue, names)
    payloads = []
    for record in selected:
        packed = read_at(stream, VOLUME_OFFSET + record.offset, record.stored_size,
                         VOLUME_OFFSET + VOLUME_SIZE)
        payloads.append((packed, decode_payload(packed, record.kind, record.size)))
    output = fresh_output_directory(output, private_root)
    files = []
    for index, (name, record, (packed, decoded)) in enumerate(zip(names, selected, payloads)):
        # Generated flat names avoid using catalogue paths as host write paths.
        filename = f"asset-{index:02d}.bin"
        with (output / filename).open("xb") as destination:
            destination.write(decoded)
        files.append({"catalogue_path": name, "output_file": filename, "kind": record.kind,
                      "volume_offset": record.offset, "stored_bytes": len(packed),
                      "decoded_bytes": len(decoded), "stored_sha256": digest(packed),
                      "decoded_sha256": digest(decoded)})
    report = {"classification": "Bounded offline selected extraction; no live identity proof",
              "catalogue_sha256": catalogue.stored_sha256,
              "decoded_catalogue_sha256": catalogue.decoded_sha256, "files": files}
    with (output / "manifest.json").open("x", encoding="utf-8") as destination:
        json.dump(report, destination, indent=2)
        destination.write("\n")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--select", action="append", required=True,
                        help="Exact catalogue path, without ./; at most eight")
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    with verified_volume(arguments.iso) as stream:
        report = extract_selected(stream, read_catalogue(stream), arguments.select,
                                  arguments.output, ROOT / "private")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
