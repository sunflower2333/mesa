#!/usr/bin/env python3
"""Inject failed SPIR-V conversion into the production compute cache miss path."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from tu_wddm_empty_submit_test import definition


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--revision', help='Older source for a negative control')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[3]
    relative = 'src/freedreno/vulkan/tu_pipeline.cc'
    source = (subprocess.check_output(['git', 'show', f'{args.revision}:{relative}'],
                                     cwd=root, text=True) if args.revision else
              (root / relative).read_text())
    production = definition(source, 'tu_compute_pipeline_create')
    begin = production.index('   if (!shader) {')
    end = production.index('\n   pipeline_feedback.duration', begin)
    fixture = (here / 'tu_compute_compile_failure_test.cpp').read_text().replace(
        '// PRODUCTION_CACHE_MISS', production[begin:end])
    with tempfile.TemporaryDirectory(prefix='turnip-compute-compile-failure-') as output:
        directory = Path(output)
        unit = directory / 'fixture.cpp'
        unit.write_text(fixture)
        compiler = shutil.which('clang-cl') if os.name == 'nt' else None
        executable = directory / ('fixture.exe' if compiler else 'fixture')
        if compiler:
            if args.sanitize:
                parser.error('--sanitize requires the Linux compiler')
            command = [compiler, '/nologo', '/EHsc', '/W4', '/WX', '/std:c++20',
                       str(unit), f'/Fe{executable}']
        else:
            command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                       str(unit), '-o', str(executable)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, cwd=directory, check=True)
        return subprocess.run([str(executable)], cwd=directory).returncode


if __name__ == '__main__':
    raise SystemExit(main())
