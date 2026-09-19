#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute production shared publication with injected fence/reset outcomes."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def extract(source, name):
    start = source.index('\n' + name + '(')
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return 'HRESULT ' + source[start:end]


parser = argparse.ArgumentParser()
parser.add_argument('--revision')
args = parser.parse_args()
here = Path(__file__).resolve().parent
source = (subprocess.check_output(
    ['git', 'show', args.revision + ':src/gallium/frontends/d3d10umd/Resource.cpp'],
    cwd=here, text=True) if args.revision else (here / 'Resource.cpp').read_text())
fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
using HRESULT = int;
constexpr HRESULT S_OK=0, E_FAIL=-1, D3DDDIERR_DEVICEREMOVED=-2;
#define SUCCEEDED(x) ((x)>=0)
constexpr uint64_t OS_TIMEOUT_INFINITE=UINT64_MAX;
enum pipe_reset_status { PIPE_NO_RESET, PIPE_UNKNOWN_CONTEXT_RESET };
struct pipe_context;
struct pipe_fence_handle {};
struct pipe_screen {
 bool (*fence_finish)(pipe_screen*,pipe_context*,pipe_fence_handle*,uint64_t);
 void (*fence_reference)(pipe_screen*,pipe_fence_handle**,pipe_fence_handle*);
};
struct pipe_context {
 pipe_screen* screen;
 void (*flush)(pipe_context*,pipe_fence_handle**,unsigned);
 pipe_reset_status (*get_device_reset_status)(pipe_context*);
};
struct Device { pipe_context* pipe; };
struct Resource { bool shared_dirty, zero_copy; };
bool ready, lost;
unsigned releases;
pipe_context* observed;
pipe_fence_handle handle;
void flush(pipe_context*,pipe_fence_handle** f,unsigned) { *f=&handle; }
bool finish(pipe_screen*,pipe_context* p,pipe_fence_handle*,uint64_t t) {
 assert(t==OS_TIMEOUT_INFINITE); observed=p; return ready;
}
void release(pipe_screen*,pipe_fence_handle** f,pipe_fence_handle*) {
 assert(*f==&handle); ++releases; *f=nullptr;
}
pipe_reset_status reset(pipe_context*) {
 return lost ? PIPE_UNKNOWN_CONTEXT_RESET : PIPE_NO_RESET;
}
HRESULT TransferSharedResource(Device*,Resource*,bool) { return S_OK; }
// FUNCTIONS
int main() {
 pipe_screen screen{finish,release}; pipe_context pipe{&screen,flush,reset};
 Device device{&pipe}; unsigned failures=0, ownershipFailures=0, contextFailures=0;
 for (unsigned scenario=0; scenario<4; ++scenario) {
  ready=scenario&1; lost=scenario&2; releases=0; observed=nullptr;
  Resource resource{true,true};
  HRESULT hr=PublishSharedResource(&device,&resource);
  HRESULT expected=lost ? D3DDDIERR_DEVICEREMOVED : ready ? S_OK : E_FAIL;
  if (hr!=expected || resource.shared_dirty!=(expected!=S_OK)) ++ownershipFailures;
  if (observed!=&pipe) ++contextFailures;
  if (releases!=1) ++failures;
 }
 failures += ownershipFailures + contextFailures;
 std::printf("shared completion: 4 scenarios, %u failures (ownership=%u, context=%u)\n",
             failures, ownershipFailures, contextFailures);
 return failures ? 1 : 0;
}
'''
fixture = fixture.replace('// FUNCTIONS', '\n'.join(
    extract(source, name) for name in ('FinishZeroCopyWrites', 'PublishSharedResource')))
with tempfile.TemporaryDirectory(prefix='shared-finish-') as temporary:
    out = Path(temporary)
    (out / 'test.cpp').write_text(fixture)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(out / 'test.cpp'), '-o', str(out / 'test')], check=True)
    raise SystemExit(subprocess.run([str(out / 'test')]).returncode)
