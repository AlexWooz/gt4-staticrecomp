"""External CLI checks with a synthetic legacy capture, never game data.

SPDX-License-Identifier: MIT
Copyright 2026 GT4Recomp contributors.
Written from the TASK-026 specification (docs/hw-renderer-spec), chapters 12.9 and 14.2-14.4.
Set GT4_HW_REPLAY_EXE to the exact hw_cpuref_replay executable under test.
"""

import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get("GT4_HW_REPLAY_EXE"), "set GT4_HW_REPLAY_EXE to test the native replay CLI")
class NativeReplayCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = Path(os.environ["GT4_HW_REPLAY_EXE"]).resolve()
        if not cls.exe.is_file():
            raise AssertionError(f"GT4_HW_REPLAY_EXE does not name an executable: {cls.exe}")

    @staticmethod
    def capture_bytes():
        # Frame 0: no enabled circuit => raw diagnostic refuses, scanout is BGCOLOR.
        # Frame 7: one decodable circuit but unknown PCRTC CMOD => raw valid, scanout refuses.
        capture = b"V" + struct.pack("<Q", 0)
        for address, value in (
            (0x12000000, 1),
            (0x12000010, 1 << 13),
            (0x12000070, 1 << 9),
            (0x12000080, 0),
        ):
            capture += b"P" + struct.pack("<IQ", address, value)
        return capture + b"V" + struct.pack("<Q", 7)

    def run_cli(self, *arguments):
        return subprocess.run([str(self.exe), *map(str, arguments)], capture_output=True, text=True, timeout=30)

    def test_default_vram_explicit_vram_and_scanout_are_distinct(self):
        with tempfile.TemporaryDirectory(prefix="hw-replay-cli-") as temporary:
            root = Path(temporary)
            capture = root / "synthetic.gscap"
            capture.write_bytes(self.capture_bytes())
            directories = {}
            metadata = {}
            for mode in ("default", "vram", "scanout"):
                output = root / mode
                options = [] if mode == "default" else ["--dump-source", mode]
                result = self.run_cli(capture, output, *options)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("images_written=1 frames_without_image=1", result.stdout)
                self.assertIn("frames=2 scanout_images=1", result.stdout)
                self.assertIn(f"dump_source={'vram' if mode == 'default' else mode}", result.stdout)
                directories[mode] = output
                rows = [json.loads(line) for line in (output / "frames.jsonl").read_text().splitlines()]
                self.assertEqual([row["capture_frame"] for row in rows], [0, 7])
                self.assertEqual([row["vram_valid"] for row in rows], [False, True])
                self.assertEqual(rows[0]["vram_refusal"], "no_enabled_circuit")
                self.assertEqual(rows[1]["selected_circuit"], 1)
                self.assertEqual(rows[1]["fbp_pages"], 0)
                self.assertEqual(rows[1]["fbp_blocks"], 0)
                self.assertEqual(rows[1]["fbw"], 1)
                self.assertEqual(rows[1]["psm"], 0)
                self.assertEqual(rows[1]["vram_width"], 1)
                self.assertEqual(rows[1]["vram_height"], 1)
                self.assertEqual(rows[1]["dispfb"], "0x0000000000000200")
                self.assertFalse(rows[1]["line_double"])
                metadata[mode] = rows
            for mode in ("default", "vram"):
                self.assertEqual([p.name for p in directories[mode].glob("*.ppm")], ["frame_00007.ppm"])
                self.assertEqual([row["image_valid"] for row in metadata[mode]], [False, True])
                self.assertEqual(metadata[mode][0]["image_refusal"], "no_enabled_circuit")
            self.assertEqual(metadata["default"], metadata["vram"])
            self.assertEqual((directories["default"] / "frame_00007.ppm").read_bytes(),
                             (directories["vram"] / "frame_00007.ppm").read_bytes())
            self.assertEqual((directories["vram"] / "frame_00007.ppm").read_bytes(), b"P6\n1 1\n255\n\0\0\0")
            self.assertEqual([p.name for p in directories["scanout"].glob("*.ppm")], ["frame_00000.ppm"])
            self.assertEqual([row["image_valid"] for row in metadata["scanout"]], [True, False])
            self.assertEqual(metadata["scanout"][1]["image_refusal"], "scanout_has_no_image")

    def test_invalid_selection_fails_before_input_or_output_access(self):
        with tempfile.TemporaryDirectory(prefix="hw-replay-cli-") as temporary:
            root = Path(temporary)
            missing = root / "does-not-exist.gscap"
            output = root / "must-not-be-created"
            for options in (
                ["--dump-source", "unknown"],
                ["--dump-source"],
                ["--dump-source", "vram", "--dump-source", "scanout"],
                ["--unknown"],
            ):
                with self.subTest(options=options):
                    result = self.run_cli(missing, output, *options)
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertNotIn("cannot open", result.stderr)
                    self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
