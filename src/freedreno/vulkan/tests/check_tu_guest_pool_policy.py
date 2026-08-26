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
    command_source = (VULKAN_DIR / "tu_cmd_buffer.cc").read_text(encoding="utf-8")

    header = canonical(device_header)
    if (
        "alignas(64)volatileuint32_tvsc_draw_overflow;"
        "volatileuint32_tvsc_prim_overflow;"
        "uint32_t_vsc_overflow_pad[14];"
    ) not in header:
        fail("guest-pool VSC feedback fields must occupy an isolated cache line")

    lazy_vsc = canonical(function_body("tu6_lazy_init_vsc", command_source))
    require_order(
        lazy_vsc,
        (
            "dev->physical_device->guest_pool_size",
            "tu_bo_sync_cache(dev,dev->global_bo,gb_offset(vsc_draw_overflow),64,TU_MEM_SYNC_CACHE_FROM_GPU)",
            "uint32_tvsc_draw_overflow=global->vsc_draw_overflow",
            "uint32_tvsc_prim_overflow=global->vsc_prim_overflow",
        ),
        "guest-pool VSC feedback must be invalidated before the CPU reads it",
    )

    create_device = canonical(function_body("tu_CreateDevice", device_source))
    require_order(
        create_device,
        (
            "global=(structtu6_global*)device->global_bo->map",
            "memset(global,0,global_size)",
            "device->global_bo_map=global",
            "if(physical_device->guest_pool_size)tu_bo_sync_cache(device,device->global_bo,0,VK_WHOLE_SIZE,TU_MEM_SYNC_CACHE_TO_GPU)",
            "tu_breadcrumbs_init(device)",
        ),
        "guest-pool global BO initialization must be zeroed and published before use",
    )

    properties = canonical(
        function_body("tu_get_physical_device_properties_1_1", device_source)
    )
    require_order(
        properties,
        (
            "p->maxMemoryAllocationSize=0xFFFFFFFFull",
            "if(pdevice->guest_pool_size)",
            "p->maxMemoryAllocationSize=MIN2(p->maxMemoryAllocationSize,pdevice->heap.size)",
        ),
        "guest-pool max allocation size must be bounded by the offered heap",
    )

    allocation = canonical(function_body("tu_AllocateMemory", device_source))
    require_order(
        allocation,
        (
            "if(device->physical_device->guest_pool_size&&pAllocateInfo->allocationSize>mem_heap->size)",
            "result=VK_ERROR_OUT_OF_DEVICE_MEMORY",
            "gotofail",
        ),
        "oversized guest-pool allocations must fail before BO creation",
    )

    print("Turnip guest-pool global-state policy passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
