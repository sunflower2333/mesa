/*
 * Copyright 2025 Mesa3D authors
 * SPDX-License-Identifier: MIT
 */

#include "WindowsVirtGpu.h"

#include <cerrno>

#include "util/log.h"

WindowsVirtGpuResource::WindowsVirtGpuResource(int64_t deviceHandle, uint32_t blobHandle,
                                               uint32_t resourceHandle, uint64_t size)
    : mDeviceHandle(deviceHandle),
      mBlobHandle(blobHandle),
      mResourceHandle(resourceHandle),
      mSize(size) {}

WindowsVirtGpuResource::~WindowsVirtGpuResource() {}

void WindowsVirtGpuResource::intoRaw() {
    mBlobHandle = INVALID_DESCRIPTOR;
    mResourceHandle = INVALID_DESCRIPTOR;
}

uint32_t WindowsVirtGpuResource::getBlobHandle() const { return mBlobHandle; }

uint32_t WindowsVirtGpuResource::getResourceHandle() const { return mResourceHandle; }

uint64_t WindowsVirtGpuResource::getSize() const { return mSize; }

VirtGpuResourceMappingPtr WindowsVirtGpuResource::createMapping() {
    mesa_loge("WindowsVirtGpuResource: mapping requires a VirtIO transport");
    return nullptr;
}

int WindowsVirtGpuResource::exportBlob(struct VirtGpuExternalHandle& handle) {
    handle.osHandle = INVALID_DESCRIPTOR;
    handle.type = kMemHandleOpaqueWin32;
    mesa_loge("WindowsVirtGpuResource: export requires a VirtIO transport");
    return -ENOTSUP;
}

int WindowsVirtGpuResource::wait() {
    mesa_loge("WindowsVirtGpuResource: wait requires a VirtIO transport");
    return -ENOTSUP;
}

int WindowsVirtGpuResource::transferToHost(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    mesa_loge("WindowsVirtGpuResource: transfer-to-host requires a VirtIO transport");
    return -ENOTSUP;
}

int WindowsVirtGpuResource::transferFromHost(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    mesa_loge("WindowsVirtGpuResource: transfer-from-host requires a VirtIO transport");
    return -ENOTSUP;
}
