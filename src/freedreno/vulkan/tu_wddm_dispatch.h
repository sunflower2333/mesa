/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TU_WDDM_DISPATCH_H
#define TU_WDDM_DISPATCH_H

#if !defined(_WIN32)
#error "tu_wddm_dispatch.h is a Windows-only interface"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

/* Keep windows.h status constants disabled, then get NTSTATUS/NT_SUCCESS from
 * the supported user-mode declaration before including the D3DKMT thunks. */
#ifndef UMDF_USING_NTSTATUS
#define UMDF_USING_NTSTATUS
#endif
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Keep this table deliberately explicit.  The UMD must not silently fall back
 * to an import library or to a different thunk revision: a missing entry is a
 * failed WDDM transport probe.  The allocation/render entries are loaded now
 * so later P2 code cannot accidentally introduce a second dispatch path.
 */
struct tu_wddm_dispatch {
   HMODULE gdi32;

   PFND3DKMT_OPENADAPTERFROMLUID OpenAdapterFromLuid;
   PFND3DKMT_CLOSEADAPTER CloseAdapter;
   PFND3DKMT_QUERYADAPTERINFO QueryAdapterInfo;
   PFND3DKMT_CREATEDEVICE CreateDevice;
   PFND3DKMT_DESTROYDEVICE DestroyDevice;
   PFND3DKMT_CREATECONTEXT CreateContext;
   PFND3DKMT_DESTROYCONTEXT DestroyContext;
   PFND3DKMT_CREATEALLOCATION CreateAllocation;
   PFND3DKMT_DESTROYALLOCATION DestroyAllocation;
   PFND3DKMT_LOCK Lock;
   PFND3DKMT_UNLOCK Unlock;
   PFND3DKMT_ESCAPE Escape;
   PFND3DKMT_RENDER Render;
   PFND3DKMT_GETDEVICESTATE GetDeviceState;

   PFND3DKMT_CREATESYNCHRONIZATIONOBJECT CreateSynchronizationObject;
   PFND3DKMT_DESTROYSYNCHRONIZATIONOBJECT DestroySynchronizationObject;
   PFND3DKMT_WAITFORSYNCHRONIZATIONOBJECT WaitForSynchronizationObject;
   PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECT SignalSynchronizationObject;
   PFND3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU WaitForSynchronizationObjectFromCpu;
   PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU SignalSynchronizationObjectFromCpu;
};

/* Returns false if gdi32 or any required thunk is unavailable. */
bool tu_wddm_dispatch_init(struct tu_wddm_dispatch *dispatch);
void tu_wddm_dispatch_finish(struct tu_wddm_dispatch *dispatch);

#ifdef __cplusplus
}
#endif

#endif /* TU_WDDM_DISPATCH_H */
