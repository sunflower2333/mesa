#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise production runtime-context creation with failing callbacks."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
text = (here / 'Device.cpp').read_text()
start = text.index('static HRESULT\nCreateRuntimeContext(')
end = text.index('\n}\n', start) + 3
function = text[start:end]
fixture = r'''
#include <cassert>
#include <cstdio>
using HRESULT = int;
constexpr HRESULT S_OK=0, E_FAIL=-1, E_NOTIMPL=-2, E_OUTOFMEMORY=-3;
#define FAILED(x) ((x)<0)
struct D3DDDICB_CREATECONTEXT { unsigned EngineAffinity=0, hContext=0, CommandBufferSize=0; };
struct Callbacks { HRESULT (*pfnCreateContextCb)(unsigned,D3DDDICB_CREATECONTEXT*); void (*pfnDestroyContextCb)(); };
struct Device { D3DDDICB_CREATECONTEXT shared_copy_context; Callbacks KTCallbacks; unsigned hDevice=17; };
HRESULT verdict=S_OK;
unsigned returnedHandle=42, calls=0;
HRESULT create(unsigned device,D3DDDICB_CREATECONTEXT* value) {
 assert(device==17 && value->EngineAffinity==1 && value->hContext==0);
 ++calls;
 if (verdict==S_OK) { value->hContext=returnedHandle; value->CommandBufferSize=8192; }
 return verdict;
}
void destroy() {}
// FUNCTION
int main() {
 Device device{{},{create,destroy}};
 assert(CreateRuntimeContext(&device)==S_OK);
 assert(device.shared_copy_context.hContext==42 && device.shared_copy_context.CommandBufferSize==8192);
 assert(CreateRuntimeContext(&device)==S_OK && calls==1);
 device.shared_copy_context={}; verdict=E_OUTOFMEMORY;
 assert(CreateRuntimeContext(&device)==E_OUTOFMEMORY && !device.shared_copy_context.hContext);
 verdict=S_OK; returnedHandle=0;
 assert(CreateRuntimeContext(&device)==E_FAIL && !device.shared_copy_context.hContext);
 returnedHandle=42; device.KTCallbacks.pfnDestroyContextCb=nullptr;
 assert(CreateRuntimeContext(&device)==E_NOTIMPL && calls==3);
 device.KTCallbacks={nullptr,destroy};
 assert(CreateRuntimeContext(&device)==E_NOTIMPL && calls==3);
 std::puts("runtime context: creation, reuse, callback failure, null handle, missing callbacks PASS");
}
'''.replace('// FUNCTION', function)
with tempfile.TemporaryDirectory(prefix='runtime-context-') as temporary:
    out = Path(temporary)
    (out / 'test.cpp').write_text(fixture)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(out / 'test.cpp'), '-o', str(out / 'test')], check=True)
    subprocess.run([str(out / 'test')], check=True)
