#!/usr/bin/env python3
"""Execute actual BO/create/status/rollback code with controlled KMT peers."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def definition(source, name):
    match = re.search(r'(?:static\s+)?(?:inline\s+)?(?:bool|void|NTSTATUS|VkResult|enum tu_wddm_residency_outcome)\s+' +
                      name + r'\([^;]*?\)\s*\{', source)
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
parser.add_argument('--negative-control-oom', action='store_true')
parser.add_argument('--negative-control-rollback-status', action='store_true')
parser.add_argument('--negative-control-alignment', action='store_true')
parser.add_argument('--negative-control-shared-alignment', action='store_true')
parser.add_argument('--negative-control-shared-reply', action='store_true')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[3]
source = (here.parent / 'tu_knl_wddm.cc').read_text()
header = (here.parent / 'tu_knl_wddm.h').read_text()
names = ['tu_wddm_init_header', 'tu_wddm_header_is_current', 'tu_wddm_validate_context_info',
         'tu_wddm_allocation_desc_valid', 'tu_wddm_destroy_allocation_handle',
         'tu_wddm_device_requires_residency', 'tu_wddm_device_note_paging_fence',
         'tu_wddm_allocation_make_resident_attempt', 'tu_wddm_allocation_make_resident',
         'tu_wddm_shared_allocation', 'tu_wddm_allocation_import',
         'tu_wddm_allocation_create', 'tu_wddm_device_check_status',
         'tu_wddm_allocation_error', 'tu_wddm_remove_bo_locked', 'tu_wddm_bo_shared', 'tu_wddm_bo_init']
production = definition(header, 'tu_wddm_shared_context') + '\n\n'
production += '\n\n'.join(definition(source, name) for name in names)
production += '\n\n' + definition((here.parent / 'tu_device.cc').read_text(), 'tu_memory_bda_alignment')
if args.negative_control_oom:
    original = 'return tu_wddm_allocation_error(dev, create_status);'
    assert production.count(original) == 1
    production = production.replace(original,
        '(void)create_status; (void)&tu_wddm_allocation_error; return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);')
elif args.negative_control_rollback_status:
    original = 'allocation->last_create_status = static_cast<uint32_t>(status);'
    assert production.count(original) == 2
    index = production.rindex(original)
    production = production[:index] + '(void)status;' + production[index + len(original):]
elif args.negative_control_alignment:
    original = 'util_vma_heap_alloc(&dev->vma, vma_size, iova_alignment)'
    assert production.count(original) == 1
    production = production.replace(original, 'util_vma_heap_alloc(&dev->vma, vma_size, os_page_size)')
elif args.negative_control_shared_alignment:
    original = 'desc.alignment = (flags & TU_BO_ALLOC_BDA_64K) ? 65536 : 4096;'
    assert production.count(original) == 1
    production = production.replace(original, 'desc.alignment = 4096;')
elif args.negative_control_shared_reply:
    original = 'reply.address % desc->alignment == 0 &&'
    assert production.count(original) == 1
    production = production.replace(original, '')
structs = '\n\n'.join(re.search(r'struct ' + name + r'\s*\{.*?\n\};', header, re.S).group()
                       for name in ['tu_wddm_allocation_desc', 'tu_wddm_allocation'])
fixture = (here / 'tu_wddm_allocation_status_test.cpp').read_text().replace(
    '// PRODUCTION_STRUCTS', structs).replace('// PRODUCTION_FUNCTIONS', production)
with tempfile.TemporaryDirectory(prefix='turnip-allocation-status-') as temporary:
    output = Path(temporary)
    unit = output / 'fixture.cpp'
    unit.write_text(fixture)
    compiler = shutil.which('clang-cl') if os.name == 'nt' else None
    binary = output / ('fixture.exe' if compiler else 'fixture')
    if compiler:
        vma_object = output / 'vma.obj'
        subprocess.run([compiler, '/nologo', '/std:c11', '/c', '/TC',
                        '/I' + str(root / 'include'), '/I' + str(root / 'src'),
                        str(root / 'src/util/vma.c'), '/Fo' + str(vma_object)], check=True, cwd=output)
        # Production intentionally zero-fills omitted designated members.
        command = [compiler, '/nologo', '/EHsc', '/W4', '/WX', '/std:c++20',
                   '/clang:-Wno-missing-field-initializers',
                   '/clang:-Wno-missing-designated-field-initializers',
                   '/I' + str(root / 'include'), '/I' + str(root / 'src'), '/I' + str(here.parent),
                   str(unit), str(vma_object), '/Fe' + str(binary)]
    else:
        vma_object = output / 'vma.o'
        subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DHAVE_ENDIAN_H=1',
                        '-I' + str(root / 'include'), '-I' + str(root / 'src'),
                        '-c', str(root / 'src/util/vma.c'), '-o', str(vma_object)], check=True, cwd=output)
        command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                   '-I' + str(root / 'include'), '-I' + str(root / 'src'), '-I' + str(here.parent),
                   str(unit), str(vma_object), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command, check=True, cwd=output)
    result = subprocess.run([str(binary)], capture_output=True, text=True, cwd=output)
    print(result.stdout, end='')
    print(result.stderr, end='')
    if args.negative_control_oom:
        expected = 'FAIL lost or gated KMT allocation status is device lost not OOM'
    elif args.negative_control_rollback_status:
        expected = 'FAIL successful partial-create rollback preserves original status classification'
    elif args.negative_control_alignment:
        expected = 'FAIL requested buffer address alignment is honored by actual VMA'
    elif args.negative_control_shared_alignment:
        expected = 'FAIL shared runtime receives and honors requested BDA alignment'
    elif args.negative_control_shared_reply:
        expected = 'FAIL misaligned shared reply rejected with exact reference unwind'
    else:
        raise SystemExit(result.returncode)
    if result.returncode != 1 or expected not in result.stdout:
        raise SystemExit('Semantic negative did not detect the intended allocation regression')
    print('PASS semantic negative: ' + expected[5:])
