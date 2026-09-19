#!/usr/bin/env python3
"""Execute production pageable registry/acquire with adversarial callbacks."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-control', choices=['drop-bo-pin', 'drop-token-pin', 'ignore-owner'])
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = (here.parent / 'tu_wddm_pageable.cc').read_text()
    # This translation unit is solely the three production entrypoints. Keep
    # their real calling convention and all control flow; replace includes only.
    functions = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
    if args.negative_control == 'drop-bo-pin':
        functions = functions.replace('bo = tu_bo_get_ref(record->bo);',
            'bo = false ? tu_bo_get_ref(record->bo) : record->bo;')
    elif args.negative_control == 'drop-token-pin':
        functions = functions.replace('retained = hr >= 0;',
            'retained = hr >= 0; if (retained) callbacks.release(owner, token);')
    elif args.negative_control == 'ignore-owner':
        functions = functions.replace('owner != device->wddm_runtime_owner ||', 'false ||')
    fixture = (here / 'tu_wddm_pageable_test.cpp').read_text().replace('// PRODUCTION_FUNCTIONS', functions)
    with tempfile.TemporaryDirectory(prefix='turnip-pageable-') as output:
        directory = Path(output)
        unit = directory / 'fixture.cpp'
        unit.write_text(fixture)
        compiler = shutil.which('clang-cl') if os.name == 'nt' else None
        executable = directory / ('fixture.exe' if compiler else 'fixture')
        if compiler:
            command = [compiler, '/nologo', '/EHsc', '/W4', '/WX', '/std:c++20',
                       '/I' + str(here.parent), str(unit), '/Fe' + str(executable)]
        else:
            command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                       '-I', str(here.parent), str(unit), '-o', str(executable)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, cwd=directory, check=True)
        result = subprocess.run([str(executable)], cwd=directory, timeout=15, capture_output=True, text=True)
        print(result.stdout, end='')
        if args.negative_control:
            expected = {
                'drop-bo-pin': 'FAIL pageable BO ownership expired inside runtime callback',
                'drop-token-pin': 'FAIL pageable output did not transfer retained allocation ownership',
                'ignore-owner': 'FAIL pageable foreign owner reached runtime callback',
            }[args.negative_control]
            if result.returncode != 1 or expected not in result.stderr:
                raise SystemExit('negative control did not fail semantically: ' + result.stderr)
            print('PASS rejected ' + args.negative_control + ': ' + expected)
            return 0
        print(result.stderr, end='')
        return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
