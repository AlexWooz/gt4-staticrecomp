"""HWR M1 replay driver and comparison metrics (TASK-027 M1, chapter 14.4 gates).

Synthetic images with known PSNR and pct_bad, a deterministic replay check, and
(when the owner's headless frame dumps exist) identical-pair and perturbed-pair
checks on real donor frames. The real-frame directory is taken from
GT4_HW_FRAME_DIR, else <repo>/private/slice157-smoke/headless-157/frames; the
test skips when it is absent.
"""

import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(REPO_ROOT / "tools" / "hw_replay"))
import hw_capture  # noqa: E402
import hw_compare as compare  # noqa: E402
import hw_replay as replay  # noqa: E402

WIDTH = 10
HEIGHT = 10
PIXELS = WIDTH * HEIGHT


def image(rgb_for_pixel) -> compare.PpmImage:
    raster = bytearray()
    for index in range(PIXELS):
        raster.extend(rgb_for_pixel(index))
    return compare.PpmImage(WIDTH, HEIGHT, bytes(raster))


def write_ppm(path: Path, picture: compare.PpmImage) -> None:
    path.write_bytes(f"P6\n{picture.width} {picture.height}\n255\n".encode("ascii") + picture.rgb)


class FrameMetricTests(unittest.TestCase):
    def test_identical_frames_have_zero_delta_and_infinite_psnr(self):
        base = image(lambda index: (index, 2 * index % 256, 7))
        metrics = compare.frame_metrics(base, base)
        self.assertEqual((metrics.pct_bad, metrics.max_delta), (0.0, 0))
        self.assertTrue(math.isinf(metrics.psnr))
        self.assertTrue(compare.GATES["calibration"].passes(metrics))

    def test_uniform_delta_of_one_gives_known_psnr(self):
        base = image(lambda index: (100, 100, 100))
        shifted = image(lambda index: (101, 101, 101))
        metrics = compare.frame_metrics(base, shifted)
        self.assertEqual(metrics.max_delta, 1)
        self.assertEqual(metrics.pct_bad, 0.0)
        self.assertAlmostEqual(metrics.psnr, 10 * math.log10(255 ** 2 / 1), places=9)

    def test_pct_bad_counts_a_pixel_once_when_any_channel_exceeds_eight(self):
        base = image(lambda index: (0, 0, 0))

        def perturbed(index):
            if index < 3:
                return (0, 0, 9)  # exceeds 8 in one channel: bad
            if index < 6:
                return (8, 8, 8)  # exactly 8 is not bad
            return (0, 0, 0)

        metrics = compare.frame_metrics(base, image(perturbed))
        self.assertEqual(metrics.pct_bad, 3 * 100.0 / PIXELS)
        self.assertEqual(metrics.max_delta, 9)

    def test_size_mismatch_is_an_error(self):
        small = compare.PpmImage(2, 2, bytes(12))
        with self.assertRaisesRegex(compare.PpmError, "size mismatch"):
            compare.frame_metrics(small, image(lambda index: (0, 0, 0)))


class PpmReadTests(unittest.TestCase):
    def test_round_trip_with_comments_in_header(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "a.ppm"
            path.write_bytes(b"P6\n# note\n2 1\n255\n" + bytes([1, 2, 3, 4, 5, 6]))
            picture = compare.read_ppm(path)
            self.assertEqual((picture.width, picture.height, picture.rgb), (2, 1, bytes([1, 2, 3, 4, 5, 6])))

    def test_wrong_raster_length_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.ppm"
            path.write_bytes(b"P6\n2 1\n255\n" + bytes(5))
            with self.assertRaisesRegex(compare.PpmError, "raster has 5 bytes"):
                compare.read_ppm(path)


class GateAndSceneTests(unittest.TestCase):
    def test_gate_bounds_follow_chapter_14_4(self):
        self.assertEqual(compare.GATES["cache_vs_writeback_1x"].max_pct_bad, 0.01)
        self.assertEqual(compare.GATES["donor_2d_1x"].min_psnr, 40.0)
        self.assertEqual(compare.GATES["donor_3d_1x"].max_pct_bad, 3.0)
        self.assertEqual(compare.GATES["donor_3d_1x"].gating_from, "M10")
        self.assertEqual(compare.GATES["downsample_menu"].min_psnr, 35.0)

    def test_scene_fails_when_one_frame_is_over_a_gating_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "ref").mkdir()
            (root / "cand").mkdir()
            base = image(lambda index: (50, 50, 50))
            write_ppm(root / "ref" / "frame_00001.ppm", base)
            write_ppm(root / "cand" / "frame_00001.ppm", base)
            write_ppm(root / "ref" / "frame_00002.ppm", base)
            write_ppm(root / "cand" / "frame_00002.ppm", image(lambda index: (80, 50, 50)))
            report = compare.compare_directories(root / "ref", root / "cand", compare.GATES["calibration"])
            self.assertEqual(report.frames_over_gate, 1)
            self.assertEqual(report.verdict, "fail")

    def test_report_only_gate_does_not_fail_the_scene(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "ref").mkdir()
            (root / "cand").mkdir()
            write_ppm(root / "ref" / "frame_00001.ppm", image(lambda index: (0, 0, 0)))
            write_ppm(root / "cand" / "frame_00001.ppm", image(lambda index: (255, 255, 255)))
            report = compare.compare_directories(root / "ref", root / "cand", compare.GATES["donor_3d_1x"])
            self.assertTrue(report.report_only)
            self.assertEqual(report.verdict, "report-only")
            self.assertEqual(report.frames_over_gate, 1)

    def test_files_present_in_one_directory_are_listed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "ref").mkdir()
            (root / "cand").mkdir()
            write_ppm(root / "ref" / "frame_00001.ppm", image(lambda index: (0, 0, 0)))
            report = compare.compare_directories(root / "ref", root / "cand", compare.GATES["calibration"])
            self.assertEqual(report.missing, ["frame_00001.ppm"])
            self.assertEqual(report.verdict, "fail")


class ReplayDriverTests(unittest.TestCase):
    def _capture(self) -> bytes:
        def gif(path, payload):
            return b"G" + struct.pack("<BI", path, len(payload)) + payload

        return (gif(0, b"abc") + b"P" + struct.pack("<IQ", 0x12001000, 5) + b"R" + struct.pack("<I", 2)
                + b"V" + struct.pack("<Q", 100) + gif(1, b"de") + b"V" + struct.pack("<Q", 200))

    def test_two_replays_give_identical_bytes(self):
        first = replay.replay(self._capture(), replay.RecordingBackend())
        second = replay.replay(self._capture(), replay.RecordingBackend())
        self.assertEqual(first.frame_sha256, second.frame_sha256)
        self.assertEqual(first.frames, [100, 200])

    def test_replay_writes_named_ppm_frames_that_compare_as_identical(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            replay.replay(self._capture(), replay.RecordingBackend(), root / "a")
            replay.replay(self._capture(), replay.RecordingBackend(), root / "b")
            self.assertEqual(sorted(p.name for p in (root / "a").glob("*.ppm")),
                             ["frame_00100.ppm", "frame_00200.ppm"])
            report = compare.compare_directories(root / "a", root / "b", compare.GATES["calibration"])
            self.assertEqual(report.frames_over_gate, 0)
            self.assertEqual(report.verdict, "pass")

    def test_fifo_read_is_performed_and_counted(self):
        backend = replay.RecordingBackend()
        result = replay.replay(self._capture(), backend)
        self.assertEqual(result.fifo_reads, 1)
        self.assertIn(("R", 2), backend.log)

    def test_donor_backend_refuses_to_construct_until_its_adapter_exists(self):
        with self.assertRaisesRegex(NotImplementedError, "donor GS replay adapter"):
            replay.DonorBackendUnavailable()

    def test_capture_stream_is_validated_before_any_call(self):
        backend = replay.RecordingBackend()
        with self.assertRaises(hw_capture.CaptureError):
            replay.replay(b"Q", backend)
        self.assertEqual(backend.log, [])


class RealFrameTests(unittest.TestCase):
    """Identical pair and perturbed pair on real donor headless dumps (skipped when absent)."""

    @classmethod
    def setUpClass(cls):
        configured = os.environ.get("GT4_HW_FRAME_DIR")
        cls.frames = Path(configured) if configured else (
            REPO_ROOT / "private" / "slice157-smoke" / "headless-157" / "frames")
        cls.sample = cls.frames / "frame_00100.ppm"

    def setUp(self):
        if not self.sample.is_file():
            self.skipTest(f"no donor headless frames at {self.frames}")

    def test_identical_pair_passes_calibration(self):
        report = compare.compare_directories(self.frames, self.frames, compare.GATES["calibration"])
        self.assertGreater(len(report.rows), 0)
        self.assertEqual(report.verdict, "pass")

    def test_perturbed_pair_reports_the_exact_pct_bad(self):
        original = compare.read_ppm(self.sample)
        pixels = original.width * original.height
        bad_pixels = pixels // 100  # exactly 1% of pixels move by 9 (bad); the rest move by 8 (not bad)
        raster = bytearray(original.rgb)
        for index in range(pixels):
            step = 9 if index < bad_pixels else 8
            channel = index * 3
            raster[channel] = min(255, raster[channel] + step) if raster[channel] + step <= 255 \
                else raster[channel] - step
        perturbed = compare.PpmImage(original.width, original.height, bytes(raster))
        metrics = compare.frame_metrics(original, perturbed)
        self.assertAlmostEqual(metrics.pct_bad, 100.0 * bad_pixels / pixels, places=9)
        self.assertLessEqual(metrics.max_delta, 9)
        with tempfile.TemporaryDirectory() as directory:
            candidate = Path(directory)
            write_ppm(candidate / "frame_00100.ppm", perturbed)
            report = compare.compare_directories(self.frames, candidate,
                                                 compare.GATES["calibration"])
            self.assertEqual(report.rows[0].name, "frame_00100.ppm")
            self.assertEqual(report.frames_over_gate, 1)
            self.assertEqual(report.verdict, "fail")


if __name__ == "__main__":
    unittest.main()
