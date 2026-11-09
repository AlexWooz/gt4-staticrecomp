"""Independent hand-packed RoFS records and bounded selected-extraction controls."""

import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import gt4_volume as volume


def raw_deflate(data, flush=zlib.Z_FINISH):
    encoder = zlib.compressobj(wbits=-15)
    return encoder.compress(data) + encoder.flush(flush)


def leaf_page(*, bad_key=False, bad_meta=False, bad_link=False):
    page = bytearray(2048)
    struct.pack_into("<HHii", page, 0, 0, 4, 1 if bad_link else -1, -1)
    # Root key parent=0/name='.'; directory metadata ID=1.
    page[12:17] = b"\0\0\0\0."
    page[17:22] = b"\0\x01\0\0\0"
    # File key parent=1/name='test'; raw metadata page=0/time=0/size=3.
    page[22:30] = b"\0\0\0\x01test"
    page[30:43] = b"\x01" + struct.pack("<III", 0, 0, 3)
    struct.pack_into("<HHHH", page, 2040, 12, 5, 17, 5)
    struct.pack_into("<HHHH", page, 2032, 2040 if bad_key else 22, 8,
                     29 if bad_meta else 30, 13)
    return bytes(page)


def catalogue_bytes(page=None, flush=zlib.Z_FINISH):
    page = page or leaf_page()
    stored_page = raw_deflate(bytes(byte ^ 0x55 for byte in page), flush)
    stored = bytearray(72)
    struct.pack_into("<IIIIHH", stored, 0, 0xACB990AD, 0x00030001,
                     72 + len(stored_page), 1, 2048, 1)
    struct.pack_into("<II", stored, 64, 72 ^ 0x14AC327A,
                     (72 + len(stored_page)) ^ ((2 * 0x14AC327A) & 0xFFFFFFFF))
    return bytes(stored) + stored_page


class VolumeTests(unittest.TestCase):
    def test_hand_packed_final_and_flush_catalogues(self):
        for flush in (zlib.Z_FINISH, zlib.Z_SYNC_FLUSH):
            catalogue = volume.parse_catalogue(catalogue_bytes(flush=flush), volume.TOC_OFFSET + 4096)
            self.assertEqual((catalogue.pages, catalogue.directories, catalogue.records), (1, 1, 2))
            self.assertEqual(catalogue.files["test"].size, 3)
            self.assertEqual(catalogue.files["test"].offset, volume.TOC_OFFSET + 2048)

    def test_bad_headers(self):
        for offset, value in ((0, 0), (4, 0), (8, 0), (12, 0), (16, 0), (18, 65535)):
            with self.subTest(offset=offset):
                data = bytearray(catalogue_bytes())
                struct.pack_into("<H" if offset >= 16 else "<I", data, offset, value)
                with self.assertRaises(ValueError):
                    volume.parse_catalogue(bytes(data), volume.TOC_OFFSET + 4096)

    def test_bad_offset_table(self):
        data = bytearray(catalogue_bytes())
        data[64] ^= 1
        with self.assertRaises(ValueError):
            volume.parse_catalogue(bytes(data), volume.TOC_OFFSET + 4096)

    def test_bad_record_bounds_and_links(self):
        for argument in ("bad_key", "bad_meta", "bad_link"):
            with self.subTest(argument=argument), self.assertRaises(ValueError):
                volume.parse_catalogue(catalogue_bytes(leaf_page(**{argument: True})),
                                       volume.TOC_OFFSET + 4096)

    def test_page_kind_and_count(self):
        for kind, count in ((2, 4), (0, 3), (1, 4), (0, 65534)):
            page = bytearray(leaf_page())
            struct.pack_into("<HH", page, 0, kind, count)
            with self.subTest(kind=kind, count=count), self.assertRaises(ValueError):
                volume.parse_catalogue(catalogue_bytes(bytes(page)), volume.TOC_OFFSET + 4096)

    def test_spec2_odd_page_count_relaxes_only_the_parity(self):
        # Spec II's RoFS 3.1 leaf/index pages carry an odd entry count (docs/SPEC2-PLAN.md, S7b).
        page = bytearray(leaf_page())
        struct.pack_into("<H", page, 2, 5)
        data = catalogue_bytes(bytes(page))
        with self.assertRaisesRegex(ValueError, "parity"):
            volume.parse_catalogue(data, volume.TOC_OFFSET + 4096)
        catalogue = volume.parse_catalogue(data, volume.TOC_OFFSET + 4096, count_parity=1)
        self.assertEqual(catalogue.files["test"].size, 3)

    def test_file_extent_beyond_volume(self):
        with self.assertRaises(ValueError):
            volume.parse_catalogue(catalogue_bytes(), volume.TOC_OFFSET + 2048)

    def test_unknown_node_kind_and_length(self):
        for offset, value in ((30, 3), (2039, 1)):
            page = bytearray(leaf_page())
            page[offset] = value
            with self.assertRaises(ValueError):
                volume.parse_catalogue(catalogue_bytes(bytes(page)), volume.TOC_OFFSET + 4096)

    def test_deflate_rejects_truncation_trailing_oversized_wrong_order(self):
        stored = catalogue_bytes()
        encoded = stored[72:]
        bad = (encoded[:-1], encoded + b"x", raw_deflate(b"x" * 2049),
               bytes(byte ^ 0x55 for byte in encoded))
        for data in bad:
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                volume.inflate_exact(data, 2048, 2048, page=True)

    def test_wrapper_and_raw_file(self):
        wrapped = struct.pack("<II", volume.PS2ZIP_MAGIC, (-3) & 0xFFFFFFFF) + raw_deflate(b"abc")
        self.assertEqual(volume.decode_payload(wrapped, 2, 3), b"abc")
        self.assertEqual(volume.decode_payload(b"abc", 1, 3), b"abc")

    def test_wrapper_failures(self):
        wrapped = struct.pack("<II", volume.PS2ZIP_MAGIC, (-3) & 0xFFFFFFFF) + raw_deflate(b"abc")
        cases = (b"", wrapped[:7], b"xxxx" + wrapped[4:], wrapped[:-1], wrapped + b"x",
                 wrapped[:4] + b"\0" * 4 + wrapped[8:],
                 wrapped[:8] + raw_deflate(b"abcd"),
                 wrapped[:8] + raw_deflate(b"abc", zlib.Z_SYNC_FLUSH))
        for data in cases:
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                volume.decode_payload(data, 2, 3)
        with self.assertRaises(ValueError):
            volume.decode_payload(b"abc", 3, 3)
        with self.assertRaisesRegex(ValueError, "not qualified"):
            volume.decode_payload(wrapped, 1, len(wrapped))

    def test_caps(self):
        with self.assertRaises(ValueError):
            volume.inflate_exact(b"", volume.FILE_CAP + 1, volume.FILE_CAP)
        catalogue = volume.Catalogue({"large": volume.Record(1, "large", 1,
                                     size=volume.FILE_CAP + 1)}, 1, 1, 1, "", "")
        with self.assertRaises(ValueError):
            volume.selected_records(catalogue, ["large"])
        with self.assertRaises(ValueError):
            volume.parse_catalogue(b"x" * (volume.CATALOGUE_CAP + 1), volume.VOLUME_SIZE)

    def test_aggregate_input_and_output_caps(self):
        for stored, size in ((volume.FILE_CAP, 0), (0, volume.FILE_CAP)):
            files = {str(index): volume.Record(1, str(index), 1, stored_size=stored, size=size)
                     for index in range(3)}
            catalogue = volume.Catalogue(files, 1, 1, 3, "", "")
            with self.assertRaisesRegex(ValueError, "total selection cap"):
                volume.selected_records(catalogue, list(files))

    def test_negative_lengths(self):
        for stored, size in ((-1, 0), (0, -1)):
            catalogue = volume.Catalogue({"a": volume.Record(1, "a", 1, stored_size=stored,
                                         size=size)}, 1, 1, 1, "", "")
            with self.assertRaises(ValueError):
                volume.selected_records(catalogue, ["a"])

    def test_catalogue_hash_pin_rejected(self):
        header = catalogue_bytes()
        with patch.object(volume, "VOLUME_OFFSET", 0), patch.object(volume, "TOC_OFFSET", 0):
            with self.assertRaisesRegex(ValueError, "reviewed slice119"):
                volume.read_catalogue(io.BytesIO(header))

    def test_directory_graph_controls(self):
        root = volume.Record(0, ".", 0, 1)
        failures = ([root, root], [root, volume.Record(9, "lost", 1)],
                    [root, volume.Record(3, "a", 0, 2), volume.Record(2, "b", 0, 3)],
                    [root, volume.Record(1, "a", 0, 1)],
                    [root, volume.Record(1, "same", 1), volume.Record(1, "same", 1)],
                    [root, volume.Record(0, "extra", 1)])
        for records in failures:
            with self.subTest(records=records), self.assertRaises(ValueError):
                volume.resolve_records(records)

    def test_unsafe_names(self):
        for name in ("", ".", "..", "../x", "x\\y", "C:x", "x\0y"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                volume.check_name(name)

    def test_selection_presence_uniqueness_and_count(self):
        catalogue = volume.parse_catalogue(catalogue_bytes(), volume.TOC_OFFSET + 4096)
        for names in ([], ["test", "test"], ["absent"], [str(i) for i in range(9)]):
            with self.assertRaises(ValueError):
                volume.selected_records(catalogue, names)

    def test_short_input_read(self):
        with self.assertRaises(ValueError):
            volume.read_at(io.BytesIO(b"a"), 0, 2, 2)

    def test_private_output_traversal_and_existing(self):
        with tempfile.TemporaryDirectory() as temporary:
            private = Path(temporary) / "private"
            private.mkdir()
            for output in (private, private / ".." / "outside", Path(temporary) / "outside"):
                with self.assertRaises(ValueError):
                    volume.fresh_output_directory(output, private)
            capsule = volume.fresh_output_directory(private / "capsule", private)
            with self.assertRaises(ValueError):
                volume.fresh_output_directory(capsule, private)

    def test_output_links_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            private = Path(temporary) / "private"
            private.mkdir()
            with patch.object(Path, "is_junction", return_value=True):
                with self.assertRaises(ValueError):
                    volume.fresh_output_directory(private / "capsule", private)
            with patch.object(Path, "is_symlink", return_value=True):
                with self.assertRaises(ValueError):
                    volume.fresh_output_directory(private / "capsule", private)

    def test_selected_extraction_and_preflight_failure(self):
        catalogue = volume.Catalogue({"dir/name": volume.Record(1, "name", 1,
                                     offset=0, stored_size=3, size=3)}, 1, 1, 1, "stored", "decoded")
        with tempfile.TemporaryDirectory() as temporary, patch.object(volume, "VOLUME_OFFSET", 0):
            private = Path(temporary) / "private"
            private.mkdir()
            output = private / "capsule"
            report = volume.extract_selected(io.BytesIO(b"abc"), catalogue, ["dir/name"], output, private)
            self.assertEqual((output / "asset-00.bin").read_bytes(), b"abc")
            self.assertEqual(json.loads((output / "manifest.json").read_text()), report)
            self.assertEqual(report["files"][0]["decoded_sha256"],
                             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
            with self.assertRaises(ValueError):
                volume.extract_selected(io.BytesIO(b"a"), catalogue, ["dir/name"], private / "bad", private)
            self.assertFalse((private / "bad").exists())

    def test_iso_pin_rejected_before_parser(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(volume.pycdlib, "PyCdlib") as parser:
            path = Path(temporary) / "wrong.iso"
            path.write_bytes(b"wrong")
            with self.assertRaisesRegex(ValueError, "pinned disc"):
                with volume.verified_volume(path):
                    self.fail("unverified input was accepted")
            parser.assert_not_called()


if __name__ == "__main__":
    unittest.main()
