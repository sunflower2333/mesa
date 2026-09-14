#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage one-revision ARM64 WGL/Zink/Turnip candidate, not a native Gallium port."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

DLLS = ('opengl32.dll', 'libgallium_wgl.dll', 'vulkan_freedreno.dll', 'z-1.dll')
PAYLOADS = DLLS + ('zink_wgl_probe.exe', 'freedreno_icd.arm64.json', 'run-probe.ps1', 'README.md')


# Reject non-ARM64 or truncated PE payloads without loading them.
def arm64_pe(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError(f'{path.name}: invalid DOS header')
    offset = struct.unpack_from('<I', data, 0x3c)[0]
    if offset < 64 or offset > len(data) - 24 or data[offset:offset + 4] != b'PE\0\0':
        raise ValueError(f'{path.name}: invalid PE header')
    if struct.unpack_from('<H', data, offset + 4)[0] != 0xaa64:
        raise ValueError(f'{path.name}: expected ARM64 AA64 image')


# Require a unique built file; never silently pick a stale alternate output.
def unique_file(build: Path, name: str) -> Path:
    matches = list(build.rglob(name))
    if len(matches) != 1 or not matches[0].is_file() or matches[0].is_symlink():
        raise ValueError(f'expected exactly one regular {name}, found {len(matches)}')
    return matches[0]


# Bind the recorded route, architecture, exact inventory and bytes to one revision.
def verify(output: Path, revision: str) -> dict:
    receipt = json.loads((output / 'bundle.json').read_text(encoding='utf-8'))
    if (receipt.get('schema') != 1 or receipt.get('mesa_commit') != revision or
            not re.fullmatch('[0-9a-f]{40}', revision) or receipt.get('architecture') != 'arm64' or
            receipt.get('native_freedreno_gallium') is not False or
            receipt.get('gpu_test_executed') is not False or
            receipt.get('route') != 'OpenGL/WGL -> Zink -> Vulkan -> Turnip/WDDM'):
        raise ValueError('invalid candidate provenance')
    if set(receipt.get('files', {})) != set(PAYLOADS):
        raise ValueError('invalid receipt inventory')
    if {p.name for p in output.iterdir()} != set(PAYLOADS) | {'bundle.json'}:
        raise ValueError('unexpected or missing bundle payload')
    for name in PAYLOADS:
        path = output / name
        if path.is_symlink() or not path.is_file() or not path.stat().st_size:
            raise ValueError(f'not a nonempty regular payload: {name}')
        if hashlib.sha256(path.read_bytes()).hexdigest() != receipt['files'][name]:
            raise ValueError(f'payload hash mismatch: {name}')
        if name.endswith(('.dll', '.exe')):
            arm64_pe(path)
    icd = json.loads((output / 'freedreno_icd.arm64.json').read_text())
    if (icd.get('file_format_version') != '1.0.1' or
            icd.get('ICD', {}).get('library_path') != '.\\vulkan_freedreno.dll' or
            icd['ICD'].get('library_arch') != '64' or
            not re.fullmatch(r'1\.4\.[0-9]+', icd['ICD'].get('api_version', ''))):
        raise ValueError('invalid app-local Turnip ICD')
    return receipt


# Stage exclusively from the current build and source; leave the OS untouched.
def stage(build: Path, probe: Path, output: Path, revision: str) -> dict:
    if not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError('revision must be a full lowercase commit SHA')
    if output.exists():
        raise ValueError('output already exists; refusing to mix revisions')
    source = Path(__file__).resolve().parent
    files = {name: unique_file(build, name) for name in DLLS}
    files['zink_wgl_probe.exe'] = probe
    for path in files.values():
        arm64_pe(path)
    icd = json.loads(unique_file(build, 'freedreno_icd.arm64.json').read_text())
    api_version = icd.get('ICD', {}).get('api_version', '')
    if not re.fullmatch(r'1\.4\.[0-9]+', api_version):
        raise ValueError('unexpected built Vulkan API version')
    output.mkdir(parents=True)
    for name, path in files.items():
        shutil.copyfile(path, output / name)
    for name in ('run-probe.ps1', 'README.md'):
        shutil.copyfile(source / name, output / name)
    manifest = {'file_format_version': '1.0.1', 'ICD': {
        'library_path': '.\\vulkan_freedreno.dll', 'library_arch': '64', 'api_version': api_version}}
    (output / 'freedreno_icd.arm64.json').write_text(json.dumps(manifest, indent=2) + '\n')
    receipt = {'schema': 1, 'mesa_commit': revision, 'architecture': 'arm64',
               'route': 'OpenGL/WGL -> Zink -> Vulkan -> Turnip/WDDM',
               'native_freedreno_gallium': False, 'gpu_test_executed': False,
               'external_dependencies': ['compatible VIOGPU KMD', 'Windows ARM64 Vulkan loader (vulkan-1.dll)'],
               'files': {name: hashlib.sha256((output / name).read_bytes()).hexdigest() for name in PAYLOADS}}
    (output / 'bundle.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return verify(output, revision)


# Return a nonzero status on any staging or validation failure.
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--revision', required=True)
    args = parser.parse_args()
    stage(args.build, args.probe, args.output, args.revision)
    print('PASS: app-local ARM64 WGL/Zink/Turnip candidate; GPU execution not performed')


if __name__ == '__main__':
    main()
