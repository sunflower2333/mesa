#!/usr/bin/env python3
"""Execute production queue metadata ownership and software drain functions."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from tu_wddm_empty_submit_test import definition


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-control', choices=['reread-token', 'double-release', 'skip-drain'])
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[3]
    driver = (here.parent / 'tu_knl_wddm.cc').read_text()
    common = (root / 'src/vulkan/runtime/vk_queue.c').read_text()
    functions = '\n'.join([
        definition(driver, 'tu_wddm_runtime_submit_data_create'),
        definition(driver, 'tu_wddm_runtime_submit_data_destroy'),
        definition(common, 'vk_queue_submit_finish_driver_data'),
        definition(common, 'vk_queue_drain'),
    ])
    if args.negative_control == 'reread-token':
        functions = functions.replace('*data = token;', '*data = found->queue;')
    elif args.negative_control == 'double-release':
        functions = functions.replace('submit->driver_data = NULL;', '')
    elif args.negative_control == 'skip-drain':
        functions = functions.replace('while (!list_is_empty(&queue->submit.submits))',
                                      'while (!list_is_empty(&queue->submit.submits) && false)')
    fixture = (here / 'tu_wddm_runtime_queue_test.cpp').read_text().replace('// PRODUCTION_FUNCTIONS', functions)
    with tempfile.TemporaryDirectory(prefix='turnip-runtime-queue-') as output:
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
        result = subprocess.run([str(executable)], cwd=directory)
        if args.negative_control:
            if not result.returncode:
                raise SystemExit('negative control escaped')
            print('PASS rejected ' + args.negative_control)
            return 0
        return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
