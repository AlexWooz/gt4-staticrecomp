"""TASK-159, TASK-095: translated EE units with the 360-degree chase camera calls and the 2D overlay canvas calls
(native/camera_360.hpp, native/display_aspect.hpp; docs/CAMERA-360.md, docs/ULTRAWIDE-OVERLAYS.md).

The calls the camera needs are direct `jal`s, and the translator turns a direct `jal` into a direct C++ call into the callee's
unit: no dispatch hook and no VBlank write can reach them. This script copies one target's translated units into a fresh folder
and adds, at each call site of the target's table, one call to a host function between the call's delay slot and the call (the
point where the reference patch acts), plus a declaration of that function after `#include "units.hpp"` (outside
`namespace gen`):

  * the chase camera's yaw spring calls the angle-difference function with the camera's current yaw in f13:
    `before_chase_yaw_call` adds the right stick angle to f13;
  * the race input's analog channel handler scales an accelerator or brake channel (cases 3 and 4) before storing it:
    `before_analog_pedal_call` sets the value to 0 so the game's own zero test skips it (no analog throttle or brake from the
    camera stick while the camera is on);
  * TASK-095: the menu context (the pre-race screens) and the strobe overlay renderer (pause, pit menu, results) set their 2D
    canvas through the ortho function with the edges in f12..f15: `before_overlay_ortho_call` widens it while the presentation
    stretches the frame to a ratio other than 16:9 (gt4::display_aspect); `after_overlay_draw`, before the renderer's first
    call after its draw, closes the window in which the strobe text is narrowed.

TASK-095 also routes statements through host functions (no call there): the text style parser stores the text x and y scale
(1.0, then the style's percentages), and each of those four stores `st32((c.r[18].w[0] + N), V);` becomes
`st32((c.r[18].w[0] + N), ::gt4::display_aspect::overlay_text_scale_x(V));` (or `_y`), found in its own labelled block exactly
once; outside a widened strobe overlay both functions return V.

It refuses a unit whose hash is not the pinned translation, a call line that is missing or appears twice, an output folder that
exists, and any result from which removing the insertions does not give the input back byte for byte. Every other file is
copied unchanged and compared. A receipt (<output>.camera360-units.json, next to the folder) records the inventories.

Idea and call site from the 360 Degree Chase Camera patch by Vyerq/unko_ (v2.00) and Nenkai's port from Duchi/Vyerq (Spec II).
No patch words are used: the call sites and callees were read from the reconstructed ELFs (docs/CAMERA-360.md). The overlay call
sites were read from the translated units (the ortho function's callers; names from gt4-decomp 3b1f2ae5b).

usage: camera360_units.py <v200|spec2> <translated units folder> <fresh output folder>
"""
import argparse
import dataclasses
import hashlib
import json
import shutil
import sys
from pathlib import Path

INCLUDE_LINE = '#include "units.hpp"\n'
CALL_MARK = '  if (depth < kMaxDepth)'
YAW_HOOK = 'gt4::camera360::before_chase_yaw_call'
PEDAL_HOOK = 'gt4::camera360::before_analog_pedal_call'
OVERLAY_HOOK = 'gt4::display_aspect::before_overlay_ortho_call'
OVERLAY_END_HOOK = 'gt4::display_aspect::after_overlay_draw'
TEXT_X_HOOK = 'gt4::display_aspect::overlay_text_scale_x'
TEXT_Y_HOOK = 'gt4::display_aspect::overlay_text_scale_y'


@dataclasses.dataclass(frozen=True)
class CallSite:
    """One translated `jal` that gets a host call."""
    hook: str            # qualified host function (namespace::name)
    call_site: int       # guest address of the `jal`
    return_label: int    # call_site + 8; native/game_target.hpp holds it for the host check
    callee: int
    line: str            # the translated line of the call, exactly (with its indent and newline)

    def declares(self, name):
        return f'void {name}(ps2::Cpu &c);'

    @property
    def place(self):
        return f'0x{self.call_site:08X}'


@dataclasses.dataclass(frozen=True)
class StatementSite:
    """One translated statement whose value goes through a host function (TASK-095)."""
    hook: str            # qualified host function std::uint32_t name(std::uint32_t)
    address: int         # guest address of the instruction
    label: str           # the labelled block that holds it (`L_00490290`)
    original: str        # the statement line exactly (indent and newline)
    replacement: str     # the same statement through the host function

    def declares(self, name):
        return f'std::uint32_t {name}(std::uint32_t value);'

    @property
    def place(self):
        return f'0x{self.address:08X}'


def text_scale_site(address, label, offset, register, axis):
    """`swc1 $fR,offset(s2)` of the text style parser: the stored scale goes through overlay_text_scale_<axis>."""
    hook = TEXT_X_HOOK if axis == 'x' else TEXT_Y_HOOK
    return StatementSite(hook, address, label, f'    st32((c.r[18].w[0] + {offset}u), c.f[{register}]);\n',
                         f'    st32((c.r[18].w[0] + {offset}u), ::{hook}(c.f[{register}]));\n')


@dataclasses.dataclass(frozen=True)
class Unit:
    """One translated unit and the sites in it."""
    name: str            # file name
    sha256: str          # the file as translated (the pinned units of the target's EE library)
    sites: tuple


def call_line(return_label, slot, unit, entry, callee):
    """The translator's line for `jal callee` with this delay slot (donor recomp/codegen.cpp control(), Jal case); an empty
    slot (a `nop`) leaves two spaces."""
    slot = f'{slot}  ' if slot else ' '
    return (f'    {{ c.r[31].sd[0] = static_cast<std::int32_t>(0x{return_label:08X}u); {slot}'
            f'if (depth < kMaxDepth) {unit}(c, {entry}u, depth + 1u); else {{ c.pc = 0x{callee:08X}u; return; }} '
            f'if (c.pc != 0x{return_label:08X}u) [[unlikely]] return; }}\n')


def hook_call(site):
    return f'::{site.hook}(c);'


def marker(hook):
    return f'::{hook}('


def declaration(unit):
    """One `namespace <ns> { <declaration> ... }` line per namespace of the unit's hooks, sorted."""
    namespaces = {}
    for site in unit.sites:
        namespace, _, name = site.hook.rpartition('::')
        namespaces.setdefault(namespace, {})[name] = site.declares(name)
    lines = ''.join(f'namespace {namespace} {{ {" ".join(names[name] for name in sorted(names))} }}\n'
                    for namespace, names in sorted(namespaces.items()))
    return ('\n// TASK-159, TASK-095: host calls (native/camera_360.hpp, display_aspect.hpp), added by scripts/camera360_units.py.\n'
            + lines)


# The analog channel handler's value scaler (v2.00 func_004386C8, Spec II 0x00314848), called with the channel value in a0 from
# its delay slot `daddu a0,a3,zero`: case 3 stores the accelerator at +8, case 4 the brake at +10, each only when the scaled
# value is not 0 (`beql a3,zero,<epilogue>`).
PEDAL_SLOT = 'c.r[4].d[0] = c.r[7].d[0] + 0ull;'

# TASK-095: the strobe renderer takes f17 (far, 1.0) in the delay slot of its ortho call, the menu context f16 (near, 0); the
# other edges are set before it.

TARGETS = {
    # v2.00. Units: private/slice217-ee-aspect-producer/generated (the units of the rc12 EE library).
    'v200': (
        # func_00345658 (chase camera spring): jal 0x0036CC40 at 0x003456D0, delay slot lwc1 $f13,0($s0).
        Unit('unit_0091.cpp', 'ebfeb6c44925658acad222f8495875acba0ae0cf66478d664d84a8028c4ce280', (
            CallSite(YAW_HOOK, 0x003456D0, 0x003456D8, 0x0036CC40,
                     call_line(0x003456D8, 'c.f[13] = ld32((c.r[16].w[0]));', 'unit_009B', 40, 0x0036CC40)),)),
        # func_004386F0 (analog channel handler): jal 0x004386C8 at 0x004387C0 (case 3) and 0x004387E8 (case 4).
        Unit('unit_00CE.cpp', '43eeeea1c396a34ecb0c9818023cf2566f1ad2de0bddd5bde7d6431087c4e2b0', (
            CallSite(PEDAL_HOOK, 0x004387C0, 0x004387C8, 0x004386C8, call_line(0x004387C8, PEDAL_SLOT, 'unit_00CE', 48, 0x004386C8)),
            CallSite(PEDAL_HOOK, 0x004387E8, 0x004387F0, 0x004386C8, call_line(0x004387F0, PEDAL_SLOT, 'unit_00CE', 48, 0x004386C8)),
        )),
        # TASK-095: mUpdateContextPS2's render 0x00214FB0 (the menu context; the pre-race screens over the race): jal 0x004A5A90
        # at 0x002151A0, 0..640 x 480..0, near 0, far 100.
        Unit('unit_0045.cpp', 'f63e461a05ca26799a443a1323e5ef26fd1b7e1dd97283a3b4361ad9090e449f', (
            CallSite(OVERLAY_HOOK, 0x002151A0, 0x002151A8, 0x004A5A90,
                     call_line(0x002151A8, 'c.f[16] = c.f[12];', 'unit_00E9', 94, 0x004A5A90)),)),
        # TASK-095: func_003EC0B0 (the strobe overlay renderer): jal 0x004A5A90 at 0x003EC1F8, 0..640 x 480..0; after the draw
        # (0x00475CA0), jal 0x004A5400 at 0x003EC240 (delay slot nop), where the text scale is put back.
        Unit('unit_00BB.cpp', '7b430cfccf3e8811ea7dfdb35744ecc4c8911fc1078845b13283222d899a5298', (
            CallSite(OVERLAY_HOOK, 0x003EC1F8, 0x003EC200, 0x004A5A90,
                     call_line(0x003EC200, 'c.f[17] = c.f[21];', 'unit_00E9', 94, 0x004A5A90)),
            CallSite(OVERLAY_END_HOOK, 0x003EC240, 0x003EC248, 0x004A5400,
                     call_line(0x003EC248, '', 'unit_00E9', 80, 0x004A5400)),
        )),
        # TASK-095: the text style parser 0x00490290: 1.0 into x and y (0x004902C8, 0x004902CC), then the style's percentages
        # (y at 0x0049038C, x at 0x00490390); the text context's x and y scale are at +0x24 and +0x28.
        Unit('unit_00E4.cpp', '2b461f117174664dd2f8402f6f91738de48f18fe2cf4f40bf67b5c27e3044f09', (
            text_scale_site(0x004902C8, 'L_00490290', 36, 0, 'x'),
            text_scale_site(0x004902CC, 'L_00490290', 40, 0, 'y'),
            text_scale_site(0x0049038C, 'L_00490374', 40, 1, 'y'),
            text_scale_site(0x00490390, 'L_00490374', 36, 0, 'x'),
        )),
    ),
    # Spec II. Units: the Spec II translation pinned by docs/inputs/spec2-ee-generated.json.
    'spec2': (
        # 0x002063D8 (the same spring, Public Beta lineage): jal 0x00230F00 at 0x00206488, delay slot mov.s $f12,$f3; f13 is loaded
        # at 0x00206484, or in the delay slot of the bc1fl at 0x00206470 that branches to the call.
        Unit('unit_0041.cpp', '888fac97a864904dc0e0af0b7200467577b836e980ec6a64673e340a81c326a1', (
            CallSite(YAW_HOOK, 0x00206488, 0x00206490, 0x00230F00,
                     call_line(0x00206490, 'c.f[12] = c.f[3];', 'unit_004C', 57, 0x00230F00)),)),
        # 0x00314870 (the analog channel handler, ten cases in Spec II): jal 0x00314848 at 0x00314940 and 0x00314968.
        Unit('unit_0085.cpp', 'c1fcc339f5ff7e6aab30565ef96651cb3e8e9334007f2b9d6a2748c5eca24e7d', (
            CallSite(PEDAL_HOOK, 0x00314940, 0x00314948, 0x00314848, call_line(0x00314948, PEDAL_SLOT, 'unit_0085', 57, 0x00314848)),
            CallSite(PEDAL_HOOK, 0x00314968, 0x00314970, 0x00314848, call_line(0x00314970, PEDAL_SLOT, 'unit_0085', 57, 0x00314848)),
        )),
        # TASK-095: 0x001FDCC8 (the strobe overlay renderer): jal 0x005A2600 at 0x001FDE10, 0..640 x 480..0; after the draw
        # (0x00341918), jal 0x005A1E08 at 0x001FDE58 (delay slot nop), where the text scale is put back.
        Unit('unit_003F.cpp', '99df01b8f95c8ca79d97e3dd9f7e79028b8b420f91a8f22dab1e5745c293d078', (
            CallSite(OVERLAY_HOOK, 0x001FDE10, 0x001FDE18, 0x005A2600,
                     call_line(0x001FDE18, 'c.f[17] = c.f[23];', 'unit_0128', 135, 0x005A2600)),
            CallSite(OVERLAY_END_HOOK, 0x001FDE58, 0x001FDE60, 0x005A1E08,
                     call_line(0x001FDE60, '', 'unit_0128', 120, 0x005A1E08)),
        )),
        # TASK-095: the text style parser 0x0033AF90 (the same code; the scales at +0x3C and +0x40): 1.0 at 0x0033AFC8 and
        # 0x0033AFCC, the style's percentages at 0x0033B08C (y) and 0x0033B090 (x).
        Unit('unit_008E.cpp', '0d0b0715f1186f990699c1a167aed95861c2d996b5dc4f17e3a53117ce30002f', (
            text_scale_site(0x0033AFC8, 'L_0033AF90', 60, 0, 'x'),
            text_scale_site(0x0033AFCC, 'L_0033AF90', 64, 0, 'y'),
            text_scale_site(0x0033B08C, 'L_0033B074', 64, 1, 'y'),
            text_scale_site(0x0033B090, 'L_0033B074', 60, 0, 'x'),
        )),
        # TASK-095: 0x0036D278 (the menu context's render): jal 0x005A2600 at 0x0036D4D0; the canvas size comes from the words at
        # 0x006661D0 and 0x006661D4, near 0, far 100.
        Unit('unit_009B.cpp', '38fa8fdeaa4795c5c4d705a58b21aabacd10ddc1d1e55f71fccd2c4b42d0d8c4', (
            CallSite(OVERLAY_HOOK, 0x0036D4D0, 0x0036D4D8, 0x005A2600,
                     call_line(0x0036D4D8, 'c.f[16] = c.f[12];', 'unit_0128', 135, 0x005A2600)),)),
    ),
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def hooked_line(site):
    if site.line.count(CALL_MARK) != 1:
        raise ValueError('the call line must hold the call exactly once')
    return site.line.replace(CALL_MARK, f' {hook_call(site)} if (depth < kMaxDepth)', 1)


def block_span(text, label):
    """The text range of a labelled block: from its label line to the next label line (or the end of the function)."""
    if text.count(f'\n{label}:\n') != 1:
        raise ValueError(f'label {label} missing or repeated')
    start = text.find(f'\n{label}:\n') + len(label) + 3
    following = text.find('\nL_', start)
    end = text.find('\n}\n', start) if following < 0 else following
    return start, end + 1


def replace_in_block(text, label, old, new):
    """`old` replaced by `new` inside the block of `label`, where it must occur exactly once."""
    start, end = block_span(text, label)
    block = text[start:end]
    if block.count(old) != 1:
        raise ValueError(f'{label}: {old.strip()!r} occurs {block.count(old)} times in the block, expected once')
    return text[:start] + block.replace(old, new, 1) + text[end:]


def apply_site(text, site, undo=False):
    if isinstance(site, StatementSite):
        old, new = (site.replacement, site.original) if undo else (site.original, site.replacement)
        return replace_in_block(text, site.label, old, new)
    old, new = (hooked_line(site), site.line) if undo else (site.line, hooked_line(site))
    if text.count(old) != 1:
        raise ValueError(f'the line of {site.place} occurs {text.count(old)} times, expected once')
    return text.replace(old, new, 1)


def hook_unit_text(text, unit):
    """The unit with the declaration and its sites added. Refuses anything but one include line and one line per site."""
    if text.count(INCLUDE_LINE) != 1 or not text.startswith(INCLUDE_LINE):
        raise ValueError(f'{unit.name}: expected one leading {INCLUDE_LINE.strip()}')
    for hook in {site.hook for site in unit.sites}:
        namespace = hook.rpartition('::')[0]
        if marker(hook) in text or f'namespace {namespace} {{' in text:
            raise ValueError(f'{unit.name}: already holds calls of {namespace}')
    result = text.replace(INCLUDE_LINE, INCLUDE_LINE + declaration(unit), 1)
    for site in unit.sites:
        try:
            result = apply_site(result, site)
        except ValueError as error:
            raise ValueError(f'{unit.name}: {error}') from None
    return result


def unhook_unit_text(text, unit):
    """Removes exactly what hook_unit_text added."""
    calls = sum(text.count(marker(hook)) for hook in {site.hook for site in unit.sites})
    if text.count(declaration(unit)) != 1 or calls != len(unit.sites):
        raise ValueError(f'{unit.name}: not a unit hooked by this script')
    result = text.replace(declaration(unit), '', 1)
    for site in unit.sites:
        try:
            result = apply_site(result, site, undo=True)
        except ValueError as error:
            raise ValueError(f'{unit.name}: {error}') from None
    return result


def qualify_unit(original, hooked, unit):
    """Raises unless `hooked` is `original` plus exactly the insertions."""
    if hook_unit_text(original, unit) != hooked or unhook_unit_text(hooked, unit) != original:
        raise ValueError(f'{unit.name}: the hooked unit differs from its input beyond the insertions')


def inventory(folder):
    files = sorted(path for path in folder.iterdir())
    for path in files:
        if not path.is_file() or path.is_symlink():
            raise ValueError(f'unexpected entry in a units folder: {path}')
    return [{'path': path.name, 'size_bytes': path.stat().st_size, 'sha256': sha256(path.read_bytes())} for path in files]


def inventory_digest(records):
    return sha256(json.dumps(records, sort_keys=True).encode())


def make_units(target, source, output, targets=TARGETS):
    units = {unit.name: unit for unit in targets[target]}
    source, output = Path(source), Path(output)
    if output.exists():
        raise ValueError(f'{output} exists; choose a fresh folder')
    originals, hooked = {}, {}
    for name, unit in units.items():
        data = (source / name).read_bytes()
        if sha256(data) != unit.sha256:
            raise ValueError(f'{source / name} is not the pinned translation ({sha256(data)})')
        originals[name] = data.decode('ascii')
        hooked[name] = hook_unit_text(originals[name], unit)
        qualify_unit(originals[name], hooked[name], unit)
    before = inventory(source)
    output.mkdir(parents=True)
    for row in before:
        if row['path'] in units:
            (output / row['path']).write_bytes(hooked[row['path']].encode('ascii'))
        else:
            shutil.copyfile(source / row['path'], output / row['path'])
    after = inventory(output)
    changed = sorted(b['path'] for a, b in zip(before, after) if a != b)
    if [a['path'] for a in before] != [b['path'] for b in after] or changed != sorted(units):
        raise ValueError(f'unexpected output inventory: changed {changed}')
    for name, unit in units.items():
        qualify_unit(originals[name], (output / name).read_bytes().decode('ascii'), unit)
    if inventory(source) != before:
        raise ValueError('the input units changed while copying')
    receipt = {
        'schema': 'camera360-units/2',
        'tasks': ['TASK-159', 'TASK-095'],
        'target': target,
        'source': str(source),
        'units': [{
            'unit': name,
            'sha256_before': unit.sha256,
            'sha256_after': sha256((output / name).read_bytes()),
            'sites': [{'hook': site.hook, 'call_site': f'0x{site.call_site:08X}', 'return_label': f'0x{site.return_label:08X}',
                       'callee': f'0x{site.callee:08X}'} if isinstance(site, CallSite) else
                      {'hook': site.hook, 'statement': f'0x{site.address:08X}', 'label': site.label} for site in unit.sites],
        } for name, unit in units.items()],
        'files': len(after),
        'source_inventory_sha256': inventory_digest(before),
        'output_inventory_sha256': inventory_digest(after),
    }
    (output.parent / f'{output.name}.camera360-units.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return receipt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('target', choices=sorted(TARGETS))
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    options = parser.parse_args(argv)
    print(json.dumps(make_units(options.target, options.source, options.output), indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
