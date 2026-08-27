/*
 * Copyright 2025 Mesa3D authors
 * SPDX-License-Identifier: MIT
 */

#include "WindowsVirtGpu.h"

#include <cerrno>

#include "util/log.h"

WindowsVirtGpuDevice::WindowsVirtGpuDevice(enum VirtGpuCapset capset, int32_t descriptor)
    : VirtGpuDevice(capset), mDeviceHandle(INVALID_DESCRIPTOR), mCaps{} {
    (void)descriptor;
}

WindowsVirtGpuDevice::~WindowsVirtGpuDevice() {}

struct VirtGpuCaps WindowsVirtGpuDevice::getCaps(void) { return mCaps; }

int64_t WindowsVirtGpuDevice::getDeviceHandle(void) { return mDeviceHandle; }

VirtGpuResourcePtr WindowsVirtGpuDevice::createResource(uint32_t width, uint32_t height,
                                                        uint32_t stride, uint32_t size,
                                                        uint32_t virglFormat, uint32_t target,
                                                        uint32_t bind) {
    (void)width;
    (void)height;
    (void)stride;
    (void)size;
    (void)virglFormat;
    (void)target;
    (void)bind;
    mesa_loge("WindowsVirtGpuDevice: resource creation requires a VirtIO transport");
    return nullptr;
}

VirtGpuResourcePtr WindowsVirtGpuDevice::createBlob(const struct VirtGpuCreateBlob& blobCreate) {
    (void)blobCreate;
    mesa_loge("WindowsVirtGpuDevice: blob creation requires a VirtIO transport");
    return nullptr;
}

VirtGpuResourcePtr WindowsVirtGpuDevice::importBlob(const struct VirtGpuExternalHandle& handle) {
    (void)handle;
    mesa_loge("WindowsVirtGpuDevice: blob import requires a VirtIO transport");
    return nullptr;
}

int WindowsVirtGpuDevice::execBuffer(struct VirtGpuExecBuffer& execbuffer,
                                     const VirtGpuResource* blob) {
    (void)execbuffer;
    (void)blob;
    mesa_loge("WindowsVirtGpuDevice: command submission requires a VirtIO transport");
    return -ENOTSUP;
}

VirtGpuDevice* osCreateVirtGpuDevice(enum VirtGpuCapset capset, int32_t descriptor) {
    (void)capset;
    (void)descriptor;
    mesa_loge("WindowsVirtGpuDevice: no native VirtIO backend is available");
    return nullptr;
}
