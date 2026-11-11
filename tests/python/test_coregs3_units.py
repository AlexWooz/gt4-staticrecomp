"""TASK-090: the CoreGS 3.0 unit insertion script (scripts/coregs3_units.py), on hand-authored unit text.

The real units are private translated code; these controls use synthetic units that only contain the anchors the script
looks for, so the insertion rules, the refusals and the byte-exact removal proof are testable without any game data.

The last class is the `.gt4gs` version 1 stream control (TASK-090 S1): it writes the documented format independently in
Python and checks the `gt4_coregs3_gs` tool's parse (`--stats`, `--list`, `--dump-frame`) and its reader refusals.
"""
import importlib.util
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('coregs3_units', ROOT / 'scripts' / 'coregs3_units.py')
coregs3_units = importlib.util.module_from_spec(SPEC)
sys.modules['coregs3_units'] = coregs3_units
SPEC.loader.exec_module(coregs3_units)

CALL_0055A64 = ('    { c.r[31].sd[0] = static_cast<std::int32_t>(0x00455A64u);  if (depth < kMaxDepth) unit_00E7(c, 53u, '
                'depth + 1u); else { c.pc = 0x0049D308u; return; } if (c.pc != 0x00455A64u) [[unlikely]] return; }\n')

SYNTHETIC_00D5 = (
    '#include "units.hpp"\n'
    '\n'
    'namespace gen {\n'
    '\n'
    'L_00455A5C:\n'
    + CALL_0055A64 +
    'L_00455A64:\n'
    '    return;\n'
    '}\n'
)

def make_synthetic_008f():
    """Every anchor the script inserts into unit_008F, once each, wrapped as a synthetic unit."""
    lines = ['#include "units.hpp"', '', 'namespace gen {', '']
    seen = set()
    for entry in coregs3_units.INSERTS:
        if entry[0] != 'unit_008F.cpp' or entry[1] in seen:
            continue
        seen.add(entry[1])
        lines.append(entry[1].rstrip('\n'))
    lines += ['    return;', '}', '']
    return '\n'.join(lines) + '\n'


# The full unit_008F anchor set (K3/K4/K5 and every K7 pass marker). The render_after and mirror ends follow the call
# statements, because their return labels are merge points reached without the call.
SYNTHETIC_008F = make_synthetic_008f()
SYNTHETIC_008F_PASSES = SYNTHETIC_008F

SYNTHETIC_00D3 = (
    '#include "units.hpp"\n'
    '\n'
    'namespace gen {\n'
    '\n'
    + coregs3_units.CALL_44E920 +
    'L_0044E920:\n'
    '    c.budget -= 2;\n'
    + coregs3_units.CALL_44EE24 +
    'L_0044EE24:\n'
    '    return;\n'
    '}\n'
)

SYNTHETIC_00D4 = (
    '#include "units.hpp"\n'
    '\n'
    'namespace gen {\n'
    '\n'
    + coregs3_units.CALL_4507AC +
    'L_004507AC:\n'
    '    return;\n'
    '}\n'
)

SYNTHETIC_00BB = (
    '#include "units.hpp"\n'
    '\n'
    'namespace gen {\n'
    '\n'
    + coregs3_units.CALL_3EC4A0 +
    'L_003EC4A0:\n'
    '    return;\n'
    '}\n'
)

SYNTHETIC = {
    'unit_00D3.cpp': SYNTHETIC_00D3,
    'unit_00D4.cpp': SYNTHETIC_00D4,
    'unit_00BB.cpp': SYNTHETIC_00BB,
    'unit_00D5.cpp': SYNTHETIC_00D5,
    'unit_008F.cpp': SYNTHETIC_008F,
}


class InsertionTest(unittest.TestCase):
    def test_apply_and_remove_are_exact(self):
        for name, text in SYNTHETIC.items():
            instrumented = coregs3_units.apply_insertions(name, text)
            self.assertNotEqual(instrumented, text)
            self.assertIn('namespace gt4::coregs3 {', instrumented)
            self.assertEqual(coregs3_units.remove_insertions(name, instrumented), text,
                             f'{name}: removal must restore the input byte for byte')

    def test_k1_call_sites(self):
        instrumented = coregs3_units.apply_insertions('unit_00D5.cpp', SYNTHETIC_00D5)
        self.assertIn('::gt4::coregs3::before_shape_call(c, 0u);\n' + CALL_0055A64, instrumented)
        self.assertIn('L_00455A64:\n    ::gt4::coregs3::after_shape_call(c);\n', instrumented)
        # The direct pgluCallShape sites carry their own call-site constant. The hook is inserted after the a0
        # assignment inside the call statement, so the shape pointer is already loaded when it runs.
        for name, constant, calls in (('unit_00D3.cpp', '2u', 2), ('unit_00D4.cpp', '1u', 1), ('unit_00BB.cpp', '3u', 1)):
            text = coregs3_units.apply_insertions(name, SYNTHETIC[name])
            self.assertEqual(text.count(f'::gt4::coregs3::before_shape_call(c, {constant});'), calls)
            self.assertEqual(text.count('::gt4::coregs3::after_shape_call(c);'), calls)
        text = coregs3_units.apply_insertions('unit_00D3.cpp', SYNTHETIC_00D3)
        self.assertIn('set32(c.r[4], c.r[18].w[0] + 32000u);\n    ::gt4::coregs3::before_shape_call(c, 2u);\n',
                      text)

    def test_pass_and_camera_sites(self):
        instrumented = coregs3_units.apply_insertions('unit_008F.cpp', SYNTHETIC_008F)
        for label, call in (('L_0033EBB4', 'projection_set'), ('L_0033EBF4', 'modelview_set'),
                            ('L_0033EE28', 'car_pass_begin'), ('L_0033EE54', 'car_pass_end')):
            self.assertIn(f'{label}:\n    ::gt4::coregs3::{call}(c);\n', instrumented)

    def test_pass_markers(self):
        instrumented = coregs3_units.apply_insertions('unit_008F.cpp', SYNTHETIC_008F_PASSES)
        for pass_id, call in (('1u', 'pass_begin'), ('2u', 'pass_begin'), ('5u', 'pass_begin'), ('9u', 'pass_begin'),
                              ('15u', 'pass_begin')):
            self.assertIn(f'::gt4::coregs3::{call}(c, {pass_id});', instrumented)
        self.assertIn('L_0033E9F0:\n    ::gt4::coregs3::pass_end(c, 1u);\n', instrumented)
        self.assertIn('::gt4::coregs3::pass_begin(c, 9u);\n' + coregs3_units.CALL_33EEF8 +
                      '    ::gt4::coregs3::pass_end(c, 9u);\n', instrumented)
        self.assertIn('L_0033EC28:\n    ::gt4::coregs3::pass_end(c, 2u);\n', instrumented)
        self.assertIn('L_0033DA84:\n    ::gt4::coregs3::pass_end(c, 15u);\n', instrumented)

    def test_a_missing_anchor_is_refused(self):
        broken = SYNTHETIC_00D5.replace('0x00455A64u);  if', '0x00455A64u); if')
        with self.assertRaises(coregs3_units.UnitError):
            coregs3_units.apply_insertions('unit_00D5.cpp', broken)

    def test_an_ambiguous_anchor_is_refused(self):
        twice = SYNTHETIC_008F.replace('L_0033EE54:\n', 'L_0033EE54:\nL_0033EE54:\n')
        with self.assertRaises(coregs3_units.UnitError):
            coregs3_units.apply_insertions('unit_008F.cpp', twice)

    def test_a_file_without_the_include_is_refused(self):
        with self.assertRaises(coregs3_units.UnitError):
            coregs3_units.apply_insertions('unit_00D5.cpp', 'int main() {}\n')

    def test_instrument_writes_a_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'units'
            source.mkdir()
            for name, text in SYNTHETIC.items():
                (source / name).write_text(text, encoding='utf-8', newline='\n')
            (source / 'unit_0000.cpp').write_text('unchanged\n', encoding='utf-8', newline='\n')
            output = Path(temporary) / 'instrumented'
            receipt = coregs3_units.instrument(source, output)
            self.assertEqual(receipt['files']['instrumented'], 5)
            self.assertEqual(receipt['files']['copied'], 1)
            self.assertEqual((output / 'unit_0000.cpp').read_text(encoding='utf-8'), 'unchanged\n')
            self.assertTrue((Path(temporary) / 'instrumented.coregs3-units.json').is_file())
            with self.assertRaises(coregs3_units.UnitError):
                coregs3_units.instrument(source, output)  # a fresh folder only


# ---------------------------------------------------------------------------------------------------------------------
# TASK-090 S1: the `.gt4gs` version 1 stream, written here from docs/COREGS3-GSSTREAM-CAPTURE.md §3.2 (an independent
# implementation of the format), parsed by the built `gt4_coregs3_gs` tool. Needs GT4_NATIVE_BUILD with that tool;
# skipped otherwise.

def _gs_record(kind, path=0, flags=0, pass_id=0, sequence=0, vblank=0, last_job=0, pc=0, ra=0, qwc=0, payload=b''):
    assert len(payload) % 16 == 0, 'a .gt4gs payload is a multiple of 16 bytes'
    return struct.pack('<IHBBIIQQIIII', len(payload), kind, path, flags, pass_id, sequence, vblank, last_job, pc, ra,
                       qwc, 0) + payload


def _gs_stream():
    """One frame (VBlank 7398) with two passes, a PATH3 packet in each and one PATH2 packet, plus frame totals."""
    registers_sky = list(range(0x10, 0x19))
    registers_course = list(range(0x20, 0x29))
    frame_begin = struct.pack('<IIQQQ', 0, 0, 0, 0, 0)
    frame_end = struct.pack('<IIQQQ', 0, 0, 16, 96, 0)
    records = [
        _gs_record(4, vblank=7398, payload=frame_begin),
        _gs_record(2, pass_id=2, sequence=1, vblank=7398, last_job=7, payload=struct.pack('<9Q2I', *registers_sky, 0, 0)),
        _gs_record(1, path=3, flags=1, pass_id=2, sequence=2, vblank=7398, last_job=7, pc=0x00100008, ra=0x0010000C,
                   qwc=2, payload=bytes(range(32))),
        _gs_record(1, path=2, pass_id=2, sequence=3, vblank=7398, last_job=8, pc=0x00100010, ra=0x00100014,
                   qwc=1, payload=bytes(range(16))),
        _gs_record(3, pass_id=2, vblank=7398, payload=struct.pack('<9Q2I', *registers_sky, 0, 0)),
        _gs_record(2, pass_id=5, vblank=7398, last_job=9, payload=struct.pack('<9Q2I', *registers_course, 0, 0)),
        _gs_record(1, path=3, pass_id=5, sequence=6, vblank=7398, last_job=9, qwc=4, payload=bytes(range(64))),
        _gs_record(3, pass_id=5, vblank=7398, payload=struct.pack('<9Q2I', *registers_course, 0, 0)),
        _gs_record(5, vblank=7398, payload=frame_end),
    ]
    header = struct.pack('<IIIIQQ', 0x47345447, 1, 0, 0, 7398, 0)
    return header + b''.join(records)


def _gs_tool():
    build = os.environ.get('GT4_NATIVE_BUILD')
    if not build:
        return None
    for name in ('gt4_coregs3_gs.exe', 'gt4_coregs3_gs'):
        candidate = Path(build) / name
        if candidate.exists():
            return candidate
    return None


@unittest.skipUnless(_gs_tool(), 'GT4_NATIVE_BUILD does not contain gt4_coregs3_gs')
class GsStreamToolTest(unittest.TestCase):
    def write(self, folder, name, data):
        path = Path(folder) / name
        path.write_bytes(data)
        return path

    def run_tool(self, *arguments):
        return subprocess.run([str(_gs_tool()), *map(str, arguments)], capture_output=True, text=True)

    def test_stats_parses_the_documented_format(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = self.write(temporary, 'stream.gt4gs', _gs_stream())
            done = self.run_tool('--stats', path)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn('version 1 target 0', done.stdout)
            self.assertIn('path2 16 in 1 records, path3 96 in 2 records', done.stdout)
            self.assertIn('frames: opened 1 closed 1 refused 0', done.stdout)
            self.assertIn('pass sky', done.stdout)
            self.assertIn('pass course', done.stdout)

    def test_list_prints_every_record(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = self.write(temporary, 'stream.gt4gs', _gs_stream())
            done = self.run_tool('--list', path)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn('pass_begin', done.stdout)
            self.assertIn('path3', done.stdout)
            self.assertIn('pc 00100008 ra 0010000C', done.stdout)
            self.assertIn('signals', done.stdout)

    def test_dump_frame_prints_registers_and_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = self.write(temporary, 'stream.gt4gs', _gs_stream())
            done = self.run_tool('--dump-frame', '0', path)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn('frame 0 vblank 7398', done.stdout)
            self.assertIn('registers fogcol', done.stdout)
            self.assertIn('frame_index 0 path2 16 path3 96', done.stdout)

    def test_malformed_streams_are_refused(self):
        good = _gs_stream()
        cases = {
            'bad magic': (b'\x00\x00\x00\x00' + good[4:], 'bad magic'),
            'unknown version': (good[:4] + struct.pack('<I', 2) + good[8:], 'unsupported GS stream version 2'),
            'header reserved': (good[:24] + struct.pack('<Q', 1) + good[32:], 'reserved word is nonzero'),
            'trailing bytes': (good + b'\x00', 'trailing partial record'),
        }
        with tempfile.TemporaryDirectory() as temporary:
            for label, (data, expected) in cases.items():
                with self.subTest(label):
                    path = self.write(temporary, 'bad.gt4gs', data)
                    done = self.run_tool('--stats', path)
                    self.assertEqual(done.returncode, 1, done.stdout)
                    self.assertIn(expected, done.stderr)

    def test_a_record_over_the_end_is_refused(self):
        good = bytearray(_gs_stream())
        # The first record after the 32-byte header is the frame_begin (48-byte header + 32-byte payload). A multiple of
        # 16 that runs past the file reaches the bounds check, not the alignment one.
        struct.pack_into('<I', good, 32, 0x10000)
        with tempfile.TemporaryDirectory() as temporary:
            path = self.write(temporary, 'over.gt4gs', bytes(good))
            done = self.run_tool('--stats', path)
            self.assertEqual(done.returncode, 1)
            self.assertIn('runs past the end', done.stderr)

    def test_a_bad_pass_id_is_refused(self):
        good = bytearray(_gs_stream())
        # The first GIF record starts after the header, frame_begin, pass_begin: 32 + 48+32 + 48+80 = 240.
        gif_at = 32 + (48 + 32) + (48 + 80)
        self.assertEqual(struct.unpack_from('<H', good, gif_at + 4)[0], 1, 'the offset names a GIF record')
        struct.pack_into('<I', good, gif_at + 8, 99)
        with tempfile.TemporaryDirectory() as temporary:
            path = self.write(temporary, 'badpass.gt4gs', bytes(good))
            done = self.run_tool('--stats', path)
            self.assertEqual(done.returncode, 1)
            self.assertIn('outside the PassId table', done.stderr)


def _numpy_python():
    """A python that can run scripts/coregs3_bench/gs_composite_metrics.py (numpy, PIL, scipy).

    The suite runs under the tooling venv, which has PIL but no numpy; the metrics control therefore runs the script with
    the first interpreter that reports numpy, and skips when there is none (never a false pass).
    """
    import shutil
    for candidate in (sys.executable, shutil.which('python'), shutil.which('python3')):
        if not candidate:
            continue
        probe = subprocess.run([candidate, '-c', 'import numpy, PIL, scipy'], capture_output=True)
        if probe.returncode == 0:
            return candidate
    return None


@unittest.skipUnless(_numpy_python(), 'no python with numpy/PIL/scipy for the metrics control')
class GsCompositeMetricsTest(unittest.TestCase):
    def test_reports_raw_and_ct16_corrected_reference(self):
        """TASK-090 post chain: the metrics script must report both the raw and the +4-corrected reference.

        Synthetic images: a constant reference (72,72,72) and a constant world (64,64,64) on a band of the clean
        mask. The raw block must show ref luma 248/3 = 72.0 and world 64.0; the corrected block must show 76.0 (the +4
        bias) with the same world luma, so a run can never silently replace the baseline with the correction.
        """
        from PIL import Image, ImageDraw
        python = _numpy_python()
        width, height = 1920, 1344
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            Image.new('RGB', (width, height), (72, 72, 72)).save(folder / 'ref.ppm')
            Image.new('RGB', (width, height), (64, 64, 64)).save(folder / 'world.png')
            mask = Image.new('L', (width, height), 0)
            ImageDraw.Draw(mask).rectangle([600, 300, 1500, 900], fill=255)  # a band outside the HUD rectangles
            mask.save(folder / 'mask.pgm')
            Image.new('L', (width, height), 0).save(folder / 'car.pgm')
            out = folder / 'metrics.json'
            done = subprocess.run([python, str(ROOT / 'scripts' / 'coregs3_bench' / 'gs_composite_metrics.py'),
                                   str(folder / 'ref.ppm'), str(folder / 'world.png'), str(folder / 'mask.pgm'),
                                   str(folder / 'car.pgm'), '--json', str(out)], capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            import json
            summary = json.loads(out.read_text())
            self.assertIn('raw dump', summary)
            self.assertIn('dump + 4 (CT16)', summary)
            raw = summary['raw dump']['all']
            corrected = summary['dump + 4 (CT16)']['all']
            self.assertAlmostEqual(raw['luma_ref'], 72.0, places=3)
            self.assertAlmostEqual(raw['luma_world'], 64.0, places=3)
            self.assertAlmostEqual(corrected['luma_ref'], 76.0, places=3)
            self.assertAlmostEqual(corrected['luma_world'], 64.0, places=3)
            self.assertEqual(summary['raw dump']['ref_bias'], 0.0)
            self.assertEqual(summary['dump + 4 (CT16)']['ref_bias'], 4.0)
            self.assertAlmostEqual(summary['raw dump']['reference_quantisation']['fraction_divisible_by_8'], 1.0)


@unittest.skipUnless(_numpy_python(), 'no python with numpy/PIL/scipy for the texture census control')
class GsTextureCensusTest(unittest.TestCase):
    """TASK-090 texture mip: the palette/texel census must recover a known texel ratio and detect a palette gain.

    The synthetic case is exact: the world stage is texel * vertex / 128, the reference is that world through the
    captured chain (FIX 127, FIX 255 and the CT16 present with DIMX 0001044501100554), and the sample dump carries the
    texel and vertex. A correct palette must come back at ratio 1.0 +/- 0.01; a 0.5x palette gain in the reference must
    come back at 0.5, which is what makes the study's fix-3 gate a measurement and not a claim. The data is generated
    with plain integer arithmetic, so only the script (run with a numpy interpreter) needs numpy.
    """

    DIMX = 0x0001044501100554

    def _chain(self, world, phase):
        value = max(0, min(255, (world * 127) >> 7))
        value = max(0, min(255, (value * 255) >> 7))
        field = (self.DIMX >> (4 * phase)) & 7
        dither = field - 8 if field >= 4 else field
        value = max(0, min(255, value + dither))
        return ((value >> 3) & 0x1F) << 3

    def _run(self, gain):
        from PIL import Image
        python = _numpy_python()
        width, height = 32, 8
        texel = [[0] * width for _ in range(height)]
        world = [[0] * width for _ in range(height)]
        reference = [[[0, 0, 0] for _ in range(width)] for _ in range(height)]
        for y in range(height):
            for x in range(width):
                value = 8 + ((x + 3 * y) % 33)
                texel[y][x] = value
                world[y][x] = round(value * 200.0 / 128.0)
                phase = ((y // 3) & 3) * 4 + ((x // 3) & 3)
                sampled = round(world[y][x] * gain)
                for channel in range(3):
                    reference[y][x][channel] = self._chain(sampled, phase)
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            world_image = Image.new('RGB', (width, height))
            world_image.putdata([(world[y][x],) * 3 for y in range(height) for x in range(width)])
            world_image.save(folder / 'world.png')
            ref_image = Image.new('RGB', (width, height))
            ref_image.putdata([tuple(reference[y][x]) for y in range(height) for x in range(width)])
            ref_image.save(folder / 'ref.ppm')
            (folder / 'report.txt').write_text(
                'chain stage seq 1094 present-ct16: sprites 20 pixels 1 written 1 changed 1; DTHE 1 '
                'DIMX %08X COLCLAMP 1\n' % self.DIMX)
            with open(folder / 'dump.bin', 'wb') as handle:
                handle.write(struct.pack('<5I', 0x53345447, 1, width, height, 24))
                for y in range(height):
                    for x in range(width):
                        row = bytearray(24)
                        row[0] = 1  # textured, no fog
                        for channel in range(4):
                            row[3 + channel] = texel[y][x] if channel < 3 else 0xFF
                            row[7 + channel] = 200 if channel < 3 else 0xFF
                            row[11 + channel] = world[y][x] if channel < 3 else 0xFF
                        row[15] = 0
                        row[19] = 0xD0
                        row[20] = 0x36  # page 0x36D0, the road page
                        handle.write(row)
            out = folder / 'census.json'
            done = subprocess.run([python, str(ROOT / 'scripts' / 'coregs3_bench' / 'gs_texture_census.py'),
                                   str(folder / 'dump.bin'), str(folder / 'ref.ppm'), str(folder / 'world.png'),
                                   str(folder / 'report.txt'), '--json', str(out)],
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            import json
            return json.loads(out.read_text())['pages']['all']

    def test_recovers_the_texel_ratio(self):
        row = self._run(1.0)
        self.assertAlmostEqual(row['texel_ratio'], 1.0, delta=0.01)
        self.assertAlmostEqual(row['world_ratio'], 1.0, delta=0.01)
        self.assertGreater(row['corr'], 0.99)

    def test_detects_a_palette_gain(self):
        row = self._run(0.5)
        self.assertAlmostEqual(row['texel_ratio'], 0.5, delta=0.02)
        self.assertAlmostEqual(row['world_ratio'], 0.5, delta=0.02)


class DitherAbTest(unittest.TestCase):
    """TASK-090 dither option: the A/B metrics script on a hand-authored pair.

    The synthetic pair is exact: a constant (16,16,16) image and a checkerboard that adds 8 to half of its pixels. The
    four-by-four crop therefore has 16 differing pixels, a max difference of 8, a mean |diff| of 4 and an MSE of 32;
    the faithful image keeps two levels (both divisible by 8, the CT16 five-bit expansion) while the off image has one.
    """

    def test_the_switch_ab_reports_the_dither(self):
        from PIL import Image
        python = sys.executable
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            width = height = 8
            faithful = Image.new('RGB', (width, height), (16, 16, 16))
            off = Image.new('RGB', (width, height), (16, 16, 16))
            faithful_pixels = faithful.load()
            for y in range(height):
                for x in range(width):
                    if (x + y) % 2:
                        faithful_pixels[x, y] = (24, 24, 24)
            faithful.save(folder / 'faithful.png')
            off.save(folder / 'off.png')
            out = folder / 'ab.json'
            done = subprocess.run(
                [python, str(ROOT / 'scripts' / 'coregs3_bench' / 'dither_ab.py'), str(folder / 'faithful.png'),
                 str(folder / 'off.png'), '--crop', '0,0,4,4', '--json', str(out),
                 '--crop-faithful', str(folder / 'crop-faithful.png'), '--crop-off', str(folder / 'crop-off.png'),
                 '--diff', str(folder / 'diff.png')],
                capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            summary = json.loads(out.read_text())
            compare = summary['compare']
            self.assertEqual(compare['pixels'], 16)
            self.assertAlmostEqual(compare['mean_abs'], 4.0, places=6)
            self.assertEqual(compare['max_abs'], 8)
            self.assertAlmostEqual(compare['mse'], 32.0, places=6)
            self.assertAlmostEqual(compare['psnr'], 10.0 * math.log10(255.0 * 255.0 / 32.0), places=4)
            self.assertEqual(compare['differing_pixels'], 8)
            self.assertAlmostEqual(compare['differing_fraction'], 0.5, places=6)
            self.assertAlmostEqual(compare['mean_signed'], 4.0, places=6)
            self.assertEqual(summary['crop'], [0, 0, 4, 4])
            self.assertAlmostEqual(summary['faithful_quantisation']['fraction_divisible_by_8'], 1.0, places=6)
            self.assertAlmostEqual(summary['off_quantisation']['fraction_divisible_by_8'], 1.0, places=6)
            self.assertEqual(summary['faithful_quantisation']['distinct_levels'], 2)
            self.assertEqual(summary['off_quantisation']['distinct_levels'], 1)
            self.assertTrue((folder / 'crop-faithful.png').exists())
            self.assertTrue((folder / 'crop-off.png').exists())
            self.assertTrue((folder / 'diff.png').exists())
            # A pair of different sizes is refused by name, never truncated or resampled.
            Image.new('RGB', (4, 4)).save(folder / 'small.png')
            done = subprocess.run(
                [python, str(ROOT / 'scripts' / 'coregs3_bench' / 'dither_ab.py'), str(folder / 'faithful.png'),
                 str(folder / 'small.png')],
                capture_output=True, text=True)
            self.assertNotEqual(done.returncode, 0)
            self.assertIn('differ in size', done.stderr)


if __name__ == '__main__':
    unittest.main()
