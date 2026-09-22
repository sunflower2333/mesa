#!/usr/bin/env python3
import ctypes
from pathlib import Path

root = Path(__file__).resolve().parents[3]
abi = (root / "freedreno/vulkan/tu_wddm_abi.h").read_text()
resource = (root / "gallium/frontends/d3d10umd/Resource.cpp").read_text()


class Header(ctypes.Structure):
    _pack_ = 4
    _fields_ = [("magic", ctypes.c_uint32), ("version", ctypes.c_uint32),
                ("size", ctypes.c_uint32), ("reserved", ctypes.c_uint32)]


class Surface(ctypes.Structure):
    _pack_ = 4
    _fields_ = [("header", Header), ("opcode", ctypes.c_uint32),
                ("flags", ctypes.c_uint32), ("expected_reset", ctypes.c_uint64),
                ("share_key", ctypes.c_uint64), ("size", ctypes.c_uint64),
                ("reset", ctypes.c_uint64), ("modifier", ctypes.c_uint64),
                ("plane_offset", ctypes.c_uint64), ("resource_id", ctypes.c_uint32),
                ("context_id", ctypes.c_uint32), ("width", ctypes.c_uint32),
                ("height", ctypes.c_uint32), ("fourcc", ctypes.c_uint32),
                ("stride", ctypes.c_uint32), ("plane_count", ctypes.c_uint32),
                ("layout_flags", ctypes.c_uint32), ("reserved", ctypes.c_uint64 * 3)]


assert ctypes.sizeof(Surface) == 128
for text in ("VIOGPU_WDDM_ESCAPE_ALLOCATE_NATIVE_SURFACE = 8",
             "VIOGPU_WDDM_ESCAPE_FREE_NATIVE_SURFACE = 9",
             "static_assert(sizeof(VIOGPU_WDDM_NATIVE_SURFACE) == 128"):
    assert text in abi

for text in ("VIOGPU_NATIVE_HOST_SURFACE", "CreateNativeHostSurfaceTexture(",
             "resource_from_handle(pipe->screen", "surface->ShareKey <= UINT32_MAX",
             "surface->Modifier == DROIDVM_DRM_FORMAT_MOD_LINEAR",
             "surface->PlaneOffset == 0", "surface->PlaneCount == 1",
             "surface->ResourceId != 0 && surface->ContextId == 0",
             "surface->LayoutFlags == DROIDVM_NATIVE_SURFACE_LINEAR_ALIAS",
             "surface->Size >= (UINT64)surface->Stride * surface->Height"):
    assert text in resource

create = resource[resource.index("void APIENTRY\nCreateResource"):resource.index("SIZE_T APIENTRY\nCalcPrivateOpenedResourceSize")]
assert create.index("if (nativeHostSurface)") < create.index("CreateSharedTextureCache")
assert "native host surface allocation/import failed" in create

destroy = resource[resource.index("void APIENTRY\nDestroyResource"):resource.index("void APIENTRY\nResourceMap")]
assert destroy.index("pipe_resource_reference(&pResource->resource, NULL);") < destroy.index(
    "ReleaseNativeHostSurface(pResource);")

print("PASS D3D10 native host surface import contract")
