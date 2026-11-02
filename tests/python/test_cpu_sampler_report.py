"""TASK-027 CPU survey: the GT4_CPU_SAMPLER report script (scripts/cpu_sampler_report.py).

Pure checks on a synthetic sampler file and a stub symboliser; they never start the game or llvm-symbolizer.
"""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import cpu_sampler_report as report  # noqa: E402

EXE_BASE = 0x7FF600000000
PREFERRED = 0x140000000

SAMPLE_FILE = f"""gt4-cpu-sampler 1 period_us=1000 from=100 to=200 window_vblanks=100 window_us=1666000 periods=1666 samples=6 dropped=0 suspensions=6 suspended_us=60
module 0x{EXE_BASE:x} 0x1000000 C:\\game\\gt4vulkan.exe
thread 11 0x0 900000000 4 400.000 gs-worker
thread 12 0x{EXE_BASE + 0x900:x} 300000000 2 100.000 -
s 11 3 3000 {EXE_BASE + 0x100:x} {EXE_BASE + 0x200:x} {EXE_BASE + 0x300:x}
s 11 1 1000 {EXE_BASE + 0x400:x} {EXE_BASE + 0x300:x}
s 12 2 2000 {EXE_BASE + 0x500:x} {EXE_BASE + 0x900:x}
"""

# Preferred VA -> innermost..outermost function names (0x100 is inlined into Batch::Flush).
NAMES = {
    PREFERRED + 0x100: ['Swizzle', 'Batch::Flush'],
    PREFERRED + 0x200: ['FrontEnd::Kick'],
    PREFERRED + 0x300: ['GsWorker::run'],
    PREFERRED + 0x400: ['Gpu::Draw'],
    PREFERRED + 0x500: ['vu1_program'],
    PREFERRED + 0x900: ['mtvu_worker_main'],
}


def fake_symbolize(exe, addresses, symbolizer):
    # Return addresses are looked up one byte earlier (inside the call): round up to the function's slot.
    return {a: NAMES.get((a + 0xFF) & ~0xFF, ['??']) for a in addresses}


def minimal_pe(path):
    blob = bytearray(4096)
    blob[0:2] = b'MZ'
    struct.pack_into('<I', blob, 0x3C, 0x80)
    blob[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<H', blob, 0x80 + 24, 0x20B)            # PE32+
    struct.pack_into('<Q', blob, 0x80 + 24 + 24, PREFERRED)   # ImageBase
    Path(path).write_bytes(bytes(blob))


class SamplerReportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.exe = Path(self.tmp.name) / 'gt4vulkan.exe'
        minimal_pe(self.exe)
        self.data = report.parse(SAMPLE_FILE)
        self.resolver = report.Resolver(self.data, self.exe, 'unused', symbolize=fake_symbolize)
        self.resolver.prepare()

    def tearDown(self):
        self.tmp.cleanup()

    def test_parse_reads_header_modules_threads_and_stacks(self):
        self.assertEqual(self.data.header['window_vblanks'], 100)
        self.assertEqual(len(self.data.modules), 1)
        self.assertEqual(self.data.threads[11]['name'], 'gs-worker')
        self.assertAlmostEqual(self.data.threads[11]['cpu_ms'], 400.0)
        self.assertEqual(len(self.data.stacks), 3)

    def test_addresses_are_rebased_to_the_preferred_image_base(self):
        self.assertEqual(self.resolver.frames(EXE_BASE + 0x100), ['Swizzle', 'Batch::Flush'])

    def test_return_addresses_are_looked_up_inside_the_call(self):
        self.assertEqual(report.stack_names(self.resolver, [EXE_BASE + 0x100, EXE_BASE + 0x200]),
                         ['Swizzle', 'Batch::Flush', 'FrontEnd::Kick'])
        self.assertIn(EXE_BASE + 0x1FF, self.resolver.cache)

    def test_exclusive_uses_the_innermost_inlined_function_and_weights_by_cycles(self):
        p = report.profile(self.data, self.resolver)[11]
        self.assertEqual(p['weight'], 4000)
        self.assertEqual(p['exclusive']['Swizzle'], 3000)
        self.assertEqual(p['exclusive']['Gpu::Draw'], 1000)
        self.assertEqual(p['inclusive']['GsWorker::run'], 4000)
        self.assertEqual(p['inclusive']['Batch::Flush'], 3000)

    def test_unnamed_threads_are_named_after_their_entry_function(self):
        self.assertEqual(report.thread_label(self.data, self.resolver, 12), 'mtvu_worker_main')

    def test_report_converts_shares_to_milliseconds_per_vblank(self):
        text = report.report(self.data, self.resolver, top=5, threads=2, min_ms=0.0)
        # gs-worker: 400 ms over 100 VBlanks = 4 ms/VBlank; Swizzle has 3/4 of its cycles.
        self.assertIn('== gs-worker (tid 11): 4.000 ms/VBlank CPU', text)
        self.assertIn('  3.000  75.0%  Swizzle', text)
        self.assertIn('process CPU in the window: 5.00 ms/VBlank', text)

    def test_focus_lists_caller_and_callee_chains(self):
        text = report.report(self.data, self.resolver, top=5, threads=1, min_ms=0.0, focus='Kick')
        # Stack: Swizzle (inlined in Batch::Flush) <- FrontEnd::Kick <- GsWorker::run.
        self.assertIn('GsWorker::run  ->  FrontEnd::Kick  ->  Batch::Flush', text)


if __name__ == '__main__':
    unittest.main()
