#!/usr/bin/env python3

from pathlib import Path
import re
import sys


TEST_DIR = Path(__file__).resolve().parent
VULKAN_DIR = TEST_DIR.parent


def fail(message: str) -> None:
    raise RuntimeError(message)


def canonical(text: str) -> str:
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"\s+", "", text)


def function_body(name: str, source: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        fail(f"function not found: {name}")

    start = source.find("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1 : index]
    fail(f"unterminated function: {name}")


def require_order(text: str, fragments: tuple[str, ...], message: str) -> None:
    offset = -1
    for fragment in fragments:
        next_offset = text.find(fragment, offset + 1)
        if next_offset < 0:
            fail(f"{message}: missing {fragment}")
        offset = next_offset


def main() -> int:
    device_source = (VULKAN_DIR / "tu_device.cc").read_text(encoding="utf-8")
    device_header = (VULKAN_DIR / "tu_device.h").read_text(encoding="utf-8")
    knl_header = (VULKAN_DIR / "tu_knl.h").read_text(encoding="utf-8")
    wddm_source = (VULKAN_DIR / "tu_knl_wddm.cc").read_text(encoding="utf-8")
    wddm_header = (VULKAN_DIR / "tu_knl_wddm.h").read_text(encoding="utf-8")
    dispatch_source = (VULKAN_DIR / "tu_wddm_dispatch.cc").read_text(encoding="utf-8")
    dispatch_header = (VULKAN_DIR / "tu_wddm_dispatch.h").read_text(encoding="utf-8")
    transport_fixture = (TEST_DIR / "tu_wddm_transport_compile.cpp").read_text(encoding="utf-8")

    retired_heap_api = "tu_wddm_select_heap_size"
    for path, source in (
        ("tu_device.cc", device_source),
        ("tu_knl_wddm.cc", wddm_source),
        ("tu_knl_wddm.h", wddm_header),
    ):
        if retired_heap_api in source:
            fail(f"fixed guest-pool heap selection remains in {path}")

    wddm = canonical(wddm_source)
    header = canonical(wddm_header)
    dispatch = canonical(dispatch_source)
    dispatch_api = canonical(dispatch_header)
    fixture = canonical(transport_fixture)

    for retired_dxgi_api in (
        "IDXGIFactory",
        "EnumAdapters1",
        "CreateDXGIFactory",
        'LoadLibraryExW(L"dxgi.dll"',
    ):
        if retired_dxgi_api in wddm_source or retired_dxgi_api in wddm_header:
            fail(f"WDDM adapter discovery still depends on DXGI: {retired_dxgi_api}")
    if "PFND3DKMT_ENUMADAPTERS2EnumAdapters2;" not in dispatch_api:
        fail("the WDDM dispatch table must expose D3DKMTEnumAdapters2")
    if "TU_WDDM_LOAD(EnumAdapters2);" not in dispatch:
        fail("D3DKMTEnumAdapters2 must be loaded from the system thunk table")
    foreach_adapter = canonical(function_body("tu_wddm_runtime_foreach_adapter", wddm_source))
    require_order(
        foreach_adapter,
        (
            "runtime->dispatch.EnumAdapters2(&enumeration)",
            "enumeration.pAdapters=adapters;",
            "runtime->dispatch.EnumAdapters2(&enumeration)",
            "tu_wddm_query_private_info(runtime,entry->hAdapter,&identity.private_info)",
            "callback(&identity,data)",
            "runtime->dispatch.CloseAdapter(&close)",
        ),
        "WDDM discovery must enumerate KMT handles, claim the private ABI, and close ownership",
    )

    if "TU_WDDM_MAX_RENDER_ALLOCATIONS=1024" not in header:
        fail("WDDM allocation-list capacity must match the KMD 1024-reference contract")
    if "TU_WDDM_MAX_BO_METADATA_SIZE=4096" not in header:
        fail("WDDM BO metadata must retain a bounded app-local storage contract")
    if "TU_WDDM_MAX_RENDER_ALLOCATIONS==1024" not in fixture:
        fail("the compile fixture must pin the shared 1024-reference contract")
    if "maximumWDDMsubmitnolongerfitstheDMAbuffer" not in wddm:
        fail("the maximum reference/command packet must have a compile-time 64 KiB fit proof")
    if "structtu_wddm_render_referencerender_refs[" in wddm:
        fail("the 1024-entry UMD reference table must not live on the stack")

    add_bo = canonical(function_body("tu_wddm_add_bo_locked", wddm_source))
    if "dev->wddm_bo_count>=TU_WDDM_MAX_RENDER_ALLOCATIONS" not in add_bo:
        fail("WDDM BO creation must preserve the all-live submit capacity invariant")
    bo_init = canonical(function_body("tu_wddm_bo_init", wddm_source))
    require_order(
        bo_init,
        (
            "capacity_available=dev->wddm_bo_count<TU_WDDM_MAX_RENDER_ALLOCATIONS;",
            "tu_wddm_add_bo_locked(dev,bo);",
            "capacity_available?VK_ERROR_OUT_OF_HOST_MEMORY:VK_ERROR_OUT_OF_DEVICE_MEMORY",
        ),
        "WDDM allocation must report capacity exhaustion before it can break submit residency",
    )

    bo_destroy = canonical(function_body("tu_wddm_allocation_destroy", wddm_source))
    if "free(allocation->metadata);" not in bo_destroy:
        fail("WDDM allocation teardown must release retained app-local metadata")
    set_metadata = canonical(function_body("tu_wddm_bo_set_metadata", wddm_source))
    require_order(
        set_metadata,
        (
            "metadata_size>TU_WDDM_MAX_BO_METADATA_SIZE",
            "void*copy=malloc(metadata_size);",
            "memcpy(copy,metadata,metadata_size);",
            "free(bo->wddm_allocation->metadata);",
            "bo->wddm_allocation->metadata_size=metadata_size;",
        ),
        "WDDM metadata writes must be bounded and replace owned storage transactionally",
    )
    get_metadata = canonical(function_body("tu_wddm_bo_get_metadata", wddm_source))
    require_order(
        get_metadata,
        (
            "allocation->metadata==NULL||allocation->metadata_size==0",
            "metadata_size<allocation->metadata_size",
            "memcpy(metadata,allocation->metadata,allocation->metadata_size);",
            "return0;",
        ),
        "WDDM metadata reads must fail closed for absent/short buffers and copy the retained value",
    )

    add_live = canonical(function_body("tu_wddm_submit_add_live_bos", wddm_source))
    queue_submit = canonical(function_body("tu_wddm_queue_submit_locked", wddm_source))
    if "for(uint32_ti=0;i<device->wddm_bo_count&&!submit->failed;i++)" not in add_live:
        fail("legacy MSM residency must retain every live WDDM BO")
    require_order(
        queue_submit,
        (
            "tu_wddm_submit_add_live_bos(device,submit)",
            "tu_wddm_submit_render(queue,submit,fence)",
        ),
        "WDDM submit must close residency before handing the packet to KMT",
    )

    if "device->heap.size=tu_get_system_heap_size(device);" not in wddm:
        fail("unprotected WDDM must budget pageable guest RAM instead of a fixed pool")

    device = canonical(device_source)
    if "memory_budget_props->heapUsage[0]=p_atomic_read(&physical_device->heap.used);" not in device:
        fail("WDDM heap usage must be read atomically")
    for field in ("uint64_theap_accounted_size;", "boolheap_accounted:1;"):
        if field not in canonical(knl_header):
            fail(f"BO heap ownership is missing: {field}")
    if "tu_bo_release_heap_accounting" not in device_header:
        fail("the WDDM final owner needs an explicit heap-accounting release hook")

    add_heap = canonical(function_body("tu_add_to_heap", device_source))
    require_order(
        add_heap,
        (
            "accounting_size=bo->wddm_allocation->vma_size;",
            "bo->heap_accounted_size=accounting_size;",
            "bo->heap_accounted=true;",
            "p_atomic_add_return(&mem_heap->used,accounting_size);",
        ),
        "WDDM heap accounting must charge the rounded backing extent once",
    )
    if "if(bo->wddm_allocation!=NULL){tu_bo_finish(dev,bo);}else" not in add_heap:
        fail("WDDM over-budget cleanup must retain accounting when KMT destruction fails")

    release_heap = canonical(function_body("tu_bo_release_heap_accounting", device_source))
    require_order(
        release_heap,
        (
            "if(device==NULL||bo==NULL||!bo->heap_accounted)return;",
            "bo->heap_accounted=false;",
            "bo->heap_accounted_size=0;",
            "p_atomic_add(&device->physical_device->heap.used,-accounting_size);",
        ),
        "heap-accounting release must be idempotent and clear ownership before subtraction",
    )

    destroy_memory = canonical(function_body("_tu_destroy_memory", device_source))
    require_order(
        destroy_memory,
        (
            "if(mem->bo->wddm_allocation==NULL)",
            "tu_bo_release_heap_accounting(device,mem->bo);",
            "tu_bo_finish(device,mem->bo);",
        ),
        "VkDeviceMemory teardown must leave WDDM accounting to the final KMT owner",
    )

    for function_name in ("tu_wddm_bo_finish", "tu_wddm_device_finish"):
        finish = canonical(function_body(function_name, wddm_source))
        require_order(
            finish,
            (
                "tu_wddm_allocation_destroy(allocation)",
                "tu_bo_release_heap_accounting(dev,bo);",
            ),
            f"{function_name} must release heap usage only after KMT destruction succeeds",
        )

    print("Turnip WDDM pageable-memory and residency policy passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
