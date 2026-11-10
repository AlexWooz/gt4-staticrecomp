"""TASK-214: the Adhoc v5 page-table reader and same-length patcher (scripts/adhoc_v5.py).

Synthetic images only: the builder below reproduces the exact constructor layout observed in the retail v2.00
``option.adc`` (page name, class id, ``0x24 0x02`` two-argument tag, root window name twice, trailer), so the tests
are independent of game data. The retail file itself is optional and gated by ``GT4_OPTION_ADC``.
"""

import os
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import adhoc_v5  # noqa: E402


def text(value: str) -> bytes:
    encoded = value.encode("ascii")
    return struct.pack("<H", len(encoded)) + encoded


def page_entry(name: str, class_id: int, root: str, trailer: int, repeat: str | None = None) -> bytes:
    repeated = root if repeat is None else repeat
    return (text(name) + struct.pack("<I", class_id) + adhoc_v5.TWO_ARG_TAG
            + text(root) + text(repeated) + struct.pack("<I", trailer))


def dummy_entry(name: str, class_id: int, next_id: int) -> bytes:
    return text(name) + struct.pack("<I", class_id) + bytes([adhoc_v5.EMPTY_ARG_TAG]) + struct.pack("<I", next_id)


def image(body: bytes) -> bytes:
    return b"ADCH005\x00" + text("_tmp_option.ad") + b"\x00" * 8 + body


def sample() -> tuple[bytes, int, int]:
    prefix = text("Group") + struct.pack("<I", 1) + b"\x16" + text("GroupGeneral") + b"\x00" * 4
    body = (prefix
            + page_entry("Race", 166, "RaceRoot", 98)
            + page_entry("LAN", 175, "LANPopup", 105)
            + dummy_entry("Dummy", 181, 182)
            + page_entry("Misc", 200, "MiscRoot", 120))
    start = 0x10
    return image(body), start, len(image(body))


class ScriptImageTest(unittest.TestCase):
    def setUp(self):
        self.blob, self.start, self.end = sample()
        self.image = adhoc_v5.ScriptImage(self.blob)

    def test_source_name_and_magic(self):
        self.assertEqual(self.image.magic, "ADCH005")
        self.assertEqual(self.image.source_name, "_tmp_option.ad")

    def test_scan_finds_two_pages_and_the_dummy(self):
        pages = self.image.pages(self.start, self.end)
        self.assertEqual([page.name for page in pages], ["Race", "LAN", "Misc"])
        self.assertEqual(pages[1].root, "LANPopup")
        self.assertEqual(pages[1].root_repeat, "LANPopup")
        self.assertEqual(pages[1].class_id, 175)
        self.assertEqual(pages[1].trailer, 105)
        dummies = self.image.dummies(self.start, self.end)
        self.assertEqual([(d.name, d.class_id, d.repeat_class_id) for d in dummies],
                         [("Dummy", 181, 182)])

    def test_roundtrip_is_byte_identical(self):
        self.assertEqual(self.image.roundtrip(), self.blob)
        self.assertEqual(adhoc_v5.digest(self.image.roundtrip()), adhoc_v5.digest(self.blob))

    def test_root_only_retarget_changes_exactly_two_strings(self):
        page = [entry for entry in self.image.pages(self.start, self.end) if entry.name == "LAN"][0]
        before = self.image.bytes()
        self.image.rename_page(page, "LAN", "RecRoot1")
        after = self.image.roundtrip()
        self.assertEqual(len(after), len(before))
        ranges = adhoc_v5.changed_ranges(before, after)
        changed = sum(item["length"] for item in ranges)
        self.assertLessEqual(changed, 2 * len("LANPopup"))
        self.assertTrue(all(item["offset"].startswith("0x") for item in ranges))
        reparsed = adhoc_v5.ScriptImage(after)
        updated = [entry for entry in reparsed.pages(self.start, self.end) if entry.name == "LAN"][0]
        self.assertEqual(updated.root, "RecRoot1")
        self.assertEqual(updated.root_repeat, "RecRoot1")
        self.assertEqual(updated.name_bytes, 3)
        self.assertEqual(updated.class_id, 175)
        self.assertEqual(after[updated.root_offset + 2:updated.root_offset + 10], b"RecRoot1")
        # Nothing outside the two root strings changed.
        for index, (left, right) in enumerate(zip(before, after)):
            if left != right:
                self.assertTrue(page.root_offset <= index < page.root_offset + 2 + len("LANPopup")
                                or page.root_repeat_offset <= index
                                < page.root_repeat_offset + 2 + len("LANPopup"))

    def test_name_and_root_retarget_keeps_length(self):
        page = [entry for entry in self.image.pages(self.start, self.end) if entry.name == "Misc"][0]
        before = self.image.bytes()
        self.image.rename_page(page, "Cert", "RecRoot1")
        after = self.image.roundtrip()
        self.assertEqual(len(after), len(before))
        names = [entry.name for entry in adhoc_v5.ScriptImage(after).pages(self.start, self.end)]
        self.assertIn("Cert", names)
        self.assertNotIn("Misc", names)

    def test_length_change_is_refused(self):
        page = [entry for entry in self.image.pages(self.start, self.end) if entry.name == "Race"][0]
        with self.assertRaises(adhoc_v5.ScriptFormatError):
            self.image.rename_page(page, "Race", "RaceRootLong")
        with self.assertRaises(adhoc_v5.ScriptFormatError):
            self.image.replace_text(page.root_offset, "Short")
        self.assertEqual(self.image.roundtrip(), self.blob)

    def test_mismatched_repeat_is_not_a_page(self):
        blob = image(page_entry("Broken", 300, "RootA", 7, repeat="RootB"))
        self.assertEqual(adhoc_v5.scan_pages(blob, 0, len(blob)), [])

    def test_bad_magic_is_refused(self):
        with self.assertRaises(adhoc_v5.ScriptFormatError):
            adhoc_v5.ScriptImage(b"NOTADHOC" + b"\x00" * 0x20)
        with self.assertRaises(adhoc_v5.ScriptFormatError):
            adhoc_v5.ScriptImage(b"ADCH005\x00" + b"\x00" * 0x10)

    def test_region_scoping_excludes_pages_outside(self):
        pages = self.image.pages(self.start, self.start + 8)
        self.assertEqual(pages, [])


class RetailImageTest(unittest.TestCase):
    """Optional: the same checks against a private decoded option.adc (never committed)."""

    def setUp(self):
        path = os.environ.get("GT4_OPTION_ADC")
        if not path or not Path(path).is_file():
            self.skipTest("GT4_OPTION_ADC is not set to a decoded option.adc")

    def test_retail_pages_round_trip(self):
        path = Path(os.environ["GT4_OPTION_ADC"])
        image = adhoc_v5.ScriptImage.load(path)
        report = adhoc_v5.summarise(image, *adhoc_v5.V200_PAGE_REGION)
        self.assertEqual(report["page_count"], 25)  # the "options" list container plus 24 pages
        self.assertEqual(report["dummy_count"], 4)
        self.assertTrue(all(page["root_repeats"] for page in report["pages"]))
        self.assertEqual(image.roundtrip(), path.read_bytes())


class AddPageTest(unittest.TestCase):
    def _image(self) -> bytes:
        body = page_entry("Race", 166, "RaceRoot", 98)
        blob = image(body)
        # Module initializer terminator: u32 line, SET_STATE_OLD(0x1B), EXIT(0).
        return blob + struct.pack("<I", 4189) + bytes([adhoc_v5.OP_SET_STATE_OLD, 0])

    def test_inserts_before_the_module_exit_and_bumps_both_counts(self):
        blob = self._image()
        before_instructions = struct.unpack_from("<I", blob, adhoc_v5.INSTRUCTION_COUNT_OFFSET)[0]
        before_statics = struct.unpack_from("<I", blob, adhoc_v5.STATIC_COUNT_OFFSET)[0]
        after = adhoc_v5.add_page(blob, name="Recomp", root="RecRoot1", static_index=272, line=204)
        block = adhoc_v5.page_block("Recomp", "RecRoot1", 272, 204)
        self.assertEqual(len(after), len(blob) + len(block))
        self.assertEqual(after[-6:], blob[-6:])
        self.assertEqual(after[len(blob) - 6:len(blob) - 6 + len(block)], block)
        self.assertEqual(struct.unpack_from("<I", after, adhoc_v5.INSTRUCTION_COUNT_OFFSET)[0],
                         before_instructions + 9)
        self.assertEqual(struct.unpack_from("<I", after, adhoc_v5.STATIC_COUNT_OFFSET)[0], 273)
        self.assertGreaterEqual(struct.unpack_from("<I", after, adhoc_v5.STATIC_COUNT_OFFSET)[0],
                                before_statics)
        pages = adhoc_v5.scan_pages(after, 0, len(after))
        self.assertIn("Recomp", [page.name for page in pages])
        new_page = [page for page in pages if page.name == "Recomp"][0]
        self.assertEqual(new_page.root, "RecRoot1")
        self.assertEqual(new_page.trailer, 272)

    def test_refuses_an_image_without_the_module_exit(self):
        blob = image(page_entry("Race", 166, "RaceRoot", 98))
        with self.assertRaises(adhoc_v5.ScriptFormatError):
            adhoc_v5.add_page(blob, name="Recomp", root="RecRoot1", static_index=272, line=204)

    def test_module_path_and_capacity_are_emitted(self):
        blob = self._image() + struct.pack("<I", 33)
        # Insert after the Race page and raise a trailing capacity field from 33 to 34.
        after = adhoc_v5.add_page(blob, name="Recomp settings", root="RecRoot1", static_index=272, line=204,
                                  after_page="Race", module=True, path="JP/Misc",
                                  capacity_offset=len(blob) - 4, capacity=34)
        block = adhoc_v5.module_block("RecRoot1", 204) + adhoc_v5.page_block(
            "Recomp settings", "RecRoot1", 272, 204, "JP/Misc")
        self.assertEqual(len(after), len(blob) + len(block))
        self.assertEqual(struct.unpack_from("<I", after, adhoc_v5.INSTRUCTION_COUNT_OFFSET)[0],
                         struct.unpack_from("<I", blob, adhoc_v5.INSTRUCTION_COUNT_OFFSET)[0] + 13)
        self.assertIn(struct.pack("<I", 34), after)
        self.assertIn(b"OptionProject::RecRoot1", after)
        pages = adhoc_v5.scan_pages(after, 0, len(after))
        self.assertIn("Recomp settings", [page.name for page in pages])

    def test_block_shape_matches_the_retail_page_encoding(self):
        block = adhoc_v5.page_block("Race", "RaceRoot", 98, 166)
        # STATIC_DEFINE (line + type + symbol) then the eight page instructions.
        define = struct.pack("<I", 166) + bytes([adhoc_v5.OP_STATIC_DEFINE]) + adhoc_v5._symbol("RaceRoot")
        self.assertTrue(block.startswith(define))
        rest = block[len(define):]
        self.assertEqual(len(rest), 110)  # the retail Race block is 0x6E bytes
        self.assertEqual(rest[:5], struct.pack("<I", 166) + bytes([adhoc_v5.OP_VARIABLE_PUSH]))
        self.assertIn(adhoc_v5._symbol("Race"), rest)


class RetailAddPageTest(unittest.TestCase):
    def setUp(self):
        path = os.environ.get("GT4_OPTION_ADC")
        if not path or not Path(path).is_file():
            self.skipTest("GT4_OPTION_ADC is not set to a decoded option.adc")

    def test_retail_page_insertion(self):
        blob = Path(os.environ["GT4_OPTION_ADC"]).read_bytes()
        after = adhoc_v5.add_page(blob, name="Recomp settings", root="RecRoot1", static_index=272,
                                  line=204, after_page="Language", module=True, path="JP/Misc",
                                  capacity_offset=0x9A0C, capacity=34)
        self.assertEqual(len(after) - len(blob), 220)
        self.assertEqual(struct.unpack_from("<I", after, adhoc_v5.INSTRUCTION_COUNT_OFFSET)[0], 3388)
        self.assertEqual(struct.unpack_from("<I", after, adhoc_v5.STATIC_COUNT_OFFSET)[0], 273)
        self.assertEqual(struct.unpack_from("<I", after, 0x9A0C + 220)[0], 34)
        pages = adhoc_v5.scan_pages(after, 0, len(after))
        names = [page.name for page in pages]
        self.assertIn("Recomp settings", names)
        self.assertNotIn("Recomp", [page.name for page in
                                    adhoc_v5.scan_pages(blob, *adhoc_v5.V200_PAGE_REGION)])
        # No existing page changed.
        original = {page.name: page for page in adhoc_v5.scan_pages(blob, *adhoc_v5.V200_PAGE_REGION)}
        for page in pages:
            if page.name in original:
                self.assertEqual(page.root, original[page.name].root)


if __name__ == "__main__":
    unittest.main()
