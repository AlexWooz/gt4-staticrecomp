"""TASK-214: the GT4.VOL (RoFS) catalogue writer (scripts/gt4_vol_toc.py).

Synthetic volumes only. The fixture builds two leaf pages, one directory and two files, and is validated with the
reviewed reader in ``scripts/gt4_volume.py`` before and after a record extent change, so the writer is checked
against an independent parser rather than against itself.
"""

from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import gt4_vol_toc as toc  # noqa: E402
import gt4_volume  # noqa: E402

PAGE = toc.PAGE_BYTES


def directory_meta(node_id: int) -> bytes:
    return b"\x00" + struct.pack("<I", node_id)


def file_meta(data_page: int, modified: int, packed: int, size: int | None = None) -> bytes:
    if size is None:
        return b"\x01" + struct.pack("<III", data_page, modified, packed)
    return b"\x02" + struct.pack("<IIII", data_page, modified, packed, size)


def leaf_page(records: list[tuple[int, str, bytes]]) -> bytes:
    """Build one leaf page from (parent id, name, metadata) records."""
    page = bytearray(PAGE)
    count = len(records) * 2
    struct.pack_into("<HHii", page, 0, 0, count, -1, -1)
    table_start = PAGE - len(records) * 8 - 4
    descriptors = []
    cursor = 12
    for parent, name, meta in records:
        key = struct.pack(">I", parent) + name.encode("utf-8")
        key_offset = cursor
        page[key_offset:key_offset + len(key)] = key
        cursor += len(key)
        descriptors.append([key_offset, len(key), None, len(meta)])
    for index, (_parent, _name, meta) in enumerate(records):
        meta_offset = cursor
        page[meta_offset:meta_offset + len(meta)] = meta
        cursor += len(meta)
        descriptors[index][2] = meta_offset
    assert cursor <= table_start, "fixture page overflow"
    for index, (key_offset, key_size, meta_offset, meta_size) in enumerate(descriptors):
        struct.pack_into("<HHHH", page, PAGE - (index + 1) * 8, key_offset, key_size,
                         meta_offset, meta_size)
    return bytes(page)


def build_volume(pages: list[bytes], physical_pages: int) -> bytes:
    chunks = [toc.encode_page(page) for page in pages]
    table_end = 64 + 4 * (len(pages) + 1)
    stored_size = table_end + sum(len(chunk) for chunk in chunks)
    blob = bytearray(stored_size)
    toc.HEADER.pack_into(blob, 0, toc.MAGIC, toc.VERSION, stored_size, physical_pages,
                         PAGE, len(pages))
    offset = table_end
    for index, chunk in enumerate(chunks):
        struct.pack_into("<I", blob, 64 + index * 4, offset ^ (((index + 1) * toc.OFFSET_SALT) & 0xFFFFFFFF))
        blob[offset:offset + len(chunk)] = chunk
        offset += len(chunk)
    struct.pack_into("<I", blob, 64 + len(pages) * 4,
                     offset ^ (((len(pages) + 1) * toc.OFFSET_SALT) & 0xFFFFFFFF))
    return bytes(blob)


def fixture() -> tuple[bytes, int]:
    """Two leaf pages: root plus 'projects'; then option.adc and option.mproject under projects."""
    root = leaf_page([(0, ".", directory_meta(1)),
                      (1, "projects", directory_meta(2))])
    files = leaf_page([(2, "option.adc", file_meta(0, 0x1111, 4000, 9000)),
                       (2, "option.mproject", file_meta(2, 0x2222, 1000))])
    physical_pages = 5
    stored = build_volume([root, files], physical_pages)
    volume = stored + bytes(physical_pages * PAGE - len(stored)) + bytes(6 * PAGE)
    return volume, physical_pages


class TocRoundTripTest(unittest.TestCase):
    def setUp(self):
        self.volume, self.physical_pages = fixture()
        self.stored = self.volume[:self.physical_pages * PAGE].rstrip(b"\x00")
        size = toc.HEADER.unpack_from(self.volume)[2]
        self.stored = self.volume[:size]
        self.image = toc.TocImage(self.stored)

    def test_header_and_records(self):
        self.assertEqual(self.image.logical_pages, 2)
        self.assertEqual(self.image.physical_pages, self.physical_pages)
        self.assertEqual(len(self.image.entries()), 4)
        self.assertEqual([entry.name for entry in self.image.files()],
                         ["option.adc", "option.mproject"])
        self.assertEqual(self.image.reserved_bytes, self.physical_pages * PAGE)

    def test_roundtrip_is_byte_identical(self):
        emitted = self.image.to_bytes()
        self.assertEqual(emitted, self.stored)
        self.assertEqual(toc.TocImage(emitted).original_sha256, toc.TocImage(self.stored).original_sha256)

    def test_reader_accepts_the_fixture(self):
        catalogue = gt4_volume.parse_catalogue(self.stored, len(self.volume), toc_offset=0)
        self.assertEqual(catalogue.records, 4)
        self.assertEqual(catalogue.files["projects/option.adc"].offset,
                         self.physical_pages * PAGE)
        self.assertEqual(catalogue.files["projects/option.adc"].stored_size, 4000)
        self.assertEqual(catalogue.files["projects/option.adc"].size, 9000)

    def test_moved_extent_is_written_and_reparsed(self):
        entry = self.image.find("option.adc")[0]
        self.image.set_file_extent(entry, data_page=4, packed=5000, size=12000)
        emitted = self.image.to_bytes()
        reparsed = toc.TocImage(emitted)
        updated = reparsed.find("option.adc")[0]
        self.assertEqual((updated.data_page, updated.packed, updated.size), (4, 5000, 12000))
        self.assertEqual(reparsed.find("option.mproject")[0].packed, 1000)
        self.assertEqual(len(reparsed.entries()), 4)
        catalogue = gt4_volume.parse_catalogue(emitted, len(self.volume) + 2 * PAGE, toc_offset=0)
        self.assertEqual(catalogue.files["projects/option.adc"].offset,
                         self.physical_pages * PAGE + 4 * PAGE)
        self.assertEqual(catalogue.files["projects/option.adc"].stored_size, 5000)
        self.assertEqual(catalogue.files["projects/option.adc"].size, 12000)

    def test_directory_record_is_refused(self):
        directory = [entry for entry in self.image.entries() if entry.node_kind == 0][1]
        with self.assertRaises(toc.TocError):
            self.image.set_file_extent(directory, data_page=1, packed=10, size=10)

    def test_kind_one_uses_the_packed_length_as_size(self):
        entry = self.image.find("option.mproject")[0]
        self.assertEqual(entry.node_kind, 1)
        self.image.set_file_extent(entry, data_page=3, packed=777)
        updated = toc.TocImage(self.image.to_bytes()).find("option.mproject")[0]
        self.assertEqual((updated.packed, updated.size), (777, 777))

    def test_slack_report(self):
        entry = self.image.find("option.adc")[0]
        report = self.image.allocation_report(entry)
        self.assertEqual(report["pages_used"], 2)
        self.assertEqual(report["next_file_page"], 2)
        self.assertEqual(report["slack_bytes_to_next_file"], 2 * PAGE - 4000)
        self.assertEqual(report["data_offset_in_volume"], self.physical_pages * PAGE)

    def test_catalogue_cannot_grow_past_its_reserved_pages(self):
        image = toc.TocImage(self.stored)
        image.physical_pages = (image.stored_bytes - 1) // PAGE
        with self.assertRaises(toc.TocError):
            image.to_bytes()


class TocRejectionTest(unittest.TestCase):
    def test_bad_magic_is_refused(self):
        with self.assertRaises(toc.TocError):
            toc.TocImage(b"\x00" * 128)

    def test_truncated_page_is_refused(self):
        volume, physical_pages = fixture()
        stored = bytearray(volume[:toc.HEADER.unpack_from(volume)[2]])
        stored[-4:] = b"\x00\x00\x00\x00"
        with self.assertRaises(toc.TocError):
            toc.TocImage(bytes(stored))


if __name__ == "__main__":
    unittest.main()
