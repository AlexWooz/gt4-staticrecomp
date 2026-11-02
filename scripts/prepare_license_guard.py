"""Derive the TASK-017 license guard capsule from pinned MIT donor runtime sources (slice206; slice205 was v1).

The capsule holds three derived donor files that replace their donor originals in the build
(native/CMakeLists.txt, GT4_LICENSE_GUARD_SOURCE):
  * dispatch.cpp: the license demo guard (scripts/license_guard_overlay.inc, rule in native/license_guard.hpp).
    Always on: it only acts on the indirect call from either license demo loader (0x001100D0 -> vtbl[154],
    0x00110230 -> vtbl[57]) into its demo-input virtual, and only changes guest memory when the decrypted demo
    fails the game's own CRC and is not the single-Recorder layout. (v1, slice205, covered 0x001100D0 only.)
  * kernel.cpp: the slice204 halt forensic dump (scripts/halt_dump_overlay.inc); silent unless GT4_HALT_DUMP=1.
  * cdvdfsv.cpp: the slice181 FSV read trace (scripts/fsv_trace_overlay.inc); silent unless GT4_FSV_TRACE=1.

With --display-aspect (TASK-080; P1 capsule slice212c for the slice212b build, P2 capsule slice217) dispatch.cpp also
carries the race camera aspect hook and the RaceDisplay::render hook (scripts/display_aspect_overlay.inc, rule in
native/display_aspect.hpp), applied on top of the license guard: they act only on the camera virtual call of
RaceBase::initMonitor (return label 0x00388B38) and on the dispatch of RaceDisplay::render (0x0039F498, HUD canvas),
and write nothing at 16:9. The receipt then names the profile 'task017-guard-v2+task080-aspect-v2' (v1 was slice212c)
and the aspect overlay hash. Without the option the
output is byte-identical to the slice206 capsule.

The capsule is independent of the performance capsule (GT4_PERF_RUNTIME_SOURCE), which derives other donor
files (system, vu_micro, vif, gif, memory, hw, dmac). Each overlay is anchored on unique donor text and
removing it must restore the normalized input. The output must be a fresh immediate child of private/.
"""

import argparse
import hashlib
import json
from pathlib import Path

import prepare_perf_runtime as perf

ROOT = Path(__file__).resolve().parents[1]
DISPATCH_PIN = {'dispatch.cpp': '691c1d045b928ef789338f0954ec820fc0fd4e938cd2b2940743b3ad6005f995'}
GUARD_OVERLAY_PATH = ROOT / 'scripts/license_guard_overlay.inc'
GUARD_OVERLAY_ANCHORS = {
    'includes': ('after', '#include "ps2/system.hpp"\n'),
    'helpers': ('before', 'void dispatch(Cpu &c, std::uint32_t, std::uint32_t depth) {\n'),
    'hook': ('after', '    if (pc == kHandlerReturn || pc == kIdle) return;\n'),
}
ASPECT_OVERLAY_PATH = ROOT / 'scripts/display_aspect_overlay.inc'
# Anchors on the license guard output (the guard's own include, helpers and hook lines are unique there).
ASPECT_OVERLAY_ANCHORS = {
    'includes': ('after', '#include "license_guard.hpp"\n'),
    'helpers': ('before', 'void dispatch(Cpu &c, std::uint32_t, std::uint32_t depth) {\n'),
    'hook': ('after', '    if (gt4::license_guard::loader_for_return(c.r[31].w[0])) [[unlikely]] license_demo_guard(c);\n'),
}
FRAME_SKIP_OVERLAY_PATH = ROOT / 'scripts/frame_skip_overlay.inc'
# Anchors on the aspect output.
FRAME_SKIP_OVERLAY_ANCHORS = {
    'includes': ('after', '#include "display_aspect.hpp"\n'),
    'helpers': ('before', 'void dispatch(Cpu &c, std::uint32_t, std::uint32_t depth) {\n'),
    'hook': ('after', '    if (pc == gt4::display_aspect::kRaceDisplayRender) [[unlikely]] gt4::display_aspect::on_race_display_render(g_ram, kRamSize, c.r[4].w[0]);\n'),
}
GUARD_PROFILE = 'task017-guard-v2'
# v1 = slice212c (camera hook and race-view marker); v2 = slice217 (P2: the marker also runs the HUD canvas hook).
ASPECT_PROFILE = 'task017-guard-v2+task080-aspect-v2'
PREFIX = '// TASK-017 license guard capsule: MIT donor derivation; see native/THIRD_PARTY.md.\n'
CAPSULE_FILES = ('cdvdfsv.cpp', 'dispatch.cpp', 'kernel.cpp')


def apply_license_guard(source):
    """License guard overlay on donor dispatch.cpp; checks that removing its sections restores the input."""
    return perf.apply_overlay(source, GUARD_OVERLAY_PATH, GUARD_OVERLAY_ANCHORS, 'license guard')


def apply_display_aspect(source):
    """TASK-080 race camera aspect overlay on the license guard output; checks that removing it restores the input."""
    return perf.apply_overlay(source, ASPECT_OVERLAY_PATH, ASPECT_OVERLAY_ANCHORS, 'display aspect')


def apply_frame_skip(source):
    """TASK-082 frame skip overlay on the aspect output; checks that removing it restores the input."""
    return perf.apply_overlay(source, FRAME_SKIP_OVERLAY_PATH, FRAME_SKIP_OVERLAY_ANCHORS, 'frame skip')


def read_pinned(donor, name, expected):
    data = (donor / name).read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f'Qualified donor source differs: {name}')
    return data.decode('utf-8').replace('\r\n', '\n')


def overlay_sha256(path):
    return hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def derive_sources(assets_root=None, display_aspect=False, frame_counters=False, frame_skip=False):
    """Returns {capsule file name: derived text (LF)}."""
    donor = Path(assets_root or ROOT) / 'private/references/ps2-web-recomp/runtime/src'
    (dispatch_name, dispatch_sha), = DISPATCH_PIN.items()
    (halt_name, halt_sha), = perf.HALT_PIN.items()
    (fsv_name, fsv_sha), = perf.FSV_PIN.items()
    dispatch = apply_license_guard(read_pinned(donor, dispatch_name, dispatch_sha))
    if display_aspect:
        dispatch = apply_display_aspect(dispatch)
    if frame_skip:
        dispatch = apply_frame_skip(dispatch)
    kernel = perf.apply_halt_dump(read_pinned(donor, halt_name, halt_sha))
    if frame_counters:
        kernel = perf.apply_kernel_frame_counters(kernel)
    return {
        'dispatch.cpp': PREFIX + dispatch,
        'kernel.cpp': kernel,
        'cdvdfsv.cpp': perf.apply_fsv_trace(read_pinned(donor, fsv_name, fsv_sha)),
    }


def prepare(output, assets_root=None, display_aspect=False, frame_counters=False, frame_skip=False):
    output = Path(output).absolute()
    private = (Path(assets_root or ROOT) / 'private').resolve()
    if output != output.resolve() or output.parent != private:
        raise ValueError('License guard capsule must be an ordinary immediate private child')
    if output.exists():
        raise ValueError('Preserve existing output; choose a fresh license guard capsule')
    if frame_skip and not display_aspect:
        raise ValueError('The frame skip overlay anchors on the display aspect output')
    sources = derive_sources(assets_root, display_aspect, frame_counters, frame_skip)
    output.mkdir()
    receipt = {
        'schema_version': 1,
        'donor_revision': '1b4808793a12c45125262fd2d5f50a9ec9f7c020',
        'donor_sha256': {**DISPATCH_PIN, **perf.HALT_PIN, **perf.FSV_PIN},
        'license_guard_profile': ASPECT_PROFILE if display_aspect else GUARD_PROFILE,
        'license_guard_overlay_sha256': overlay_sha256(GUARD_OVERLAY_PATH),
        'halt_dump_overlay_sha256': overlay_sha256(perf.HALT_OVERLAY_PATH),
        'fsv_trace_overlay_sha256': overlay_sha256(perf.FSV_OVERLAY_PATH),
        'source_sha256': {},
    }
    if display_aspect:
        receipt['display_aspect_overlay_sha256'] = overlay_sha256(ASPECT_OVERLAY_PATH)
    if frame_counters:
        # TASK-084 step 0: the kernel counters (syscalls by number, thread switches) on top of the halt dump.
        receipt['frame_counters_profile'] = 'task084-frame-counters-v1'
    if frame_skip:
        receipt['frame_skip_overlay_sha256'] = overlay_sha256(FRAME_SKIP_OVERLAY_PATH)
    for name in CAPSULE_FILES:
        path = output / name
        path.write_text(sources[name], encoding='utf-8', newline='\n')
        receipt['source_sha256'][name] = hashlib.sha256(path.read_bytes()).hexdigest()
    (output / 'runtime.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8', newline='\n')
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--assets-root', type=Path)
    parser.add_argument('--display-aspect', action='store_true',
                        help='also apply the TASK-080 aspect hooks (current overlay: v2, slice217 capsule)')
    parser.add_argument('--frame-counters', action='store_true',
                        help='also apply the TASK-084 kernel counters (syscalls by number, thread switches)')
    parser.add_argument('--frame-skip', action='store_true', help='also apply the TASK-082 frame skip hook (needs --display-aspect)')
    options = parser.parse_args()
    print(json.dumps(prepare(options.output, options.assets_root, options.display_aspect, options.frame_counters,
                             options.frame_skip),
                     indent=2))
