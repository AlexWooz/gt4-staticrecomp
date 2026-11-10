"""TASK-215e: the stutter-aware summary (scripts/coregs2_bench/cpu_summary.py) on synthetic logs.

Pure Python: every test writes its own log lines in a temporary folder, so nothing here starts the game, reads private
data or needs a native build. The figures are checked against hand-computed distributions, the CSV header and values
against a frozen contract, and the TASK-027 averages table against the output of the old tool (its keys and format must
not change).
"""
import csv
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'scripts/coregs2_bench/cpu_summary.py'
sys.path.insert(0, str(TOOL.parent))
import cpu_summary as summary  # noqa: E402

OLD_KEYS = {
    'gs_cpu', 'process_cpu',
    'ee_gif_cmds', 'ee_gif_kib', 'ee_gif_ms', 'ee_copy_ms', 'ee_drain_wait_ms', 'ee_read_fifo_ms', 'ee_wake_pushes',
    'ee_vsync_wait_ms', 'worker_gif_ms', 'worker_idle_ms', 'worker_wakeups', 'wall',
    'x_total', 'x_gif_parse', 'x_batch_own', 'x_translate', 'x_targets', 'x_texture', 'x_clut', 'x_draw',
    'x_upload_own', 'x_swizzle', 'x_written', 'x_readback', 'x_patch', 'x_glm', 'x_glm_hook', 'x_hook',
    'x_sprite_check', 'x_cpu_sprites', 'x_flush', 'x_compose', 'x_vblank_own', 'x_fifo_own',
    'ee_gs_drain', 'ee_mtvu_wait_download', 'ee_critical_path', 'ee_thread_cpu',
    'xe_ee_dispatch', 'xe_iop_run', 'xe_vif_gif', 'xe_vu0_aot', 'xe_run_due_events', 'xe_kernel_service',
    'xe_host_vblank', 'xe_syscall_dispatch', 'frame_p50', 'frame_p95', 'gpu_wait',
    'wait_download', 'readbacks_download', 'wait_before_write', 'readbacks_before_write', 'wait_keep',
    'readbacks_keep', 'wait_clut', 'readbacks_clut', 'wait_other', 'readbacks_other', 'gpu_busy',
    'gpu_draw_passes', 'mtvu_worker_busy', 'mtvu_worker_cpu', 'mtvu_vu1_program', 'mtvu_output_path12',
    'w2ee_wakeups', 'w2ee_notifies',
}

CSV_FIXED = ['run', 'scene', 'vblank_start', 'vblank_end', 'vblanks', 'wall_ms', 'gs_worker_ms', 'gs_exclusive_ms',
             'process_cpu_ms', 'ee_critical_path_ms', 'gpu_busy_ms', 'gpu_draw_passes_ms', 'gpu_wait_ms',
             'gpu_wait_not_readback_ms']
CSV_REASONS = ['download', 'before_write', 'keep', 'clut', 'other']
CSV_WAITS = [f'gpu_wait_{reason}_ms' for reason in CSV_REASONS]
CSV_READBACKS = [f'readbacks_{reason}_per_vblank' for reason in CSV_REASONS]
CSV_TAIL = ['submits', 'submits_per_vblank', 'uploads_per_vblank', 'clut_loads_per_vblank', 'swizzle_kib_per_vblank',
            'prims_per_vblank', 'fragments_per_vblank', 'batches', 'batches_per_vblank', 'draws', 'draws_per_vblank',
            'readbacks', 'readbacks_per_vblank']
CSV_EXPECTED = CSV_FIXED + CSV_WAITS + CSV_READBACKS + CSV_TAIL


# ---- synthetic logs --------------------------------------------------------------------------------------------

def counter_line(vblank, readbacks, batches, draws, submits):
    return (f'[core2] vblank={vblank} targets created=0 aged=0 replaced=0 grown=0 textures targets=0 memory=0 '
            f'cache_hits=0 decodes=0 dirty=0 dirty_px=0 readbacks={readbacks} readback_px=0 tone_runs=0 '
            f'tone_skipped=0 tone_early=0 tone_alpha_only=0 display targets=0 memory=0 indexed_readbacks=0 '
            f'direct_readbacks=0 clut_readbacks=0 download_readbacks=0 batched_readbacks=0 dirty_merged=0 '
            f'forced_bilinear=0 cpu_sprites=0 live_targets=0 target_slots=0 decodes_first=0 decodes_stale=0 '
            f'decodes_clash=0 decodes_over=0 clut_gpu_loads=0 gpu_palettes=0 clut_reads=0 glm_writes=0 '
            f'textures_over_targets=0 palette_rows=0 palette_row_hits=0 gpu batches={batches} draws={draws} '
            f'submits={submits} mip_draws=0 trilinear=0 mip_lcm=0 mip_max_mxl=0 mip_textures=0 mip_levels=0 waits=0 '
            f'recycled_images=0 window_decodes=0 window_mb=0.0 full_barriers=0 barriers_skipped=0 '
            f'fragment_barriers=0 render_passes=0 draw_flushes=0 image_mb=0.0 texture_mb=0.0 self_texture=0 '
            f'self_copies=0 self_copy_mpx=0.0')


def interval_lines(vblank, wall, gpu_busy=4.0, gpu_wait=0.85, prims=1200.0, uploads=72.0, clut_loads=48.0,
                   swizzle_kib=512.0, submits=3.0, draw_passes=15.0, reasons=None, not_readback=0.2,
                   counters=(0, 0, 0, 0), exclusive=6.5):
    reasons = reasons if reasons is not None else [('download', 0.50, 1.00), ('before write', 0.10, 0.20),
                                                   ('keep', 0.0, 0.0), ('clut', 0.0, 0.0), ('other', 0.05, 0.10)]
    reason_text = ''.join(f' {reason}={wait:.2f} ({count:.2f})' for reason, wait, count in reasons)
    lines = [
        counter_line(vblank, *counters),
        f'[core2] wall ms/vblank={wall:.2f} (the whole emulator, GS thread included)',
        f'[core2] ms/vblank gif=1.00 batch=2.00 upload=0.50 ({uploads:.0f}) written=0.00 clut=0.20 '
        f'({clut_loads:.0f}) hook=0.00 glm_hook=0.00 translate=0.30 targets=0.40 texture=0.60 draw=1.00 '
        f'readback=0.10 patch=0.00 compose=0.20 flush=0.30 cpu_sprites=0.00 sprite_check=0.00 '
        f'gpu_wait={gpu_wait:.2f} glm=0.00 (0 calls, 0 pages) swizzle=0.05 ({swizzle_kib:.0f} KiB) '
        f'gpu_busy={gpu_busy:.2f} ({submits:.1f} submits) gpu_draw_passes={draw_passes:.2f} prims={prims:.0f} '
        f'fragments=1000 compute=0',
        f'[core2] exclusive ms/vblank total={exclusive:.2f} gif_parse=1.00 batch_own=2.00 translate=0.30 '
        f'targets=0.40 texture=0.60 clut=0.20 draw=1.00 upload_own=0.50 swizzle=0.05 written=0.00 readback=0.10 '
        f'patch=0.00 glm=0.00 glm_hook=0.00 hook=0.00 sprite_check=0.00 cpu_sprites=0.00 flush=0.30 compose=0.20 '
        f'vblank_own=0.10 fifo_own=0.00',
        f'[core2] gpu_wait ms/vblank by readback reason (readbacks/vblank):{reason_text} '
        f'not_readback={not_readback:.2f} stores_skipped=0.00/vblank bit_patches=0 block_merges=0 (0 rects) '
        f'gpu_stores=0',
        f'[hw] cost vsync={vblank} gs_thread_cpu_ms=7.00 process_cpu_core_ms=25.00 render_passes=1.0 barriers=2.0 '
        f'upload_mb=0.10 readback_mb=0.02 scaled_image_mb=1.0',
        f'[hw] transport vsync={vblank} ee: gif_cmds=100.0 gif_kib=64 priv_cmds=1.0 gif_ms=2.000 copy_ms=0.100 '
        f'priv_ms=0.010 vsync_wait_ms=0.100 (1.00) drain_wait_ms=2.000 (1.00) read_fifo_ms=1.000 (1.00) '
        f'wake_pushes=10.0 | worker: gif_ms=1.000 priv_ms=0.010 vsync_ms=0.100 other_ms=0.100 idle_ms=1.000 '
        f'wakeups=10.0 max_depth=4 mtvu_jobs=10.0 mtvu_records=100.0 mtvu_wait_ms=0.100 mtvu_sleeps=1.0',
    ]
    return '\n'.join(lines) + '\n'


def report_lines(critical, samples=600, p50=7.0, p95=9.0, p99=11.0, maximum=12.0):
    return (
        f'[perf] periodic: {samples} VBlank samples in 5.001s = 59.99 VBlanks/s; fresh_scene_frames=Unknown; '
        f'measurement_mode=frame-only\n'
        f'[perf] frame ms: median {p50:.3f} p95 {p95:.3f} p99 {p99:.3f} max {maximum:.3f}; '
        f'samples={samples}/{samples} (tail cap4096)\n'
        f'[perf] measured_thread_cpu_ms/frame=25.000; occupancy=99.0\n'
        f'[frame] pacing sleep=0.000 ms/frame; EE frame wall=7.000; critical path={critical:.3f} ms/frame '
        f'(wall minus pacing sleep)\n'
        f'[perf] exclusive wall ms/frame: ee_dispatch=1.000 iop_run=0.100 vif_gif=0.200 vu0_aot=0.300 '
        f'run_due_events=0.400 kernel_service=0.100 host_vblank=0.100 syscall_dispatch=0.100\n')


def mtvu_lines():
    return ('[mtvu] EE gs_drain ms/frame=1.500 (journal GS front end on the EE thread)\n'
            '[mtvu] EE wait ms/frame: vblank=0.100 download=0.200\n'
            '[mtvu] worker ms/frame: vu1_program=1.000 output_path12=2.000 busy=3.000\n'
            '[mtvu] worker thread cpu ms/frame=3.000\n'
            '[mtvu] worker_to_ee rings=10.0 waits=1.0 wakeups=5.0 notifies=5.0\n')


def spike_line(vblank, wall, gs=None, gpu_wait=9.0, gpu_busy=8.0):
    gs = wall * 0.8 if gs is None else gs
    return (f'[core2] spike vblank={vblank} wall={wall:.1f} gs={gs:.1f} upload=1.0 (10, 64 KiB) texture=2.0 '
            f'readback=1.0 translate=0.5 draw=3.0 targets=0.4 clut=0.2 patch=0.0 gpu_wait={gpu_wait:.1f} '
            f'readbacks=12 (download 10, store 1, keep 1) created=2 images=2 allocated=2 decodes=1 submits=4 '
            f'glm_writes=0 dirty=3 gpu_busy={gpu_busy:.1f} fragments=100 prims=900')


def make_log(folder, walls, first=600, step=600, criticals=None, reports_per_block=1, extra=''):
    """A run under <folder>/night-demo: [core2] blocks, [hw] cost/transport, [perf]/[frame] and [mtvu] lines."""
    readbacks = batches = draws = submits = 0
    parts = []
    for index, wall in enumerate(walls):
        vblank = first + index * step
        readbacks += 10
        batches += 480
        draws += 700
        submits += 9
        parts.append(interval_lines(vblank, wall, counters=(readbacks, batches, draws, submits)))
        per_report = max(1, step // reports_per_block) if reports_per_block else step
        for report in range(max(1, reports_per_block)):
            value = 7.0
            if criticals:
                value = criticals[index * reports_per_block + report]
            parts.append(report_lines(value, samples=per_report))
        parts.append(mtvu_lines() if index == 0 else '')
    parts.append(extra)
    run = Path(folder) / 'night-demo'
    run.mkdir(parents=True, exist_ok=True)
    (run / 'log.txt').write_text(''.join(parts), encoding='utf-8')
    return run


def run_tool(folder, *arguments):
    environment = {key: value for key, value in os.environ.items()
                   if key not in ('GT4_BENCH_SCENE', 'GT4_BENCH_CSV', 'SUMMARY_LO', 'SUMMARY_HI')}
    environment['GT4_BENCH_OUT'] = str(folder)
    return subprocess.run([sys.executable, str(TOOL), *arguments], capture_output=True, text=True,
                          env=environment)


# ---- statistics ------------------------------------------------------------------------------------------------

class DistributionTests(unittest.TestCase):
    def test_nearest_rank_percentiles_counts_and_one_percent_low(self):
        # 89 x 10 (under budget), 10 x 20 (over 16.7), 1 x 30 (over both): p95 and p99 land on the 20s, the max on 30.
        values = [10.0] * 89 + [20.0] * 10 + [30.0]
        stats = summary.distribution(values)
        self.assertEqual(stats['n'], 100)
        self.assertAlmostEqual(stats['mean'], 11.2)
        self.assertEqual(stats['p50'], 10.0)
        self.assertEqual(stats['p95'], 20.0)
        self.assertEqual(stats['p99'], 20.0)
        self.assertEqual(stats['max'], 30.0)
        self.assertAlmostEqual(stats['low_1pct_fps'], 50.0)
        self.assertEqual(stats['over_budget'], 11)
        self.assertEqual(stats['over_hard'], 1)
        self.assertAlmostEqual(stats['ratio_over_budget'], 0.11)
        self.assertAlmostEqual(stats['ratio_over_hard'], 0.01)

    def test_counts_are_strictly_greater_than_the_budget(self):
        stats = summary.distribution([16.7, 20.0, 16.71, 20.01])
        self.assertEqual(stats['over_budget'], 3)  # 20.0, 16.71 and 20.01: the line itself is not over it
        self.assertEqual(stats['over_hard'], 1)    # only 20.01

    def test_no_samples_has_no_distribution(self):
        self.assertIsNone(summary.distribution([]))
        self.assertIsNone(summary.distribution(None))


# ---- the old table and the new sections ------------------------------------------------------------------------

class SummaryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_old_metrics_keep_their_keys_and_means(self):
        make_log(self.root, [10.0, 20.0])
        run = summary.analyse_run('demo', 600, 1200, root=self.root)
        self.assertEqual(set(run.average), OLD_KEYS)
        self.assertAlmostEqual(run.average['gs_cpu'], 7.0)
        self.assertAlmostEqual(run.average['process_cpu'], 25.0)
        self.assertAlmostEqual(run.average['wall'], 15.0)
        self.assertAlmostEqual(run.average['gpu_busy'], 4.0)
        self.assertAlmostEqual(run.average['gpu_wait'], 0.85)
        self.assertAlmostEqual(run.average['ee_critical_path'], 7.0)
        self.assertAlmostEqual(run.average['wait_download'], 0.5)
        self.assertAlmostEqual(run.average['readbacks_before_write'], 0.2)
        self.assertAlmostEqual(run.average['x_total'], 6.5)
        self.assertAlmostEqual(run.average['frame_p50'], 7.0)
        self.assertAlmostEqual(run.average['frame_p95'], 9.0)

    def test_table_only_is_the_old_output_and_the_default_starts_with_it(self):
        make_log(self.root, [10.0, 20.0])
        classic = run_tool(self.root, 'demo', '--table-only', '--lo', '600', '--hi', '1200')
        self.assertEqual(classic.returncode, 0, classic.stderr)
        self.assertTrue(classic.stdout.startswith('metric'.ljust(26) + 'demo'.rjust(15)))
        self.assertIn('wall'.ljust(26) + f'{15.0:.3f}'.rjust(15), classic.stdout)
        for section in ('stutter:', 'statistics (', 'histogram', 'worst ', 'csv:'):
            self.assertNotIn(section, classic.stdout)
        full = run_tool(self.root, 'demo', '--lo', '600', '--hi', '1200')
        self.assertEqual(full.returncode, 0, full.stderr)
        self.assertTrue(full.stdout.startswith(classic.stdout))

    def test_stutter_statistics_worst_and_histogram(self):
        walls = [10.0] * 89 + [20.0] * 10 + [30.0]
        make_log(self.root, walls)
        result = run_tool(self.root, 'demo', '--lo', '600', '--hi', '60000', '--worst', '2')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('stutter: window VBlank 600-60000', result.stdout)
        # The wall row of the statistics table: p50 10, p95/p99 20, 1% low 50.0, max 30, 11 over 16.7 and 1 over 20.
        row = [line for line in result.stdout.splitlines() if line.split()[:2] == ['wall', 'demo']][0].split()
        self.assertEqual(row[2], '100')
        self.assertEqual(row[3], '10.000')
        self.assertEqual(row[4], '20.000')
        self.assertEqual(row[5], '20.000')
        self.assertEqual(row[6], '50.0')
        self.assertEqual(row[7], '30.000')
        self.assertEqual(row[8:10], ['11', '1'])
        self.assertEqual(row[10], '0.110')
        self.assertIn('worst 2 intervals by wall (run demo, scene night)', result.stdout)
        self.assertIn('wall interval histogram (run demo, scene night, 100 samples, bucket 1 ms)', result.stdout)

    def test_stats_all_prints_every_old_metric(self):
        make_log(self.root, [10.0, 20.0])
        timing = run_tool(self.root, 'demo', '--lo', '600', '--hi', '1200')
        every = run_tool(self.root, 'demo', '--lo', '600', '--hi', '1200', '--stats', 'all')
        self.assertIn('ee_gif_cmds', timing.stdout)  # the old table always has every metric
        timing_section = timing.stdout.split('statistics (', 1)[1]
        every_section = every.stdout.split('statistics (', 1)[1]
        self.assertNotIn('ee_gif_cmds', timing_section)
        self.assertIn('ee_gif_cmds', every_section)
        self.assertIn('x_upload_own', every_section)

    def test_window_filters_the_intervals(self):
        make_log(self.root, [10.0, 11.0, 12.0, 13.0, 14.0])
        run = summary.analyse_run('demo', 1200, 3000, root=self.root)
        self.assertEqual([row['vblank_end'] for row in run.rows], [1200, 1800, 2400, 3000])
        self.assertEqual([row['vblank_start'] for row in run.rows], [601, 1201, 1801, 2401])
        # The statistics use the same window: 4 samples, not 5.
        self.assertEqual(summary.distribution([row['wall'] for row in run.rows])['n'], 4)

    def test_no_data_in_the_window_still_reports_and_writes_a_header(self):
        make_log(self.root, [10.0, 11.0])
        csv_path = self.root / 'empty.csv'
        result = run_tool(self.root, 'demo', '--lo', '5000', '--hi', '6000', '--csv', str(csv_path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('stutter: window VBlank 5000-6000', result.stdout)
        self.assertEqual(len(csv_path.read_text(encoding='utf-8').splitlines()), 1)

    def test_missing_log_is_reported(self):
        result = run_tool(self.root, 'absent')
        self.assertEqual(result.returncode, 1)
        self.assertIn('no log at', result.stderr)

    def test_cost_only_log_still_produces_rows(self):
        # A GSdx or CoreGS 1.1 run has [hw] cost lines but no [core2] blocks: the CSV falls back to those intervals.
        log = ''.join(
            f'[hw] cost vsync={vsync} gs_thread_cpu_ms=7.00 process_cpu_core_ms=25.00 render_passes=1.0 '
            f'barriers=2.0 upload_mb=0.10 readback_mb=0.02 scaled_image_mb=1.0\n' for vsync in (600, 1200))
        run = self.root / 'night-demo'
        run.mkdir()
        (run / 'log.txt').write_text(log, encoding='utf-8')
        csv_path = self.root / 'cost.csv'
        result = run_tool(self.root, 'demo', '--lo', '600', '--hi', '1200', '--csv', str(csv_path))
        self.assertEqual(result.returncode, 0, result.stderr)
        with csv_path.open(newline='') as handle:
            rows = list(csv.DictReader(handle))
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]['gs_worker_ms'], '7.000')
        self.assertEqual(rows[0]['wall_ms'], '')


# ---- CSV -------------------------------------------------------------------------------------------------------

class CsvTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, *arguments):
        path = self.root / 'out.csv'
        result = run_tool(self.root, *arguments, '--csv', str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        with path.open(newline='') as handle:
            reader = csv.DictReader(handle)
            return reader.fieldnames, list(reader)

    def test_columns_are_the_frozen_contract(self):
        make_log(self.root, [10.0, 12.0])
        fields, rows = self.write('demo', '--lo', '600', '--hi', '1200')
        self.assertEqual(fields, CSV_EXPECTED)
        self.assertEqual(len(rows), 2)

    def test_values_interval_vblank_and_counter_deltas(self):
        make_log(self.root, [10.0, 12.0])
        _, rows = self.write('demo', '--lo', '600', '--hi', '1200')
        first, second = rows
        self.assertEqual((first['run'], first['scene'], first['vblank_start'], first['vblank_end'], first['vblanks']),
                         ('demo', 'night', '1', '600', '600'))
        self.assertEqual((second['vblank_start'], second['vblank_end']), ('601', '1200'))
        self.assertEqual(first['wall_ms'], '10.000')
        self.assertEqual(first['gs_exclusive_ms'], '6.500')
        self.assertEqual(first['gs_worker_ms'], '7.000')
        self.assertEqual(first['process_cpu_ms'], '25.000')
        self.assertEqual(first['ee_critical_path_ms'], '7.000')
        self.assertEqual(first['gpu_busy_ms'], '4.000')
        self.assertEqual(first['gpu_draw_passes_ms'], '15.000')
        self.assertEqual(first['gpu_wait_ms'], '0.850')
        self.assertEqual(first['gpu_wait_not_readback_ms'], '0.200')
        self.assertEqual(first['gpu_wait_download_ms'], '0.500')
        self.assertEqual(first['gpu_wait_before_write_ms'], '0.100')
        self.assertEqual(first['readbacks_before_write_per_vblank'], '0.200')
        self.assertEqual(first['submits'], '9')
        self.assertEqual(first['submits_per_vblank'], '3.000')
        self.assertEqual(first['uploads_per_vblank'], '72.000')
        self.assertEqual(first['clut_loads_per_vblank'], '48.000')
        self.assertEqual(first['swizzle_kib_per_vblank'], '512.000')
        self.assertEqual(first['prims_per_vblank'], '1200.000')
        self.assertEqual(first['fragments_per_vblank'], '1000.000')
        self.assertEqual(first['batches'], '480')
        self.assertEqual(first['batches_per_vblank'], '0.800')
        self.assertEqual(first['draws'], '700')
        self.assertEqual(first['readbacks'], '10')
        # The counters are cumulative in the log; the second row carries the delta of its own interval.
        self.assertEqual((second['batches'], second['draws'], second['readbacks'], second['submits']),
                         ('480', '700', '10', '9'))

    def test_scene_from_environment_scene_file_and_argument(self):
        run = make_log(self.root, [10.0, 12.0])
        environment = {key: value for key, value in os.environ.items() if key not in ('GT4_BENCH_CSV',)}
        environment['GT4_BENCH_OUT'] = str(self.root)
        environment['GT4_BENCH_SCENE'] = 'tsukuba-wet'
        path = self.root / 'env.csv'
        result = subprocess.run([sys.executable, str(TOOL), 'demo', '--lo', '600', '--hi', '1200', '--csv', str(path)],
                                capture_output=True, text=True, env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        with path.open(newline='') as handle:
            self.assertEqual(list(csv.DictReader(handle))[0]['scene'], 'tsukuba-wet')
        self.assertIn('scene tsukuba-wet', result.stdout)
        # The run's own scene.txt wins over the environment (a summary months later must not guess).
        (run / 'scene.txt').write_text('opera-paris\n', encoding='utf-8')
        self.assertEqual(self.write('demo', '--lo', '600', '--hi', '1200')[1][0]['scene'], 'opera-paris')
        # --scene wins over both.
        self.assertEqual(self.write('demo', '--lo', '600', '--hi', '1200', '--scene', 'license-b1')[1][0]['scene'],
                         'license-b1')

    def test_a_new_renderer_reason_becomes_an_extra_column(self):
        reasons = [('download', 0.50, 1.00), ('flare', 1.50, 2.00), ('tone CLUT', 0.30, 0.40),
                   ('before write', 0.10, 0.20)]
        run = self.root / 'night-demo'
        run.mkdir()
        (run / 'log.txt').write_text(interval_lines(600, 10.0, reasons=reasons), encoding='utf-8')
        fields, rows = self.write('demo', '--lo', '600', '--hi', '600')
        expected = (CSV_FIXED + CSV_WAITS + ['gpu_wait_flare_ms', 'gpu_wait_tone_clut_ms'] + CSV_READBACKS
                    + ['readbacks_flare_per_vblank', 'readbacks_tone_clut_per_vblank'] + CSV_TAIL)
        self.assertEqual(fields, expected)
        self.assertEqual(rows[0]['gpu_wait_flare_ms'], '1.500')
        self.assertEqual(rows[0]['gpu_wait_tone_clut_ms'], '0.300')
        self.assertEqual(rows[0]['readbacks_flare_per_vblank'], '2.000')
        self.assertEqual(rows[0]['readbacks_tone_clut_per_vblank'], '0.400')

    def test_csv_environment_variable(self):
        make_log(self.root, [10.0, 12.0])
        environment = dict(os.environ)
        environment['GT4_BENCH_OUT'] = str(self.root)
        path = self.root / 'from-env.csv'
        environment['GT4_BENCH_CSV'] = str(path)
        result = subprocess.run([sys.executable, str(TOOL), 'demo', '--lo', '600', '--hi', '1200'],
                                capture_output=True, text=True, env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(path.exists())

    def test_two_runs_share_the_file_in_run_order(self):
        make_log(self.root, [10.0, 12.0])
        other = self.root / 'night-other'
        other.mkdir()
        (other / 'log.txt').write_text(interval_lines(600, 20.0), encoding='utf-8')
        fields, rows = self.write('demo', 'other', '--lo', '600', '--hi', '600')
        self.assertEqual(fields, CSV_EXPECTED)
        self.assertEqual([row['run'] for row in rows], ['demo', 'other'])


# ---- frame and spike sources -----------------------------------------------------------------------------------

class PerFrameTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_present_and_shown_intervals(self):
        run = make_log(self.root, [10.0])
        (run / 'frametime.csv').write_text(
            'id,vblank,present_ms,call_ms,shown_ms\n'
            '0,100,0.000,0.500,0.000\n'
            '1,102,16.600,0.500,16.300\n'
            '2,104,33.200,0.500,32.900\n'
            '3,106,49.800,0.500,49.500\n'
            '4,108,83.000,0.500,82.700\n', encoding='utf-8')
        result = run_tool(self.root, 'demo', '--lo', '100', '--hi', '108')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('per frame (frametime.csv', result.stdout)
        # The zero timestamp of the first presentation is the session start (frametime_plot.py drops it too): three
        # intervals remain, 16.6, 16.6 and 33.2 ms.
        self.assertIn('stutter presented demo (night): 1 of 3 frame intervals over 16.7 ms (33.3%), 1 over 20 ms; '
                      '1% low 30.1 fps; worst 33.2 ms', result.stdout)
        self.assertIn('stutter shown demo (night): 1 of 3 frame intervals over 16.7 ms (33.3%), 1 over 20 ms; '
                      '1% low 30.1 fps; worst 33.2 ms', result.stdout)

    def test_shown_column_all_zero_is_skipped(self):
        run = make_log(self.root, [10.0])
        (run / 'frametime.csv').write_text(
            'id,vblank,present_ms,call_ms,shown_ms\n'
            '0,100,0.000,0.500,0.000\n'
            '1,102,16.600,0.500,0.000\n'
            '2,104,33.200,0.500,0.000\n', encoding='utf-8')
        result = run_tool(self.root, 'demo', '--lo', '100', '--hi', '108')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('stutter presented demo', result.stdout)
        self.assertNotIn('stutter shown demo', result.stdout)


class SpikeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def make_spikes(self, count, first=100):
        extra = '\n'.join(spike_line(first + index, 16.0 + index * 0.1) for index in range(count)) + '\n'
        run = self.root / 'night-demo'
        run.mkdir(exist_ok=True)
        (run / 'log.txt').write_text(extra, encoding='utf-8')
        return run

    def test_censored_trace_reports_count_and_max_only(self):
        self.make_spikes(5)
        path = self.root / 'out.csv'
        result = run_tool(self.root, 'demo', '--lo', '100', '--hi', '199', '--csv', str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('no (censored)', result.stdout)
        self.assertIn('hint: run demo logged 5 spikes', result.stdout)
        with (self.root / 'out-spikes.csv').open(newline='') as handle:
            spikes = list(csv.DictReader(handle))
        self.assertEqual([row['vblank'] for row in spikes], ['100', '101', '102', '103', '104'])
        self.assertEqual(spikes[0]['wall_ms'], '16.000')

    def test_complete_trace_gives_per_vblank_percentiles(self):
        self.make_spikes(100)
        result = run_tool(self.root, 'demo', '--lo', '100', '--hi', '199')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('100.0%', result.stdout)
        self.assertIn('yes', result.stdout)
        # Sorted walls 16.0 .. 25.9: the nearest-rank p99 is the 99th of 100 samples.
        self.assertIn('25.800', result.stdout)


class EeReportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_cumulative_samples_place_each_report_in_its_interval(self):
        # Two reports of 300 samples per 600-VBlank block: the block's critical path is their mean.
        make_log(self.root, [10.0, 10.0], criticals=[7.0, 9.0, 5.0, 5.0], reports_per_block=2)
        run = summary.analyse_run('demo', 1, 1200, root=self.root)
        self.assertEqual([row['ee_reports'] for row in run.rows], [2, 2])
        self.assertAlmostEqual(run.rows[0]['ee_critical_path'], 8.0)
        self.assertAlmostEqual(run.rows[1]['ee_critical_path'], 5.0)


class RunScriptTests(unittest.TestCase):
    def test_run_scripts_record_the_scene_tag(self):
        common = (ROOT / 'scripts/coregs2_bench/common.sh').read_text(encoding='utf-8')
        self.assertIn('scene.txt', common)
        self.assertIn('GT4_BENCH_SCENE', common)
        for script in ('night-run.sh', 'arcade-run.sh', 'replay-run.sh', 'replay-gsdx.sh', 'spec2-run.sh'):
            text = (ROOT / 'scripts/coregs2_bench' / script).read_text(encoding='utf-8')
            self.assertIn('record_scene', text, script)


if __name__ == '__main__':
    unittest.main()
