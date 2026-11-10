"""GT4.VOL (RoFS) table-of-contents reader and writer.

The GT4.VOL catalogue is a B-tree stored as deflated, XOR-0x55 pages at the start of the volume. This module reads
that catalogue, lets one **existing leaf record's extent fields** be changed, and re-emits a valid catalogue. It
never adds, removes or renames a record: keys, key order, page links and the index pages stay byte-identical, so
the game's own B-tree routing is preserved. Only the leaf page that owns the record is re-deflated, and the offset
table plus the stored size are rebuilt around it.

Format (all confirmed against the v2.00 ISO and the reviewed slice 119/120 reader; see ``docs/VOLUME-READER.md``):

* header at the ToC offset, little endian::

      u32 magic 0xACB990AD | u32 version 0x00030001 | u32 stored_size
      u32 physical_pages   | u16 page_bytes 2048   | u16 logical_pages

* ``logical_pages + 1`` u32 page offsets at 64: ``stored[64 + i*4] = offset ^ ((i + 1) * 0x14AC327A)``;
  ``offset[0] = 64 + 4 * (logical_pages + 1)`` and ``offset[logical_pages] = stored_size``.
* each stored chunk is a raw DEFLATE stream of one 2048-byte page; the page is then XORed with 0x55.
* page header: u16 kind (0 leaf, 1 index), u16 entry count, i32 next page, i32 previous page. Entry descriptors
  are 8 bytes each, indexed from the **end** of the page: u16 key_offset, u16 key_size, u16 meta_offset,
  u16 meta_size, where ``table_start = 2048 - entry_count / 2 * 8 - 4``.
* leaf key: u32 big-endian parent directory id followed by the UTF-8 name.
* leaf metadata: kind byte, then 4 bytes (directory id) / 12 bytes (u32 data page, u32 modified, u32 packed) /
  16 bytes more (u32 decoded size) for kinds 0/1/2.

The writer refuses to grow the catalogue beyond the reserved ``physical_pages`` span and refuses to touch index
pages. It does not allocate volume space: the caller supplies the new data page and lengths, and a separate
allocation check reports whether the new extent overlaps another file.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

MAGIC = 0xACB990AD
VERSION = 0x00030001
PAGE_BYTES = 2048
HEADER = struct.Struct("<IIIIHH")
OFFSET_SALT = 0x14AC327A
XOR_BYTE = 0x55
# v2.00 ISO geometry (docs/VOLUME-READER.md and scripts/gt4_volume.py).
V200_VOLUME_OFFSET = 216_840_192
V200_TOC_OFFSET = 17_483_776
V200_VOLUME_SIZE = 2_459_502_592
CATALOGUE_CAP = 2 * 1024 * 1024


class TocError(ValueError):
    """The input is not a supported, structurally valid RoFS catalogue."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TocError(message)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def inflate_page(chunk: bytes) -> bytes:
    inflater = zlib.decompressobj(-zlib.MAX_WBITS)
    try:
        decoded = inflater.decompress(chunk, PAGE_BYTES + 1)
    except zlib.error as error:
        raise TocError("invalid raw DEFLATE page") from error
    require(len(decoded) == PAGE_BYTES, "page does not decode to 2048 bytes")
    require(not inflater.unconsumed_tail, "page has unconsumed DEFLATE input")
    require(not inflater.unused_data, "page has trailing DEFLATE data")
    return decoded


def deflate_page(page: bytes) -> bytes:
    require(len(page) == PAGE_BYTES, "page must be 2048 bytes")
    encoder = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
    return encoder.compress(page) + encoder.flush()


def decode_page(chunk: bytes) -> bytes:
    return bytes(value ^ XOR_BYTE for value in inflate_page(chunk))


def encode_page(page: bytes) -> bytes:
    return deflate_page(bytes(value ^ XOR_BYTE for value in page))


@dataclass
class LeafEntry:
    """One leaf record inside one logical page."""

    page_index: int
    entry_index: int
    parent: int
    name: str
    node_kind: int
    meta_offset: int
    data_page: int | None
    modified: int | None
    packed: int | None
    size: int | None

    @property
    def path_name(self) -> str:
        return self.name

    def as_report(self) -> dict:
        report = {
            "page_index": self.page_index,
            "entry_index": self.entry_index,
            "parent_id": self.parent,
            "name": self.name,
            "node_kind": self.node_kind,
            "meta_offset": f"0x{self.meta_offset:X}",
        }
        if self.node_kind:
            report.update({
                "data_page": self.data_page,
                "modified": self.modified,
                "packed": self.packed,
                "size": self.size,
            })
        return report


class TocImage:
    """A parsed catalogue that can re-emit itself and update existing leaf extents."""

    def __init__(self, stored: bytes, toc_offset: int = 0):
        require(64 <= len(stored) <= CATALOGUE_CAP, "catalogue size is outside the supported range")
        magic, version, stored_size, physical_pages, page_bytes, logical_pages = HEADER.unpack_from(stored)
        require(magic == MAGIC and version == VERSION, "unsupported RoFS header")
        require(stored_size == len(stored), "stored size does not match the buffer")
        require(page_bytes == PAGE_BYTES, "unsupported page size")
        require(logical_pages > 0 and logical_pages * PAGE_BYTES <= CATALOGUE_CAP,
                "logical page count is outside the supported range")
        require(0 < physical_pages <= 0x10000, "unsupported physical page count")

        self.physical_pages = physical_pages
        self.logical_pages = logical_pages
        self.toc_offset = toc_offset
        # Data pages are numbered from the first page after the reserved ToC span.
        self.data_start = toc_offset + physical_pages * PAGE_BYTES
        table_end = 64 + 4 * (logical_pages + 1)
        require(table_end <= len(stored), "offset table exceeds the buffer")
        offsets = [struct.unpack_from("<I", stored, 64 + index * 4)[0]
                   ^ (((index + 1) * OFFSET_SALT) & 0xFFFFFFFF)
                   for index in range(logical_pages + 1)]
        require(offsets[0] == table_end, "offset table start mismatch")
        require(offsets[-1] == len(stored), "offset table end mismatch")
        require(all(0 < right - left <= 4096 for left, right in zip(offsets, offsets[1:])),
                "unordered or oversized stored page")

        self._header_tail = HEADER.pack(magic, version, 0, physical_pages, page_bytes, logical_pages)
        self._chunks: list[bytes] = [stored[left:right]
                                     for left, right in zip(offsets, offsets[1:])]
        self._pages: list[bytes] = [decode_page(chunk) for chunk in self._chunks]
        self._dirty: set[int] = set()
        self._entries: list[LeafEntry] = []
        self._parse_entries()
        self._original_sha = digest(stored)

    # ---- parsing -------------------------------------------------------------------------------------------
    def _parse_entries(self) -> None:
        for page_index, page in enumerate(self._pages):
            kind, count, next_page, previous_page = struct.unpack_from("<HHii", page, 0)
            require(kind in (0, 1), "unsupported page kind")
            require(all(link == -1 or 0 <= link < self.logical_pages
                        for link in (next_page, previous_page)), "page link out of range")
            require(count % 2 == (0 if kind == 0 else 1), "page entry count parity")
            entries = count // 2
            require(entries <= (PAGE_BYTES - 16) // 8, "page table exceeds the page")
            table_start = PAGE_BYTES - entries * 8 - 4
            for index in range(entries):
                key_offset, key_size, meta_offset, meta_size = struct.unpack_from(
                    "<HHHH", page, PAGE_BYTES - (index + 1) * 8)
                require(12 <= key_offset and key_offset + key_size <= table_start, "key outside page")
                if kind == 1:
                    child = meta_offset | meta_size << 16
                    require(child < self.logical_pages, "index child out of range")
                    continue
                require(meta_size > 0 and 12 <= meta_offset and meta_offset + meta_size <= table_start,
                        "metadata outside page")
                require(key_size >= 5, "short leaf key")
                key = page[key_offset:key_offset + key_size]
                try:
                    name = key[4:].decode("utf-8")
                except UnicodeDecodeError as error:
                    raise TocError("invalid record name encoding") from error
                node_kind = page[meta_offset]
                require(node_kind in (0, 1, 2) and meta_size == (5, 13, 17)[node_kind],
                        "unsupported node metadata")
                if node_kind == 0:
                    require(meta_size == 5, "directory metadata size")
                    self._entries.append(LeafEntry(page_index, index, int.from_bytes(key[:4], "big"),
                                                   name, 0, meta_offset, None, None, None, None))
                    continue
                data_page, modified, packed = struct.unpack_from("<III", page, meta_offset + 1)
                size = struct.unpack_from("<I", page, meta_offset + 13)[0] if node_kind == 2 else packed
                self._entries.append(LeafEntry(page_index, index, int.from_bytes(key[:4], "big"),
                                               name, node_kind, meta_offset, data_page, modified,
                                               packed, size))

    # ---- queries -------------------------------------------------------------------------------------------
    @property
    def original_sha256(self) -> str:
        return self._original_sha

    @property
    def stored_bytes(self) -> int:
        return 64 + 4 * (self.logical_pages + 1) + sum(len(chunk) for chunk in self._chunks)

    @property
    def reserved_bytes(self) -> int:
        return self.physical_pages * PAGE_BYTES

    def entries(self) -> list[LeafEntry]:
        return list(self._entries)

    def files(self) -> list[LeafEntry]:
        return [entry for entry in self._entries if entry.node_kind in (1, 2)]

    def find(self, name: str) -> list[LeafEntry]:
        return [entry for entry in self._entries if entry.name == name]

    def used_pages(self) -> list[tuple[int, int, str]]:
        """Every file extent as (first page, exclusive end page, name)."""
        used = []
        for entry in self.files():
            end = entry.data_page + (entry.packed + PAGE_BYTES - 1) // PAGE_BYTES
            used.append((entry.data_page, end, entry.name))
        return sorted(used)

    def allocation_report(self, entry: LeafEntry) -> dict:
        require(entry.node_kind in (1, 2), "not a file record")
        start = entry.data_page
        end = start + (entry.packed + PAGE_BYTES - 1) // PAGE_BYTES
        following = [left for left, _right, name in self.used_pages()
                     if name != entry.name and left >= end]
        next_start = min(following) if following else None
        slack = None if next_start is None else next_start * PAGE_BYTES - (start * PAGE_BYTES + entry.packed)
        return {
            "name": entry.name,
            "data_page": start,
            "data_offset_in_volume": self.data_start + start * PAGE_BYTES,
            "toc_relative_data_start": self.data_start,
            "packed": entry.packed,
            "size": entry.size,
            "pages_used": end - start,
            "next_file_page": next_start,
            "slack_bytes_to_next_file": slack,
        }

    # ---- mutation ------------------------------------------------------------------------------------------
    def set_file_extent(self, entry: LeafEntry, *, data_page: int, packed: int,
                        size: int | None = None) -> None:
        """Rewrite one existing file record's extent fields in its leaf page."""
        require(entry.node_kind in (1, 2), "not a file record")
        require(0 <= data_page < 0xFFFFFFFF and 0 <= packed, "invalid extent")
        if entry.node_kind == 1:
            require(size is None or size == packed, "kind 1 stores one length")
            size = packed
        else:
            require(size is not None and 0 <= size, "kind 2 needs the decoded size")
        page = bytearray(self._pages[entry.page_index])
        struct.pack_into("<I", page, entry.meta_offset + 1, data_page)
        struct.pack_into("<I", page, entry.meta_offset + 5, entry.modified or 0)
        struct.pack_into("<I", page, entry.meta_offset + 9, packed)
        if entry.node_kind == 2:
            struct.pack_into("<I", page, entry.meta_offset + 13, size)
        self._pages[entry.page_index] = bytes(page)
        self._dirty.add(entry.page_index)
        entry.data_page, entry.packed, entry.size = data_page, packed, size

    def set_file_payload_same_extent(self, entry: LeafEntry, payload: bytes) -> None:
        """Convenience: replace a file's stored bytes with a same-or-smaller payload at the same data page."""
        require(len(payload) <= entry.packed, "payload exceeds the record's stored extent")
        require(entry.node_kind in (1, 2), "not a file record")
        size = entry.size if entry.node_kind == 2 else len(payload)
        self.set_file_extent(entry, data_page=entry.data_page, packed=len(payload), size=size)

    # ---- output --------------------------------------------------------------------------------------------
    def to_bytes(self) -> bytes:
        chunks = [encode_page(self._pages[index]) if index in self._dirty else self._chunks[index]
                  for index in range(self.logical_pages)]
        table_end = 64 + 4 * (self.logical_pages + 1)
        stored_size = table_end + sum(len(chunk) for chunk in chunks)
        require(stored_size <= self.reserved_bytes, "catalogue no longer fits its reserved page span")
        out = bytearray(stored_size)
        HEADER.pack_into(out, 0, MAGIC, VERSION, stored_size, self.physical_pages, PAGE_BYTES,
                         self.logical_pages)
        offset = table_end
        for index, chunk in enumerate(chunks):
            struct.pack_into("<I", out, 64 + index * 4, offset ^ (((index + 1) * OFFSET_SALT) & 0xFFFFFFFF))
            out[offset:offset + len(chunk)] = chunk
            offset += len(chunk)
        struct.pack_into("<I", out, 64 + self.logical_pages * 4,
                         offset ^ (((self.logical_pages + 1) * OFFSET_SALT) & 0xFFFFFFFF))
        return bytes(out)

    def reparse(self) -> "TocImage":
        """Re-parse the emitted catalogue (used by tests and by the CLI's self-check)."""
        return TocImage(self.to_bytes())


def read_volume_toc(stream, volume_offset: int, toc_offset: int) -> bytes:
    stream.seek(volume_offset + toc_offset)
    header = stream.read(64)
    require(len(header) == 64, "short ToC header read")
    size = HEADER.unpack_from(header)[2]
    require(64 <= size <= CATALOGUE_CAP, "catalogue size is outside the supported range")
    stream.seek(volume_offset + toc_offset)
    stored = stream.read(size)
    require(len(stored) == size, "short ToC read")
    return stored


def _load(args) -> TocImage:
    if args.volume:
        with args.volume.open("rb") as stream:
            return TocImage(read_volume_toc(stream, 0, 0))
    if args.iso:
        with args.iso.open("rb") as stream:
            return TocImage(read_volume_toc(stream, V200_VOLUME_OFFSET, V200_TOC_OFFSET),
                            toc_offset=V200_TOC_OFFSET)
    raise SystemExit("pass --iso or --volume")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="GT4.VOL (RoFS) catalogue reader/writer")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--iso", type=Path, help="read the v2.00 ISO read-only at the known ToC offset")
    source.add_argument("--volume", type=Path, help="a volume image whose ToC starts at offset 0")
    commands = parser.add_subparsers(dest="command", required=True)

    inspect = commands.add_parser("inspect", help="header, counts and the extent of one file")
    inspect.add_argument("--file", help="record name, for example option.adc")

    roundtrip = commands.add_parser("roundtrip", help="re-emit the catalogue and report whether it is identical")
    roundtrip.add_argument("--output", type=Path, help="optional path for the re-emitted catalogue blob")

    patch = commands.add_parser("patch", help="rewrite one file record's extent in the catalogue blob")
    patch.add_argument("--file", required=True)
    patch.add_argument("--data-page", type=int, required=True)
    patch.add_argument("--packed", type=int, required=True)
    patch.add_argument("--size", type=int)
    patch.add_argument("--output", type=Path, required=True)

    args = parser.parse_args(argv)
    image = _load(args)

    if args.command == "inspect":
        report = {
            "original_sha256": image.original_sha256,
            "stored_bytes": image.stored_bytes,
            "reserved_bytes": image.reserved_bytes,
            "reserved_slack": image.reserved_bytes - image.stored_bytes,
            "logical_pages": image.logical_pages,
            "physical_pages": image.physical_pages,
            "records": len(image.entries()),
            "files": len(image.files()),
        }
        if args.file:
            matches = image.find(args.file)
            require(len(matches) == 1, f"expected one record named {args.file}, found {len(matches)}")
            report["entry"] = matches[0].as_report()
            report["allocation"] = image.allocation_report(matches[0])
        print(json.dumps(report, indent=2))
        return 0

    if args.command == "roundtrip":
        emitted = image.to_bytes()
        identical = emitted == (args.iso.read_bytes()[V200_VOLUME_OFFSET + V200_TOC_OFFSET:
                                                            V200_VOLUME_OFFSET + V200_TOC_OFFSET
                                                            + len(emitted)]
                                if args.iso else args.volume.read_bytes()[:len(emitted)])
        reparsed = TocImage(emitted)
        if args.output:
            args.output.write_bytes(emitted)
        print(json.dumps({
            "stored_bytes": len(emitted),
            "sha256": digest(emitted),
            "byte_identical": identical,
            "reparsed_records": len(reparsed.entries()),
            "reparsed_stored_bytes": reparsed.stored_bytes,
        }, indent=2))
        return 0 if identical and len(reparsed.entries()) == len(image.entries()) else 1

    matches = image.find(args.file)
    require(len(matches) == 1, f"expected one record named {args.file}, found {len(matches)}")
    entry = matches[0]
    image.set_file_extent(entry, data_page=args.data_page, packed=args.packed, size=args.size)
    emitted = image.to_bytes()
    reparsed = TocImage(emitted)
    check = reparsed.find(args.file)[0]
    args.output.write_bytes(emitted)
    print(json.dumps({
        "output": str(args.output),
        "stored_bytes_before": image.stored_bytes,
        "stored_bytes_after": len(emitted),
        "sha256": digest(emitted),
        "entry_before": entry.as_report(),
        "entry_after_reparsed": check.as_report(),
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
