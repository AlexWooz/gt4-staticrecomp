"""Read-only ledger of EE function entries that have no translated entry (TASK-014 stage B, Phase 1).

The translator emits code only for addresses it was told about. A call to any other address
halts with "no translated code". This script lists candidate function starts that have no
translated entry today, so that Phase 2 can decide which ones to add. It changes no pin,
no generated file and no build input.

Sources, each recorded per candidate:
  decomp       function addresses from the name table (docs/data/ee-names.csv) and from the
               gt4-decomp clone (config/symbol_addrs.txt, config/adhoc_methods.txt) inside the text;
  fallthrough  the word after the delay slot of an unconditional jump (jr ra, j, b) when no branch
               or direct jump targets it and it opens a stack frame (addiu/daddiu sp,sp,-N);
  dataword     aligned words in non-executable load segments whose value is a non-zero word inside
               the text (vtables, function tables, callback registrations);
  luipair      lui followed by addiu or ori on the same register (up to 3 instructions later, no
               intervening write) that builds an address inside the text, over the whole text.

Translated entries are read from the generated units: every `case N: goto L_XXXXXXXX;` label is a
registered entry (the same rule as render_contract_inventory.resolve_dispatch).

Confidence labels (see docs/EE-ENTRY-LEDGER.md):
  high    stack frame and (decomp name, fallthrough, or at least two sources);
  medium  stack frame alone, or a decomp name without a stack frame;
  low     one weak source and no stack frame (most data false positives land here);
  rejected the word at the candidate is zero or all ones (certain false positive).

Usage:
  python scripts/ee_entry_ledger.py --assets-root C:/Antigravity/gt4-staticrecomp \
      --decomp C:/Antigravity/gt4-staticrecomp/private/slice151-research/gt4-decomp \
      --names C:/Antigravity/gt4-staticrecomp/private/slice207-ee-ledger/ee-names-e0b70c4d6.csv \
      --output-dir C:/Antigravity/gt4-staticrecomp/private/slice207-ee-ledger
  add --spot-check N to print N seeded new candidates with a short disassembly.
"""

import argparse
import csv
import hashlib
import json
import random
import re
import struct
import subprocess
import sys
from collections import Counter, namedtuple
from itertools import combinations
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ee_symbols import parse_adhoc_methods, parse_symbol_addrs  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
ELF_RELATIVE = 'private/reconstructed/SCUS_973.28.elf'
GENERATED_RELATIVE = 'private/reference-acceleration-study/generated'
NAMES_RELATIVE = 'docs/data/ee-names.csv'
LEDGER_NAME = 'ee-entry-ledger.json'
SUMMARY_NAME = 'ee-entry-ledger-summary.json'
SOURCES = ('decomp', 'fallthrough', 'dataword', 'luipair')

PT_LOAD = 1
PF_EXECUTE = 1
OP_SPECIAL, OP_REGIMM, OP_J, OP_JAL = 0x00, 0x01, 0x02, 0x03
OP_BEQ, OP_BNE, OP_BLEZ, OP_BGTZ = 0x04, 0x05, 0x06, 0x07
OP_BRANCH_LIKELY = (0x14, 0x15, 0x16, 0x17)
OP_COP0, OP_COP1, OP_COP2, OP_COP3 = 0x10, 0x11, 0x12, 0x13
OP_ADDIU, OP_DADDIU, OP_ORI, OP_LUI = 0x09, 0x19, 0x0D, 0x0F
STORE_OPS = frozenset(range(0x28, 0x30)) | {0x1F, 0x31, 0x39, 0x3F}
NO_DESTINATION_SPECIAL = frozenset({0x08, 0x18, 0x19, 0x1A, 0x1B})  # jr, mult, multu, div, divu
REGISTER_NAMES = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5',
                  't6', 't7', 's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1',
                  'gp', 'sp', 's8', 'ra']
SP = 29
JR_RA = 0x03E00008

Segment = namedtuple('Segment', 'address body executable')


def sign16(value):
    return value - 0x10000 if value & 0x8000 else value


def fields(word):
    return {'op': word >> 26, 'rs': (word >> 21) & 31, 'rt': (word >> 16) & 31,
            'rd': (word >> 11) & 31, 'sa': (word >> 6) & 31, 'funct': word & 63,
            'imm': word & 0xFFFF, 'simm': sign16(word & 0xFFFF)}


def is_stack_frame(word):
    """addiu or daddiu sp,sp,-N: the usual function prologue on EE code."""
    parts = fields(word)
    return parts['op'] in (OP_ADDIU, OP_DADDIU) and parts['rs'] == SP and parts['rt'] == SP and parts['simm'] < 0


def is_placeholder_word(word):
    return word in (0, 0xFFFFFFFF)


def load_segments(elf):
    """PT_LOAD segments of an ELF32 little-endian MIPS file. Bytes beyond p_filesz are zero and not kept."""
    if len(elf) < 52 or elf[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Expected ELF32 little-endian input')
    header = struct.unpack_from('<HHIIIIIHHHHHH', elf, 16)
    if header[0:3] != (2, 8, 1) or header[8] != 32 or header[9] > 64:
        raise ValueError('Unexpected MIPS ELF/program header')
    table, stride, count = header[4], header[8], header[9]
    if table + stride * count > len(elf):
        raise ValueError('Truncated ELF program headers')
    segments = []
    for index in range(count):
        kind, offset, address, _physical, size, memory, flags, _align = struct.unpack_from(
            '<8I', elf, table + index * stride)
        if kind != PT_LOAD:
            continue
        if offset + size > len(elf) or size > memory or address + memory > 0x100000000:
            raise ValueError('Invalid PT_LOAD bounds')
        body = elf[offset:offset + size]
        if flags & PF_EXECUTE and (address % 4 or size % 4):
            raise ValueError('Unaligned executable segment')
        segments.append(Segment(address, body, bool(flags & PF_EXECUTE)))
    if not any(segment.executable for segment in segments):
        raise ValueError('No executable segment')
    return segments


class Image:
    def __init__(self, segments):
        self.segments = segments

    def segment_covering(self, address, size=4):
        for segment in self.segments:
            if segment.address <= address and address + size <= segment.address + len(segment.body):
                return segment
        return None

    def word(self, address):
        segment = self.segment_covering(address)
        if segment is None or address % 4:
            return None
        return struct.unpack_from('<I', segment.body, address - segment.address)[0]

    def is_text(self, address):
        segment = self.segment_covering(address)
        return address % 4 == 0 and segment is not None and segment.executable

    def text_segments(self):
        return [segment for segment in self.segments if segment.executable]

    def data_segments(self):
        return [segment for segment in self.segments if not segment.executable]


def branch_target(word, pc):
    """Target of a conditional or always branch (beq, bne, blez, bgtz, *l, regimm, bc1/bc2), else None."""
    parts = fields(word)
    op = parts['op']
    if op in (OP_BEQ, OP_BNE, OP_BLEZ, OP_BGTZ) + OP_BRANCH_LIKELY:
        return (pc + 4 + parts['simm'] * 4) & 0xFFFFFFFF
    if op == OP_REGIMM or (op in (OP_COP1, OP_COP2) and parts['rs'] == 8):
        return (pc + 4 + parts['simm'] * 4) & 0xFFFFFFFF
    return None


def jump_target(word, pc):
    return ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)


def scan_control_flow(image):
    """Collect branch targets, direct jump targets, call targets and unconditional-jump addresses in text."""
    branch_targets, jump_targets, call_targets, unconditional = set(), set(), set(), []
    for segment in image.text_segments():
        for offset in range(0, len(segment.body) - 3, 4):
            pc = segment.address + offset
            word = struct.unpack_from('<I', segment.body, offset)[0]
            parts = fields(word)
            target = branch_target(word, pc)
            if target is not None:
                branch_targets.add(target)
            if parts['op'] == OP_J:
                jump_targets.add(jump_target(word, pc))
            if parts['op'] == OP_JAL:
                call_targets.add(jump_target(word, pc))
            is_b = parts['op'] == OP_BEQ and parts['rs'] == 0 and parts['rt'] == 0
            if word == JR_RA or parts['op'] == OP_J or is_b:
                unconditional.append(pc)
    return {'branch': branch_targets, 'jump': jump_targets, 'call': call_targets,
            'unconditional': unconditional}


def detect_fallthrough(image, flow):
    """Stack-frame openers right after an unconditional jump's delay slot, with no branch or jump targeting them."""
    excluded = flow['branch'] | flow['jump']
    hits, no_frame = {}, 0
    for pc in flow['unconditional']:
        slot, candidate = pc + 4, pc + 8
        if not (image.is_text(slot) and image.is_text(candidate)):
            continue
        if candidate in excluded:
            continue
        if not is_stack_frame(image.word(candidate)):
            no_frame += 1
            continue
        hits.setdefault(candidate, f'after jump at 0x{pc:08X}')
    return hits, no_frame


def detect_datawords(image):
    hits = {}
    for segment in image.data_segments():
        for offset in range(0, len(segment.body) - 3, 4):
            value = struct.unpack_from('<I', segment.body, offset)[0]
            if not image.is_text(value):
                continue
            target_word = image.word(value)
            if target_word is None or is_placeholder_word(target_word):
                continue
            hits.setdefault(value, f'word at 0x{segment.address + offset:08X}')
    return hits


def writes_register(word, register):
    """Approximate: does this instruction write `register`? Used only to stop a lui/addiu scan early."""
    parts = fields(word)
    op = parts['op']
    if op == OP_SPECIAL:
        return parts['funct'] not in NO_DESTINATION_SPECIAL and parts['rd'] == register
    if op in (OP_J, OP_JAL, OP_BEQ, OP_BNE, OP_BLEZ, OP_BGTZ, OP_REGIMM) or op in OP_BRANCH_LIKELY:
        return False
    if op in STORE_OPS:
        return False
    if op in (OP_COP0, OP_COP1, OP_COP2, OP_COP3):
        return parts['rs'] == 0 and parts['rt'] == register
    return parts['rt'] == register


def detect_luipairs(image, window=3):
    """lui rt,hi then addiu rd,rt,lo (or ori rd,rt,lo) within `window` instructions builds hi<<16 + lo."""
    hits = {}
    for segment in image.text_segments():
        for offset in range(0, len(segment.body) - 3, 4):
            pc = segment.address + offset
            word = struct.unpack_from('<I', segment.body, offset)[0]
            parts = fields(word)
            if parts['op'] != OP_LUI:
                continue
            register, high = parts['rt'], parts['imm']
            value = None
            for step in range(1, window + 1):
                following = image.word(pc + 4 * step)
                if following is None:
                    break
                follow = fields(following)
                if follow['rs'] == register and follow['op'] == OP_ADDIU:
                    value = ((high << 16) + follow['simm']) & 0xFFFFFFFF
                    break
                if follow['rs'] == register and follow['op'] == OP_ORI:
                    value = (high << 16) | follow['imm']
                    break
                if writes_register(following, register):
                    value = None
                    break
            else:
                value = None
            if value is not None and image.is_text(value):
                hits.setdefault(value, f'lui at 0x{pc:08X}')
    return hits


def translated_entries(generated_folder):
    """Addresses that have a registered entry: `case N: goto L_XXXXXXXX;` with N non-zero."""
    label = re.compile(r'case (\d+)u: goto L_([0-9A-Fa-f]{8});')
    entries = set()
    for path in sorted(generated_folder.glob('unit_*.cpp')):
        for entry_id, address in label.findall(path.read_text(encoding='utf-8')):
            if int(entry_id):
                entries.add(int(address, 16))
    if not entries:
        raise ValueError('No translated entries found in the generated units')
    return entries


def read_names_csv(path):
    """Map address -> names for kind=func rows of the name table."""
    names = {}
    with path.open(newline='', encoding='utf-8') as handle:
        for row in csv.DictReader(handle):
            if row['kind'] == 'func':
                names.setdefault(int(row['address'], 16), set()).add(row['name'])
    return names


def read_decomp_clone(root):
    """Function names from the gt4-decomp config files, plus the checkout commit."""
    config = root / 'config'
    rows = parse_symbol_addrs((config / 'symbol_addrs.txt').read_text(encoding='utf-8'))
    rows += parse_adhoc_methods((config / 'adhoc_methods.txt').read_text(encoding='utf-8'))
    names = {}
    for row in rows:
        if row['kind'] == 'func':
            names.setdefault(int(row['address'], 16), set()).add(row['name'])
    commit = subprocess.run(['git', '-C', str(root), 'rev-parse', 'HEAD'], capture_output=True,
                            text=True, check=True).stdout.strip()
    return names, commit


def file_sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def add_hits(store, source, hits):
    for address, detail in hits.items():
        store.setdefault(address, {})[source] = detail


def confidence_of(sources, prologue, word):
    if is_placeholder_word(word):
        return 'rejected'
    strong = bool(sources & {'decomp', 'fallthrough'}) or len(sources) >= 2
    if prologue and strong:
        return 'high'
    if prologue or 'decomp' in sources:
        return 'medium'
    return 'low'


def build_ledger(assets_root, names_path, decomp_root=None, elf_path=None, generated=None):
    elf_path = elf_path or assets_root / ELF_RELATIVE
    generated = generated or assets_root / GENERATED_RELATIVE
    image = Image(load_segments(elf_path.read_bytes()))
    flow = scan_control_flow(image)

    decomp_names = read_names_csv(names_path) if names_path is not None else {}
    decomp_commit = 'not read (no --decomp clone)'
    decomp_clone_names = {}
    if decomp_root is not None:
        decomp_clone_names, decomp_commit = read_decomp_clone(decomp_root)
    all_names = {}
    for source_names in (decomp_names, decomp_clone_names):
        for address, names in source_names.items():
            all_names.setdefault(address, set()).update(names)

    store = {}
    decomp_hits = {}
    outside_text = 0
    for address in all_names:
        if image.is_text(address):
            decomp_hits[address] = 'name table' if address in decomp_names else 'symbol_addrs/adhoc'
        else:
            outside_text += 1
    add_hits(store, 'decomp', decomp_hits)
    fallthrough_hits, no_frame_after_jump = detect_fallthrough(image, flow)
    add_hits(store, 'fallthrough', fallthrough_hits)
    add_hits(store, 'dataword', detect_datawords(image))
    add_hits(store, 'luipair', detect_luipairs(image))
    # Sensitivity only: the stage A study used a 40-word window; it does not add candidates.
    wide_luipair = set(detect_luipairs(image, window=40))

    translated = translated_entries(generated)
    rows = []
    for address in sorted(store):
        sources = set(store[address])
        word = image.word(address)
        prologue = is_stack_frame(word)
        rows.append({'address': f'0x{address:08X}', 'sources': sorted(sources),
                     'evidence': {source: store[address][source] for source in sorted(sources)},
                     'names': sorted(all_names.get(address, ())),
                     'translated': address in translated,
                     'stack_frame': prologue,
                     'word': f'0x{word:08X}',
                     'confidence': confidence_of(sources, prologue, word)})
    return {
        'rows': rows,
        'translated_count': len(translated),
        'translated_addresses': translated,
        'store': store,
        'outside_text_decomp_names': outside_text,
        'luipair_wide_window_addresses': sorted(wide_luipair),
        'no_frame_after_jump': no_frame_after_jump,
        'decomp_commit': decomp_commit,
        'inputs': {'elf': {'path': str(elf_path), 'sha256': file_sha256(elf_path)},
                   'names_table': None if names_path is None else {'path': str(names_path),
                                                                  'sha256': file_sha256(names_path)},
                   'generated_units_dir': str(generated),
                   'decomp_clone': None if decomp_root is None else str(decomp_root)},
    }


def summarize(result):
    rows = result['rows']
    translated = result['translated_addresses']
    per_source = {}
    for source in SOURCES:
        found = {row['address'] for row in rows if source in row['sources']}
        new = {row['address'] for row in rows if source in row['sources'] and not row['translated']}
        per_source[source] = {'raw_unique': len(found), 'new_untranslated': len(new),
                              'on_translated_entries': len(found) - len(new),
                              'precision_proxy': round((len(found) - len(new)) / len(found), 4) if found else None,
                              'recall_proxy': round((len(found) - len(new)) / len(translated), 6)}
    overlaps = {}
    for left, right in combinations(SOURCES, 2):
        both = [row for row in rows if left in row['sources'] and right in row['sources']]
        overlaps[f'{left}+{right}'] = {'total': len(both),
                                       'new_untranslated': sum(1 for row in both if not row['translated'])}
    new_rows = [row for row in rows if not row['translated']]
    by_confidence = Counter(row['confidence'] for row in new_rows)
    by_source_count = Counter(len(row['sources']) for row in new_rows)
    return {
        'translated_entries': result['translated_count'],
        'candidates_total': len(rows),
        'candidates_new': len(new_rows),
        'per_source': per_source,
        'overlaps': overlaps,
        'new_by_confidence': dict(sorted(by_confidence.items())),
        'new_by_source_count': dict(sorted(by_source_count.items())),
        'new_without_stack_frame': sum(1 for row in new_rows if not row['stack_frame']),
        'decomp_names_outside_text': result['outside_text_decomp_names'],
        'sensitivity_luipair_window_40': {
            'addresses_in_window_40': len(result['luipair_wide_window_addresses']),
            'new_untranslated_only_in_window_40': sum(
                1 for address in result['luipair_wide_window_addresses']
                if address not in result['translated_addresses'] and address not in result['store']),
        },
        'fallthrough_jumps_without_frame': result['no_frame_after_jump'],
        'decomp_commit': result['decomp_commit'],
        'inputs': result['inputs'],
    }


def disassemble(word, pc):
    """Small decoder for the instructions that dominate EE code; anything else is shown as a raw word."""
    if word == 0:
        return 'nop'
    parts = fields(word)
    op, rs, rt, rd, sa = parts['op'], parts['rs'], parts['rt'], parts['rd'], parts['sa']
    reg = lambda number: '$' + REGISTER_NAMES[number]  # noqa: E731
    if op == OP_SPECIAL:
        named = {0x21: 'addu', 0x23: 'subu', 0x24: 'and', 0x25: 'or', 0x26: 'xor', 0x2A: 'slt', 0x2B: 'sltu',
                 0x2C: 'dadd', 0x2D: 'daddu', 0x2E: 'dsub', 0x2F: 'dsubu'}
        if parts['funct'] == 0x08:
            return f'jr {reg(rs)}'
        if parts['funct'] == 0x09:
            return f'jalr {reg(rd)},{reg(rs)}'
        if parts['funct'] == 0x00:
            return f'sll {reg(rd)},{reg(rt)},{sa}'
        if parts['funct'] in named:
            return f"{named[parts['funct']]} {reg(rd)},{reg(rs)},{reg(rt)}"
        return f'.word 0x{word:08X}'
    if op == OP_J:
        return f'j 0x{jump_target(word, pc):08X}'
    if op == OP_JAL:
        return f'jal 0x{jump_target(word, pc):08X}'
    target = branch_target(word, pc)
    if op == OP_BEQ:
        if rs == 0 and rt == 0:
            return f'b 0x{target:08X}'
        return f'beq {reg(rs)},{reg(rt)},0x{target:08X}'
    if op == OP_BNE:
        return f'bne {reg(rs)},{reg(rt)},0x{target:08X}'
    immediate = {OP_ADDIU: 'addiu', OP_DADDIU: 'daddiu', OP_ORI: 'ori', 0x0C: 'andi', 0x0A: 'slti'}
    if op in immediate:
        value = parts['simm'] if op in (OP_ADDIU, OP_DADDIU, 0x0A) else parts['imm']
        return f'{immediate[op]} {reg(rt)},{reg(rs)},{value}'
    if op == OP_LUI:
        return f'lui {reg(rt)},0x{parts["imm"]:04X}'
    memory = {0x20: 'lb', 0x23: 'lw', 0x27: 'lwu', 0x37: 'ld', 0x1E: 'lq', 0x28: 'sb', 0x2B: 'sw',
              0x3F: 'sd', 0x1F: 'sq', 0x31: 'lwc1', 0x39: 'swc1'}
    if op in memory:
        return f'{memory[op]} {reg(rt)},{parts["simm"]}({reg(rs)})'
    return f'.word 0x{word:08X}'


def spot_check(image_path_root, result, count, seed):
    image = Image(load_segments((image_path_root / ELF_RELATIVE).read_bytes()))
    new_rows = [row for row in result['rows'] if not row['translated']]
    random.Random(seed).shuffle(new_rows)
    for row in new_rows[:count]:
        address = int(row['address'], 16)
        lines = []
        for step in range(3):
            pc = address + 4 * step
            word = image.word(pc)
            lines.append(f'    0x{pc:08X}: {word:08X}  {disassemble(word, pc)}')
        print(f"{row['address']} {row['confidence']} sources={','.join(row['sources'])} "
              f"names={'|'.join(row['names']) or '-'}")
        print('\n'.join(lines))
    return len(new_rows[:count])


def write_outputs(output_dir, result, summary):
    ledger_path = output_dir / LEDGER_NAME
    summary_path = output_dir / SUMMARY_NAME
    if ledger_path.exists() or summary_path.exists():
        raise ValueError('Ledger already written here; choose a fresh output directory')
    output_dir.mkdir(parents=True, exist_ok=True)
    document = {'schema_version': 1, 'task': 'TASK-014 stage B', 'summary': summary,
                'rows': result['rows']}
    ledger_path.write_text(json.dumps(document, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + '\n', encoding='utf-8', newline='\n')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--assets-root', type=Path, default=ROOT,
                        help='checkout holding private/ (the main repository, not a worktree)')
    parser.add_argument('--names', type=Path, default=ROOT / NAMES_RELATIVE, help='name table CSV')
    parser.add_argument('--no-names', action='store_true',
                        help='generic sources only (a second target has no v2.00 decomp names)')
    parser.add_argument('--elf', type=Path, help='ELF to analyse (default: the pinned v2.00 reconstruction)')
    parser.add_argument('--generated', type=Path, help='generated unit directory (default: the v2.00 units)')
    parser.add_argument('--decomp', type=Path, help='gt4-decomp clone (config/symbol_addrs.txt, adhoc_methods.txt)')
    parser.add_argument('--output-dir', type=Path, help='fresh private directory for the ledger')
    parser.add_argument('--spot-check', type=int, default=0, help='print N seeded new candidates')
    parser.add_argument('--seed', type=int, default=207)
    args = parser.parse_args(argv)
    try:
        result = build_ledger(args.assets_root, None if args.no_names else args.names, args.decomp,
                              elf_path=args.elf, generated=args.generated)
        summary = summarize(result)
        if args.output_dir is not None:
            write_outputs(args.output_dir, result, summary)
        print(json.dumps(summary, indent=2, sort_keys=True))
        if args.spot_check:
            spot_check(args.assets_root, result, args.spot_check, args.seed)
        return 0
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        print(f'ee-entry-ledger: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
