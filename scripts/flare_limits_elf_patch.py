"""TASK-117 flare limits: the derived v2.00 ELF (four `jal` redirects plus our helper code) and its EE library profile.

Performance mode draws fewer light flares at night. The fix lives in guest code, because the four call sites are direct
`jal`s (compiled as direct C++ calls by our translator, so no host hook can see them):

  * 0x003B7BE4  track-light queue fill 0x003B7B70 -> visibility test 0x003B7C98  (distance cull, light cap)
  * 0x003B85C0  car-light queue fill 0x003B8560 -> visibility test 0x003B7C98    (distance cull, light cap)
  * 0x003B8BA0  car flare draw -> flare routine 0x00498198                       (no halo)
  * 0x003B8CE8  track flare draw -> flare routine 0x00498198                     (no halo)

Each redirect points at a small routine of ours in the zero padding 0x00495574..0x00495FFF, which reads one host-written
guest word (the flare-limits switch at 0x7F10, native/flare_limits.hpp), does nothing when the switch is 0 and otherwise
applies the change before jumping to the original routine with the original return address (`j`, not `jal`, so `ra` is
untouched). `Performance mode off` therefore runs the original instructions in effect.

The helpers only touch caller-saved registers: t0-t2 and $f0 for the fills, $f19 for the drawing sites (the halo scale,
loaded immediately before each call and not read after it). The fills rely on a0 being the flare object at both call
sites (0x3B7BB0/0x3B85B4: `addu a0,s2,zero`, s2 = a0 from the prologue) and on f13 being the per-type maximum view
depth (loaded in the delay slot of the original `jal`).

Commands:
  check --elf PATH                  print the table and verify the pinned reconstructed ELF: old words, the zero padding
                                    and the absence of references into it
  verify --elf PATH                 a derived ELF is exactly `patch(pinned ELF)`: every redirect word, the whole helper
                                    block, and every other byte identical
  produce --assets-root A --output private/slice299-ee-flare-producer
                                    fresh producer: the verified ELF (scripts/ee_producer.py prepare), the derived
                                    SCUS_973.28.flare.elf, translation with the pinned accepted seed set (slice208),
                                    receipt flare-generation.json
  record-library --output <same>    after building game-build/ps2game.lib (tools/ee_producer, clang-cl 23.1.2): checks
                                    the object inventory against the slice208 library and writes
                                    docs/inputs/slice299-ee-libraries.json (profile "flare")
"""

import argparse
import hashlib
import json
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

import aspect_elf_patch  # the TASK-080 P2 rows: the derived ELF keeps them

PARAMETER_BLOCK = 0x00007F10      # native/flare_limits.hpp kFlareParameterBlock
LIBRARY_MANIFEST = 'docs/inputs/slice299-ee-libraries.json'
PROFILE = 'flare'
SEED_SET = 'accepted'
BASE_PRODUCER = 'private/slice208-ee-entries-producer'
ELF_SHA256 = '10f82e2231a51404b95682ed3ea81171100a1af2fefdeed3391943016c7c935c'
PATCHED_ELF_NAME = 'SCUS_973.28.flare.elf'
HOLE_START = 0x00495574
HOLE_END = 0x00496000            # the function at 0x00496000 is the first thing after the padding
VISIBILITY_TEST = 0x003B7C98
FLARE_ROUTINE = 0x00498198
HALO_HELPER = 0x00495580
TRACK_HELPER = 0x004955A0
CAR_HELPER = 0x00495600


@dataclass(frozen=True)
class Redirect:
    address: int
    old_word: int
    target: int
    helper: str
    site: str
    effect: str

    @property
    def new_word(self):
        return jal(self.target)


REDIRECTS = (
    Redirect(0x003B7BE4, 0x0C0EDF26, TRACK_HELPER, 'track',
             'track-light queue fill 0x003B7B70 (caller loop 0x397BA0)',
             'track distance cull + track light cap, then the original visibility test'),
    Redirect(0x003B85C0, 0x0C0EDF26, CAR_HELPER, 'car',
             'car-light queue fill 0x003B8530 (RaceCarModel::virtual_35 0x3911D0)',
             'car distance cull + car light cap, then the original visibility test'),
    Redirect(0x003B8BA0, 0x0C126066, HALO_HELPER, 'halo',
             'car flare draw (queue A walk)',
             'no halo: $f19 = 0 before the flare routine, glow untouched'),
    Redirect(0x003B8CE8, 0x0C126066, HALO_HELPER, 'halo',
             'track flare draw (queue B walk)',
             'no halo: $f19 = 0 before the flare routine, glow untouched'),
)

# The helper sources: our own assembly, assembled by assemble() below and pinned word by word by the tests.
HALO_SOURCE = """
        lw   t0, 0x7F10(zero)
        andi t0, t0, 1
        beq  t0, zero, original
        nop
        mtc1 zero, f19
original:
        j 0x00498198
        nop
"""

FILL_SOURCE = """
        lw   t0, 0x7F10(zero)
        andi t1, t0, 4
        beq  t1, zero, depth
        nop
        lw   t1, {count}(a0)
        lw   t2, {cap}(zero)
        slt  t1, t1, t2
        bne  t1, zero, depth
        nop
        jr   ra
        addu v0, zero, zero
depth:
        andi t1, t0, 2
        beq  t1, zero, original
        nop
        lwc1 f0, {depth}(zero)
        c.lt.s f0, f13
        bc1f original
        nop
        mov.s f13, f0
original:
        j 0x003B7C98
        nop
"""

HELPERS = (
    ('halo', HALO_HELPER, HALO_SOURCE),
    ('track', TRACK_HELPER, FILL_SOURCE.format(count='0x1848', cap='0x7F20', depth='0x7F14')),
    ('car', CAR_HELPER, FILL_SOURCE.format(count='0x0A34', cap='0x7F24', depth='0x7F18')),
)


# --- a tiny MIPS assembler for the instructions the helpers use -----------------------------------------------------
# Register numbers (o32 names): the R5900 uses the same encoding for the forms below.
REGISTERS = {'zero': 0, 'at': 1, 'v0': 2, 'v1': 3, 'a0': 4, 'a1': 5, 'a2': 6, 'a3': 7, 't0': 8, 't1': 9, 't2': 10,
             't3': 11, 'ra': 31, 'sp': 29}
OPCODES = {'lw': 0x23, 'lwc1': 0x31, 'swc1': 0x39, 'sw': 0x2B, 'beq': 0x04, 'bne': 0x05, 'andi': 0x0C,
           'addiu': 0x09, 'slt': 0x00, 'sltu': 0x00}


def jal(target):
    return (0x03 << 26) | ((target >> 2) & 0x03FFFFFF)


def jump(target):
    return (0x02 << 26) | ((target >> 2) & 0x03FFFFFF)


def encode_i(op, rs, rt, imm):
    if not -0x8000 <= imm < 0x10000:
        raise ValueError('immediate out of range')
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def float_register(name):
    name = name.strip()
    if name.startswith('$'):
        name = name[1:]
    if not name.startswith('f') or not name[1:].isdigit():
        raise ValueError(f'not a float register: {name}')
    return int(name[1:])


def operand(token):
    """`t0`, `f13` or `0x7F10(zero)`: returns (kind, register number, offset)."""
    token = token.strip()
    if '(' in token:
        offset, rest = token.split('(', 1)
        base = rest.rstrip(')').strip().lstrip('$')
        return 'memory', REGISTERS[base], parse_number(offset)
    token = token.lstrip('$')
    if token in REGISTERS:
        return 'register', REGISTERS[token], 0
    if token.startswith('f'):
        return 'float', float_register(token), 0
    return 'immediate', 0, parse_number(token)


def parse_number(text):
    text = text.strip()
    if '+' in text:
        left, right = text.split('+', 1)
        return parse_number(left) + parse_number(right)
    return int(text, 0)


def assemble(source, base, labels):
    """Two passes over one function body; `labels` accumulates name -> address across every helper."""
    text = [(number, line.split('#', 1)[0].strip()) for number, line in enumerate(source.strip().splitlines())]
    lines = [(number, line) for number, line in text if line]
    parsed = []
    address = base
    local = {}
    for number, line in lines:
        if line.endswith(':'):
            local[line[:-1].strip()] = address
            continue
        mnemonic, _, rest = line.partition(' ')
        parsed.append((number, address, mnemonic.lower(), [item.strip() for item in rest.split(',') if item.strip()]))
        address += 4
    labels.update(local)
    words = []
    for number, at, mnemonic, args in parsed:
        if mnemonic == 'nop':
            words.append(0)
        elif mnemonic in ('lw', 'lwc1', 'swc1', 'sw'):
            kind, base_register, offset = operand(args[1])
            if kind != 'memory':
                raise ValueError(f'line {number}: {mnemonic} needs offset(base)')
            target = float_register(args[0]) if mnemonic in ('lwc1', 'swc1') else REGISTERS[args[0].lstrip('$')]
            words.append(encode_i(OPCODES[mnemonic], base_register, target, offset))
        elif mnemonic == 'andi':
            words.append(encode_i(0x0C, REGISTERS[args[1].lstrip('$')], REGISTERS[args[0].lstrip('$')],
                                  parse_number(args[2])))
        elif mnemonic == 'addiu':
            words.append(encode_i(0x09, REGISTERS[args[1].lstrip('$')], REGISTERS[args[0].lstrip('$')],
                                  parse_number(args[2])))
        elif mnemonic in ('beq', 'bne'):
            _, rs, _ = operand(args[0])
            _, rt, _ = operand(args[1])
            label = args[2].strip()
            destination = local[label] if label in local else labels[label]
            words.append(encode_i(OPCODES[mnemonic], rs, rt, (destination - (at + 4)) // 4))
        elif mnemonic == 'bc1f':
            label = args[0].strip()
            destination = local[label] if label in local else labels[label]
            words.append(encode_i(0x11, 0x08, 0, (destination - (at + 4)) // 4))
        elif mnemonic == 'slt':
            words.append((REGISTERS[args[1].lstrip('$')] << 21) | (REGISTERS[args[2].lstrip('$')] << 16) |
                         (REGISTERS[args[0].lstrip('$')] << 11) | 0x2A)
        elif mnemonic == 'addu':
            words.append((REGISTERS[args[1].lstrip('$')] << 21) | (REGISTERS[args[2].lstrip('$')] << 16) |
                         (REGISTERS[args[0].lstrip('$')] << 11) | 0x21)
        elif mnemonic == 'jr':
            words.append((REGISTERS[args[0].lstrip('$')] << 21) | 0x08)
        elif mnemonic == 'mtc1':
            words.append((0x11 << 26) | (0x04 << 21) | (REGISTERS[args[0].lstrip('$')] << 16) |
                         (float_register(args[1]) << 11))
        elif mnemonic == 'mov.s':
            words.append((0x11 << 26) | (0x10 << 21) | (float_register(args[1]) << 11) |
                         (float_register(args[0]) << 6) | 0x06)
        elif mnemonic == 'c.lt.s':
            words.append((0x11 << 26) | (0x10 << 21) | (float_register(args[1]) << 16) |
                         (float_register(args[0]) << 11) | 0x34)
        elif mnemonic == 'j':
            words.append(jump(parse_number(args[0])))
        else:
            raise ValueError(f'line {number}: unsupported mnemonic {mnemonic}')
    return words


def helper_blocks():
    """name -> (address, words) for every helper, with the label table shared so a jump may cross helpers."""
    labels = {}
    blocks = {}
    for name, address, source in HELPERS:
        blocks[name] = (address, assemble(source, address, labels))
    return blocks


def helper_bytes():
    """The padding content: a patch table of (address, word) for every helper word, and the zeros between them."""
    return helper_blocks()


# --- ELF helpers ------------------------------------------------------------------------------------------------------
def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_segments(elf):
    if elf[:4] != b'\x7fELF' or elf[4] != 1 or elf[5] != 1:
        raise ValueError('not a 32-bit little-endian ELF')
    phoff, = struct.unpack_from('<I', elf, 0x1C)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x2A)
    segments = []
    for index in range(phnum):
        kind, offset, vaddr, _, filesz, _, _, _ = struct.unpack_from('<8I', elf, phoff + index * phentsize)
        if kind == 1:
            segments.append((vaddr, offset, filesz))
    return segments


def file_offset(elf, address):
    for vaddr, offset, filesz in load_segments(elf):
        if vaddr <= address and address + 4 <= vaddr + filesz:
            return offset + address - vaddr
    raise ValueError(f'0x{address:08X} is not in a loaded segment')


def word_at(elf, address):
    return struct.unpack_from('<I', elf, file_offset(elf, address))[0]


def check_table():
    """Self-consistency of the table and the helpers (the fixture repeats this against the pinned words)."""
    targets = {}
    for redirect in REDIRECTS:
        if redirect.old_word != jal(FLARE_ROUTINE if redirect.helper == 'halo' else VISIBILITY_TEST):
            raise ValueError(f'0x{redirect.address:08X}: unexpected original call')
        helper_address = {'halo': HALO_HELPER, 'track': TRACK_HELPER, 'car': CAR_HELPER}[redirect.helper]
        if redirect.target != helper_address:
            raise ValueError(f'0x{redirect.address:08X}: redirect points outside its helper')
        if not HOLE_START <= helper_address < HOLE_END:
            raise ValueError('helper outside the zero padding')
        if redirect.address in targets:
            raise ValueError('two redirects at one address')
        targets[redirect.address] = redirect
    if len(REDIRECTS) != 4:
        raise ValueError('four redirects expected')
    blocks = helper_blocks()
    for name, (address, words) in blocks.items():
        if address + len(words) * 4 > HOLE_END:
            raise ValueError(f'{name} helper overruns the padding')
    return True


def hole_words(elf):
    return [word_at(elf, address) for address in range(HOLE_START, HOLE_END, 4)]


def references_into_hole(elf):
    """Every `j`/`jal` word whose target is the padding, plus a `lui rt,0x0049` + `ori/addiu rt,rt,imm` pair that builds
    an address inside it. (A raw data word equal to a padding address is not a reference: nothing loads the helpers.)"""
    found = []
    for vaddr, offset, filesz in load_segments(elf):
        if vaddr >= HOLE_START:
            continue
        previous_lui = None
        for at in range(vaddr, vaddr + filesz - 4, 4):
            word = struct.unpack_from('<I', elf, offset + at - vaddr)[0]
            opcode = word >> 26
            if opcode in (0x02, 0x03):
                target = 0x80000000 | ((word & 0x03FFFFFF) << 2)
                if HOLE_START <= (target & 0x7FFFFFFF) < HOLE_END:
                    found.append((at, f'jump 0x{target & 0x7FFFFFFF:08X}'))
            elif opcode == 0x0F and (word & 0xFFFF) == (HOLE_START >> 16):
                previous_lui = (at, (word >> 16) & 31)
            elif previous_lui and opcode in (0x0D, 0x09, 0x19) and ((word >> 16) & 31) == previous_lui[1]:
                address = (HOLE_START & 0xFFFF0000) | (word & 0xFFFF)
                if HOLE_START <= address < HOLE_END:
                    found.append((previous_lui[0], f'lui+ori address 0x{address:08X}'))
                previous_lui = None
            else:
                previous_lui = None
    return found


def verify_old_words(elf):
    check_table()
    for patch in aspect_elf_patch.PATCHES:
        observed = word_at(elf, patch.address)
        if observed != patch.old_word:
            raise ValueError(f'0x{patch.address:08X}: ELF word 0x{observed:08X}, table old word 0x{patch.old_word:08X}')
    for redirect in REDIRECTS:
        observed = word_at(elf, redirect.address)
        if observed != redirect.old_word:
            raise ValueError(f'0x{redirect.address:08X}: ELF word 0x{observed:08X}, table old word 0x{redirect.old_word:08X}')
    if any(hole_words(elf)):
        raise ValueError('the padding is not all zeros in the input ELF')
    references = references_into_hole(elf)
    if references:
        raise ValueError(f'code references the padding: {references}')
    return True


def patch_elf(elf):
    """The derived ELF: the TASK-080 P2 rows, the four redirects and our helpers; refuses unless every old word matches."""
    verify_old_words(elf)
    patched = bytearray(elf)
    for patch in aspect_elf_patch.PATCHES:
        struct.pack_into('<I', patched, file_offset(elf, patch.address), patch.new_word)
    for redirect in REDIRECTS:
        struct.pack_into('<I', patched, file_offset(elf, redirect.address), redirect.new_word)
    for name, (address, words) in helper_blocks().items():
        for index, word in enumerate(words):
            struct.pack_into('<I', patched, file_offset(elf, address + index * 4), word)
    return bytes(patched)


def verify_derived(elf, original):
    """`elf` must be exactly `patch(original)`, and `original` must be the pinned reconstructed ELF."""
    if sha256(original) != ELF_SHA256:
        raise ValueError('the reference ELF is not the pinned reconstructed ELF')
    verify_old_words(original)
    expected = patch_elf(original)
    if len(elf) != len(expected):
        raise ValueError('derived ELF size differs')
    changed = [index for index in range(len(expected)) if elf[index] != original[index]]
    expected_changed = [index for index in range(len(expected)) if expected[index] != original[index]]
    if changed != expected_changed:
        raise ValueError(f'{len(changed)} changed bytes, the table changes {len(expected_changed)}')
    if elf != expected:
        raise ValueError('derived ELF differs from the table outside the changed bytes')
    for redirect in REDIRECTS:
        if word_at(elf, redirect.address) != redirect.new_word:
            raise ValueError(f'0x{redirect.address:08X}: redirect word')
    for name, (address, words) in helper_blocks().items():
        for index, word in enumerate(words):
            if word_at(elf, address + index * 4) != word:
                raise ValueError(f'{name} helper word {index}')
    owned = {address + index * 4 for name, (address, words) in helper_blocks().items()
             for index in range(len(words))}
    stray = [address for address in range(HOLE_START, HOLE_END, 4)
             if address not in owned and word_at(elf, address) != 0]
    if stray:
        raise ValueError(f'non-zero words in the padding outside our helpers: {stray[:4]}')
    return True


def table_rows():
    rows = [{'kind': 'redirect', 'address': f'0x{r.address:08X}', 'old_word': f'0x{r.old_word:08X}',
             'new_word': f'0x{r.new_word:08X}', 'target': f'0x{r.target:08X}', 'helper': r.helper,
             'site': r.site, 'effect': r.effect} for r in REDIRECTS]
    for name, (address, words) in helper_blocks().items():
        for index, word in enumerate(words):
            rows.append({'kind': 'helper', 'helper': name, 'address': f'0x{address + index * 4:08X}',
                         'word': f'0x{word:08X}'})
    return rows


def patch_pairs():
    """(address, word) for every word the table writes: the two TASK-080 rows, the redirects and the helpers."""
    pairs = [(patch.address, patch.new_word) for patch in aspect_elf_patch.PATCHES]
    pairs += [(redirect.address, redirect.new_word) for redirect in REDIRECTS]
    for name, (address, words) in helper_blocks().items():
        pairs += [(address + index * 4, word) for index, word in enumerate(words)]
    return pairs


# --- producer and library ---------------------------------------------------------------------------------------------
def unit_holding(sources, address):
    label = f'L_{address:08X}:'
    owners = [f'unit_{name}.cpp' for name, text in sources.items() if label in text]
    if len(owners) == 1:
        return owners[0]
    for delta in range(4, 0x400, 4):
        label = f'L_{address - delta:08X}:'
        owners = [f'unit_{name}.cpp' for name, text in sources.items() if label in text]
        if len(owners) == 1:
            return owners[0]
    raise ValueError(f'no unit holds 0x{address:08X}')


# Units the study (2026-10-24) expects to be re-translated beyond the ones holding our words: the hole is in unit_00E5
# and its new entries renumber ids, so the units calling into that window are regenerated too (00A4, 00A5, 00AD, 00AE and
# the files calling the changed helpers). Recorded here after the first generation; the test pins the exact set.
EXPECTED_EXTRA_UNITS = ('unit_00A4.cpp', 'unit_00A5.cpp', 'unit_00AD.cpp', 'unit_00AE.cpp')
# The accepted seed set pins 138,813 translator entries for the original ELF (slice208 and the aspect profile, which only
# substitutes instructions). The four redirects add three dispatched helper entries (unit_00E5 cases 91-93: 0x495580,
# 0x4955A0, 0x495600); the translator reports 138,815 for this derived ELF (measured 2026-10-25, pinned here so a change
# in its accounting is caught).
EXPECTED_TRANSLATOR_ENTRIES = 138815
# Where each redirect must call its helper in the generated code: the return address is the original call's (call + 8),
# after its delay slot.
GENERATED_CALLS = (
    (0x003B7BE4, 0x003B7BEC, TRACK_HELPER),
    (0x003B85C0, 0x003B85C8, CAR_HELPER),
    (0x003B8BA0, 0x003B8BA8, HALO_HELPER),
    (0x003B8CE8, 0x003B8CF0, HALO_HELPER),
)


def check_generated_calls(sources):
    """Every redirect must be a call of its helper with the original return address (the delay slot runs before it)."""
    for site, return_address, helper in GENERATED_CALLS:
        matches = [unit for unit, text in sources.items()
                   if any(f'0x{return_address:08X}u' in line and f'c.pc = 0x{helper:08X}u' in line
                          for line in text.splitlines())]
        if len(matches) != 1:
            raise ValueError(f'0x{site:08X}: expected one generated call to 0x{helper:08X} returning to '
                             f'0x{return_address:08X}, found {matches}')
    return True


def produce(assets_root, output):
    import ee_entries
    import ee_producer
    ee_producer.prepare(assets_root, output)
    output, metadata, study = ee_producer.existing_producer(output)
    original = (output / 'SCUS_973.28.elf').read_bytes()
    if sha256(original) != ELF_SHA256:
        raise ValueError('reconstructed ELF differs from the pinned ELF')
    patched = patch_elf(original)
    patched_path = output / PATCHED_ELF_NAME
    with patched_path.open('xb') as destination:
        destination.write(patched)
    seed_file, seeds, pin = ee_entries.seed_set_pin(SEED_SET)
    ee_entries.check_seeds_executable(patched, seeds)
    copied = output / f'entries-{SEED_SET}.txt'
    with copied.open('xb') as destination:
        destination.write(seed_file.read_bytes())
    artifact = next(item for item in study['artifacts'] if item['path'].endswith('/eerecomp.exe'))
    translator = Path(metadata['assets_root']) / artifact['path']
    ee_producer.verify_identity(translator, {key: artifact[key] for key in ('size_bytes', 'sha256')})
    arguments = [str(translator), str(patched_path), '--out', str(output / 'generated'), '--stats', '--entries',
                 str(copied)]
    log_path = output / 'generation-flare.log'
    with log_path.open('xb') as log:
        subprocess.run(arguments, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=900)
    stats = ee_entries.parse_stats(log_path.read_text(encoding='utf-8', errors='replace'))
    if stats['rejected_seeds']:
        raise ValueError(f"Translator rejected {stats['rejected_seeds']} seed(s)")
    if stats['entries'] != EXPECTED_TRANSLATOR_ENTRIES:
        raise ValueError(f"Translator entries {stats['entries']}, expected {EXPECTED_TRANSLATOR_ENTRIES}")
    records = ee_producer.generated_inventory(output / 'generated')
    table, sources = ee_entries.read_generated_sources(output / 'generated')
    check_generated_calls(sources)
    resolved, failures = ee_entries.verify_resolution(table, sources, seeds)
    if failures:
        raise ValueError(f'Unresolved seeds: {failures}')
    if sum(text.count('stop(c,') for text in sources.values()):
        raise ValueError('generation emitted unsupported-code stops')
    base = Path(metadata['assets_root']) / BASE_PRODUCER
    base_generation = ee_producer.read_json(base / 'generation-entries.json')
    if ee_producer.generated_inventory(base / 'generated') != base_generation['generated']:
        raise ValueError('slice208 generated source differs from its receipt')
    changed = ee_entries.changed_units(base_generation['generated'], records)
    holders = sorted({unit_holding(sources, address) for address, _ in patch_pairs()})
    missing = [name for name in holders if name not in changed]
    if missing:
        raise ValueError(f'changed units {changed} do not include the units holding our words {missing}')
    result = {'schema_version': 1, 'task': 'TASK-117', 'slice': 299,
              'elf': {'path': 'SCUS_973.28.elf', 'sha256': sha256(original)},
              'patched_elf': {'path': patched_path.name, 'sha256': sha256(patched), 'size_bytes': len(patched)},
              'parameter_block': f'0x{PARAMETER_BLOCK:08X}', 'patches': table_rows(),
              'aspect_patches': aspect_elf_patch.table_rows(),
              'seed_set': SEED_SET, 'seed_pin': pin, 'translator': ee_producer.identity(translator),
              'arguments': arguments, 'stats': stats, 'resolved_seed_count': len(resolved),
              'generated': records, 'generated_summary': ee_producer.inventory_summary(records),
              'changed_units_vs_slice208': changed, 'units_holding_our_words': holders,
              'generated_calls': [{'site': f'0x{site:08X}', 'return_address': f'0x{back:08X}',
                                   'helper': f'0x{helper:08X}'} for site, back, helper in GENERATED_CALLS],
              'qualification': 'Source generation only; library, runtime and gameplay unqualified'}
    ee_producer.write_json(output / 'flare-generation.json', result)
    return result


def record_library(output):
    import ee_archive
    import ee_producer
    output, metadata, _ = ee_producer.existing_producer(output)
    generation = ee_producer.read_json(output / 'flare-generation.json')
    records = ee_producer.generated_inventory(output / 'generated')
    if records != generation['generated']:
        raise ValueError('generated source changed after the generation receipt')
    library = output / 'game-build/ps2game.lib'
    ee_producer.reject_link(library)
    receipt = ee_producer.verify_toolchain(output / 'game-build/toolchain.txt', metadata, output / 'generated')
    objects = ee_archive.object_inventory(library.read_bytes())
    base_library = Path(metadata['assets_root']) / BASE_PRODUCER / 'game-build/ps2game.lib'
    base_objects = {row['name']: row for row in ee_archive.object_inventory(base_library.read_bytes())}
    if {row['name'] for row in objects} != set(base_objects):
        raise ValueError('library members differ from the slice208 library')
    changed = sorted(row['name'] for row in objects if row != base_objects[row['name']])
    expected = sorted(name + '.obj' for name in generation['changed_units_vs_slice208'])
    if changed != expected:
        raise ValueError(f'changed objects {changed} differ from the changed units {expected}')
    relative = output.resolve().relative_to(Path(metadata['assets_root']).resolve()).as_posix()
    entry = {'path': f'{relative}/game-build/ps2game.lib', **ee_producer.identity(library),
             'patched_elf_sha256': generation['patched_elf']['sha256'],
             'parameter_block': generation['parameter_block'], 'patches': generation['patches'],
             'aspect_patches': generation['aspect_patches'], 'seed_set': generation['seed_set'],
             'seed_sha256': generation['seed_pin']['sha256'], 'translator_entries': generation['stats']['entries'],
             'source_inventory': generation['generated_summary'], 'object_count': len(objects),
             'object_inventory_without_timestamps': ee_archive.inventory_digest(objects),
             'changed_objects_vs_slice208': changed, 'compiler_receipt': receipt,
             'base_library': f'{BASE_PRODUCER}/game-build/ps2game.lib',
             'qualification': 'slice208 coverage library with the TASK-080 P2 rows and the TASK-117 flare limits; '
                              'owner gameplay pending'}
    manifest = {'schema_version': 1, 'slice': 299, 'donor_commit': metadata['donor_commit'],
                'profiles': {PROFILE: entry}, 'production_pins_modified': False}
    path = ROOT / LIBRARY_MANIFEST
    with path.open('x', encoding='utf-8', newline='\n') as destination:
        destination.write(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    return entry


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    check = commands.add_parser('check')
    check.add_argument('--elf', type=Path, required=True)
    verify = commands.add_parser('verify')
    verify.add_argument('--elf', type=Path, required=True)
    verify.add_argument('--original', type=Path, required=True)
    prod = commands.add_parser('produce')
    prod.add_argument('--assets-root', type=Path, required=True)
    prod.add_argument('--output', type=Path, required=True)
    rec = commands.add_parser('record-library')
    rec.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == 'check':
            verify_old_words(args.elf.read_bytes())
            print(json.dumps(table_rows(), indent=2))
        elif args.command == 'verify':
            verify_derived(args.elf.read_bytes(), args.original.read_bytes())
            print('flare-elf-patch: the derived ELF matches the table')
        elif args.command == 'produce':
            result = produce(args.assets_root, args.output)
            print(f"Generated {result['generated_summary']['file_count']} files; changed units "
                  f"{len(result['changed_units_vs_slice208'])}; patched ELF {result['patched_elf']['sha256']}")
        else:
            entry = record_library(args.output)
            print(f"Recorded library {entry['sha256']}; changed objects {len(entry['changed_objects_vs_slice208'])}")
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError, KeyError, StopIteration) as error:
        print(f'flare-elf-patch: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
