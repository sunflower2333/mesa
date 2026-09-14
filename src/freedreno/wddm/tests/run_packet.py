#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the actual native packet encoder as C11; run no graphics APIs."""
import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
SHARED = HERE.parent


# Retry only transient cleanup locks; never discard a test or cleanup failure.
@contextmanager
def fixture_directory():
    root = Path(tempfile.mkdtemp(prefix='fd-native-packet-'))
    try:
        yield root
    finally:
        for attempt in range(8):
            try:
                shutil.rmtree(root)
                break
            except PermissionError:
                if attempt == 7:
                    raise
                time.sleep(0.05 * 2 ** attempt)


# Require the specified deliberate regression to fail at its intended assertion.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--negative-control', choices=('capacity', 'iova'))
    args = parser.parse_args()
    compiler = os.environ.get('CC') or ('cl' if os.name == 'nt' else 'cc')
    if not shutil.which(compiler):
        raise RuntimeError(f'Compiler not found: {compiler}')
    msvc = Path(compiler).stem.lower() == 'cl'
    source = (SHARED / 'freedreno_wddm_submit.c').read_text(encoding='utf-8')
    expected = None
    if args.negative_control:
        anchor, replacement, expected = {
            'capacity': ('capacity < size', 'capacity == UINT32_MAX', 'reject-short-capacity'),
            'iova': ('bo.flags = bos[i].flags | TU_WDDM_MSM_SUBMIT_BO_NO_IMPLICIT;',
                     'bo.flags = bos[i].flags | TU_WDDM_MSM_SUBMIT_BO_NO_IMPLICIT;\n      bo.presumed = 1;',
                     'golden-byte-exact'),
        }[args.negative_control]
        if source.count(anchor) != 1:
            raise RuntimeError('Negative-control anchor drifted')
        source = source.replace(anchor, replacement)
    with fixture_directory() as root:
        unit = root / 'packet.c'
        unit.write_text(source, encoding='utf-8')
        exe = root / ('packet.exe' if msvc else 'packet')
        command = ([compiler, '/nologo', '/std:c11', '/W4', '/WX', '/MT',
                    '/I' + str(SHARED), str(unit), str(HERE / 'packet_test.c'), '/Fe:' + str(exe)]
                   if msvc else
                   [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wpedantic',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                    '-I', str(SHARED), str(unit), str(HERE / 'packet_test.c'), '-o', str(exe)])
        subprocess.run(command, cwd=root, check=True, timeout=120)
        result = subprocess.run([str(exe)], cwd=root, capture_output=True, text=True, timeout=60)
        if expected:
            if result.returncode != 1 or result.stderr.strip() != f'FAIL: {expected}':
                raise RuntimeError(f'Wrong negative-control result: {result}')
            print(f'PASS negative control: {args.negative_control} -> {expected}')
        else:
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            print(result.stdout, end='')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
