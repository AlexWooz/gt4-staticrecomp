"""Adhoc v5 (``ADCH005``) structure reader and same-length page-table patcher.

This is our own clean-room tool. It was written from the bytes of the retail ``option.adc`` and from the published
format notes in ``docs/tasks/INGAME-SETTINGS-MAP.md``; no third-party compiler, decompiler or assembler source was
read or copied. It is deliberately **not** a compiler: it never re-serializes the instruction stream and never
moves a byte. Everything it writes is an in-place, equal-length text replacement, which keeps every id, string
offset, jump target and the total decoded length exactly as the publisher left them.

Format facts used here (all confirmed against the v2.00 ``option.adc``, 251,715 decoded bytes):

* The file starts with the 8-byte magic ``ADCH005\\0`` (v7 is ``ADCH007\\0``).
* A u16 length and the source file name follow at ``0x08`` (``_tmp_option.ad`` in the retail file).
* Text is stored as a u16 little-endian length word followed by that many ASCII bytes.
* A **page-table entry** is a two-string constructor call::

      u16 name_len, name bytes          # page name, for example "Screen"
      u32 class_id                      # sequential symbol id, for example 168
      0x24, 0x02, 0x00, 0x00, 0x00      # two-argument call header (0x0224 tag + one pad byte)
      u16 root_len, root bytes          # RootWindow name, for example "ScreenRoot"
      u16 root_len, root bytes          # the same name again (window + script module)
      u32 trailer                       # per-entry page/tile value, not an index

  The same encoding is used outside the page list (menus, enum tables, key bindings), so the scanner is scoped to
  the Options page-table region by default and validated by the equal-repeat rule.
* A **Dummy** entry is a zero-argument page: name, class id, then the single byte ``0x16`` and the next class id.
  The retail v2.00 file has exactly four (class ids 181..184 at ``0x90AC``, ``0x90FA``, ``0x9148``, ``0x9196``).
  They carry no root window and no room for one, so they cannot become a real page without rewriting the stream.

The page-table region of the v2.00 file is ``0x8A40``..``0x99DD`` (see the map document); the CLI defaults to that
region for this disc and accepts an explicit range for other targets.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from dataclasses import asdict, dataclass
from pathlib import Path
import struct
import sys

MAGIC_V5 = b"ADCH005\x00"
MAGIC_V7 = b"ADCH007\x00"
# v2.00 option.adc page-table region (length word of the group constructor .. last page name).
V200_PAGE_REGION = (0x8A40, 0x99DD)
TWO_ARG_TAG = b"\x24\x02\x00\x00\x00"
EMPTY_ARG_TAG = 0x16
IDENTIFIER = re.compile(rb"[A-Za-z][A-Za-z0-9_]*$")


class ScriptFormatError(ValueError):
    """The input is not a supported, structurally valid Adhoc image."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ScriptFormatError(message)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True)
class PageEntry:
    """One two-string page constructor found in the image."""

    length_offset: int
    name: str
    class_id: int
    root_offset: int
    root: str
    root_repeat_offset: int
    root_repeat: str
    trailer: int

    @property
    def name_bytes(self) -> int:
        return len(self.name.encode("ascii"))

    @property
    def root_bytes(self) -> int:
        return len(self.root.encode("ascii"))

    def as_report(self) -> dict:
        return {
            "name_offset": f"0x{self.length_offset:X}",
            "name": self.name,
            "class_id": self.class_id,
            "root_offset": f"0x{self.root_offset:X}",
            "root": self.root,
            "root_repeat": self.root_repeat,
            "trailer": self.trailer,
            "root_repeats": self.root == self.root_repeat,
        }


@dataclass(frozen=True)
class DummyEntry:
    length_offset: int
    name: str
    class_id: int
    repeat_class_id: int

    def as_report(self) -> dict:
        return {
            "name_offset": f"0x{self.length_offset:X}",
            "name": self.name,
            "class_id": self.class_id,
            "repeat_class_id": self.repeat_class_id,
        }


def _u16(blob: bytes, offset: int) -> int | None:
    if offset < 0 or offset + 2 > len(blob):
        return None
    return struct.unpack_from("<H", blob, offset)[0]


def _text_at(blob: bytes, offset: int, limit: int = 64) -> tuple[str, int] | None:
    length = _u16(blob, offset)
    if length is None or not 1 <= length <= limit:
        return None
    raw = blob[offset + 2:offset + 2 + length]
    if len(raw) != length or not IDENTIFIER.match(raw):
        return None
    try:
        return raw.decode("ascii"), offset + 2 + length
    except UnicodeDecodeError:
        return None


def _source_name(blob: bytes) -> tuple[str, int]:
    """The header source name is a plain path (``_tmp_option.ad``), not an identifier."""
    length = _u16(blob, 8)
    if length is None or not 1 <= length <= 200:
        return "", 8
    raw = blob[10:10 + length]
    if len(raw) != length or any(byte < 0x20 or byte > 0x7E for byte in raw):
        return "", 8
    return raw.decode("ascii"), 10 + length


def scan_pages(blob: bytes, start: int = 0, end: int | None = None) -> list[PageEntry]:
    """Return the validated two-string page constructors inside ``[start, end)``.

    Validation: identifier text, u32 class id below 0x10000, the ``0x0224`` two-argument tag, two identifier
    strings of equal text, and a u32 trailer below 0x10000. Overlapping matches are dropped (a name that also
    occurs inside an earlier accepted string cannot produce a second entry).
    """
    limit = len(blob) if end is None else min(end, len(blob))
    entries: list[PageEntry] = []
    last_end = -1
    for offset in range(start, max(start, limit - 8)):
        if offset < last_end:
            continue
        parsed = _text_at(blob, offset)
        if parsed is None:
            continue
        name, after_name = parsed
        if after_name + 9 > len(blob):
            continue
        class_id = struct.unpack_from("<I", blob, after_name)[0]
        if class_id > 0xFFFF:
            continue
        if blob[after_name + 4:after_name + 9] != TWO_ARG_TAG:
            continue
        first = _text_at(blob, after_name + 9)
        if first is None:
            continue
        root, after_root = first
        second = _text_at(blob, after_root)
        if second is None:
            continue
        repeat, after_repeat = second
        if repeat != root or after_repeat + 4 > len(blob):
            continue
        trailer = struct.unpack_from("<I", blob, after_repeat)[0]
        if trailer > 0xFFFF:
            continue
        entries.append(PageEntry(offset, name, class_id, after_name + 9, root,
                                 after_root, repeat, trailer))
        last_end = after_repeat + 4
    return entries


def scan_dummies(blob: bytes, start: int, end: int | None = None) -> list[DummyEntry]:
    """Return zero-argument ``Dummy`` page constructors (name, class id, ``0x16``, next class id)."""
    limit = len(blob) if end is None else min(end, len(blob))
    found: list[DummyEntry] = []
    for offset in range(start, max(start, limit - 8)):
        parsed = _text_at(blob, offset)
        if parsed is None:
            continue
        name, after_name = parsed
        if name != "Dummy" or after_name + 9 > len(blob):
            continue
        if blob[after_name + 4] != EMPTY_ARG_TAG:
            continue
        class_id = struct.unpack_from("<I", blob, after_name)[0]
        next_id = struct.unpack_from("<I", blob, after_name + 5)[0]
        if class_id > 0xFFFF or next_id > 0xFFFF:
            continue
        found.append(DummyEntry(offset, name, class_id, next_id))
    return found


class ScriptImage:
    """A read-only model plus an in-place, equal-length text patcher for a v5/v7 script image."""

    def __init__(self, blob: bytes):
        require(len(blob) >= 0x20, "image is shorter than the header")
        magic = blob[:8]
        require(magic in (MAGIC_V5, MAGIC_V7), "not an ADCH v5/v7 image")
        self.magic = magic.rstrip(b"\x00").decode("ascii")
        self._data = bytearray(blob)
        self._sha = digest(blob)
        self.source_name, self.source_name_end = _source_name(blob)

    def __len__(self) -> int:
        return len(self._data)

    @classmethod
    def load(cls, path: Path) -> "ScriptImage":
        return cls(path.read_bytes())

    @property
    def sha256(self) -> str:
        return self._sha

    def bytes(self) -> bytes:
        return bytes(self._data)

    def pages(self, start: int = 0, end: int | None = None) -> list[PageEntry]:
        return scan_pages(bytes(self._data), start, end)

    def dummies(self, start: int = 0, end: int | None = None) -> list[DummyEntry]:
        return scan_dummies(bytes(self._data), start, end)

    def replace_text(self, length_offset_or: PageEntry, text: str) -> None:
        """Replace one length-prefixed string in place; the byte length must not change."""
        if isinstance(length_offset_or, PageEntry):
            raise TypeError("pass a string offset, not a page entry")
        offset = int(length_offset_or)
        encoded = text.encode("ascii")
        current = _u16(bytes(self._data), offset)
        require(current is not None, "string offset is outside the image")
        require(len(encoded) == current,
                f"replacement must keep the length ({len(encoded)} != {current})")
        self._data[offset + 2:offset + 2 + current] = encoded

    def rename_page(self, page: PageEntry, name: str, root: str) -> None:
        """Retarget one registered page, keeping both strings and both repeat copies equal-length."""
        require(len(name.encode("ascii")) == page.name_bytes,
                f"page name must stay {page.name_bytes} bytes")
        require(len(root.encode("ascii")) == page.root_bytes,
                f"root name must stay {page.root_bytes} bytes")
        if name != page.name:
            self.replace_text(page.length_offset, name)
        self.replace_text(page.root_offset, root)
        self.replace_text(page.root_repeat_offset, root)

    def roundtrip(self) -> bytes:
        """Emit the image; byte-identical to the input unless an edit was made."""
        return bytes(self._data)


def changed_ranges(before: bytes, after: bytes) -> list[dict]:
    require(len(before) == len(after), "edited image changed length")
    ranges: list[dict] = []
    start = None
    for index, (left, right) in enumerate(zip(before, after)):
        if left != right and start is None:
            start = index
        elif left == right and start is not None:
            ranges.append({"offset": f"0x{start:X}", "length": index - start})
            start = None
    if start is not None:
        ranges.append({"offset": f"0x{start:X}", "length": len(before) - start})
    return ranges


def summarise(image: ScriptImage, start: int, end: int) -> dict:
    pages = image.pages(start, end)
    dummies = image.dummies(start, end)
    return {
        "magic": image.magic,
        "source_name": image.source_name,
        "decoded_bytes": len(image),
        "sha256": image.sha256,
        "page_region": f"0x{start:X}..0x{end:X}",
        "pages": [page.as_report() for page in pages],
        "dummies": [dummy.as_report() for dummy in dummies],
        "page_count": len(pages),
        "dummy_count": len(dummies),
    }


def _region(value: str) -> tuple[int, int]:
    try:
        start, end = value.split(":", 1)
        return int(start, 0), int(end, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("region must be START:END in hexadecimal or decimal") from error


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Adhoc v5/v7 page-table reader and same-length patcher")
    parser.add_argument("--region", type=_region, default=V200_PAGE_REGION,
                        help="page-table byte range (default: the v2.00 option.adc region)")
    commands = parser.add_subparsers(dest="command", required=True)

    inspect = commands.add_parser("inspect", help="list the pages, roots and Dummy entries")
    inspect.add_argument("image", type=Path)

    patch = commands.add_parser("patch", help="retarget one registered page in place (equal lengths)")
    patch.add_argument("image", type=Path)
    patch.add_argument("--page", required=True, help="existing page name, for example LAN")
    patch.add_argument("--name", help="new page name, same byte length (omit to keep the name)")
    patch.add_argument("--root", required=True, help="new root window name, same byte length")
    patch.add_argument("--output", type=Path, required=True)

    roundtrip = commands.add_parser("roundtrip", help="prove that the image re-emits byte-identically")
    roundtrip.add_argument("image", type=Path)

    args = parser.parse_args(argv)
    start, end = args.region

    if args.command == "roundtrip":
        image = ScriptImage.load(args.image)
        emitted = image.roundtrip()
        identical = emitted == args.image.read_bytes()
        print(json.dumps({"image": str(args.image), "decoded_bytes": len(image),
                          "sha256": image.sha256, "byte_identical": identical}, indent=2))
        return 0 if identical else 1

    image = ScriptImage.load(args.image)
    report = summarise(image, start, end)

    if args.command == "inspect":
        print(json.dumps(report, indent=2))
        return 0

    matches = [page for page in image.pages(start, end) if page.name == args.page]
    require(len(matches) == 1, f"expected exactly one page named {args.page}, found {len(matches)}")
    page = matches[0]
    before = image.bytes()
    image.rename_page(page, args.name or page.name, args.root)
    after = image.roundtrip()
    require(len(after) == len(before), "edit changed the decoded length")
    args.output.write_bytes(after)
    print(json.dumps({
        "input": str(args.image),
        "output": str(args.output),
        "decoded_bytes": len(after),
        "sha256_before": digest(before),
        "sha256_after": digest(after),
        "changed_ranges": changed_ranges(before, after),
        "page": page.as_report(),
        "renamed_to": {"name": args.name or page.name, "root": args.root},
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
