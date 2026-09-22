#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile production image provenance/submit helpers with controlled peers."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def definition(source, name):
    match = re.search(r'(?:static\s+)?(?:inline\s+)?(?:bool|void|VkResult)\s+' +
                      name + r'\([^;]*?\)\s*\{', source)
    assert match, name
    depth = 0
    for index in range(source.index('{', match.start()), len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if not depth:
            return source[match.start():index + 1]
    raise AssertionError(name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parent
    fixture = (here / 'tu_wddm_image_access_test.cpp').read_text()
    access = (root / 'tu_wddm_access.h').read_text().replace('#include "util/u_dynarray.h"', '')
    functions = []
    sources = {
        'tu_descriptor_set.cc': ('tu_descriptor_image_write', 'tu_descriptor_image_copy'),
        'tu_shader.cc': ('tu_shader_record_image_use',),
        'tu_cmd_buffer.cc': ('tu_cmd_use_bo', 'tu_cmd_use_image', 'tu_cmd_use_attachment',
                             'tu_cmd_use_renderpass', 'tu_cmd_record_descriptor_use',
                             'tu_cmd_use_descriptors', 'tu_cmd_submit_image_uses'),
        'tu_knl_wddm.cc': ('tu_wddm_import_references_valid',
                           'tu_wddm_submit_add_reference', 'tu_wddm_submit_add_live_bos'),
    }
    for filename, names in sources.items():
        source = (root / filename).read_text()
        functions.extend(definition(source, name) for name in names)
    fixture = fixture.replace('// ACCESS_HEADER', access).replace(
        '// PRODUCTION_FUNCTIONS', '\n\n'.join(functions))
    with tempfile.TemporaryDirectory(prefix='turnip-image-access-') as temporary:
        output = Path(temporary)
        unit = output / 'fixture.cpp'
        unit.write_text(fixture)
        compiler = shutil.which('clang-cl') if os.name == 'nt' else None
        executable = output / ('fixture.exe' if compiler else 'fixture')
        if compiler:
            command = [compiler, '/nologo', '/EHsc', '/std:c++20', '/W4',
                       '/I' + str(root), '/I' + str(here.parents[3] / 'include'),
                       str(unit), '/Fe' + str(executable)]
        else:
            command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                       '-pthread', '-Wno-missing-field-initializers', '-I', str(root),
                       '-I', str(here.parents[3] / 'include'), str(unit), '-o', str(executable)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, cwd=output)
        subprocess.run([str(executable)], check=True, cwd=output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
