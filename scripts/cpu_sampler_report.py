"""Per-thread CPU profiles from a GT4_CPU_SAMPLER file (TASK-027 CPU survey).

The game's sampler (native/hw_gs/hw_cpu_sampler.cpp) writes, for one window of VBlanks, the module table, the thread
table (with each thread's CPU time in the window) and the sampled stacks (absolute return addresses, identical stacks
merged with their count and the CPU cycles they stand for). This script symbolises the addresses of the game
executable with llvm-symbolizer (the build needs line tables: /Z7 -gline-tables-only and /DEBUG), names system DLL
addresses by their nearest export, and prints for every busy thread its CPU time per VBlank and the functions that
use it:

  * exclusive: the function running when the sample was taken (the innermost inlined function at the leaf);
  * inclusive: every function on the stack, counted once per sample (inlined functions included).

Each sample is weighted by the cycles its thread used since the previous visit, so a thread that was mostly idle in a
period counts little; milliseconds are the thread's CPU time per VBlank times the function's share of the weights.

usage: cpu_sampler_report.py <sampler file> [--exe gt4vulkan.exe] [--top 40] [--threads N] [--min-ms 0.05]
       [--symbolizer path] [--focus REGEX] [--out report.txt]
"""
import argparse
import collections
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

# A system DLL address further than this from the export below it is internal code with no public name.
NEAR_EXPORT = 0x400
DEFAULT_SYMBOLIZER = r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe'
# Frames of the C++ runtime's thread start-up, skipped when a thread is named after its entry function.
# Library frames skipped by --blame when looking for the code that asked for the matched work.
LIBRARY_FRAME = re.compile(r'^(std::|operator new|operator delete|malloc|free|_malloc|`|__|memcpy|memmove|memset)|'
                           r'\.dll[!( ]', re.IGNORECASE)
THREAD_PLUMBING = re.compile(r'^(std::|invoke|__|_?thread_start|BaseThreadInitThunk|RtlUserThreadStart|'
                             r'thread_start|\?\?)')


class SamplerFile:
    def __init__(self):
        self.header = {}
        self.modules = []  # (base, size, path)
        self.threads = {}  # tid -> dict
        self.stacks = []   # (tid, count, cycles, [addresses])


def parse(text):
    data = SamplerFile()
    for line in text.splitlines():
        if not line:
            continue
        kind, _, rest = line.partition(' ')
        if kind == 'gt4-cpu-sampler':
            for field in rest.split()[1:]:
                key, _, value = field.partition('=')
                data.header[key] = float(value) if '.' in value else int(value)
        elif kind == 'module':
            base, size, path = rest.split(' ', 2)
            data.modules.append((int(base, 16), int(size, 16), path))
        elif kind == 'thread':
            tid, start, cycles, samples, cpu_ms, name = rest.split(' ', 5)
            data.threads[int(tid)] = {'start': int(start, 16), 'cycles': int(cycles), 'samples': int(samples),
                                      'cpu_ms': float(cpu_ms), 'name': name}
        elif kind == 's':
            parts = rest.split()
            data.stacks.append((int(parts[0]), int(parts[1]), int(parts[2]), [int(a, 16) for a in parts[3:]]))
    return data


def module_of(modules, address):
    for base, size, path in modules:
        if base <= address < base + size:
            return base, path
    return None, None


def pe_image_base(path):
    with open(path, 'rb') as f:
        head = f.read(4096)
    pe = struct.unpack_from('<I', head, 0x3C)[0]
    magic = struct.unpack_from('<H', head, pe + 24)[0]
    return struct.unpack_from('<Q', head, pe + 24 + 24)[0] if magic == 0x20B else struct.unpack_from('<I', head, pe + 52)[0]


def pe_exports(path):
    """Sorted (rva, name) of a DLL's named exports; empty when the file cannot be read."""
    try:
        blob = Path(path).read_bytes()
    except OSError:
        return []
    pe = struct.unpack_from('<I', blob, 0x3C)[0]
    optional = pe + 24
    magic = struct.unpack_from('<H', blob, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    export_rva, export_size = struct.unpack_from('<II', blob, directories)
    if export_rva == 0:
        return []
    sections_at = optional + struct.unpack_from('<H', blob, pe + 20)[0]
    count = struct.unpack_from('<H', blob, pe + 6)[0]
    sections = []
    for i in range(count):
        at = sections_at + 40 * i
        vsize, va, rsize, raw = struct.unpack_from('<IIII', blob, at + 8)
        sections.append((va, max(vsize, rsize), raw))

    def offset(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(rva)

    try:
        e = offset(export_rva)
        names_count = struct.unpack_from('<I', blob, e + 24)[0]
        functions_rva, names_rva, ordinals_rva = struct.unpack_from('<III', blob, e + 28)
        functions, names, ordinals = offset(functions_rva), offset(names_rva), offset(ordinals_rva)
        result = []
        for i in range(names_count):
            name_at = offset(struct.unpack_from('<I', blob, names + 4 * i)[0])
            name = blob[name_at:blob.index(b'\0', name_at)].decode('ascii', 'replace')
            ordinal = struct.unpack_from('<H', blob, ordinals + 2 * i)[0]
            rva = struct.unpack_from('<I', blob, functions + 4 * ordinal)[0]
            if not (export_rva <= rva < export_rva + export_size):  # skip forwarders
                result.append((rva, name))
        return sorted(result)
    except (ValueError, struct.error):
        return []


def llvm_symbolize(exe, addresses, symbolizer):
    """{preferred VA: [innermost .. outermost function names]} for the executable's addresses."""
    if not addresses:
        return {}
    command = [symbolizer, '--obj=' + str(Path(exe).resolve()), '--output-style=JSON', '--demangle', '--functions=linkage',
               '--inlining']
    text = '\n'.join(hex(a) for a in addresses)
    # The executable names its PDB without a folder (/PDBALTPATH:%_PDB%), so look it up next to the executable.
    run = subprocess.run(command, input=text, capture_output=True, text=True, check=True, cwd=str(Path(exe).parent))
    names = {}
    for line in run.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        entry = json.loads(line)
        frames = [s.get('FunctionName') or '??' for s in entry.get('Symbol', [])]
        names[int(entry['Address'], 16)] = frames or ['??']
    return names


class Resolver:
    def __init__(self, data, exe, symbolizer, symbolize=llvm_symbolize):
        self.data = data
        self.exe = Path(exe) if exe else None
        self.symbolizer = symbolizer
        self.symbolize = symbolize
        self.cache = {}
        self.exports = {}
        self.exe_base = None
        self.exe_preferred = None
        if self.exe:
            for base, size, path in data.modules:
                if Path(path).name.lower() == self.exe.name.lower():
                    self.exe_base = base
            if self.exe_base is not None:
                self.exe_preferred = pe_image_base(self.exe)

    def prepare(self):
        """Symbolises every executable address in one llvm-symbolizer run.

        Frames after the leaf are return addresses; they are looked up one byte earlier (inside the call instruction),
        so the call site is named rather than whatever inlined code follows the call."""
        if self.exe_base is None:
            return
        wanted = set()
        for _, _, _, frames in self.data.stacks:
            if frames:
                wanted.add(frames[0])
            wanted.update(a - 1 for a in frames[1:])
        for t in self.data.threads.values():
            wanted.add(t['start'])
        exe_addresses = sorted(a for a in wanted if module_of(self.data.modules, a)[0] == self.exe_base)
        preferred = [a - self.exe_base + self.exe_preferred for a in exe_addresses]
        names = self.symbolize(self.exe, preferred, self.symbolizer)
        for address, va in zip(exe_addresses, preferred):
            self.cache[address] = names.get(va, ['??'])

    def frames(self, address):
        """Function names at one address, innermost first (several when the address is inside inlined code)."""
        if address in self.cache:
            return self.cache[address]
        base, path = module_of(self.data.modules, address)
        if base is None:
            names = ['[unknown]']
        else:
            module = Path(path).name
            if path not in self.exports:
                self.exports[path] = pe_exports(path) if module.lower() != (self.exe.name.lower() if self.exe else '') else []
            table = self.exports[path]
            rva = address - base
            name = None
            lo, hi = 0, len(table)
            while lo < hi:
                mid = (lo + hi) // 2
                if table[mid][0] <= rva:
                    lo = mid + 1
                else:
                    hi = mid
            if lo > 0 and rva - table[lo - 1][0] < NEAR_EXPORT:
                name = table[lo - 1][1]
            names = [f'{module}!{name}' if name else f'{module} (internal)']
        self.cache[address] = names
        return names


def stack_names(resolver, frames):
    """Innermost-first function names of one stack (return addresses looked up at address - 1)."""
    names = []
    for i, address in enumerate(frames):
        names.extend(resolver.frames(address if i == 0 else address - 1))
    return names


def thread_label(data, resolver, tid):
    t = data.threads[tid]
    if t['name'] not in ('-', ''):
        return t['name']
    # Name an unnamed thread after the outermost interesting function of its most common stack.
    best = None
    for stack_tid, count, cycles, frames in data.stacks:
        if stack_tid == tid and (best is None or cycles > best[0]):
            best = (cycles, frames)
    candidates = []
    if best:
        for name in reversed(stack_names(resolver, best[1])):
            if not THREAD_PLUMBING.match(name) and '.dll' not in name.lower() and '[' not in name[:1]:
                candidates.append(name)
    start = resolver.frames(t['start'])[0] if t['start'] else ''
    label = candidates[0] if candidates else start or f'tid{tid}'
    return label[:60]


def profile(data, resolver, focus=None, blame=None):
    """Per-thread exclusive and inclusive weights by function.

    focus: caller -> function -> callee chains of the first matching frame. blame: for samples with a matching frame,
    the nearest caller of it that is not library code (std::, the allocator, system DLLs)."""
    result = {}
    focus_re = re.compile(focus) if focus else None
    blame_re = re.compile(blame) if blame else None
    for tid, count, cycles, frames in data.stacks:
        entry = result.setdefault(tid, {'weight': 0, 'exclusive': collections.Counter(),
                                        'inclusive': collections.Counter(), 'callers': collections.Counter(),
                                        'blame': collections.Counter()})
        entry['weight'] += cycles
        expanded = stack_names(resolver, frames)
        if not expanded:
            continue
        entry['exclusive'][expanded[0]] += cycles
        for name in set(expanded):
            entry['inclusive'][name] += cycles
        if blame_re:
            for i, name in enumerate(expanded):
                if blame_re.search(name):
                    owner = next((n for n in expanded[i + 1:] if not LIBRARY_FRAME.search(n)), '[root]')
                    entry['blame'][owner] += cycles
                    break
        if focus_re:
            for i, name in enumerate(expanded):
                if focus_re.search(name):
                    caller = expanded[i + 1] if i + 1 < len(expanded) else '[root]'
                    callee = expanded[i - 1] if i > 0 else '[self]'
                    entry['callers'][f'{caller}  ->  {name}  ->  {callee}'] += cycles
                    break
    return result


def report(data, resolver, top=40, threads=8, min_ms=0.05, focus=None, blame=None):
    vblanks = max(1, int(data.header.get('window_vblanks', 1)))
    lines = []
    h = data.header
    lines.append(f"window: {vblanks} VBlanks, {h.get('window_us', 0) / 1000.0:.0f} ms wall "
                 f"({h.get('window_us', 0) / 1000.0 / vblanks:.2f} ms/VBlank); samples {h.get('samples')}, "
                 f"dropped {h.get('dropped')}, period {h.get('period_us')} us, "
                 f"suspended {h.get('suspended_us', 0) / max(1, h.get('suspensions', 1)):.1f} us per visit")
    profiles = profile(data, resolver, focus, blame)
    order = sorted(data.threads, key=lambda tid: -data.threads[tid]['cpu_ms'])
    total_cpu = sum(t['cpu_ms'] for t in data.threads.values())
    lines.append(f'process CPU in the window: {total_cpu / vblanks:.2f} ms/VBlank (threads seen: {len(data.threads)})')
    lines.append('')
    lines.append('threads by CPU (ms/VBlank):')
    for tid in order:
        t = data.threads[tid]
        if t['cpu_ms'] / vblanks < min_ms:
            continue
        lines.append(f"  {t['cpu_ms'] / vblanks:7.3f}  tid {tid:<6} samples {t['samples']:<7} {thread_label(data, resolver, tid)}")
    for tid in order[:threads]:
        t = data.threads[tid]
        p = profiles.get(tid)
        if not p or p['weight'] == 0:
            continue
        cpu = t['cpu_ms'] / vblanks
        lines.append('')
        lines.append(f"== {thread_label(data, resolver, tid)} (tid {tid}): {cpu:.3f} ms/VBlank CPU, "
                     f"{t['samples']} samples")
        for kind in ('exclusive', 'inclusive'):
            lines.append(f'  -- {kind} (ms/VBlank, share)')
            for name, weight in p[kind].most_common(top):
                share = weight / p['weight']
                ms = cpu * share
                if ms < min_ms and kind == 'inclusive':
                    break
                lines.append(f'  {ms:7.3f} {share * 100:5.1f}%  {name[:150]}')
        if blame and p['blame']:
            lines.append(f'  -- blame {blame}: nearest non-library caller')
            for owner, weight in p['blame'].most_common(top):
                lines.append(f"  {cpu * weight / p['weight']:7.3f}  {owner[:200]}")
        if focus and p['callers']:
            lines.append(f'  -- focus {focus}: caller -> match -> callee')
            for chain, weight in p['callers'].most_common(top):
                lines.append(f"  {cpu * weight / p['weight']:7.3f}  {chain[:220]}")
    return '\n'.join(lines) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('samples')
    parser.add_argument('--exe', help='the game executable the samples came from (with its PDB next to it)')
    parser.add_argument('--top', type=int, default=40)
    parser.add_argument('--threads', type=int, default=8)
    parser.add_argument('--min-ms', type=float, default=0.05)
    parser.add_argument('--symbolizer', default=DEFAULT_SYMBOLIZER)
    parser.add_argument('--focus', help='regex: also list caller -> function -> callee chains for matching functions')
    parser.add_argument('--blame', help='regex: attribute matching work (e.g. the allocator) to its nearest caller')
    parser.add_argument('--out')
    args = parser.parse_args(argv)
    data = parse(Path(args.samples).read_text(encoding='utf-8', errors='replace'))
    resolver = Resolver(data, args.exe, args.symbolizer)
    resolver.prepare()
    text = report(data, resolver, args.top, args.threads, args.min_ms, args.focus, args.blame)
    if args.out:
        Path(args.out).write_text(text, encoding='utf-8')
    sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
