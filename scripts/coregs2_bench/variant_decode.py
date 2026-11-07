"""Decodes a CoreGS 2.0 variant list (variants.bin, GT4VARS1) and explains what a key means.

usage:
  variant_decode.py <variants.bin> [more.bin ...] [--against <core2_known_variants.inc>]
                    [--summary] [--json <report.json>] [--keys]

A key is 33 32-bit words: the 30 draw-state fields kSpecFields names (native/hw_gs/core2/core2_gpu.cpp),
the tex_scale push word, the mip anisotropy (tex_origin_x bits 21-23 of a mipmapped draw) and the ROP word
of a draw through colour attachments. It holds draw-state numbers only, no game data (IN-507).
"""
import argparse
import json
import struct
import sys
from pathlib import Path

KEY_WORDS = 33
HEADER = struct.Struct('<8sQII')
SHIPPED = Path(__file__).resolve().parents[2] / 'native' / 'hw_gs' / 'core2' / 'core2_known_variants.inc'

# kSpecKeyNames: the kSpecFields order, then push, aniso and rop (core2_gpu.cpp, TASK-170c).
NAMES = [
    'Format', 'Fba', 'Dither', 'Ate', 'Atst', 'Afail', 'DateOn', 'Datm', 'Ztest', 'Ztst',
    'Zmsk', 'Zformat', 'BlendOn', 'Pabe', 'BlendA', 'BlendB', 'BlendC', 'BlendD', 'Colclamp', 'TexOn',
    'Tfx', 'Tcc', 'TexelKind', 'Aem', 'FogOn', 'TexFst', 'TexLinear', 'WrapS', 'WrapT', 'KeepTopByte',
    'push', 'aniso', 'rop',
]
FIELD_NAMES = NAMES[:30]

FORMAT = {0: 'CT32', 1: 'CT24', 2: 'CT16'}
ZFORMAT = {0: 'Z32', 1: 'Z24', 2: 'Z16'}
TFX = {0: 'Modulate', 1: 'Decal', 2: 'Highlight', 3: 'Highlight2'}
TEXEL = {0: 'RGBA8', 1: 'CT16raw', 2: 'CT24raw', 3: 'Indexed'}
ATST = {0: 'Never', 1: 'Always', 2: 'Less', 3: 'LessEqual', 4: 'Equal', 5: 'GreaterEqual',
        6: 'Greater', 7: 'NotEqual'}
AFAIL = {0: 'Keep', 1: 'FrameOnly', 2: 'DepthOnly', 3: 'RgbOnly'}
ZTST = {0: 'Never', 1: 'Always', 2: 'GreaterEqual', 3: 'Greater'}
WRAP = {0: 'Repeat', 1: 'Clamp', 2: 'RegionClamp', 3: 'RegionRepeat'}
BLEND_SEL = {0: 'Cs', 1: 'Cd', 2: 'Zero'}

# Push-word bits read by hw_pixel_uber.frag. 0-19 is the texture scale in whole texels of the internal scale.
PUSH_BITS = [
    (0x000FFFFF, 'tex_scale', 'scale of the sampled texture image (its whole-texel scale, 1x..12x)'),
    (0x00100000, 'scanmask.0', 'SCANMSK bit 0 (row mask, time trial ghost / split screen)'),
    (0x00200000, 'scanmask.1', 'SCANMSK bit 1 (row mask, time trial ghost / split screen)'),
    (0x00400000, 'replacement_texture', 'a replacement texture image is sampled instead of the game texture'),
    (0x00800000, 'mip', 'texture sampled with mip levels (LOD), aniso in slot 31'),
    (0x01000000, 'textured_image', 'the batch samples a texture image (atlas or drawn target); never set when TexOn=0'),
    (0x02000000, 'diagnostic_noz', 'GT4_HW_SCALED_DIAG=noz diagnostic'),
    (0x04000000, 'diagnostic_notex', 'GT4_HW_SCALED_DIAG=notex diagnostic'),
    (0x08000000, 'alpha_test_before_interlock', 'alpha test may discard before the interlock'),
    (0x10000000, 'depth_from_position', 'depth from gl_FragCoord; set with dither_fine when a target is scaled above 1x'),
    (0x20000000, 'vertex_snap', 'vertex-stage native-alignment snap; cleared from the key'),
    (0x40000000, 'dither_fine', 'fine dither; set with depth_from_position when a target is scaled above 1x'),
    (0x80000000, 'force_linear', 'forced linear filter for 1x textures'),
]


def read_bin(path):
    data = Path(path).read_bytes()
    if len(data) < HEADER.size:
        raise SystemExit(f'{path}: too short for a variant list')
    magic, shader_set, key_words, count = HEADER.unpack_from(data)
    if magic != b'GT4VARS1' or key_words != KEY_WORDS or len(data) != HEADER.size + count * KEY_WORDS * 4:
        raise SystemExit(f'{path}: not a variant list of this build (magic {magic!r}, words {key_words}, '
                         f'{len(data)} bytes)')
    words = struct.unpack_from(f'<{count * KEY_WORDS}I', data, HEADER.size)
    return shader_set, [tuple(words[i:i + KEY_WORDS]) for i in range(0, len(words), KEY_WORDS)]


def read_inc(path):
    keys = []
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if not line or line.startswith('//'):
            continue
        parts = [int(p.strip().rstrip('uU'), 16) for p in line.split(',') if p.strip()]
        if len(parts) != KEY_WORDS:
            raise SystemExit(f'{path}: a row has {len(parts)} words, want {KEY_WORDS}')
        keys.append(tuple(parts))
    return keys


def enum_text(name, value):
    table = {'Format': FORMAT, 'Zformat': ZFORMAT, 'Tfx': TFX, 'TexelKind': TEXEL, 'Atst': ATST,
             'Afail': AFAIL, 'Ztst': ZTST, 'WrapS': WRAP, 'WrapT': WRAP,
             'BlendA': BLEND_SEL, 'BlendB': BLEND_SEL, 'BlendD': BLEND_SEL}.get(name)
    if table is not None:
        return table.get(value, f'?{value}')
    if name in ('Fba', 'Dither', 'Ate', 'DateOn', 'Datm', 'Ztest', 'Zmsk', 'BlendOn', 'Pabe', 'Colclamp',
                'TexOn', 'Tcc', 'Aem', 'FogOn', 'TexFst', 'TexLinear', 'KeepTopByte'):
        return {0: 'off', 1: 'on'}.get(value, f'?{value}')
    if name == 'BlendC':
        return {0: 'As', 1: 'Ad', 2: 'FIX'}.get(value, f'?{value}')
    if name == 'push':
        return push_text(value)
    if name == 'rop':
        return 'none' if value == 0 else f'0x{value:X}'
    return str(value)


def push_text(word):
    parts = [f'scale={word & 0xFFFFF}']
    for mask, label, _ in PUSH_BITS[1:]:
        if word & mask:
            parts.append(label)
    return ' '.join(parts)


def push_bits(word):
    return [label for mask, label, _ in PUSH_BITS if word & mask]


def describe_key(key):
    fields = {name: enum_text(name, key[i]) for i, name in enumerate(FIELD_NAMES)}
    fields['push'] = push_text(key[30])
    fields['aniso'] = str(key[31])
    fields['rop'] = 'none' if key[32] == 0 else f'0x{key[32]:X}'
    return fields


def diff(a, b):
    """Field-level difference a -> b, with decoded values."""
    out = []
    for i, name in enumerate(NAMES):
        if a[i] != b[i]:
            out.append({'field': name, 'from': enum_text(name, a[i]), 'to': enum_text(name, b[i]),
                        'from_raw': a[i], 'to_raw': b[i]})
    return out


def distance(a, b):
    return sum(1 for x, y in zip(a, b) if x != y)


def nearest(key, corpus):
    best = None
    best_d = 1 << 30
    for other in corpus:
        d = distance(key, other)
        if d < best_d:
            best, best_d = other, d
            if d == 1:
                break
    return best, best_d


def signature(key):
    """The part of the key that selects shader code paths, without the scale/aniso/rop words."""
    names = ('Format', 'Fba', 'Dither', 'Ate', 'Atst', 'Afail', 'DateOn', 'Datm', 'Ztest', 'Ztst', 'Zmsk',
             'Zformat', 'BlendOn', 'Pabe', 'BlendA', 'BlendB', 'BlendC', 'BlendD', 'Colclamp', 'TexOn', 'Tfx',
             'Tcc', 'TexelKind', 'Aem', 'FogOn', 'TexFst', 'TexLinear', 'WrapS', 'WrapT', 'KeepTopByte')
    return tuple(key[NAMES.index(n)] for n in names)


def classify(key):
    """A short label for what the draw state is, from the fields alone."""
    f = dict(zip(FIELD_NAMES, key[:30]))
    labels = []
    labels.append(f"format={FORMAT.get(f['Format'], f['Format'])}")
    if f['TexOn']:
        labels.append(f"tex={TFX.get(f['Tfx'], f['Tfx'])}/{TEXEL.get(f['TexelKind'], f['TexelKind'])}")
    else:
        labels.append('untextured')
    if f['BlendOn']:
        labels.append('blend')
    if f['Ate']:
        labels.append(f"atest={ATST.get(f['Atst'], f['Atst'])}/{AFAIL.get(f['Afail'], f['Afail'])}")
    if f['Ztest']:
        labels.append(f"z={ZTST.get(f['Ztst'], f['Ztst'])}/{ZFORMAT.get(f['Zformat'], f['Zformat'])}"
                      f"{'/masked' if f['Zmsk'] else ''}")
    if f['FogOn']:
        labels.append('fog')
    if f['DateOn']:
        labels.append('date')
    labels.append(f"scale={key[30] & 0xFFFFF}")
    if key[30] & 0x01000000:
        labels.append('textured-image')
    if key[30] & 0x00800000:
        labels.append(f"mip/aniso={key[31]}")
    if key[30] & 0x00300000:
        labels.append('scanmask')
    if key[32]:
        labels.append(f'rop=0x{key[32]:X}')
    return ' '.join(labels)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+')
    parser.add_argument('--against', default=str(SHIPPED))
    parser.add_argument('--json')
    parser.add_argument('--summary', action='store_true')
    parser.add_argument('--keys', action='store_true', help='print every key')
    parser.add_argument('--corpus', action='store_true', help='print the field statistics of --against')
    args = parser.parse_args(argv)

    if args.corpus:
        corpus = read_inc(args.against)
        print(f'{args.against}: {len(corpus)} keys, {len(set(corpus))} distinct')
        for i, name in enumerate(NAMES):
            values = sorted({key[i] for key in corpus})
            text = ', '.join(enum_text(name, value) for value in values[:24])
            more = f' ... ({len(values)} values)' if len(values) > 24 else ''
            print(f'{i:2d} {name:14s} {len(values):3d}: {text}{more}')
        words = sorted({key[30] for key in corpus})
        print(f'push: {len(words)} distinct word(s)')
        for word in words:
            print(f'  0x{word:08X} {push_text(word)}')
        print('aniso:', sorted({key[31] for key in corpus}))
        print('rop:', sorted({f'0x{key[32]:X}' for key in corpus}))
        return 0

    corpus = read_inc(args.against)
    corpus_set = set(corpus)
    report = {'against': str(args.against), 'corpus': len(corpus), 'files': []}
    new_keys = {}
    for path in args.files:
        shader_set, keys = read_bin(path)
        record = {'file': path, 'shader_set': f'{shader_set:016x}', 'keys': len(keys),
                  'new': 0, 'new_keys': []}
        for key in keys:
            if key in corpus_set or key in new_keys:
                continue
            base, d = nearest(key, corpus)
            entry = {'key': list(key), 'nearest_distance': d, 'nearest': list(base),
                     'diff': diff(base, key), 'class': classify(key), 'push_bits': push_bits(key[30])}
            new_keys[key] = entry
            record['new'] += 1
            record['new_keys'].append(list(key))
        report['files'].append(record)
        print(f'{path}: {len(keys)} keys, {record["new"]} not in the shipped {len(corpus)}')
    report['new_total'] = len(new_keys)
    report['union_total'] = len(corpus_set | set(new_keys))
    report['new'] = list(new_keys.values())
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8', newline='\n')
        print(f'{args.json}: {len(new_keys)} new keys, union {report["union_total"]}')
    if args.summary or args.keys:
        for entry in new_keys.values():
            print(f'  [{entry["nearest_distance"]}] {classify(tuple(entry["key"]))}')
            for d in entry['diff']:
                print(f'      {d["field"]}: {d["from"]} -> {d["to"]}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
