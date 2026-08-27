/*
 * Copyright 2025 Mesa3D authors
 * SPDX-License-Identifier: MIT
 */

#include "WindowsSync.h"

#include <windows.h>

#include <cerrno>
#include <io.h>

#include "util/log.h"

namespace {

/* SyncHelper keeps the historical POSIX-shaped fd interface on Windows.  The
 * platform implementation therefore owns CRT descriptors and only unwraps
 * them at the Win32 boundary. */
HANDLE getHandle(int syncFd) {
    if (syncFd < 0) {
        return INVALID_HANDLE_VALUE;
    }

    intptr_t rawHandle = _get_osfhandle(syncFd);
    if (rawHandle == -1) {
        return INVALID_HANDLE_VALUE;
    }
    return reinterpret_cast<HANDLE>(rawHandle);
}

int win32ErrorToErrno(DWORD error) {
    switch (error) {
        case ERROR_INVALID_HANDLE:
        case ERROR_INVALID_PARAMETER:
            return EINVAL;
        case ERROR_ACCESS_DENIED:
            return EACCES;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return ENOMEM;
        case ERROR_OPERATION_ABORTED:
            return ECANCELED;
        default:
            return EIO;
    }
}

int failWithLastError() {
    return -win32ErrorToErrno(GetLastError());
}

}  // namespace

namespace gfxstream {

WindowsSyncHelper::WindowsSyncHelper() {}

int WindowsSyncHelper::wait(int syncFd, int timeoutMilliseconds) {
    HANDLE handle = getHandle(syncFd);
    if (handle == INVALID_HANDLE_VALUE) {
        return -EINVAL;
    }

    DWORD timeout = timeoutMilliseconds < 0 ? INFINITE : static_cast<DWORD>(timeoutMilliseconds);
    DWORD result = WaitForSingleObject(handle, timeout);
    switch (result) {
        case WAIT_OBJECT_0:
            return 0;
        case WAIT_TIMEOUT:
            return -ETIMEDOUT;
        case WAIT_ABANDONED_0:
            /* Sync objects are expected to be events/fences, not mutexes. */
            return -EIO;
        case WAIT_FAILED:
            return failWithLastError();
        default:
            return -EIO;
    }
}

void WindowsSyncHelper::debugPrint(int syncFd) {
    HANDLE handle = getHandle(syncFd);
    if (handle == INVALID_HANDLE_VALUE) {
        mesa_loge("WindowsSyncHelper: invalid sync fd %d", syncFd);
        return;
    }

    DWORD flags = 0;
    if (!GetHandleInformation(handle, &flags)) {
        mesa_loge("WindowsSyncHelper: GetHandleInformation failed for fd %d (error %lu)", syncFd,
                  static_cast<unsigned long>(GetLastError()));
        return;
    }

    mesa_logi("WindowsSyncHelper: fd %d handle %p flags 0x%08lx", syncFd, handle,
              static_cast<unsigned long>(flags));
}

int WindowsSyncHelper::dup(int syncFd) {
    if (syncFd < 0) {
        errno = EINVAL;
        return -1;
    }

    int duplicate = _dup(syncFd);
    if (duplicate < 0) {
        /* _dup already reports a CRT errno; keep the interface POSIX-like. */
        return -errno;
    }
    return duplicate;
}

int WindowsSyncHelper::close(int syncFd) {
    if (syncFd < 0) {
        errno = EINVAL;
        return -1;
    }
    return _close(syncFd);
}

SyncHelper* osCreateSyncHelper() {
    return new WindowsSyncHelper();
}

}  // namespace gfxstream
