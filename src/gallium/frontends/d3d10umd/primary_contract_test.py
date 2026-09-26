#!/usr/bin/env python3
"""Exercise the actual primary admission wrappers and portable orientation policy."""
from pathlib import Path
import os
import re
import resource
import subprocess
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
here = Path(__file__).resolve().parent
source = (here / 'Resource.cpp').read_text()
policy = (here / 'PrimaryContract.h').read_text()

def extract(name):
    match = re.search(r'static bool\s+' + name + r'\([^;]*?\)\s*\{', source)
    assert match, name
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

production = '\n'.join(extract(n) for n in ('PrimaryCanScanOut', 'OptionalPrimaryCanScanOut'))
fixture = (here / 'primary_contract_test.cpp').read_text().replace('// INSERT_PRODUCTION', production)
variants = [('production', policy)]
for name, old, new in (
    ('nonidentity-without-profile', 'p.mode_rotation == 1 &&', 'true &&'),
    ('unaware-rotated-resource', '(p.flags & 2U) != 0', 'true'),
    ('ignore-physical-storage', '!VioGpuScanoutStorageMatches(&profile->geometry, p.width, p.height)', 'false'),
    ('skip-smooth-update-readiness', '!VioGpuScanoutReady(profile->readiness)', 'false'),
):
    assert policy.count(old) == 1, name
    variants.append((name, policy.replace(old, new)))

with tempfile.TemporaryDirectory(prefix='.primary-contract-', dir=here) as temp:
    output = Path(temp)
    for name, header in variants:
        (output / 'PrimaryContract.h').write_text(header)
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(fixture)
        subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                        '-I'+str(here.parents[2] / 'freedreno/vulkan'), str(unit), '-o',str(binary)],
                       env={**os.environ, 'TMPDIR':temp}, check=True)
        result = subprocess.run([str(binary)],capture_output=True,text=True)
        if (result.returncode == 0) != (name == 'production'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print(result.stdout.strip() if name == 'production' else 'PASS semantic negative '+name+' rejected')
