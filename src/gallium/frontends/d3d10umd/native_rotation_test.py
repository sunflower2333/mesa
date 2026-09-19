#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute the production native rotation with mock Gallium ownership/failures."""
from pathlib import Path
import subprocess
import tempfile

# Expected assertion failures are negative controls, not useful core dumps.
try:
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
except ImportError:
    pass

here = Path(__file__).resolve().parent
source = (here / 'NativeRotation.cpp').read_text()
source = '\n'.join(line for line in source.splitlines()
                   if line not in ('#include "State.h"', '#include "Resource.h"'))
fixture = (here / 'native_rotation_test.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='native-rotation-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    exe = Path(tmp) / 'test'
    cpp.write_text(fixture.replace('// PRODUCTION', source))
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    # Semantic negatives: the two independent pieces of the original defect.
    for label, statement in [('texture', 'dst->resource = src->resource;'),
                             ('key', 'dst->zero_copy_key = src->zero_copy_key;'),
                             ('surface', 'pipe_resource_reference(&view->surface->texture, next->resource);'),
                             ('binding', 'device->sampler_views[s][slot] = item->sampler;')]:
        assert source.count(statement) == 1
        broken = source.replace(statement, '(void)0;')
        cpp.write_text(fixture.replace('// PRODUCTION', broken))
        subprocess.run(['c++', '-std=c++17', str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True)
        assert result.returncode != 0, f'negative control survived: {label}'
        print(f'PASS semantic negative: {label}')

# Check the actual DDI dispatch and view lifetimes, not only helper behavior.
dxgi = (here / 'DxgiFns.cpp').read_text()
assert dxgi.index('return RotateNativeResourceIdentities(') < dxgi.index('const bool identityOnly')
for name, created, destroyed in [('OutputMerger.cpp', 2, 2), ('Shader.cpp', 2, 1)]:
    text = (here / name).read_text()
    assert text.count('RegisterResourceView(') == created
    assert text.count('UnregisterResourceView(') == destroyed
print('PASS DDI dispatch and view registration coverage')
