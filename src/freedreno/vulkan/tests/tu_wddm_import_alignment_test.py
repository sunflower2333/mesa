#!/usr/bin/env python3
"""Execute production BDA parsing and native import validation, with mutations."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def definition(source, name):
    match = re.search(r'static\s+VkResult\s+' + name + r'\([^;]*?\)\s*\{', source)
    if not match:
        raise ValueError('Missing production function: ' + name)
    depth = 0
    for index in range(source.index('{', match.start()), len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if not depth:
            return source[match.start():index + 1]
    raise ValueError('Unterminated production function: ' + name)


parser = argparse.ArgumentParser()
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--negative-control', choices=['legacy-rejection', 'drop-address-check'])
args = parser.parse_args()
here = Path(__file__).resolve().parent
source = (here.parent / 'tu_device.cc').read_text()
functions = '\n\n'.join(definition(source, name) for name in
    ['tu_memory_bda_alignment', 'tu_memory_import_runtime'])
if args.negative_control == 'legacy-rejection':
    original = '(alloc_flags & ~TU_BO_ALLOC_BDA_64K) != 0'
    assert functions.count(original) == 1
    functions = functions.replace(original, 'alloc_flags != TU_BO_ALLOC_NO_FLAGS')
elif args.negative_control == 'drop-address-check':
    original = 'if (misaligned || invisible)'
    assert functions.count(original) == 1
    functions = functions.replace(original, 'if ((false && misaligned) || invisible)')
fixture = (here / 'tu_wddm_import_alignment_test.cpp').read_text().replace('// PRODUCTION_FUNCTIONS', functions)
with tempfile.TemporaryDirectory(prefix='turnip-import-alignment-') as temporary:
    output = Path(temporary)
    unit = output / 'fixture.cpp'
    unit.write_text(fixture)
    compiler = shutil.which('clang-cl') if os.name == 'nt' else None
    binary = output / ('fixture.exe' if compiler else 'fixture')
    includes = [str(here.parent), str(here.parents[3] / 'include')]
    if compiler:
        command = [compiler, '/nologo', '/EHsc', '/W4', '/WX', '/std:c++20',
                   '/clang:-Wno-missing-field-initializers']
        command += ['/I' + item for item in includes] + [str(unit), '/Fe' + str(binary)]
    else:
        command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers']
        for item in includes:
            command += ['-I', item]
        command += [str(unit), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command, check=True, cwd=output)
    result = subprocess.run([str(binary)], cwd=output, capture_output=True, text=True, timeout=15)
    print(result.stdout, end='')
    if args.negative_control:
        expected = {'legacy-rejection': 'FAIL aligned native runtime import accepted',
                    'drop-address-check': 'FAIL misaligned imported address rejected and released'}[args.negative_control]
        if result.returncode != 1 or expected not in result.stderr:
            raise SystemExit('Negative control did not fail semantically: ' + result.stderr)
        print('PASS rejected ' + args.negative_control + ': ' + expected)
    else:
        print(result.stderr, end='')
        raise SystemExit(result.returncode)
