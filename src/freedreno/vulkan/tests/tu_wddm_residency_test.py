#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute the production WDDM transport against a fake D3DKMT layer.

The fixture compiles the actual adapter/device/allocation/render code from
tu_knl_wddm.cc and the shared tu_wddm_residency.h policy, then drives WDDM 1.x
and WDDM 2.0 adapters through a fake KMT that enforces the WDDM 2.0 residency
contract the way dxgkrnl does: a Render naming a non-resident allocation, or an
allocation whose paging fence has not completed, is rejected.

Each --negative-control removes one piece of the residency implementation and
requires the fixture to fail with the matching diagnostic.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

TRANSPORT_FUNCTIONS = [
    'tu_wddm_init_header', 'tu_wddm_header_is_current', 'tu_wddm_validate_adapter_info',
    'tu_wddm_validate_context_info', 'tu_wddm_private_info_equal', 'tu_wddm_query_private_info',
    'tu_wddm_query_driver_version', 'tu_wddm_device_requires_residency', 'tu_wddm_adapter_open',
    'tu_wddm_adapter_close', 'tu_wddm_device_note_paging_fence', 'tu_wddm_device_wait_paging_fence',
    'tu_wddm_device_residency_init', 'tu_wddm_device_residency_finish', 'tu_wddm_device_open',
    'tu_wddm_device_close', 'tu_wddm_device_execution_active', 'tu_wddm_allocation_desc_valid',
    'tu_wddm_destroy_allocation_handle', 'tu_wddm_allocation_make_resident_attempt',
    'tu_wddm_allocation_make_resident', 'tu_wddm_allocation_evict',
    'tu_wddm_close_import_resource', 'tu_wddm_open_import_resource', 'tu_wddm_allocation_create',
    'tu_wddm_allocation_destroy', 'tu_wddm_fence_after', 'tu_wddm_render_reference_valid',
    'tu_wddm_render_references_unique', 'tu_wddm_native_submit_valid',
    'tu_wddm_render_replacements_valid', 'tu_wddm_import_references_valid',
    'tu_wddm_context_render_imports', 'tu_wddm_context_render',
]

# (production file, exact original text, replacement, required failure text)
NEGATIVE_CONTROLS = {
    'no-make-resident': (
        'tu_knl_wddm.cc', 'if (!tu_wddm_allocation_make_resident(allocation)) {',
        'if (false && !tu_wddm_allocation_make_resident(allocation)) {',
        'FAIL WDDM 2.0 Render referenced a non-resident allocation'),
    'no-paging-wait': (
        'tu_knl_wddm.cc',
        'if (!tu_wddm_device_wait_paging_fence(context->device))\n      return rejected("paging-fence");',
        '',
        'FAIL WDDM 2.0 Render issued before its paging fence completed'),
    'no-evict': (
        'tu_knl_wddm.cc', '(void)tu_wddm_allocation_evict(allocation);',
        '(void)&tu_wddm_allocation_evict;',
        'FAIL destroy evicts exactly once before DestroyAllocation2'),
    'ungated': (
        'tu_knl_wddm.cc',
        'return device != NULL &&\n          device->adapter.driver_version >= TU_WDDM_DRIVER_VERSION_WDDM_2_0;',
        'return device != NULL;',
        'FAIL WDDM 1.x adapter issues no residency or paging call'),
    'pending-as-resident': (
        'tu_wddm_residency.h',
        'return fence != 0 ? TU_WDDM_RESIDENCY_PENDING : TU_WDDM_RESIDENCY_RESIDENT;',
        'return TU_WDDM_RESIDENCY_RESIDENT;',
        'FAIL WDDM 2.0 Render issued before its paging fence completed'),
    'unbounded-retry': (
        'tu_wddm_residency.h',
        'if (cant_trim_further)\n         return TU_WDDM_RESIDENCY_OVER_BUDGET;',
        '',
        'FAIL over budget without trimmable residency makes exactly one CantTrimFurther retry'),
}


def extract(source, name):
    """Return the full column-zero definition of a production function."""
    match = re.search(r'(?m)^' + re.escape(name) + r'\(', source)
    if not match:
        raise ValueError('Missing production function: ' + name)
    start = source.rfind('\n', 0, match.start() - 1) + 1  # return-type line
    brace = source.index('{', match.end())
    depth = 0
    for index in range(brace, len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if depth == 0:
            return source[start:index + 1]
    raise ValueError('Unterminated production function: ' + name)


def region(source, begin, end):
    first = source.index(begin)
    last = source.index(end, first) + len(end)
    return source[first:last]


def build_fixture(here, work, control):
    vulkan = here.parent
    source = (vulkan / 'tu_knl_wddm.cc').read_text()
    policy = (vulkan / 'tu_wddm_residency.h').read_text()
    header = (vulkan / 'tu_knl_wddm.h').read_text()
    expected = None
    if control:
        path, original, replacement, expected = NEGATIVE_CONTROLS[control]
        if path == 'tu_knl_wddm.cc':
            assert source.count(original) == 1, control
            source = source.replace(original, replacement)
        else:
            assert policy.count(original) == 1, control
            policy = policy.replace(original, replacement)
    (work / 'tu_wddm_residency.h').write_text(policy)

    constants = region(source, 'static constexpr NTSTATUS TU_WDDM_STATUS_SUCCESS',
                       '"KMTQAITYPE_DRIVERVERSION payload width changed");')
    msm = region(source, 'enum : uint32_t {\n   TU_WDDM_MSM_CCMD_GEM_SUBMIT',
                 '"MSM submit command IOVA offset changed");')
    limits = region(header, 'enum {\n   TU_WDDM_MAX_RENDER_ALLOCATIONS', '};')
    structs = '\n\n'.join(
        re.search(r'struct ' + name + r' \{.*?\n\};', header, re.S).group()
        for name in ['tu_wddm_runtime', 'tu_wddm_adapter_info', 'tu_wddm_adapter', 'tu_wddm_device',
                     'tu_wddm_context', 'tu_wddm_allocation_desc', 'tu_wddm_allocation',
                     'tu_wddm_render_reference'])
    functions = '\n\n'.join(extract(source, name) for name in TRANSPORT_FUNCTIONS)
    fixture = (here / 'tu_wddm_residency_test.cpp').read_text()
    for marker, text in (('// PRODUCTION_CONSTANTS', constants + '\n' + msm),
                         ('// PRODUCTION_STRUCTS', limits + '\n\n' + structs),
                         ('// PRODUCTION_FUNCTIONS', functions)):
        assert fixture.count(marker) == 1, marker
        fixture = fixture.replace(marker, text)
    (work / 'fixture.cpp').write_text(fixture)
    return expected


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-control', choices=sorted(NEGATIVE_CONTROLS))
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix='turnip-wddm-residency-') as temporary:
        work = Path(temporary)
        expected = build_fixture(here, work, args.negative_control)
        compiler = shutil.which('clang-cl') if os.name == 'nt' else None
        binary = work / ('fixture.exe' if compiler else 'fixture')
        if compiler:
            if args.sanitize:
                parser.error('--sanitize requires the Linux compiler')
            command = [compiler, '/nologo', '/EHsc', '/W4', '/WX', '/std:c++20', '/I' + str(work),
                       '/I' + str(here.parent), str(work / 'fixture.cpp'), '/Fe' + str(binary)]
        else:
            command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                       '-Wno-missing-field-initializers', '-I' + str(work),
                       '-I' + str(here.parent), str(work / 'fixture.cpp'), '-o', str(binary)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-fno-omit-frame-pointer']
        subprocess.run(command, cwd=work, check=True)
        result = subprocess.run([str(binary)], cwd=work, capture_output=True, text=True)
        print(result.stdout, end='')
        print(result.stderr, end='')
        if expected is None:
            return result.returncode
        if result.returncode != 1 or expected not in result.stdout:
            raise SystemExit('Negative control %s was not detected (expected "%s")'
                             % (args.negative_control, expected))
        print('PASS negative control %s: %s' % (args.negative_control, expected[5:]))
        return 0


if __name__ == '__main__':
    raise SystemExit(main())
