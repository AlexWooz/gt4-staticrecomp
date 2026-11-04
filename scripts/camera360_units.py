"""TASK-159, TASK-160: translated EE units with the camera calls (native/camera_360.hpp, native/fov_adjust.hpp,
docs/CAMERA-360.md, docs/FOV-ADJUST.md).

The calls the camera needs are direct `jal`s, and the translator turns a direct `jal` into a direct C++ call into the callee's
unit: no dispatch hook and no VBlank write can reach them. This script copies one target's translated units into a fresh folder
and adds, at each call site of the target's table, one call to a host function between the call's delay slot and the call (the
point where the reference patch acts), plus a declaration of that function after `#include "units.hpp"` (outside
`namespace gen`):

  * the chase camera's yaw spring calls the angle-difference function with the camera's current yaw in f13:
    `before_chase_yaw_call` adds the right stick angle to f13;
  * the race input's DualShock analog loop hands each analog channel, with the function the controller setup assigns to it, to
    the channel handler: `before_pad_analog_channel_call` hands a right stick Y channel assigned to the accelerator or the
    brake over as 0, so the handler's own zero test skips it (no analog throttle or brake from the camera stick while the
    camera is on; TASK-159b narrowed it from the handler's cases to this loop and these channels);
  * TASK-160: the race cameras' projection function stores its scales through a call: `before_projection_scale_call` gives
    them the zoom factor of the field of view adjustment (gt4::fov_adjust).

It refuses a unit whose hash is not the pinned translation, a call line that is missing or appears twice, an output folder that
exists, and any result from which removing the insertions does not give the input back byte for byte. Every other file is
copied unchanged and compared. A receipt (<output>.camera360-units.json, next to the folder) records the inventories.

Idea and call site from the 360 Degree Chase Camera patch by Vyerq/unko_ (v2.00) and Nenkai's port from Duchi/Vyerq (Spec II).
No patch words are used: the call sites and callees were read from the reconstructed ELFs (docs/CAMERA-360.md).

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
CHANNEL_HOOK = 'gt4::camera360::before_pad_analog_channel_call'
PROJECTION_HOOK = 'gt4::fov_adjust::before_projection_scale_call'


@dataclasses.dataclass(frozen=True)
class CallSite:
    """One translated `jal` that gets a host call."""
    hook: str            # qualified host function (namespace::name)
    call_site: int       # guest address of the `jal`
    return_label: int    # call_site + 8; native/game_target.hpp holds it for the host check
    callee: int
    line: str            # the translated line of the call, exactly (with its indent and newline)


@dataclasses.dataclass(frozen=True)
class Unit:
    """One translated unit and the sites in it."""
    name: str            # file name
    sha256: str          # the file as translated (the pinned units of the target's EE library)
    sites: tuple


def call_line(return_label, slot, unit, entry, callee):
    """The translator's line for `jal callee` with this delay slot (donor recomp/codegen.cpp control(), Jal case)."""
    return (f'    {{ c.r[31].sd[0] = static_cast<std::int32_t>(0x{return_label:08X}u); {slot}  '
            f'if (depth < kMaxDepth) {unit}(c, {entry}u, depth + 1u); else {{ c.pc = 0x{callee:08X}u; return; }} '
            f'if (c.pc != 0x{return_label:08X}u) [[unlikely]] return; }}\n')


def hook_call(site):
    return f'::{site.hook}(c);'


def declaration(unit):
    """One `namespace <ns> { void <name>(ps2::Cpu &c); ... }` line per namespace of the unit's hooks, sorted."""
    namespaces = {}
    for site in unit.sites:
        namespace, _, name = site.hook.rpartition('::')
        namespaces.setdefault(namespace, set()).add(name)
    lines = ''.join(f'namespace {namespace} {{ {" ".join(f"void {name}(ps2::Cpu &c);" for name in sorted(names))} }}\n'
                    for namespace, names in sorted(namespaces.items()))
    return ('\n// TASK-159: the camera calls (native/camera_360.hpp, native/fov_adjust.hpp), added by scripts/camera360_units.py.\n'
            + lines)


# The DualShock analog loop's call to the channel handler (v2.00 func_0043BBC0 calls func_004386F0, Spec II 0x00319xxx calls
# 0x00314870) with a1 = 2, a2 = the slot's function and a3 = the channel value; the handler's case 3 stores the accelerator at
# +8 and case 4 the brake at +10, each only when the scaled value is not 0 (`beql a3,zero,<epilogue>`).

TARGETS = {
    # v2.00. Units: private/slice217-ee-aspect-producer/generated (the units of the rc12 EE library).
    'v200': (
        # func_00345658 (chase camera spring): jal 0x0036CC40 at 0x003456D0, delay slot lwc1 $f13,0($s0).
        Unit('unit_0091.cpp', 'ebfeb6c44925658acad222f8495875acba0ae0cf66478d664d84a8028c4ce280', (
            CallSite(YAW_HOOK, 0x003456D0, 0x003456D8, 0x0036CC40,
                     call_line(0x003456D8, 'c.f[13] = ld32((c.r[16].w[0]));', 'unit_009B', 40, 0x0036CC40)),)),
        # func_0037A170 (race camera projection): jal 0x00378F18 at 0x0037A1BC, delay slot mov.s $f13,$f12.
        Unit('unit_009E.cpp', 'b098efefef5b4572b2c8cac6f04c53232499db9e960963c6803e8c2ecdf6a525', (
            CallSite(PROJECTION_HOOK, 0x0037A1BC, 0x0037A1C4, 0x00378F18,
                     call_line(0x0037A1C4, 'c.f[13] = c.f[12];', 'unit_009E', 117, 0x00378F18)),)),
        # func_0043BBC0 (DualShock analog loop): jal 0x004386F0 at 0x0043BE6C, delay slot addiu a1,zero,2.
        Unit('unit_00CE.cpp', '43eeeea1c396a34ecb0c9818023cf2566f1ad2de0bddd5bde7d6431087c4e2b0', (
            CallSite(CHANNEL_HOOK, 0x0043BE6C, 0x0043BE74, 0x004386F0,
                     call_line(0x0043BE74, 'set32(c.r[5], 0u + 2u);', 'unit_00CE', 49, 0x004386F0)),)),
    ),
    # Spec II. Units: the Spec II translation pinned by docs/inputs/spec2-ee-generated.json.
    'spec2': (
        # 0x002063D8 (the same spring, Public Beta lineage): jal 0x00230F00 at 0x00206488, delay slot mov.s $f12,$f3; f13 is loaded
        # at 0x00206484, or in the delay slot of the bc1fl at 0x00206470 that branches to the call.
        Unit('unit_0041.cpp', '888fac97a864904dc0e0af0b7200467577b836e980ec6a64673e340a81c326a1', (
            CallSite(YAW_HOOK, 0x00206488, 0x00206490, 0x00230F00,
                     call_line(0x00206490, 'c.f[12] = c.f[3];', 'unit_004C', 57, 0x00230F00)),)),
        # 0x0023E7E0 (the same projection function): jal 0x0023D558 at 0x0023E82C, delay slot mov.s $f13,$f12.
        Unit('unit_004F.cpp', '1ae48aed3b77b2fea9a8cacbd9679989ca3478791b0403f8076883602c8aa26e', (
            CallSite(PROJECTION_HOOK, 0x0023E82C, 0x0023E834, 0x0023D558,
                     call_line(0x0023E834, 'c.f[13] = c.f[12];', 'unit_004F', 167, 0x0023D558)),)),
        # The same DualShock analog loop: jal 0x00314870 at 0x003190EC, delay slot daddu a3,t0,zero (the value).
        Unit('unit_0086.cpp', '8e721d41de7bcea54f0a9f8005bc3723b2e106c2d78482e4b889b37a3cd71223', (
            CallSite(CHANNEL_HOOK, 0x003190EC, 0x003190F4, 0x00314870,
                     call_line(0x003190F4, 'c.r[7].d[0] = c.r[8].d[0] + 0ull;', 'unit_0085', 58, 0x00314870)),)),
    ),
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def hooked_line(site):
    if site.line.count(CALL_MARK) != 1:
        raise ValueError('the call line must hold the call exactly once')
    return site.line.replace(CALL_MARK, f' {hook_call(site)} if (depth < kMaxDepth)')


def hook_unit_text(text, unit):
    """The unit with the declaration and its calls added. Refuses anything but one include line and one line per site."""
    if text.count(INCLUDE_LINE) != 1 or not text.startswith(INCLUDE_LINE):
        raise ValueError(f'{unit.name}: expected one leading {INCLUDE_LINE.strip()}')
    if any(f'::{site.hook}(' in text for site in unit.sites) or '// TASK-159: the camera calls' in text:
        raise ValueError(f'{unit.name}: already holds camera calls')
    result = text.replace(INCLUDE_LINE, INCLUDE_LINE + declaration(unit), 1)
    for site in unit.sites:
        if text.count(site.line) != 1:
            raise ValueError(f'{unit.name}: the call line of 0x{site.call_site:08X} occurs {text.count(site.line)} times, expected once')
        result = result.replace(site.line, hooked_line(site), 1)
    return result


def unhook_unit_text(text, unit):
    """Removes exactly what hook_unit_text added."""
    if text.count(declaration(unit)) != 1 or sum(text.count(hook_call(site)) for site in unit.sites) != len(unit.sites):
        raise ValueError(f'{unit.name}: not a unit hooked by this script')
    result = text.replace(declaration(unit), '', 1)
    for site in unit.sites:
        if result.count(hooked_line(site)) != 1:
            raise ValueError(f'{unit.name}: the hooked line of 0x{site.call_site:08X} is missing')
        result = result.replace(hooked_line(site), site.line, 1)
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
        'task': 'TASK-159',
        'target': target,
        'source': str(source),
        'units': [{
            'unit': name,
            'sha256_before': unit.sha256,
            'sha256_after': sha256((output / name).read_bytes()),
            'sites': [{'hook': site.hook, 'call_site': f'0x{site.call_site:08X}', 'return_label': f'0x{site.return_label:08X}',
                       'callee': f'0x{site.callee:08X}'} for site in unit.sites],
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
