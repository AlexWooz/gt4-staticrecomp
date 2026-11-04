"""TASK-166: the TASK-080 P2 ELF patch table for Spec II (the race font x scale and the mirror divisor in ultrawide).

v2.00 loads two race HUD constants from the host-filled HUD parameter block (scripts/aspect_elf_patch.py, decision 0071):
the race font x scale (0.75 in wide mode) and the rear-view mirror divisor (640). Spec II kept both immediates, so at a
ratio wider than 16:9 the glyphs drawn with the race font (the digital speedometer and the odometer of the bumper and
cockpit gauges) and the mirror were stretched with the frame. This table patches the same two instructions in the Spec II
derived ELF, one instruction for one instruction:

    0x001C519C  lui at,0x3F40       -> lw at,0x7F00(zero)   RaceDisplay::render 0x001C5040
    0x001EB65C  addiu t0,zero,640   -> lw t0,0x7F04(zero)   mirror placement after the jal at 0x001EB634

Spec II's render multiplies the font object's own x scale (this + 0xB0) by the constant where v2.00 loads the constant
alone, so the block word means the same thing in both images: 0.75 at 16:9 (the original), (4/3)/R otherwise. The mirror
code is the same instruction sequence as v2.00's (0x0033EF78..0x0033EF98, here 0x001EB63C..0x001EB65C). The host writes
the block at every guest VBlank (native/display_aspect.cpp at_guest_vblank) for both targets; at 16:9 it holds the
original constants, so the patched code computes exactly what the original did.

Only the units holding a patched word change; the rest of the translation must be byte-identical to the pinned Spec II
translation (docs/inputs/spec2-ee-generated.json). The generated C++ is game-derived and stays under private/.

Commands:
  check --elf PATH              verify every old word in the pinned derived Spec II ELF and print the table
  produce --assets-root A --base-units DIR --output DIR
                                the patched derivative SCUS_974.36.aspect.elf, translation with the pinned translator and
                                the pinned S8c entry seeds into DIR/generated, and the receipt spec2-aspect-generation.json;
                                refuses unless the units that differ from DIR (the pinned translation) are exactly the units
                                holding the patched words
"""

import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import aspect_elf_patch as v200  # noqa: E402  (the Patch row, the instruction helpers and the block address)

ELF_PIN = ROOT / 'docs/inputs/spec2-elf.json'
GENERATED_PIN = ROOT / 'docs/inputs/spec2-ee-generated.json'
DERIVED_ELF = 'private/slice272-spec2-elf/SCUS_974.36'
ENTRY_SEEDS = 'private/slice278-spec2-ee-producer/entry-seeds.txt'
TRANSLATOR = 'private/reference-acceleration-study/eerecomp.exe'
TRANSLATOR_SHA256 = '5aec705e31da349c941414fd54be264000459c3cb0d22727e7d810b5b24b03c0'

PATCHES = (
    v200.Patch(0x001C519C, 0x3C013F40, 0x8C017F00, 0x00, 0x3F400000,
               'RaceDisplay::render (0x001C5040)',
               'race HUD font x scale in wide mode: lui at,0x3F40 (0.75 = (4/3)/(16/9)) -> lw at,0x7F00(zero); '
               'multiplied by the font x scale at this + 0xB0 and set on the race font object 0x00665048 through '
               '0x0031EB78; block value (4/3)/R'),
    v200.Patch(0x001EB65C, 0x24080280, 0x8C087F04, 0x04, 640,
               'rear-view mirror placement (code after the jal at 0x001EB634, RacePS2Base render path)',
               'mirror horizontal divisor: addiu t0,zero,640 -> lw t0,0x7F04(zero); the mirror is 260 x width / divisor '
               'frame pixels wide and centred, block value round(640 x R / (16/9)) for R > 16:9'),
)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def check_table():
    """Each new word is a load of the same register from the block; each 16:9 value is the original constant; the block
    words are the v2.00 table's, so one host block serves both targets."""
    by_offset = {patch.block_offset: patch for patch in v200.PATCHES}
    for patch in PATCHES:
        fields = v200.decode(patch.old_word)
        if patch.new_word != v200.encode_lw(fields['rt'], v200.PARAMETER_BLOCK + patch.block_offset):
            raise ValueError(f'0x{patch.address:08X}: new word is not lw rt,block+offset(zero)')
        if v200.original_value(patch.old_word) != patch.value_16_9:
            raise ValueError(f'0x{patch.address:08X}: 16:9 block value differs from the original constant')
        twin = by_offset.get(patch.block_offset)
        if twin is None or (twin.old_word, twin.new_word, twin.value_16_9) != (patch.old_word, patch.new_word,
                                                                             patch.value_16_9):
            raise ValueError(f'0x{patch.address:08X}: not the same instruction and block word as the v2.00 row')
    if len({patch.block_offset for patch in PATCHES}) != len(PATCHES):
        raise ValueError('two patches share a block word')
    return True


def verify_old_words(elf):
    check_table()
    for patch in PATCHES:
        observed = v200.word_at(elf, patch.address)
        if observed != patch.old_word:
            raise ValueError(f'0x{patch.address:08X}: ELF word 0x{observed:08X}, table old word 0x{patch.old_word:08X}')
    return True


def patch_elf(elf):
    """The patched derivative; refuses unless every old word matches."""
    verify_old_words(elf)
    patched = bytearray(elf)
    for patch in PATCHES:
        struct.pack_into('<I', patched, v200.file_offset(elf, patch.address), patch.new_word)
    return bytes(patched)


def table_rows():
    return [{'address': f'0x{p.address:08X}', 'old_word': f'0x{p.old_word:08X}', 'new_word': f'0x{p.new_word:08X}',
             'block_word': f'0x{v200.PARAMETER_BLOCK + p.block_offset:08X}', 'value_16_9': f'0x{p.value_16_9:08X}',
             'function': p.function, 'purpose': p.purpose} for p in PATCHES]


def inventory(directory):
    """The record list of scripts/spec2_ee_generate.py (name, size, hash per file), so the pinned digest compares."""
    return [{'name': path.name, 'size_bytes': path.stat().st_size, 'sha256': sha256_file(path)}
            for path in sorted(Path(directory).iterdir()) if path.is_file()]


def inventory_digest(records):
    return hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest()


def unit_holding(directory, address):
    """The unit whose code holds the guest address: its own label, or the nearest label before it."""
    texts = {path.name: path.read_text(encoding='ascii') for path in sorted(Path(directory).glob('unit_*.cpp'))}
    for delta in range(0, 0x400, 4):
        label = f'L_{address - delta:08X}:'
        owners = [name for name, text in texts.items() if label in text]
        if len(owners) == 1:
            return owners[0]
        if len(owners) > 1:
            raise ValueError(f'label {label} in several units: {owners}')
    raise ValueError(f'no unit holds 0x{address:08X}')


def produce(assets_root, base_units, output):
    assets_root, base_units, output = Path(assets_root), Path(base_units), Path(output)
    if output.exists():
        raise ValueError(f'{output} exists; choose a fresh folder')
    elf_pin = json.loads(ELF_PIN.read_text(encoding='utf-8'))['derived']
    generated_pin = json.loads(GENERATED_PIN.read_text(encoding='utf-8'))
    elf_path = assets_root / DERIVED_ELF
    original = elf_path.read_bytes()
    if sha256_bytes(original) != elf_pin['sha256'] or len(original) != elf_pin['size_bytes']:
        raise ValueError('the derived Spec II ELF differs from docs/inputs/spec2-elf.json')
    seeds = assets_root / ENTRY_SEEDS
    if sha256_file(seeds) != generated_pin['entry_seeds']['sha256']:
        raise ValueError('the entry seeds differ from docs/inputs/spec2-ee-generated.json')
    translator = assets_root / TRANSLATOR
    if sha256_file(translator) != TRANSLATOR_SHA256:
        raise ValueError('the translator differs from its pin')
    base_records = inventory(base_units)
    if inventory_digest(base_records) != generated_pin['inventory_sha256']:
        raise ValueError(f'{base_units} is not the pinned Spec II translation')

    patched = patch_elf(original)
    output.mkdir(parents=True)
    patched_path = output / 'SCUS_974.36.aspect.elf'
    patched_path.write_bytes(patched)
    generated = output / 'generated'
    arguments = [str(translator), str(patched_path), '--out', str(generated), '--entries', str(seeds)]
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=1200)
    (output / 'generation-aspect.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    if result.returncode != 0:
        raise ValueError(f'translator failed ({result.returncode}); see generation-aspect.log')
    stats = result.stdout.strip().splitlines()[0] if result.stdout.strip() else ''
    if stats != generated_pin['stats']:
        raise ValueError(f'translator stats differ from the pinned translation: {stats!r}')

    records = inventory(generated)
    base_by_name = {row['name']: row for row in base_records}
    if [row['name'] for row in records] != [row['name'] for row in base_records]:
        raise ValueError('the generated file list differs from the pinned translation')
    changed = sorted(row['name'] for row in records if row != base_by_name[row['name']])
    expected = sorted({unit_holding(generated, patch.address) for patch in PATCHES})
    if changed != expected:
        raise ValueError(f'changed units {changed} differ from the units holding the patched words {expected}')
    receipt = {
        'schema_version': 1, 'task': 'TASK-166', 'target': 'spec2',
        'elf': {'path': DERIVED_ELF, 'sha256': elf_pin['sha256']},
        'patched_elf': {'path': patched_path.name, 'sha256': sha256_bytes(patched), 'size_bytes': len(patched)},
        'parameter_block': f'0x{v200.PARAMETER_BLOCK:08X}', 'patches': table_rows(),
        'entry_seeds': {'path': ENTRY_SEEDS, 'sha256': generated_pin['entry_seeds']['sha256']},
        'translator': {'path': TRANSLATOR, 'sha256': TRANSLATOR_SHA256}, 'stats': stats,
        'base_units': {'path': str(base_units), 'inventory_sha256': generated_pin['inventory_sha256']},
        'generated': {'files': len(records), 'inventory_sha256': inventory_digest(records)},
        'changed_units': [{'unit': name, 'sha256_before': base_by_name[name]['sha256'],
                           'sha256_after': next(r['sha256'] for r in records if r['name'] == name)} for name in changed],
        'qualification': 'Source generation only: the rest of the translation is byte-identical to the pinned one',
    }
    (output / 'spec2-aspect-generation.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return receipt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    check = commands.add_parser('check')
    check.add_argument('--elf', type=Path, required=True)
    prod = commands.add_parser('produce')
    prod.add_argument('--assets-root', type=Path, required=True)
    prod.add_argument('--base-units', type=Path, required=True)
    prod.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == 'check':
            verify_old_words(args.elf.read_bytes())
            print(json.dumps(table_rows(), indent=2))
        else:
            receipt = produce(args.assets_root, args.base_units, args.output)
            print(f"Generated {receipt['generated']['files']} files; changed units "
                  f"{[row['unit'] for row in receipt['changed_units']]}; patched ELF {receipt['patched_elf']['sha256']}")
        return 0
    except (ValueError, OSError, subprocess.SubprocessError, KeyError) as error:
        print(f'spec2-aspect-patch: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
