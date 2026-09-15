#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual WDDM lifetime/staging functions in a fault-injection fixture."""
import argparse
from contextlib import contextmanager
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[4]
VULKAN = HERE.parents[1]


# Extract a unique production definition, failing closed if its signature changes.
def extract(text, signature):
    if text.count(signature) != 1:
        raise ValueError(f'Expected one definition: {signature}')
    start = text.index(signature)
    brace = text.index('{', start)
    if ';' in text[start:brace]:
        raise ValueError(f'Not a definition: {signature}')
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


# Do not hide assertions or permanent cleanup failures behind Windows file locks.
@contextmanager
def fixture_directory():
    directory = tempfile.mkdtemp(prefix='tu-wddm-lifetime-')
    try:
        yield Path(directory)
    finally:
        for attempt in range(8):
            try:
                shutil.rmtree(directory)
                break
            except PermissionError:
                if attempt == 7:
                    raise
                time.sleep(0.05 * (2 ** attempt))


# Build positive tests and intentional regressions from the same production text.
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--negative-control', choices=('fence', 'busy', 'residency', 'scratch'))
    args = parser.parse_args()
    source = (VULKAN / 'tu_knl_wddm.cc').read_text(encoding='utf-8')
    header = (VULKAN / 'tu_knl_wddm.h').read_text(encoding='utf-8')
    records = '\n'.join(extract(header, f'struct {name} {{') + ';' for name in (
        'tu_wddm_allocation', 'tu_wddm_render_reference'))
    records += '\n' + source[source.index('enum : uint32_t {'):source.index('/* Native-context fences')]
    for name in ('tu_wddm_submit_entry', 'tu_wddm_submit_reference', 'tu_wddm_submit_scratch', 'tu_wddm_submit'):
        records += '\n' + extract(source, f'struct {name} {{') + ';'
    functions = (
        'static NTSTATUS\ntu_wddm_destroy_allocation_handle(',
        'static NTSTATUS\ntu_wddm_allocation_try_destroy(',
        'static inline bool\ntu_wddm_bo_valid(',
        'static inline bool\ntu_wddm_bo_valid_for_device(',
        'static void\ntu_wddm_remove_bo_locked(',
        'static bool\ntu_wddm_reap_retired_bos_locked(',
        'static void\ntu_wddm_bo_finish(',
        'static void\ntu_wddm_report_lifetime_stats(',
        'static void\ntu_wddm_device_finish(',
        'static bool\ntu_wddm_submit_add_live_bos(',
        'static VkResult\ntu_wddm_submit_render(',
    )
    production = '\n\n'.join(extract(source, signature) for signature in functions)
    mutation = {
        'fence': ('if (!tu_wddm_retirement_observe(&allocation->retirement, completed))', 'if (false)'),
        'busy': ('if (status == TU_WDDM_STATUS_DEVICE_BUSY ||\n          status == TU_WDDM_STATUS_GRAPHICS_ALLOCATION_BUSY)', 'if (false)'),
        'residency': ('!bo->wddm_allocation->retirement.pending &&', 'true &&'),
        'scratch': ('if (device->wddm_submit_scratch == NULL) {', 'if (true) {'),
    }
    if args.negative_control:
        old, new = mutation[args.negative_control]
        assert production.count(old) == 1
        production = production.replace(old, new)
    unit_text = (HERE / 'lifetime_test.cpp').read_text(encoding='utf-8')
    unit_text = unit_text.replace('// INSERT_RECORDS', records).replace('// INSERT_PRODUCTION', production)
    with fixture_directory() as temp:
        unit = temp / 'lifetime.cpp'
        unit.write_text(unit_text, encoding='utf-8')
        msvc = shutil.which('cl')
        exe = temp / ('lifetime.exe' if msvc else 'lifetime')
        command = (['cl', '/nologo', '/EHsc', '/std:c++20', '/W4', '/WX',
                    '/I' + str(VULKAN), str(unit), '/Fe:' + str(exe)] if msvc else
                   ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                    '-I', str(VULKAN), str(unit), '-o', str(exe)])
        subprocess.run(command, cwd=temp, check=True, timeout=120)
        result = subprocess.run([str(exe)], cwd=temp, text=True, capture_output=True, timeout=60)
        output = result.stdout + result.stderr
        if args.negative_control:
            expected = {
                'fence': 'pending_fence_retains_owner', 'busy': 'busy_retains_owner',
                'residency': 'retired_excluded_from_residency', 'scratch': 'scratch_reused',
            }[args.negative_control]
            if result.returncode == 0 or expected not in output:
                raise RuntimeError('Negative control did not fail as intended:\n' + output)
            print('PASS negative control:', args.negative_control, expected)
        else:
            if result.returncode:
                raise RuntimeError(output)
            print(output, end='')


if __name__ == '__main__':
    main()
