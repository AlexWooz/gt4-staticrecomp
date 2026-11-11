"""Stutter-aware summary of the report lines of CoreGS 2.0 runs (TASK-215e).

usage: cpu_summary.py [options] <tag> ...

<tag> is the tag of night-run.sh, series.sh, arcade-run.sh or a run's folder name such as spec2-george-a1, under
GT4_BENCH_OUT (default build/evidence of this checkout). The window is the guest VBlank range SUMMARY_LO..SUMMARY_HI
(default the night race, 7800 to 11400), or --lo/--hi.

The first table is the averages-only summary of TASK-027 and is unchanged: one row per metric, the mean of its report
line over the window. The sections after it answer what averages hide, which is what the owner feels (TASK-215e):

  stutter    per run: how many report intervals are over the 60 Hz budget, the worst one and its VBlank, the share over
             budget, and the 1% low;
  worst      the N worst intervals with the numbers that explain them;
  statistics p50/p95/p99, 1% low, max and the counts over 16.7/20 ms of the timing metrics (--stats all: every metric);
  histogram  report intervals in 1 ms buckets;
  per frame  the presented and shown frame intervals of the run folder's frametime.csv (GT4_HW_FRAMETIME_LOG, written
             by a window run), with the same figures counted per frame;
  spikes     the [core2] spike lines (GT4_CORE2_SPIKE_MS): count, worst, and per-VBlank figures when the trace covers
             the whole window.

--csv writes one row per [core2] report interval (columns in README.md) and, when the log has spike lines, a
<name>-spikes.csv beside it with one row per spike VBlank. The scene tag (--scene, then scene.txt in the run folder,
then GT4_BENCH_SCENE, default night) is recorded in every row and in the section headers.

An interval is one report, normally 600 VBlanks: the log prints one [core2] line group and one [hw] cost line per
interval (a run needs GT4_MTVU_REPORT_EVERY and GT4_HW_PROFILE_EVERY; series.sh sets both to 600, and the [core2] lines
are always per 600 VBlanks). Every number of an interval is a mean over its VBlanks, so a spike inside an interval is
invisible here: the spike trace and frametime.csv are the per-VBlank sources (README.md, "Stutter measurement").
"""
import argparse
import csv
import math
import os
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

BUDGET_MS = 16.7  # 60 Hz: a frame (or an interval mean) over it cannot hold 60 FPS
HARD_MS = 20.0    # the second counting line of the brief: visibly uneven even at 50 FPS
DEFAULT_LO, DEFAULT_HI = 7800, 11400
DEFAULT_SCENE = 'night'
DEFAULT_ROOT = Path(__file__).resolve().parents[2] / 'build' / 'evidence'
STUTTER_METRICS = ('wall', 'gs_cpu', 'process_cpu', 'gpu_busy', 'gpu_wait', 'ee_critical_path', 'ee_gs_drain')
KNOWN_REASONS = ('download', 'before write', 'keep', 'clut', 'other')
# The spike lines are complete when they cover at least this share of the window's VBlanks (GT4_CORE2_SPIKE_MS=0.01
# with a cap high enough); below it they are a censored sample and only their count and maximum are meaningful.
SPIKE_COMPLETE = 0.98


# ---- statistics ------------------------------------------------------------------------------------------------

def percentile(ordered, percent):
    """Nearest rank, as native/perf_probe.cpp computes its p95/p99: the sample at rank ceil(percent * n / 100)."""
    rank = (percent * len(ordered) + 99) // 100
    return ordered[max(rank, 1) - 1]


def distribution(values):
    """p50/p95/p99, the 1% low as FPS, max and the counts over the two budgets of one sample series."""
    if not values:
        return None
    ordered = sorted(values)
    p95, p99 = percentile(ordered, 95), percentile(ordered, 99)
    over_budget = sum(value > BUDGET_MS for value in values)
    over_hard = sum(value > HARD_MS for value in values)
    return {
        'n': len(values), 'mean': statistics.fmean(values), 'p50': statistics.median(ordered), 'p95': p95, 'p99': p99,
        'low_1pct_fps': 1000.0 / p99 if p99 > 0.0 else None, 'max': ordered[-1],
        'over_budget': over_budget, 'over_hard': over_hard,
        'ratio_over_budget': over_budget / len(values), 'ratio_over_hard': over_hard / len(values),
    }


# ---- input -----------------------------------------------------------------------------------------------------

def bench_root():
    return Path(os.environ.get('GT4_BENCH_OUT', DEFAULT_ROOT))


def summary_window():
    return int(os.environ.get('SUMMARY_LO', DEFAULT_LO)), int(os.environ.get('SUMMARY_HI', DEFAULT_HI))


def run_folder(name, root=None):
    root = Path(root) if root is not None else bench_root()
    folder = root / f'night-{name}'
    return folder if folder.exists() else root / name


def scene_tag(run, forced=None):
    """--scene wins, then the run's own scene.txt (written by the run scripts), then GT4_BENCH_SCENE, then night."""
    if forced:
        return forced
    tag = run / 'scene.txt'
    if tag.exists():
        text = tag.read_text(encoding='utf-8', errors='replace').strip()
        if text:
            return text
    return os.environ.get('GT4_BENCH_SCENE') or DEFAULT_SCENE


class Collector:
    """The old averages plus the sample series behind each of them (the new distributions use the same samples)."""

    def __init__(self):
        self.average = {}
        self.samples = {}

    def add(self, key, values):
        values = list(values)
        if values:
            self.average[key] = sum(values) / len(values)
            self.samples[key] = values


def collect_averages(text, lo=None, hi=None):
    """The averages-only summary of TASK-027, moved here unchanged so the old table stays identical."""
    lo, hi = summary_window() if lo is None or hi is None else (lo, hi)
    out = Collector()

    # [hw] cost / transport lines carry their vsync
    cost = [(int(m.group(1)), float(m.group(2)), float(m.group(3))) for m in
            re.finditer(r'cost vsync=(\d+) gs_thread_cpu_ms=([\d.]+) process_cpu_core_ms=([\d.]+)', text)]
    cost = [c for c in cost if lo <= c[0] <= hi]
    out.add('gs_cpu', [c[1] for c in cost])
    out.add('process_cpu', [c[2] for c in cost])
    tr = [m for m in re.finditer(r'transport vsync=(\d+) ee: (.*?) \| worker: (.*?max_depth=\d+)', text)
          if lo <= int(m.group(1)) <= hi]
    for name in ('gif_cmds', 'gif_kib', 'gif_ms', 'copy_ms', 'drain_wait_ms', 'read_fifo_ms', 'wake_pushes',
                 'vsync_wait_ms'):
        out.add('ee_' + name, [float(re.search(name + r'=([\d.]+)', m.group(2)).group(1)) for m in tr])
    for name in ('gif_ms', 'idle_ms', 'wakeups'):
        out.add('worker_' + name, [float(re.search(name + r'=([\d.]+)', m.group(3)).group(1)) for m in tr])
    # [core2] lines have no vsync: take the ones after the matching [core2] vblank= counter line
    blocks = []
    for m in re.finditer(r'core2\] vblank=(\d+) ', text):
        vb = int(m.group(1))
        rest = text[m.end():m.end() + 20000]
        w = re.search(r'core2\] wall ms/vblank=([\d.]+)', rest)
        x = re.search(r'core2\] exclusive ms/vblank (.*?)\n', rest)
        blocks.append((vb, w, x))
    blocks = [b for b in blocks if lo <= b[0] <= hi]
    out.add('wall', [float(b[1].group(1)) for b in blocks if b[1]])
    ex = {}
    for b in blocks:
        if b[2]:
            for key, value in re.findall(r'(\w+)=([\d.]+)', b[2].group(1)):
                ex.setdefault(key, []).append(float(value))
    for key, values in ex.items():
        out.add('x_' + key, values)
    # EE-side MTVU and perf lines (no vsync on them; take the race-time ones by position after vsync lo)
    start = text.find(f'vsync={lo}')
    tail = text[start:] if start >= 0 else text
    out.add('ee_gs_drain', [float(v) for v in re.findall(r'EE gs_drain ms/frame=([\d.]+)', tail)])
    out.add('ee_mtvu_wait_download',
            [float(v) for v in re.findall(r'EE wait ms/frame: vblank=[\d.]+ download=([\d.]+)', tail)])
    out.add('ee_critical_path', [float(v) for v in re.findall(r'critical path=([\d.]+)', tail)])
    out.add('ee_thread_cpu', [float(v) for v in re.findall(r'measured_thread_cpu_ms/frame=([\d.]+)', tail)])
    ee = {}
    for line in re.findall(r'perf\] exclusive wall ms/frame[^:]*: (.*)', tail):
        for key, value in re.findall(r'(\w+)=([\d.]+)', line):
            if key in ('ee_dispatch', 'iop_run', 'vif_gif', 'vu0_aot', 'run_due_events', 'kernel_service',
                       'host_vblank', 'syscall_dispatch'):
                ee.setdefault(key, []).append(float(value))
    for key, values in ee.items():
        out.add('xe_' + key, values)
    out.add('frame_p50', [float(a) for a, b in re.findall(r'frame ms: median ([\d.]+) p95 ([\d.]+)', tail)])
    out.add('frame_p95', [float(b) for a, b in re.findall(r'frame ms: median ([\d.]+) p95 ([\d.]+)', tail)])
    gw, gb, gd = [], [], []
    for m in re.finditer(r'core2\] vblank=(\d+) ', text):
        if lo <= int(m.group(1)) <= hi:
            g = re.search(r'gpu_wait=([\d.]+)', text[m.end():m.end() + 20000])
            if g:
                gw.append(float(g.group(1)))
            b = re.search(r'gpu_busy=([\d.]+) \([\d.]+ submits\) gpu_draw_passes=([\d.]+)',
                          text[m.end():m.end() + 20000])
            if b:
                gb.append(float(b.group(1)))
                gd.append(float(b.group(2)))
    out.add('gpu_wait', gw)
    reasons = {}
    for m in re.finditer(r'core2\] vblank=(\d+) ', text):
        if lo <= int(m.group(1)) <= hi:
            line = re.search(r'gpu_wait ms/vblank by readback reason \(readbacks/vblank\):(.*?)\n',
                             text[m.end():m.end() + 20000])
            if line:
                for reason, wait, count in re.findall(r' ([a-z ]+?)=([\d.]+) \(([\d.]+)\)', line.group(1)):
                    reasons.setdefault('wait_' + reason.replace(' ', '_'), []).append(float(wait))
                    reasons.setdefault('readbacks_' + reason.replace(' ', '_'), []).append(float(count))
    for key, values in reasons.items():
        out.add(key, values)
    out.add('gpu_busy', gb)
    out.add('gpu_draw_passes', gd)
    out.add('mtvu_worker_busy',
            [float(v) for v in re.findall(r'worker ms/frame: vu1_program=[\d.]+ output_path12=[\d.]+ busy=([\d.]+)',
                                         tail)])
    out.add('mtvu_worker_cpu', [float(v) for v in re.findall(r'worker thread cpu ms/frame=([\d.]+)', tail)])
    out.add('mtvu_vu1_program', [float(v) for v in re.findall(r'worker ms/frame: vu1_program=([\d.]+)', tail)])
    out.add('mtvu_output_path12',
            [float(v) for v in re.findall(r'worker ms/frame: vu1_program=[\d.]+ output_path12=([\d.]+)', tail)])
    out.add('w2ee_wakeups',
            [float(v) for v in re.findall(r'worker_to_ee rings=[\d.]+ waits=[\d.]+ wakeups=([\d.]+)', tail)])
    out.add('w2ee_notifies',
            [float(v) for v in re.findall(r'worker_to_ee rings=[\d.]+ waits=[\d.]+ wakeups=[\d.]+ notifies=([\d.]+)',
                                          tail)])
    return out


# ---- per-interval rows (the CSV) -------------------------------------------------------------------------------

COUNTER = re.compile(r'(?<![a-z_])(readbacks|draws|submits)=(\d+)')
# A readback reason may contain spaces ("before write"); the counts in parentheses are the readbacks per VBlank.
REASON = re.compile(r' ([A-Za-z][A-Za-z_ ]*?)=([\d.]+) \(([\d.]+)\)')
SPIKE = re.compile(
    r'\[core2\] spike vblank=(\d+) wall=([\d.]+) gs=([\d.]+) upload=([\d.]+) \(([\d.]+), ([\d.]+) KiB\) '
    r'texture=([\d.]+) readback=([\d.]+) translate=([\d.]+) draw=([\d.]+) targets=([\d.]+) clut=([\d.]+) '
    r'patch=([\d.]+) gpu_wait=([\d.]+) readbacks=(\d+) \(download (\d+), store (\d+), keep (\d+)\) created=(\d+) '
    r'images=(\d+)(?: mid=(\d+))? allocated=(\d+) decodes=(\d+) submits=(\d+) glm_writes=(\d+) dirty=(\d+) gpu_busy=([\d.]+) '
    r'fragments=(\d+) prims=(\d+)')
PERF_REPORT = re.compile(r'\[perf\] [A-Za-z0-9 _-]+: (\d+) VBlank samples')


def parse_counters(marker):
    values = {name: None for name in ('readbacks', 'draws', 'submits', 'batches')}
    for name, value in COUNTER.findall(marker):
        values[name] = int(value)
    batches = re.search(r'gpu batches=(\d+)', marker)
    if batches:
        values['batches'] = int(batches.group(1))
    return values


def parse_interval(vblank, content, marker):
    """One [core2] report block: its means and its cumulative counters, None where the log has no such field."""
    row = {'vblank_end': vblank, 'waits': {}, 'readbacks_by_reason': {}, 'counters': parse_counters(marker)}
    wall = re.search(r'core2\] wall ms/vblank=([\d.]+)', content)
    row['wall'] = float(wall.group(1)) if wall else None
    exclusive = re.search(r'core2\] exclusive ms/vblank total=([\d.]+)', content)
    row['gs_exclusive'] = float(exclusive.group(1)) if exclusive else None
    line = re.search(r'core2\] ms/vblank ([^\n]*)', content)
    fields = {}
    if line:
        fields = {key: float(value) for key, value in re.findall(r'([a-z][a-z0-9_]*)=([\d.]+)', line.group(1))}
        row['gpu_busy'] = fields.get('gpu_busy')
        row['gpu_draw_passes'] = fields.get('gpu_draw_passes')
        row['gpu_wait'] = fields.get('gpu_wait')
        row['prims'] = fields.get('prims')
        row['fragments'] = fields.get('fragments')
        for name, pattern in (('uploads', r'upload=[\d.]+ \(([\d.]+)\)'),
                              ('clut_loads', r'clut=[\d.]+ \(([\d.]+)\)'),
                              ('swizzle_kib', r'swizzle=[\d.]+ \(([\d.]+) KiB\)'),
                              ('submits_timed', r'gpu_busy=[\d.]+ \(([\d.]+) submits\)')):
            found = re.search(pattern, line.group(1))
            row[name] = float(found.group(1)) if found else None
    reasons = re.search(r'gpu_wait ms/vblank by readback reason \(readbacks/vblank\):([^\n]*)', content)
    row['not_readback'] = None
    if reasons:
        segment = reasons.group(1)
        for reason, wait, count in REASON.findall(segment.split(' not_readback=')[0]):
            row['waits'][reason] = float(wait)
            row['readbacks_by_reason'][reason] = float(count)
        missing = re.search(r'not_readback=([\d.]+)', segment)
        row['not_readback'] = float(missing.group(1)) if missing else None
    return row


def parse_intervals(text):
    """Every [core2] report block in the log, in order, with the per-interval delta of its cumulative counters."""
    rows, previous = [], None
    marks = list(re.finditer(r'\[core2\] vblank=(\d+) ', text))
    for index, mark in enumerate(marks):
        line_end = text.find('\n', mark.start())
        start = line_end + 1 if line_end >= 0 else len(text)
        end = marks[index + 1].start() if index + 1 < len(marks) else len(text)
        marker = text[mark.start():line_end if line_end >= 0 else len(text)]
        row = parse_interval(int(mark.group(1)), text[start:end], marker)
        row['vblank_start'] = (rows[-1]['vblank_end'] + 1) if rows else 1
        row['vblanks'] = row['vblank_end'] - row['vblank_start'] + 1
        for name, value in row['counters'].items():
            if value is None:
                row[name + '_total'] = None
            elif previous is not None and previous.get(name) is not None:
                row[name + '_total'] = value - previous[name]
            else:
                row[name + '_total'] = value
        for name in ('batches', 'draws', 'readbacks'):
            total = row[name + '_total']
            row[name + '_per_vblank'] = total / row['vblanks'] if total is not None and row['vblanks'] else None
        if any(value is not None for value in row['counters'].values()):
            previous = row['counters']
        rows.append(row)
    return rows


def parse_costs(text):
    """[hw] cost lines by vsync: gs thread CPU, process CPU, render passes, barriers and the traffic means."""
    costs = {}
    pattern = (r'cost vsync=(\d+) gs_thread_cpu_ms=([\d.]+) process_cpu_core_ms=([\d.]+) render_passes=([\d.]+) '
               r'barriers=([\d.]+) upload_mb=([\d.]+) readback_mb=([\d.]+)')
    for m in re.finditer(pattern, text):
        costs[int(m.group(1))] = {'gs_worker': float(m.group(2)), 'process_cpu': float(m.group(3)),
                                  'render_passes': float(m.group(4)), 'barriers': float(m.group(5)),
                                  'upload_mb': float(m.group(6)), 'readback_mb': float(m.group(7))}
    return costs


def cost_intervals(costs):
    """Rows from the [hw] cost lines alone: a run on another renderer (GSdx, CoreGS 1.1) has no [core2] blocks."""
    rows, previous = [], 0
    for vsync in sorted(costs):
        cost = costs[vsync]
        row = {'vblank_end': vsync, 'vblank_start': previous + 1, 'vblanks': vsync - previous, 'wall': None,
               'gs_exclusive': None, 'gpu_busy': None, 'gpu_draw_passes': None, 'gpu_wait': None, 'prims': None,
               'fragments': None, 'uploads': None, 'clut_loads': None, 'swizzle_kib': None, 'submits_timed': None,
               'not_readback': None, 'waits': {}, 'readbacks_by_reason': {}, 'counters': {}, 'ee_critical_path': None,
               'ee_reports': 0}
        row.update(cost)
        rows.append(row)
        previous = vsync
    return rows


def parse_ee_reports(text):
    """[perf]/[frame] reports with the guest VBlank their cumulative sample count ends at.

    The [frame] line has no VBlank of its own, and neither does [perf]; their `N VBlank samples` line is the only
    printed bridge. A report's end VBlank is the running sum of those counts from the start of the log.
    """
    reports, starts = [], [m.start() for m in PERF_REPORT.finditer(text)]
    cumulative = 0
    for index, m in enumerate(PERF_REPORT.finditer(text)):
        cumulative += int(m.group(1))
        end = starts[index + 1] if index + 1 < len(starts) else min(len(text), m.end() + 6000)
        segment = text[m.end():end]
        critical = re.search(r'critical path=([\d.]+)', segment)
        frame = re.search(r'frame ms: median ([\d.]+) p95 ([\d.]+) p99 ([\d.]+) max ([\d.]+)', segment)
        reports.append({'end': cumulative,
                        'critical': float(critical.group(1)) if critical else None,
                        'p50': float(frame.group(1)) if frame else None,
                        'p95': float(frame.group(2)) if frame else None,
                        'p99': float(frame.group(3)) if frame else None,
                        'max': float(frame.group(4)) if frame else None})
    return reports


def parse_spikes(text):
    spikes = []
    for m in SPIKE.finditer(text):
        values = m.groups()
        spikes.append({
            'vblank': int(values[0]), 'wall': float(values[1]), 'gs': float(values[2]),
            'upload': float(values[3]), 'uploads': float(values[4]), 'upload_kib': float(values[5]),
            'texture': float(values[6]), 'readback': float(values[7]), 'translate': float(values[8]),
            'draw': float(values[9]), 'targets': float(values[10]), 'clut': float(values[11]),
            'patch': float(values[12]), 'gpu_wait': float(values[13]),
            'readbacks': int(values[14]), 'readback_download': int(values[15]), 'readback_store': int(values[16]),
            'readback_keep': int(values[17]), 'created': int(values[18]), 'images': int(values[19]),
            'images_mid': int(values[20]) if values[20] is not None else None,
            'allocated': int(values[21]), 'decodes': int(values[22]),
            'submits': int(values[23]), 'glm_writes': int(values[24]), 'dirty': int(values[25]),
            'gpu_busy': float(values[26]), 'fragments': int(values[27]), 'prims': int(values[28]),
        })
    return spikes


def read_frametime(run, lo, hi):
    """The run folder's frametime.csv (GT4_HW_FRAMETIME_LOG), filtered to the guest VBlanks of the window."""
    path = run / 'frametime.csv'
    if not path.exists():
        return None, []
    with open(path, newline='') as handle:
        rows = [row for row in csv.DictReader(handle)
                if row.get('vblank', '').isdigit() and lo <= int(row['vblank']) <= hi]
    return path, rows


def frame_intervals(rows, column):
    """Intervals between the positive timestamps of a frametime.csv column (present_ms or shown_ms), as spelled."""
    times = [(int(row['vblank']), float(row[column])) for row in rows
             if row.get(column) and float(row[column]) > 0.0]
    return [(later[0], later[1] - earlier[1]) for earlier, later in zip(times, times[1:])]


# ---- a run -----------------------------------------------------------------------------------------------------

@dataclass
class Run:
    name: str
    folder: Path
    scene: str
    average: dict = field(default_factory=dict)
    samples: dict = field(default_factory=dict)
    rows: list = field(default_factory=list)
    spikes: list = field(default_factory=list)
    frametime_path: Path = None
    frametime_rows: list = field(default_factory=list)


def analyse_run(name, lo=None, hi=None, root=None, scene=None):
    lo, hi = summary_window() if lo is None or hi is None else (lo, hi)
    folder = run_folder(name, root)
    text = (folder / 'log.txt').read_text(encoding='utf-8', errors='replace')
    collected = collect_averages(text, lo, hi)
    costs = parse_costs(text)
    reports = parse_ee_reports(text)
    parsed = parse_intervals(text)
    rows = []
    for row in parsed if parsed else cost_intervals(costs):
        if not lo <= row['vblank_end'] <= hi:
            continue
        if parsed:
            cost = costs.get(row['vblank_end'], {})
            row.update({key: cost.get(key) for key in ('gs_worker', 'process_cpu', 'render_passes', 'barriers',
                                                       'upload_mb', 'readback_mb')})
            inside = [report for report in reports
                      if row['vblank_start'] <= report['end'] <= row['vblank_end']]
            row['ee_reports'] = len(inside)
            row['ee_critical_path'] = (statistics.fmean(r['critical'] for r in inside if r['critical'] is not None)
                                       if any(r['critical'] is not None for r in inside) else None)
        rows.append(row)
    spikes = [spike for spike in parse_spikes(text) if lo <= spike['vblank'] <= hi]
    path, frame_rows = read_frametime(folder, lo, hi)
    return Run(name=name, folder=folder, scene=scene_tag(folder, scene), average=collected.average,
               samples=collected.samples, rows=rows, spikes=spikes, frametime_path=path,
               frametime_rows=frame_rows)


def summarise(name):
    """The TASK-027 entry point: the averages of one run, unchanged."""
    return analyse_run(name).average


# ---- printing ---------------------------------------------------------------------------------------------------

def show(value, digits=3):
    if value is None:
        return '-'
    if isinstance(value, int):
        return str(value)
    return f'{value:.{digits}f}'


def print_table(headers, rows):
    if not rows:
        return
    widths = [max([len(str(headers[index]))] + [len(str(row[index])) for row in rows])
              for index in range(len(headers))]
    print('  '.join(str(header).ljust(width) for header, width in zip(headers, widths)))
    for row in rows:
        print('  '.join(str(value).ljust(width) if index == 0 else str(value).rjust(width)
                        for index, (value, width) in enumerate(zip(row, widths))))


def print_old_table(names, averages):
    keys = []
    for row in averages.values():
        for key in row:
            if key not in keys:
                keys.append(key)
    print('metric'.ljust(26) + ''.join(n[:14].rjust(15) for n in names))
    for key in keys:
        print(key.ljust(26) + ''.join((f'{averages[n][key]:.3f}' if key in averages[n] else '-').rjust(15)
                                      for n in names))


def stats_of(run, metrics):
    return {metric: distribution(run.samples[metric]) for metric in metrics if metric in run.samples}


def print_stutter(runs, lo, hi):
    print()
    print(f'stutter: window VBlank {lo}-{hi}, interval = one [core2] report (its figures are means over its VBlanks;')
    print('  frames >16.7/>20 count intervals whose mean is over the budget, not single frames - see per frame/spikes)')
    rows = []
    for run in runs:
        wall = stats_of(run, ['wall']).get('wall')
        wait = stats_of(run, ['gpu_wait']).get('gpu_wait')
        measured = [row for row in run.rows if row['wall'] is not None]
        worst = max(measured, key=lambda row: row['wall']) if measured else None
        rows.append([run.name, run.scene, len(run.rows), sum(row['vblanks'] for row in run.rows),
                     show(wall['p50'] if wall else None), show(wall['p95'] if wall else None),
                     show(wall['p99'] if wall else None),
                     show(wall['low_1pct_fps'] if wall else None, 1), show(wall['max'] if wall else None),
                     show(wall['over_budget'] if wall else None), show(wall['over_hard'] if wall else None),
                     show(wall['ratio_over_budget'] if wall else None),
                     show(worst['vblank_end'] if worst else None), show(worst['wall'] if worst else None),
                     show(wait['p99'] if wait else None)])
    print_table(['run', 'scene', 'n', 'vblanks', 'p50', 'p95', 'p99', '1%low', 'max', '>16.7', '>20', 'ratio',
                 'worst vb', 'worst ms', 'wait p99'], rows)


def print_stats(runs, metrics):
    if metrics == 'all':
        metrics = []
        for run in runs:
            for metric in run.average:
                if metric not in metrics:
                    metrics.append(metric)
    print()
    print('statistics (interval samples; p50/p95/p99 ms, 1% low = 1000/p99 FPS, counts strict >)')
    rows = []
    for run in runs:
        for metric, stats in stats_of(run, metrics).items():
            rows.append([metric, run.name, stats['n'], show(stats['p50']), show(stats['p95']), show(stats['p99']),
                         show(stats['low_1pct_fps'], 1), show(stats['max']), stats['over_budget'], stats['over_hard'],
                         show(stats['ratio_over_budget'])])
    print_table(['metric', 'run', 'n', 'p50', 'p95', 'p99', '1%low', 'max', '>16.7', '>20', 'ratio'], rows)


def print_worst(runs, count):
    if count <= 0:
        return
    for run in runs:
        worst = sorted([row for row in run.rows if row['wall'] is not None], key=lambda row: row['wall'],
                       reverse=True)[:count]
        if not worst:
            continue
        print()
        print(f'worst {len(worst)} intervals by wall (run {run.name}, scene {run.scene})')
        rows = [[rank, row['vblank_end'], show(row['wall']), show(row.get('gs_worker')),
                 show(row.get('process_cpu')), show(row.get('gpu_busy')), show(row.get('gpu_wait')),
                 show(row.get('submits_timed'), 1), show(row.get('prims'), 0), show(row.get('batches_per_vblank'), 1)]
                for rank, row in enumerate(worst, 1)]
        print_table(['#', 'vblank', 'wall', 'gs', 'process', 'gpu_busy', 'gpu_wait', 'submits', 'prims/vb',
                     'batch/vb'], rows)


def print_histogram(name, scene, label, values, bucket, width=40):
    if not values:
        return
    low = math.floor(min(values) / bucket) * bucket
    high = math.ceil(max(values) / bucket) * bucket
    if high <= low:
        high = low + bucket
    counts = {}
    for value in values:
        key = math.floor((value - low) / bucket)
        counts[key] = counts.get(key, 0) + 1
    top = max(counts.values())
    print()
    print(f'{label} histogram (run {name}, scene {scene}, {len(values)} samples, bucket {bucket:g} ms)')
    for key in sorted(counts):
        start = low + key * bucket
        count = counts[key]
        bar = '#' * (round(count / top * width) if count else 0)
        print(f'{start:7.1f} - {start + bucket:7.1f} | {count:5d} {bar}')


def print_per_frame(runs):
    if not any(run.frametime_rows for run in runs):
        return
    print()
    print('per frame (frametime.csv of the run folder, GT4_HW_FRAMETIME_LOG: intervals between present/shown timestamps)')
    rows, histograms = [], []
    for run in runs:
        for column in ('present_ms', 'shown_ms'):
            intervals = frame_intervals(run.frametime_rows, column)
            stats = distribution([value for _, value in intervals])
            if not stats:
                continue
            kind = 'presented' if column == 'present_ms' else 'shown'
            rows.append([run.name, run.scene, kind, stats['n'], show(stats['mean']), show(stats['p50']),
                         show(stats['p95']), show(stats['p99']), show(stats['low_1pct_fps'], 1),
                         show(stats['max']), stats['over_budget'], stats['over_hard'],
                         show(stats['ratio_over_budget'])])
            histograms.append((run.name, run.scene, kind, [value for _, value in intervals]))
    print_table(['run', 'scene', 'kind', 'n', 'mean', 'p50', 'p95', 'p99', '1%low', 'max', '>16.7', '>20', 'ratio'],
                rows)
    for name, scene, kind, values in [entry for entry in histograms if entry[2] == 'presented']:
        print_histogram(name, scene, f'{kind} frame', values, 1.0)
    for name, scene, kind, values in histograms:
        if not values:
            continue
        print_stutter_line(name, scene, kind, values)


def print_stutter_line(name, scene, kind, values):
    stats = distribution(values)
    low = f'{stats["low_1pct_fps"]:.1f}' if stats['low_1pct_fps'] is not None else '-'
    print(f'stutter {kind} {name} ({scene}): {stats["over_budget"]} of {stats["n"]} frame intervals over '
          f'{BUDGET_MS:g} ms ({stats["ratio_over_budget"] * 100:.1f}%), {stats["over_hard"]} over {HARD_MS:g} ms; '
          f'1% low {low} fps; worst {stats["max"]:.1f} ms')


def print_spikes(runs, lo, hi):
    window = hi - lo + 1
    if not any(run.spikes for run in runs):
        return
    print()
    print('spikes ([core2] spike lines; a complete per-VBlank trace needs GT4_CORE2_SPIKE_MS=0.01 and a high '
          'GT4_CORE2_SPIKE_LINES)')
    rows = []
    for run in runs:
        if not run.spikes:
            continue
        walls = [spike['wall'] for spike in run.spikes]
        gs = [spike['gs'] for spike in run.spikes]
        stats = distribution(walls)
        complete = len(run.spikes) / window >= SPIKE_COMPLETE
        worst = max(run.spikes, key=lambda spike: spike['wall'])
        rows.append([run.name, run.scene, len(run.spikes), window, f'{len(run.spikes) / window * 100:.1f}%',
                     'yes' if complete else 'no (censored)', show(stats['mean']), show(stats['p99'])
                     if complete else '-', show(stats['max']), worst['vblank'], show(max(gs))])
    print_table(['run', 'scene', 'lines', 'vblanks', 'coverage', 'complete', 'wall mean', 'wall p99', 'wall max',
                 'max vb', 'gs max'], rows)
    for run in runs:
        if run.spikes and len(run.spikes) / window < SPIKE_COMPLETE:
            print(f'hint: run {run.name} logged {len(run.spikes)} spikes in a {window}-VBlank window, so the sample is '
                  'not the whole story: lower GT4_CORE2_SPIKE_MS (16.7 for the budget, 0.01 for every VBlank) and '
                  'raise GT4_CORE2_SPIKE_LINES (default 40 per 600 VBlanks).')
            break


# ---- CSV -------------------------------------------------------------------------------------------------------

def reason_slug(reason):
    return reason.lower().replace(' ', '_')


def csv_fieldnames(runs):
    reasons = list(KNOWN_REASONS)
    for run in runs:
        for row in run.rows:
            for reason in row['waits']:
                if reason not in reasons:
                    reasons.append(reason)
    fields = ['run', 'scene', 'vblank_start', 'vblank_end', 'vblanks', 'wall_ms', 'gs_worker_ms', 'gs_exclusive_ms',
              'process_cpu_ms', 'ee_critical_path_ms', 'gpu_busy_ms', 'gpu_draw_passes_ms', 'gpu_wait_ms',
              'gpu_wait_not_readback_ms']
    fields += [f'gpu_wait_{reason_slug(reason)}_ms' for reason in reasons]
    fields += [f'readbacks_{reason_slug(reason)}_per_vblank' for reason in reasons]
    fields += ['submits', 'submits_per_vblank', 'uploads_per_vblank', 'clut_loads_per_vblank',
               'swizzle_kib_per_vblank', 'prims_per_vblank', 'fragments_per_vblank', 'batches', 'batches_per_vblank',
               'draws', 'draws_per_vblank', 'readbacks', 'readbacks_per_vblank']
    return fields, reasons


def csv_row(run, row, reasons):
    values = {'run': run.name, 'scene': run.scene, 'vblank_start': row['vblank_start'],
              'vblank_end': row['vblank_end'], 'vblanks': row['vblanks'], 'wall_ms': row['wall'],
              'gs_worker_ms': row.get('gs_worker'), 'gs_exclusive_ms': row['gs_exclusive'],
              'process_cpu_ms': row.get('process_cpu'), 'ee_critical_path_ms': row.get('ee_critical_path'),
              'gpu_busy_ms': row.get('gpu_busy'), 'gpu_draw_passes_ms': row.get('gpu_draw_passes'),
              'gpu_wait_ms': row.get('gpu_wait'), 'gpu_wait_not_readback_ms': row.get('not_readback'),
              'submits': row.get('submits_total'), 'submits_per_vblank': row.get('submits_timed'),
              'uploads_per_vblank': row.get('uploads'), 'clut_loads_per_vblank': row.get('clut_loads'),
              'swizzle_kib_per_vblank': row.get('swizzle_kib'), 'prims_per_vblank': row.get('prims'),
              'fragments_per_vblank': row.get('fragments'), 'batches': row.get('batches_total'),
              'batches_per_vblank': row.get('batches_per_vblank'), 'draws': row.get('draws_total'),
              'draws_per_vblank': row.get('draws_per_vblank'), 'readbacks': row.get('readbacks_total'),
              'readbacks_per_vblank': row.get('readbacks_per_vblank')}
    for reason in reasons:
        values[f'gpu_wait_{reason_slug(reason)}_ms'] = row['waits'].get(reason)
        values[f'readbacks_{reason_slug(reason)}_per_vblank'] = row['readbacks_by_reason'].get(reason)
    return values


def format_csv(value):
    if value is None:
        return ''
    if isinstance(value, str):
        return value
    if isinstance(value, int):
        return str(value)
    return f'{value:.3f}'


def write_csv(runs, path):
    fields, reasons = csv_fieldnames(runs)
    with open(path, 'w', newline='') as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for run in runs:
            for row in run.rows:
                values = csv_row(run, row, reasons)
                writer.writerow({key: format_csv(values.get(key)) for key in fields})
    return sum(len(run.rows) for run in runs)


SPIKE_FIELDS = ['run', 'scene', 'vblank', 'wall_ms', 'gs_ms', 'upload_ms', 'uploads', 'upload_kib', 'texture_ms',
                'readback_ms', 'translate_ms', 'draw_ms', 'targets_ms', 'clut_ms', 'patch_ms', 'gpu_wait_ms',
                'readbacks', 'readback_download', 'readback_store', 'readback_keep', 'created', 'images', 'images_mid',
                'allocated', 'decodes', 'submits', 'glm_writes', 'dirty', 'gpu_busy_ms', 'fragments', 'prims']


def write_spike_csv(runs, path):
    spikes = [(run, spike) for run in runs for spike in run.spikes]
    if not spikes:
        return 0
    with open(path, 'w', newline='') as handle:
        writer = csv.DictWriter(handle, fieldnames=SPIKE_FIELDS)
        writer.writeheader()
        for run, spike in spikes:
            values = {'run': run.name, 'scene': run.scene, 'vblank': spike['vblank'], 'wall_ms': spike['wall'],
                      'gs_ms': spike['gs'], 'upload_ms': spike['upload'], 'uploads': int(spike['uploads']),
                      'upload_kib': int(spike['upload_kib']), 'texture_ms': spike['texture'],
                      'readback_ms': spike['readback'], 'translate_ms': spike['translate'], 'draw_ms': spike['draw'],
                      'targets_ms': spike['targets'], 'clut_ms': spike['clut'], 'patch_ms': spike['patch'],
                      'gpu_wait_ms': spike['gpu_wait'], 'readbacks': spike['readbacks'],
                      'readback_download': spike['readback_download'], 'readback_store': spike['readback_store'],
                      'readback_keep': spike['readback_keep'], 'created': spike['created'], 'images': spike['images'],
                      'images_mid': spike['images_mid'] if spike['images_mid'] is not None else '',
                      'allocated': spike['allocated'], 'decodes': spike['decodes'], 'submits': spike['submits'],
                      'glm_writes': spike['glm_writes'], 'dirty': spike['dirty'], 'gpu_busy_ms': spike['gpu_busy'],
                      'fragments': spike['fragments'], 'prims': spike['prims']}
            writer.writerow({key: format_csv(values.get(key)) for key in SPIKE_FIELDS})
    return len(spikes)


# ---- entry point -----------------------------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(description='Stutter-aware summary of the CoreGS 2.0 report lines (TASK-215e).')
    parser.add_argument('names', nargs='+', help="tags of night-run.sh/series.sh or a run's folder name")
    parser.add_argument('--scene', default=None,
                        help='scene name for every run (default: the run folder scene.txt, then GT4_BENCH_SCENE, '
                             'then night)')
    parser.add_argument('--csv', default=os.environ.get('GT4_BENCH_CSV') or None,
                        help='write one row per report interval to this CSV (env GT4_BENCH_CSV)')
    parser.add_argument('--worst', type=int, default=5, help='worst intervals per run to list (0: none)')
    parser.add_argument('--stats', default='timing', choices=('all', 'timing'),
                        help="timing: only the stutter metrics (default); all: every metric of the old table (its "
                             "budget counts only mean something for the millisecond metrics)")
    parser.add_argument('--lo', type=int, default=None, help='first VBlank of the window (env SUMMARY_LO)')
    parser.add_argument('--hi', type=int, default=None, help='last VBlank of the window (env SUMMARY_HI)')
    parser.add_argument('--buckets', type=float, default=1.0, help='histogram bucket in ms (at least 0.1)')
    parser.add_argument('--table-only', action='store_true', help='only the TASK-027 averages table (old output)')
    args = parser.parse_args(argv)
    bucket = max(0.1, args.buckets)

    lo, hi = summary_window()
    lo = args.lo if args.lo is not None else lo
    hi = args.hi if args.hi is not None else hi
    runs, missing = [], []
    for name in args.names:
        folder = run_folder(name)
        if not (folder / 'log.txt').exists():
            print(f'cpu_summary: no log at {folder / "log.txt"}', file=sys.stderr)
            missing.append(name)
        else:
            runs.append(analyse_run(name, lo, hi, scene=args.scene))
    if not runs:
        return 1
    print_old_table([run.name for run in runs], {run.name: run.average for run in runs})
    if not args.table_only:
        print_stutter(runs, lo, hi)
        print_stats(runs, STUTTER_METRICS if args.stats == 'timing' else 'all')
        print_worst(runs, args.worst)
        for run in runs:
            print_histogram(run.name, run.scene, 'wall interval', [row['wall'] for row in run.rows
                                                                   if row['wall'] is not None], bucket)
        print_per_frame(runs)
        print_spikes(runs, lo, hi)
    if args.csv:
        rows = write_csv(runs, args.csv)
        print(f'csv: {args.csv} ({rows} rows)')
        sidecar = Path(args.csv).with_name(Path(args.csv).stem + '-spikes.csv')
        spikes = write_spike_csv(runs, sidecar)
        if spikes:
            print(f'spikes csv: {sidecar} ({spikes} rows)')
    return 1 if missing else 0



if __name__ == '__main__':
    sys.exit(main())
