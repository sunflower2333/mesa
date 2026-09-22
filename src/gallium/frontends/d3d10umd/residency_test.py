#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute the D3D10/11 UMD WDDM 2.0 residency code against fake runtime callbacks.

The fixture compiles Residency.cpp plus the kernel-allocation paths of
Resource.cpp (EnsureSharedCopy, SubmitSharedCopy) with fake D3DDDI device
callbacks and a fake gdi32 KMT enumeration.  The fake runtime rejects a
RenderCb that names a non-resident allocation or precedes a pending paging
fence, as dxgkrnl does on a WDDM 2.0 adapter.  The call sites that the fixture
cannot execute (CreateResource, OpenResource, DestroyResource, Present,
SetDisplayMode, RotateResourceIdentities, CreateDevice, DestroyDevice,
OpenAdapter) are checked for their exact residency ordering.

Each --negative-control removes one piece of the implementation and requires
the matching failure.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

RESIDENCY_FUNCTIONS = [
    'ResidencyAdapterClaimsDroidVmAbi', 'ResidencyEnumerateDriverVersion', 'ResidencyRequired',
    'ResidencyEnsurePagingQueue', 'ResidencyAttempt', 'ResidencyTrim', 'ResidencyMakeResident',
    'ResidencyEvict', 'ResidencyPrepareSubmission', 'ResidencyDestroyDevice',
    'ResidencyAdmitCreatedAllocation', 'ReleaseStagingAllocation', 'ResidencyTrimStaging',
    'ReleaseResourceAllocations',
]
RESOURCE_FUNCTIONS = ['SharedPrivateFormat', 'EnsureSharedCopy', 'SubmitSharedCopy']

NEGATIVE_CONTROLS = {
    'no-native-copy-guard': (
        'Resource.cpp',
        '   if (resource->native_host_backing)\n      return DXGI_DDI_ERR_UNSUPPORTED;\n',
        '', 'FAIL native HostSurface refuses CPU copy before runtime callbacks'),
    'no-make-resident': (
        'Residency.cpp', 'if (*resident || !ResidencyRequired(device))\n      return S_OK;',
        'if (resident != NULL)\n      return S_OK;',
        'FAIL WDDM 2.0 RenderCb referenced a non-resident allocation'),
    'no-paging-wait': (
        'Resource.cpp', 'HRESULT hr = ResidencyPrepareSubmission(device);\n   LogSharedCopyFailure("residency-wait", hr);',
        'HRESULT hr = S_OK;',
        'FAIL WDDM 2.0 RenderCb issued before its paging fence completed'),
    'no-trim': (
        'Residency.cpp', 'return ResidencyTrimStaging(request->device, request->keep, bytes_to_trim);',
        '(void)request; (void)bytes_to_trim; return 0;',
        'FAIL out-of-memory trims idle staging and retries'),
    'no-evict-on-destroy': (
        'Residency.cpp',
        '(void)ResidencyEvict(device, resource->hAllocation, &resource->allocation_resident);',
        '',
        'FAIL destroy evicts each reference exactly once'),
    'ungated': (
        'Residency.cpp',
        'return device != NULL && device->kmt_driver_version >= TU_WDDM_DRIVER_VERSION_WDDM_2_0;',
        'return device != NULL;',
        'FAIL WDDM 1.x adapter issues no residency callback'),
    'pending-as-failure': (
        'tu_wddm_residency.h',
        'case UINT32_C(0x8000000a): /* E_PENDING: FAILED() is true, yet it succeeded */\n      return TU_WDDM_RESIDENCY_PENDING;',
        '',
        'FAIL E_PENDING is a residency reference with a recorded paging fence'),
}


def extract(source, name):
    match = re.search(r'(?m)^' + re.escape(name) + r'\(', source)
    if not match:
        raise ValueError('Missing production function: ' + name)
    start = source.rfind('\n', 0, match.start() - 1) + 1
    brace = source.index('{', match.end())
    depth = 0
    for index in range(brace, len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if depth == 0:
            return source[start:index + 1]
    raise ValueError('Unterminated production function: ' + name)


def canonical(text):
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'\s+', '', text)


def require_order(label, text, fragments):
    offset = -1
    for fragment in fragments:
        found = text.find(fragment, offset + 1)
        if found < 0:
            raise SystemExit('FAIL %s: missing or out of order: %s' % (label, fragment))
        offset = found


def check_call_sites(here):
    resource = (here / 'Resource.cpp').read_text()
    dxgi = (here / 'DxgiFns.cpp').read_text()
    device = (here / 'Device.cpp').read_text()
    adapter = (here / 'Adapter.cpp').read_text()
    residency = (here / 'Residency.cpp').read_text()
    body = lambda source, name: canonical(extract(source, name))
    require_order('CreateResource admits the new allocation before it can be used',
                  body(resource, 'CreateResource'),
                  ['pDevice->KTCallbacks.pfnAllocateCb(pDevice->hDevice,&allocate)',
                   'HRESULTrhr=ResidencyAdmitCreatedAllocation(pDevice,pResource);',
                   'SetError(hDevice,rhr);', 'pDevice->shared_resources=pResource;'])
    require_order('OpenResource makes the opened allocation resident',
                  body(resource, 'OpenResource'),
                  ['pResource->hAllocation=openInfo->hAllocation;',
                   'ResidencyMakeResident(device,pResource,pResource->hAllocation,&pResource->allocation_resident)',
                   'device->shared_resources=pResource;'])
    destroy = body(resource, 'DestroyResource')
    require_order('DestroyResource evicts through ReleaseResourceAllocations', destroy,
                  ['PublishSharedResource(device,pResource)', 'ReleaseResourceAllocations(device,pResource);',
                   'pipe_resource_reference(&pResource->resource,NULL);'])
    if 'pfnDeallocateCb' in destroy:
        raise SystemExit('FAIL DestroyResource must not deallocate outside ReleaseResourceAllocations')
    require_order('TransferSharedResource marks staging idle only after success',
                  body(resource, 'TransferSharedResource'),
                  ['EnsureSharedCopy(device,resource)', 'SubmitSharedCopy(device,resource,false)',
                   'if(SUCCEEDED(hr))resource->staging_idle=true;'])
    require_order('Present waits for the paging fence before PresentCb', body(dxgi, '_Present'),
                  ['PreparePresentResource(device,pSrcResource,false,pPresentData->Flags.Flip!=0)', 'hr=ResidencyPrepareSubmission(device);',
                   'device->pDXGIBaseCallbacks->pfnPresentCb(device->hDevice,&present)'])
    require_order('SetDisplayMode waits for the paging fence', body(dxgi, '_SetDisplayMode'),
                  ['HRESULThr=ResidencyPrepareSubmission(device);',
                   'device->KTCallbacks.pfnSetDisplayModeCb(device->hDevice,&mode)'])
    require_order('RotateResourceIdentities moves residency with the allocation handle',
                  body(dxgi, '_RotateResourceIdentities'),
                  ['constboolfirstResident=first->allocation_resident;',
                   'current->allocation_resident=next->allocation_resident;',
                   'last->allocation_resident=firstResident;'])
    require_order('CreateDevice inherits the adapter driver model', body(device, 'CreateDevice'),
                  ['Adapter*pAdapter=CastAdapter(hAdapter);',
                   'pDevice->kmt_driver_version=pAdapter->kmt_driver_version;'])
    require_order('DestroyDevice releases the paging queue', body(device, 'DestroyDevice'),
                  ['pfnDestroyContextCb(pDevice->hDevice,&destroy)', 'ResidencyDestroyDevice(pDevice);',
                   'pipe->destroy(pipe);'])
    require_order('OpenAdapter queries the driver model', body(adapter, 'OpenAdapterCommon'),
                  ['pAdaptor->screen=d3d10_create_screen();',
                   'pAdaptor->kmt_driver_version=ResidencyQueryAdapterDriverVersion();'])
    if re.search(r'[^"]\bD3DKMT(EnumAdapters2|QueryAdapterInfo|CloseAdapter)\s*\(', residency):
        raise SystemExit('FAIL Residency.cpp must resolve gdi32 thunks, not this DLL\'s D3DKMT stubs')
    if "'Residency.cpp'" not in (here / 'meson.build').read_text():
        raise SystemExit('FAIL meson.build does not compile Residency.cpp')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-control', choices=sorted(NEGATIVE_CONTROLS))
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    vulkan = here.parents[2] / 'freedreno' / 'vulkan'
    check_call_sites(here)
    print('PASS D3D10 UMD residency call-site ordering')

    residency = (here / 'Residency.cpp').read_text()
    resource = (here / 'Resource.cpp').read_text()
    policy = (vulkan / 'tu_wddm_residency.h').read_text()
    expected = None
    if args.negative_control:
        path, original, replacement, expected = NEGATIVE_CONTROLS[args.negative_control]
        texts = {'Residency.cpp': residency, 'Resource.cpp': resource, 'tu_wddm_residency.h': policy}
        assert texts[path].count(original) == 1, args.negative_control
        texts[path] = texts[path].replace(original, replacement)
        residency, resource, policy = texts['Residency.cpp'], texts['Resource.cpp'], texts['tu_wddm_residency.h']

    structs = '\n\n'.join(re.search(r'struct ' + name + r'\n\{.*?\n\};', residency, re.S).group()
                          for name in ['ResidencyKmt', 'ResidencyRequest'])
    functions = '\n\n'.join([extract(residency, name) for name in RESIDENCY_FUNCTIONS] +
                            [extract(resource, 'EnsureSharedPresentContext')] +
                            [extract(resource, name) for name in RESOURCE_FUNCTIONS])
    fixture = (here / 'residency_test.cpp').read_text()
    for marker, text in (('// PRODUCTION_STRUCTS', structs), ('// PRODUCTION_FUNCTIONS', functions)):
        assert fixture.count(marker) == 1
        fixture = fixture.replace(marker, text)

    with tempfile.TemporaryDirectory(prefix='d3d10umd-residency-') as temporary:
        work = Path(temporary)
        (work / 'tu_wddm_residency.h').write_text(policy)
        (work / 'fixture.cpp').write_text(fixture)
        binary = work / ('fixture.exe' if os.name == 'nt' else 'fixture')
        if os.name == 'nt':
            if args.sanitize:
                parser.error('--sanitize requires the Linux compiler')
            compiler = shutil.which('clang-cl') or shutil.which('cl')
            command = [compiler, '/nologo', '/EHsc', '/std:c++17', '/W4', '/WX', '/wd4100',
                       '/I' + str(work), '/I' + str(vulkan), str(work / 'fixture.cpp'), '/Fe' + str(binary)]
        else:
            command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                       '-Wno-missing-field-initializers', '-I' + str(work), '-I' + str(vulkan),
                       str(work / 'fixture.cpp'), '-o', str(binary)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
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
