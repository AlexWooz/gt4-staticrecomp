"""Frametime graphs and figures from GT4_HW_FRAMETIME_LOG files (the game's own log, no overlay needed).

usage: frametime_plot.py <out.png> <name>=<csv>[:<first vblank>:<last vblank>] ...

`presented` is the time between vkQueuePresentKHR calls, what RivaTuner draws. `shown` is the time between the frames
the display took (VK_KHR_present_wait2); a frame that never reached the display shows up as an interval near 0 followed by
a long one, and is counted as dropped. Prints one line of figures per run. The first frames of a file (session start) and
frames outside the given VBlanks are left out.
"""
import csv
import statistics
import sys

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt

REFRESH_MS = 1000.0 / 60.0


def read_run(spec):
    name, _, rest = spec.partition('=')
    parts = rest.rsplit(':', 2)
    if len(parts) == 3 and parts[1].isdigit() and parts[2].isdigit():
        path, first, last = parts[0], int(parts[1]), int(parts[2])
    else:
        path, first, last = rest, 0, 1 << 62
    with open(path, newline='') as handle:
        rows = [r for r in csv.DictReader(handle) if first <= int(r['vblank']) <= last]
    return name, rows


def intervals(rows, column):
    times = [(int(r['vblank']), float(r[column])) for r in rows if float(r[column]) > 0.0]
    return [(b[0], b[1] - a[1]) for a, b in zip(times, times[1:])]


def figures(values):
    ordered = sorted(values)
    pick = lambda p: ordered[min(int(p * len(ordered)), len(ordered) - 1)]
    steps = [abs(a - b) for a, b in zip(values, values[1:])]
    return {
        'n': len(values), 'mean': statistics.mean(values), 'stdev': statistics.pstdev(values), 'p01': pick(0.01), 'p99': pick(0.99),
        'max': ordered[-1], 'over17': sum(v > 17.5 for v in values), 'over20': sum(v > 20.0 for v in values),
        'over25': sum(v > 25.0 for v in values), 'under12': sum(v < 12.0 for v in values), 'step': statistics.mean(steps) if steps else 0.0,
    }


def describe(name, kind, f):
    return ('%-12s %-9s n=%5d mean=%6.3f stdev=%5.2f p1=%5.1f p99=%5.1f max=%5.1f >17.5=%4d >20=%4d >25=%4d <12=%4d step=%4.2f' % (
        name, kind, f['n'], f['mean'], f['stdev'], f['p01'], f['p99'], f['max'], f['over17'], f['over20'], f['over25'], f['under12'], f['step']))


def main():
    out = sys.argv[1]
    runs = [read_run(spec) for spec in sys.argv[2:]]
    figure, axes = plt.subplots(len(runs), 1, figsize=(15, 3.2 * len(runs)), squeeze=False, sharey=True)
    for axis, (name, rows) in zip(axes[:, 0], runs):
        presented = intervals(rows, 'present_ms')
        shown = intervals(rows, 'shown_ms')
        if presented:
            f = figures([v for _, v in presented])
            print(describe(name, 'presented', f))
            axis.plot([x for x, _ in presented], [v for _, v in presented], linewidth=0.7, color='#c0392b', label='presented (RivaTuner) p99 %.1f max %.1f' % (f['p99'], f['max']))
        if shown:
            f = figures([v for _, v in shown])
            print(describe(name, 'shown', f))
            axis.plot([x for x, _ in shown], [v for _, v in shown], linewidth=0.7, color='#2471a3', alpha=0.8, label='shown (display) p99 %.1f max %.1f' % (f['p99'], f['max']))
        lag = sorted(float(r['shown_ms']) - float(r['present_ms']) for r in rows if float(r['shown_ms']) > 0)
        if lag:
            print('%-12s present to shown: mean %.1f ms, p1 %.1f, p99 %.1f' % (name, statistics.mean(lag), lag[len(lag) // 100], lag[len(lag) * 99 // 100]))
        axis.axhline(REFRESH_MS, color='#555555', linewidth=0.5)
        axis.set_ylim(0, 45)
        axis.set_ylabel('ms')
        axis.set_title(name, loc='left', fontsize=10)
        axis.legend(loc='upper right', fontsize=8)
    axes[-1, 0].set_xlabel('VBlank')
    figure.tight_layout()
    figure.savefig(out, dpi=90)


if __name__ == '__main__':
    main()
