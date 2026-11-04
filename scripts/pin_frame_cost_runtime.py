"""Pin one TASK-084 (slice260..269) frame-cost runtime capsule into docs/inputs/<capsule name>.json.

Run after scripts/prepare_perf_runtime.py --mtvu --gs-stream --frame-cost <capsule>. The capsule's own runtime.json
carries the donor pins, the derived source hashes and the derivation profile; the manifest adds the qualified port and
shader inputs (copied from the manifest of the port the build uses, slice200-port by default) and the receipt hash.

The output file must not exist: a new capsule always gets a new pin file (docs/inputs are history, never edited).
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASE = 'docs/inputs/slice200-gs-stream-runtime.json'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pin(capsule_name, assets_root=None, base_manifest=DEFAULT_BASE, apply=True):
    assets = Path(assets_root or ROOT)
    capsule = assets / 'private' / capsule_name
    receipt_path = capsule / 'runtime.json'
    if not receipt_path.exists():
        raise ValueError(f'capsule receipt not found: {receipt_path}')
    receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
    base = json.loads((ROOT / base_manifest).read_text(encoding='utf-8'))
    if 'inputs' not in base:
        raise ValueError(f'base manifest has no port/shader inputs: {base_manifest}')
    manifest = {
        'donor_revision': receipt['donor_revision'],
        'donor_sha256': receipt['donor_sha256'],
        'source_sha256': {name: sha256(capsule / name) for name in receipt['source_sha256']},
        'receipt_sha256': sha256(receipt_path),
        'inputs': base['inputs'],
        'vu_scope_profile': receipt.get('vu_scope_profile'),
        'vu1_aot_profile': receipt.get('vu1_aot_profile'),
        'vu1_aot_overlay_sha256': receipt.get('vu1_aot_overlay_sha256'),
        'vu0_aot_profile': receipt.get('vu0_aot_profile'),
        'vu0_aot_overlay_sha256': receipt.get('vu0_aot_overlay_sha256'),
        'mtvu_runtime_profile': receipt.get('mtvu_runtime_profile'),
        'gs_stream_profile': receipt.get('gs_stream_profile'),
        'gs_stream_sources': receipt.get('gs_stream_sources'),
        'spec2_iop_profile': receipt.get('spec2_iop_profile'),
        'spec2_iop_overlay_sha256': receipt.get('spec2_iop_overlay_sha256'),
    }
    if receipt['source_sha256'] != manifest['source_sha256']:
        raise ValueError('capsule sources changed after the receipt was written')
    output = ROOT / 'docs/inputs' / f'{capsule_name}.json'
    if apply:
        if output.exists():
            raise ValueError(f'preserve the existing pin: {output}')
        output.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
    return manifest, output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capsule', help='capsule directory name under private/')
    parser.add_argument('--assets-root', type=Path)
    parser.add_argument('--base-manifest', default=DEFAULT_BASE)
    options = parser.parse_args()
    manifest, output = pin(options.capsule, options.assets_root, options.base_manifest)
    print(json.dumps({'pin': str(output), 'receipt_sha256': manifest['receipt_sha256']}, indent=2))
