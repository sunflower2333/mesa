#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual UMD query/flush functions against reset-injecting callbacks."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time


def definition(text, name):
    match = re.search(r'(?:static inline bool|void APIENTRY)\s+' + name + r'\([^;]*?\)\s*(?://[^\n]*)?\s*\{', text)
    if not match:
        raise ValueError(name)
    start = text.index('{', match.start())
    depth = 0
    for index in range(start, len(text)):
        depth += (text[index] == '{') - (text[index] == '}')
        if depth == 0:
            return text[match.start():index + 1]
    raise ValueError('Unterminated ' + name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--revision', help='Compare previous entry points with the same failure scenarios')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    def read(name):
        if args.revision:
            return subprocess.check_output(['git', 'show', args.revision + ':src/gallium/frontends/d3d10umd/' + name], cwd=here, text=True)
        return (here / name).read_text()
    # Old entry points do not call the helper; use the current definition so
    # both revisions can execute against the identical fixture declarations.
    code = definition((here / 'State.h').read_text(), 'CheckDeviceRemoved')
    code += '\n' + definition(read('Device.cpp'), 'Flush')
    code += '\n' + definition(read('Query.cpp'), 'QueryGetData')
    fixture = (here / 'device_loss_test.cpp').read_text().replace('// PRODUCTION_FUNCTIONS', code)
    with tempfile.TemporaryDirectory(prefix='d3d10-device-loss-') as temporary:
        out = Path(temporary)
        source = out / 'test.cpp'
        source.write_text(fixture)
        executable = out / ('test.exe' if os.name == 'nt' else 'test')
        if os.name == 'nt':
            compiler = shutil.which('clang-cl') or shutil.which('cl')
            command = [compiler, '/nologo', '/EHsc', '/std:c++17', '/W4', '/WX', '/wd4100', str(source), '/Fe' + str(executable)]
        else:
            command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', str(source), '-o', str(executable)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, cwd=out, check=True)
        result = subprocess.run([str(executable)], cwd=out).returncode
        for attempt in range(51):
            try:
                executable.unlink()
                break
            except PermissionError:
                if attempt == 50:
                    raise
                time.sleep(0.1)
        return result


if __name__ == '__main__':
    raise SystemExit(main())
