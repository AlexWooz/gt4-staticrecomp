"""Merges CoreGS 2.0 variant lists into a candidate corpus and reports what changed.

usage:
  variant_survey.py <variants.bin|folder> ... [--out <candidate.inc>] [--json <report.json>]
                    [--shipped <core2_known_variants.inc>] [--rop-off|--rop-on] [--plain-off|--plain-on]

The reported filters copy variants_to_prepare (native/hw_gs/core2/core2_gpu.cpp): the plain words are prepared only
when the plain path exists (integrated GPUs), and a ROP word only when the colour-attachment path is on. A file from
another build is refused by its key size. No key is ever truncated silently: the cap is reported.
"""
import argparse
import json
import struct
import sys
from collections import OrderedDict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import variant_decode as decode

KEY_WORDS = decode.KEY_WORDS
HEADER = decode.HEADER
PLAIN_WORD = 0x80000000
PLAIN_SHEEN_WORD = 0xC0000000
CAP = 2048


def read_any(paths, excludes):
    out = []
    for path in paths:
        path = Path(path)
        if path.is_dir():
            files = sorted(path.glob('*.bin'))
        else:
            files = [path]
        for file in files:
            if any(file.name.startswith(prefix) for prefix in excludes):
                continue
            try:
                shader_set, keys = decode.read_bin(file)
            except SystemExit as problem:
                print(f'refused: {problem}', file=sys.stderr)
                continue
            out.append((file, shader_set, keys))
    return out


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+')
    parser.add_argument('--out')
    parser.add_argument('--json')
    parser.add_argument('--shipped', default=str(decode.SHIPPED))
    parser.add_argument('--rop-off', action='store_true',
                        help='this card has no colour-attachment path (a variant with a ROP word is not prepared)')
    parser.add_argument('--rop-on', action='store_true')
    parser.add_argument('--plain-off', action='store_true',
                        help='this card has no disjoint plain path (0x80000000/0xC0000000 are not prepared)')
    parser.add_argument('--plain-on', action='store_true')
    parser.add_argument('--exclude', action='append', default=[],
                        help='skip files whose name starts with this prefix (repeatable)')
    parser.add_argument('--shader-set', help='expected shader-set hash (hex) of the release; another build is refused')
    parser.add_argument('--with-shipped', action='store_true',
                        help='write the union with the shipped list (a candidate for the next release), not only the recordings')
    args = parser.parse_args(argv)

    rop_on = args.rop_on or not args.rop_off
    plain_on = args.plain_on or not args.plain_off
    shipped = decode.read_inc(args.shipped)
    shipped_set = set(shipped)
    files = read_any(args.files, args.exclude)
    if not files:
        raise SystemExit('no variant list read')
    if args.shader_set:
        expected = args.shader_set.lower().removeprefix('0x').zfill(16)
        refused = [file for file, shader_set, _ in files if f'{shader_set:016x}' != expected]
        if refused:
            raise SystemExit('refused, written for another build (shader set differs): ' +
                             ', '.join(str(file) for file in refused))

    union = OrderedDict()
    provenance = {}
    per_file = []
    duplicates = 0
    conflicts = 0
    for file, shader_set, keys in files:
        record = {'file': str(file), 'shader_set': f'{shader_set:016x}', 'keys': len(keys), 'new': 0,
                  'duplicates_in_file': 0}
        seen = set()
        for key in keys:
            if key in seen:
                record['duplicates_in_file'] += 1
                duplicates += 1
                continue
            seen.add(key)
            if key not in union:
                union[key] = True
                provenance[key] = []
            if key not in shipped_set:
                record['new'] += 1
            provenance[key].append(file.name)
        per_file.append(record)

    # The filters of variants_to_prepare on a card with the given paths.
    kept = OrderedDict()
    filtered = {'plain': 0, 'rop': 0}
    for key in union:
        word = key[32]
        plain = word == PLAIN_WORD or word == PLAIN_SHEEN_WORD
        if plain and not plain_on:
            filtered['plain'] += 1
            continue
        if not plain and word != 0 and not rop_on:
            filtered['rop'] += 1
            continue
        kept[key] = True

    new_kept = [key for key in kept if key not in shipped_set]
    # Field values the shipped list never shows, and the values of every field across the union.
    field_report = {}
    for index, name in enumerate(decode.NAMES):
        shipped_values = sorted({key[index] for key in shipped})
        union_values = sorted({key[index] for key in kept})
        added = [value for value in union_values if value not in shipped_values]
        field_report[name] = {
            'shipped_values': [decode.enum_text(name, value) for value in shipped_values],
            'union_values': [decode.enum_text(name, value) for value in union_values],
            'new_values': [decode.enum_text(name, value) for value in added],
        }
    pushes = sorted({key[30] for key in kept})
    scales = sorted({key[30] & 0xFFFFF for key in kept})
    report = {
        'shipped': len(shipped),
        'files': per_file,
        'union': len(union),
        'kept': len(kept),
        'new_beyond_shipped': len(new_kept),
        'duplicates': duplicates,
        'filtered': filtered,
        'cap': CAP,
        'cap_exceeded': len(kept) > CAP,
        'scales': scales,
        'push_values': [f'0x{word:08X}' for word in pushes],
        'fields': field_report,
        'new_keys': [
            {'key': list(key), 'class': decode.classify(key), 'provenance': provenance.get(key, [])}
            for key in new_kept
        ],
    }
    print(f'shipped {len(shipped)}, union {len(union)}, kept {len(kept)} '
          f'(filtered {filtered["plain"]} plain, {filtered["rop"]} rop), new {len(new_kept)}, '
          f'duplicates {duplicates}, cap {CAP}{" EXCEEDED" if len(kept) > CAP else ""}')
    for record in per_file:
        print(f'  {record["file"]}: {record["keys"]} keys, {record["new"]} not shipped')
    if args.out:
        output = sorted(set(kept) | shipped_set) if args.with_shipped else sorted(kept)
        rows = [','.join(f'0x{word:X}u' for word in key) + ',' for key in output]
        Path(args.out).write_text('// Candidate corpus merged by scripts/coregs2_bench/variant_survey.py (TASK-205). '
                                  'One key per row.\n' + '\n'.join(rows) + '\n', encoding='utf-8', newline='\n')
        print(f'{args.out}: {len(output)} keys')
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8', newline='\n')
        print(f'{args.json}: report')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
