/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * Device.h --
 *    Functions that provide the 3D device functionality.
 */

#ifndef DEVICE_H
#define DEVICE_H

#include "DriverIncludes.h"

SIZE_T APIENTRY CalcPrivateDeviceSize(D3D10DDI_HADAPTER hAdapter,
                             __in const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pData);

HRESULT APIENTRY CreateDevice(D3D10DDI_HADAPTER hAdapter,
                     __in D3D10DDIARG_CREATEDEVICE *pCreateData);

/*
 * Shared with the D3D11 device function table, which publishes the same
 * implementations.
 */
void APIENTRY DestroyDevice(D3D10DDI_HDEVICE hDevice);

void APIENTRY Flush(D3D10DDI_HDEVICE hDevice);

void APIENTRY CheckFormatSupport(D3D10DDI_HDEVICE hDevice, DXGI_FORMAT Format,
                                 __out UINT *pFormatCaps);

void APIENTRY CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,
                                            DXGI_FORMAT Format, UINT SampleCount,
                                            __out UINT *pNumQualityLevels);

void APIENTRY SetTextFilterSize(D3D10DDI_HDEVICE hDevice, UINT Width, UINT Height);

#endif   /* DEVICE_H */
