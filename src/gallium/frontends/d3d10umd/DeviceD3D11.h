/**************************************************************************
 *
 * Copyright 2025 the Mesa contributors
 * SPDX-License-Identifier: MIT
 *
 **************************************************************************/

#ifndef DEVICE_D3D11_H
#define DEVICE_D3D11_H

#include "DriverIncludes.h"
#include "State.h"

#if SUPPORT_D3D11
void FillDeviceFuncs11(D3D11DDI_DEVICEFUNCS *pDeviceFuncs);
#endif

#endif /* DEVICE_D3D11_H */
