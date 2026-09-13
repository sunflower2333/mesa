/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Residency.h --
 *    WDDM 2.0 residency for the kernel allocations this D3D10/11 UMD creates
 *    or opens through the runtime callbacks.
 */

#ifndef RESIDENCY_H
#define RESIDENCY_H

#include "DriverIncludes.h"

struct Device;
struct Resource;

/* D3DKMT_DRIVERVERSION of the DroidVM adapter(s) visible to this process, or
 * zero when none reports at least WDDM 2.0.  Only dxgkrnl-answered queries are
 * issued unless an adapter reports WDDM 2.0. */
UINT ResidencyQueryAdapterDriverVersion(void);

bool ResidencyRequired(const Device *device);

/* Take one residency reference on allocation unless *resident already says it
 * is held.  WDDM 1.x: no callback, S_OK.  Over budget: idle private staging of
 * other shared resources is trimmed between bounded retries. */
HRESULT ResidencyMakeResident(Device *device, Resource *owner,
                              D3DKMT_HANDLE allocation, bool *resident);
/* Drop the reference named by *resident, exactly once. */
HRESULT ResidencyEvict(Device *device, D3DKMT_HANDLE allocation, bool *resident);
/* Wait for every pending MakeResident paging operation.  Must precede any
 * RenderCb, PresentCb or SetDisplayModeCb that references this device's
 * allocations. */
HRESULT ResidencyPrepareSubmission(Device *device);
void ResidencyDestroyDevice(Device *device);

/* CreateResource: make the new kernel allocation resident or release it. */
HRESULT ResidencyAdmitCreatedAllocation(Device *device, Resource *resource);
/* Release idle private staging; returns the bytes released. */
UINT64 ResidencyTrimStaging(Device *device, Resource *keep, UINT64 bytes_to_trim);
/* DestroyResource: evict, then deallocate what this device owns. */
void ReleaseResourceAllocations(Device *device, Resource *resource);

#endif /* RESIDENCY_H */
