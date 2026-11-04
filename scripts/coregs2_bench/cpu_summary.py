"""Race-window averages of the report lines of night runs (VBlank intervals ending in [LO, HI]).

usage: cpu_summary.py <tag> ...   (the tags of night-run.sh or series.sh, or a run's folder name such as spec2-george-a1,
under GT4_BENCH_OUT). It averages the reports, so a run needs GT4_MTVU_REPORT_EVERY and GT4_HW_PROFILE_EVERY (series.sh
sets them to 600; the [core2] lines are always per 600 VBlanks). SUMMARY_LO and SUMMARY_HI set the window (default the
night race, 7800 to 11400); George V Paris races from 20400 to about 21300 (spec2-run.sh).
"""
import os
import re
import sys
from pathlib import Path

LO, HI = int(os.environ.get('SUMMARY_LO', 7800)), int(os.environ.get('SUMMARY_HI', 11400))
ROOT = Path(os.environ.get('GT4_BENCH_OUT', Path(__file__).resolve().parents[2] / 'build' / 'evidence'))


def num(s):
    return float(s)


def summarise(name):
    folder = ROOT / f'night-{name}'
    t = ((folder if folder.exists() else ROOT / name) / 'log.txt').read_text(encoding='utf-8', errors='replace')
    out = {}

    def avg(key, values):
        if values:
            out[key] = sum(values) / len(values)

    # [hw] cost / transport lines carry their vsync
    cost = [(int(m.group(1)), num(m.group(2)), num(m.group(3))) for m in
            re.finditer(r'cost vsync=(\d+) gs_thread_cpu_ms=([\d.]+) process_cpu_core_ms=([\d.]+)', t)]
    cost = [c for c in cost if LO <= c[0] <= HI]
    avg('gs_cpu', [c[1] for c in cost])
    avg('process_cpu', [c[2] for c in cost])
    tr = [m for m in re.finditer(r'transport vsync=(\d+) ee: (.*?) \| worker: (.*?max_depth=\d+)', t)
          if LO <= int(m.group(1)) <= HI]
    for field in ('gif_cmds', 'gif_kib', 'gif_ms', 'copy_ms', 'drain_wait_ms', 'read_fifo_ms', 'wake_pushes',
                  'vsync_wait_ms'):
        avg('ee_' + field, [num(re.search(field + r'=([\d.]+)', m.group(2)).group(1)) for m in tr])
    for field in ('gif_ms', 'idle_ms', 'wakeups'):
        avg('worker_' + field, [num(re.search(field + r'=([\d.]+)', m.group(3)).group(1)) for m in tr])
    # [core2] lines have no vsync: take the ones after the matching [core2] vblank= counter line
    blocks = []
    for m in re.finditer(r'core2\] vblank=(\d+) ', t):
        vb = int(m.group(1))
        rest = t[m.end():m.end() + 20000]
        w = re.search(r'core2\] wall ms/vblank=([\d.]+)', rest)
        x = re.search(r'core2\] exclusive ms/vblank (.*?)\n', rest)
        blocks.append((vb, w, x))
    blocks = [b for b in blocks if LO <= b[0] <= HI]
    avg('wall', [num(b[1].group(1)) for b in blocks if b[1]])
    ex = {}
    for b in blocks:
        if b[2]:
            for k, v in re.findall(r'(\w+)=([\d.]+)', b[2].group(1)):
                ex.setdefault(k, []).append(num(v))
    for k, v in ex.items():
        avg('x_' + k, v)
    # EE-side MTVU and perf lines (no vsync on them; take the race-time ones by position after vsync LO)
    start = t.find(f'vsync={LO}')
    tail = t[start:] if start >= 0 else t
    avg('ee_gs_drain', [num(v) for v in re.findall(r'EE gs_drain ms/frame=([\d.]+)', tail)])
    avg('ee_mtvu_wait_download', [num(v) for v in re.findall(r'EE wait ms/frame: vblank=[\d.]+ download=([\d.]+)', tail)])
    avg('ee_critical_path', [num(v) for v in re.findall(r'critical path=([\d.]+)', tail)])
    avg('ee_thread_cpu', [num(v) for v in re.findall(r'measured_thread_cpu_ms/frame=([\d.]+)', tail)])
    ee = {}
    for line in re.findall(r'perf\] exclusive wall ms/frame[^:]*: (.*)', tail):
        for k, v in re.findall(r'(\w+)=([\d.]+)', line):
            if k in ('ee_dispatch', 'iop_run', 'vif_gif', 'vu0_aot', 'run_due_events', 'kernel_service', 'host_vblank', 'syscall_dispatch'):
                ee.setdefault(k, []).append(num(v))
    for k, v in ee.items():
        avg('xe_' + k, v)
    avg('frame_p50', [num(a) for a, b in re.findall(r'frame ms: median ([\d.]+) p95 ([\d.]+)', tail)])
    avg('frame_p95', [num(b) for a, b in re.findall(r'frame ms: median ([\d.]+) p95 ([\d.]+)', tail)])
    gw, gb, gd = [], [], []
    for m in re.finditer(r'core2\] vblank=(\d+) ', t):
        if LO <= int(m.group(1)) <= HI:
            g = re.search(r'gpu_wait=([\d.]+)', t[m.end():m.end() + 20000])
            if g: gw.append(num(g.group(1)))
            b = re.search(r'gpu_busy=([\d.]+) \([\d.]+ submits\) gpu_draw_passes=([\d.]+)', t[m.end():m.end() + 20000])
            if b:
                gb.append(num(b.group(1)))
                gd.append(num(b.group(2)))
    avg('gpu_wait', gw)
    reasons = {}
    for m in re.finditer(r'core2\] vblank=(\d+) ', t):
        if LO <= int(m.group(1)) <= HI:
            line = re.search(r'gpu_wait ms/vblank by readback reason \(readbacks/vblank\):(.*?)\n', t[m.end():m.end() + 20000])
            if line:
                for reason, wait, count in re.findall(r' ([a-z ]+?)=([\d.]+) \(([\d.]+)\)', line.group(1)):
                    reasons.setdefault('wait_' + reason.replace(' ', '_'), []).append(num(wait))
                    reasons.setdefault('readbacks_' + reason.replace(' ', '_'), []).append(num(count))
    for k, v in reasons.items():
        avg(k, v)
    avg('gpu_busy', gb)
    avg('gpu_draw_passes', gd)
    avg('mtvu_worker_busy', [num(v) for v in re.findall(r'worker ms/frame: vu1_program=[\d.]+ output_path12=[\d.]+ busy=([\d.]+)', tail)])
    avg('mtvu_worker_cpu', [num(v) for v in re.findall(r'worker thread cpu ms/frame=([\d.]+)', tail)])
    avg('mtvu_vu1_program', [num(v) for v in re.findall(r'worker ms/frame: vu1_program=([\d.]+)', tail)])
    avg('mtvu_output_path12', [num(v) for v in re.findall(r'worker ms/frame: vu1_program=[\d.]+ output_path12=([\d.]+)', tail)])
    avg('w2ee_wakeups', [num(v) for v in re.findall(r'worker_to_ee rings=[\d.]+ waits=[\d.]+ wakeups=([\d.]+)', tail)])
    avg('w2ee_notifies', [num(v) for v in re.findall(r'worker_to_ee rings=[\d.]+ waits=[\d.]+ wakeups=[\d.]+ notifies=([\d.]+)', tail)])
    return out


names = sys.argv[1:]
rows = {n: summarise(n) for n in names}
keys = []
for r in rows.values():
    for k in r:
        if k not in keys:
            keys.append(k)
print('metric'.ljust(26) + ''.join(n[:14].rjust(15) for n in names))
for k in keys:
    print(k.ljust(26) + ''.join((f'{rows[n][k]:.3f}' if k in rows[n] else '-').rjust(15) for n in names))
