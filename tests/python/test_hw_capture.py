"""Synthetic GT4_PGS_CAPTURE streams for the HWR capture reader (TASK-027 M1)."""

from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import hw_capture as capture  # noqa: E402


def gif(path: int, payload: bytes) -> bytes:
    return b"G" + struct.pack("<B", path) + struct.pack("<I", len(payload)) + payload


def privileged(address: int, value: int) -> bytes:
    return b"P" + struct.pack("<IQ", address, value)


def fifo(qwords: int) -> bytes:
    return b"R" + struct.pack("<I", qwords)


def frame_end(number: int) -> bytes:
    return b"V" + struct.pack("<Q", number)


class CaptureRecordTests(unittest.TestCase):
    def test_all_four_record_kinds_parse_with_little_endian_fields(self):
        data = (gif(2, b"\x01\x02\x03") + privileged(0x12001000, 0x0102030405060708)
                + fifo(16) + frame_end(7))
        records = capture.parse_records(data)
        self.assertEqual([record.tag for record in records], ["G", "P", "R", "V"])
        self.assertEqual((records[0].path, records[0].data), (2, b"\x01\x02\x03"))
        self.assertEqual((records[1].address, records[1].value), (0x12001000, 0x0102030405060708))
        self.assertEqual(records[2].qwords, 16)
        self.assertEqual(records[3].frame, 7)
        self.assertEqual([record.offset for record in records], [0, 9, 22, 27])

    def test_empty_gif_packet_is_allowed(self):
        records = capture.parse_records(gif(0, b""))
        self.assertEqual(records[0].data, b"")

    def test_unknown_tag_names_the_offset(self):
        data = frame_end(0) + b"Z"
        with self.assertRaisesRegex(capture.CaptureError, "offset 9: unknown record tag 0x5A"):
            capture.parse_records(data)

    def test_truncated_payload_is_rejected(self):
        data = gif(1, b"abcdef")[:-2]
        with self.assertRaisesRegex(capture.CaptureError, "truncated G payload"):
            capture.parse_records(data)

    def test_truncated_header_is_rejected(self):
        with self.assertRaisesRegex(capture.CaptureError, "truncated P body"):
            capture.parse_records(b"P\x00\x10")

    def test_gif_path_above_three_is_rejected(self):
        with self.assertRaisesRegex(capture.CaptureError, "GIF path 4"):
            capture.parse_records(gif(4, b"x"))

    def test_gif_packet_above_size_limit_is_rejected_before_reading_it(self):
        header = b"G" + struct.pack("<B", 0) + struct.pack("<I", capture.MAX_GIF_BYTES + 1)
        with self.assertRaisesRegex(capture.CaptureError, "exceeds"):
            capture.parse_records(header)


class CaptureFrameTests(unittest.TestCase):
    def test_records_belong_to_the_frame_end_that_follows_them(self):
        data = gif(0, b"a") + frame_end(10) + fifo(4) + frame_end(11) + privileged(0x12000000, 1)
        frames, trailing = capture.split_frames(capture.parse_records(data))
        self.assertEqual([frame.number for frame in frames], [10, 11])
        self.assertEqual([record.tag for record in frames[0].records], ["G"])
        self.assertEqual([record.tag for record in frames[1].records], ["R"])
        self.assertEqual([record.tag for record in trailing], ["P"])

    def test_summary_counts_bytes_per_gif_path(self):
        data = gif(0, b"123") + gif(3, b"4567") + gif(0, b"8") + frame_end(1)
        summary = capture.summarize(data)
        self.assertTrue(summary.valid)
        self.assertEqual(summary.gif_bytes_per_path, [4, 0, 0, 4])
        self.assertEqual(summary.record_counts, {"G": 3, "P": 0, "R": 0, "V": 1})
        self.assertEqual(summary.frame_numbers, [1])

    def test_non_increasing_frame_numbers_are_reported(self):
        summary = capture.summarize(frame_end(5) + frame_end(5))
        self.assertFalse(summary.valid)
        self.assertIn("not strictly increasing", summary.issues[0])

    def test_capture_without_frame_end_is_reported_not_silently_accepted(self):
        summary = capture.summarize(privileged(0x12000000, 0))
        self.assertFalse(summary.valid)
        self.assertEqual(summary.open_records_after_last_frame, 1)

    def test_empty_stream_is_reported(self):
        summary = capture.summarize(b"")
        self.assertFalse(summary.valid)


if __name__ == "__main__":
    unittest.main()
