/**************************************************************************
 *
 * Copyright 2025 the Mesa contributors
 * SPDX-License-Identifier: MIT
 *
 **************************************************************************/

/*
 * DeviceD3D11.cpp --
 *    The D3D11 device function table.
 *
 * The D3D11 DDI repeats almost all of the D3D10 callback surface with the same
 * signatures, so most of this table is the D3D10 implementation the rest of
 * this frontend already provides.  What is genuinely different is small:
 *
 *   - seven entries whose argument structures changed, translated below;
 *   - the shader stages D3D11 adds (hull, domain, compute), the unordered
 *     access views, the indirect draws and the deferred contexts.
 *
 * This driver advertises pipeline level 10_1 as its ceiling (see GetCaps in
 * Adapter.cpp), and the runtime does not call that last group below 11_0.  They
 * are still defined, because the runtime rejects a table with null entries.
 * Raising the ceiling to 11_0 means implementing them for real.
 */

#include "Device.h"
#include "DeviceD3D11.h"
#include "Draw.h"
#include "InputAssembly.h"
#include "OutputMerger.h"
#include "Query.h"
#include "Rasterizer.h"
#include "Resource.h"
#include "Shader.h"
#include "State.h"

#if SUPPORT_D3D11

/*
 * ----------------------------------------------------------------------
 *
 * The seven translations.
 *
 * ----------------------------------------------------------------------
 */

/*
 * D3D11DDIARG_CREATERESOURCE is D3D10DDIARG_CREATERESOURCE with members
 * appended, in the same order, so this is a field copy.  ByteStride and the
 * later members describe structured buffers and typed texture layouts, which
 * belong to 11_0.
 */
static void
Translate11CreateResource(const D3D11DDIARG_CREATERESOURCE *pIn,
                          D3D10DDIARG_CREATERESOURCE *pOut)
{
   memset(pOut, 0, sizeof *pOut);
   pOut->pMipInfoList     = pIn->pMipInfoList;
   pOut->pInitialDataUP   = pIn->pInitialDataUP;
   pOut->ResourceDimension = pIn->ResourceDimension;
   pOut->Usage            = pIn->Usage;
   pOut->BindFlags        = pIn->BindFlags;
   pOut->MapFlags         = pIn->MapFlags;
   pOut->MiscFlags        = pIn->MiscFlags;
   pOut->Format           = pIn->Format;
   pOut->SampleDesc       = pIn->SampleDesc;
   pOut->MipLevels        = pIn->MipLevels;
   pOut->ArraySize        = pIn->ArraySize;
   pOut->pPrimaryDesc     = pIn->pPrimaryDesc;
}

static SIZE_T APIENTRY
CalcPrivateResourceSize11(D3D10DDI_HDEVICE hDevice,
                          __in const D3D11DDIARG_CREATERESOURCE *pCreateResource)
{
   D3D10DDIARG_CREATERESOURCE create;
   Translate11CreateResource(pCreateResource, &create);
   return CalcPrivateResourceSize(hDevice, &create);
}

static void APIENTRY
CreateResource11(D3D10DDI_HDEVICE hDevice,
                 __in const D3D11DDIARG_CREATERESOURCE *pCreateResource,
                 D3D10DDI_HRESOURCE hResource,
                 D3D10DDI_HRTRESOURCE hRTResource)
{
   D3D10DDIARG_CREATERESOURCE create;
   Translate11CreateResource(pCreateResource, &create);
   CreateResource(hDevice, &create, hResource, hRTResource);
}

/*
 * The D3D11 shader resource view is the D3D10.1 one with a BufferEx arm added
 * to the union; every other arm has the same type, so they copy across.
 * BufferEx describes raw and structured buffer views, which are 11_0.
 */
static bool
Translate11CreateShaderResourceView(const D3D11DDIARG_CREATESHADERRESOURCEVIEW *pIn,
                                    D3D10_1DDIARG_CREATESHADERRESOURCEVIEW *pOut)
{
   memset(pOut, 0, sizeof *pOut);
   pOut->hDrvResource      = pIn->hDrvResource;
   pOut->Format            = pIn->Format;
   pOut->ResourceDimension = pIn->ResourceDimension;

   switch (pIn->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER:
      pOut->Buffer = pIn->Buffer;
      return true;
   case D3D10DDIRESOURCE_TEXTURE1D:
      pOut->Tex1D = pIn->Tex1D;
      return true;
   case D3D10DDIRESOURCE_TEXTURE2D:
      pOut->Tex2D = pIn->Tex2D;
      return true;
   case D3D10DDIRESOURCE_TEXTURE3D:
      pOut->Tex3D = pIn->Tex3D;
      return true;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      pOut->TexCube = pIn->TexCube;
      return true;
   default:
      LOG_UNSUPPORTED_ENTRYPOINT();
      return false;
   }
}

static SIZE_T APIENTRY
CalcPrivateShaderResourceViewSize11(D3D10DDI_HDEVICE hDevice,
                                    __in const D3D11DDIARG_CREATESHADERRESOURCEVIEW *pView)
{
   D3D10_1DDIARG_CREATESHADERRESOURCEVIEW view;
   if (!Translate11CreateShaderResourceView(pView, &view)) {
      return 0;
   }
   return CalcPrivateShaderResourceViewSize1(hDevice, &view);
}

static void APIENTRY
CreateShaderResourceView11(D3D10DDI_HDEVICE hDevice,
                           __in const D3D11DDIARG_CREATESHADERRESOURCEVIEW *pView,
                           D3D10DDI_HSHADERRESOURCEVIEW hView,
                           D3D10DDI_HRTSHADERRESOURCEVIEW hRTView)
{
   D3D10_1DDIARG_CREATESHADERRESOURCEVIEW view;
   if (!Translate11CreateShaderResourceView(pView, &view)) {
      return;
   }
   CreateShaderResourceView1(hDevice, &view, hView, hRTView);
}

/*
 * The D3D11 depth stencil view adds a Flags member before the union.  Both
 * flags it can carry - read-only depth and read-only stencil - are 11_0.
 */
static void
Translate11CreateDepthStencilView(const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *pIn,
                                  D3D10DDIARG_CREATEDEPTHSTENCILVIEW *pOut)
{
   memset(pOut, 0, sizeof *pOut);
   pOut->hDrvResource      = pIn->hDrvResource;
   pOut->Format            = pIn->Format;
   pOut->ResourceDimension = pIn->ResourceDimension;

   switch (pIn->ResourceDimension) {
   case D3D10DDIRESOURCE_TEXTURE1D:
      pOut->Tex1D = pIn->Tex1D;
      break;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      pOut->TexCube = pIn->TexCube;
      break;
   default:
      pOut->Tex2D = pIn->Tex2D;
      break;
   }
}

static SIZE_T APIENTRY
CalcPrivateDepthStencilViewSize11(D3D10DDI_HDEVICE hDevice,
                                  __in const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *pView)
{
   D3D10DDIARG_CREATEDEPTHSTENCILVIEW view;
   Translate11CreateDepthStencilView(pView, &view);
   return CalcPrivateDepthStencilViewSize(hDevice, &view);
}

static void APIENTRY
CreateDepthStencilView11(D3D10DDI_HDEVICE hDevice,
                         __in const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *pView,
                         D3D10DDI_HDEPTHSTENCILVIEW hView,
                         D3D10DDI_HRTDEPTHSTENCILVIEW hRTView)
{
   D3D10DDIARG_CREATEDEPTHSTENCILVIEW view;
   Translate11CreateDepthStencilView(pView, &view);
   CreateDepthStencilView(hDevice, &view, hView, hRTView);
}

/*
 * D3D11 render targets carry unordered access views alongside them.  At
 * pipeline level 10_1 the runtime never binds any, so the remaining arguments
 * are the D3D10 ones.
 */
static void APIENTRY
SetRenderTargets11(D3D10DDI_HDEVICE hDevice,
                   __in_ecount(NumRTVs) const D3D10DDI_HRENDERTARGETVIEW *phRenderTargetView,
                   UINT NumRTVs, UINT ClearSlots,
                   D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView,
                   __in_ecount(NumUAVs) const D3D11DDI_HUNORDEREDACCESSVIEW *phUnorderedAccessView,
                   __in_ecount(NumUAVs) const UINT *pUAVInitialCounts,
                   UINT UAVStartSlot, UINT NumUAVs,
                   UINT UAVRangeStart, UINT UAVRangeSize)
{
   if (NumUAVs != 0) {
      LOG_UNSUPPORTED_ENTRYPOINT();
   }
   (void)phUnorderedAccessView;
   (void)pUAVInitialCounts;
   (void)UAVStartSlot;
   (void)UAVRangeStart;
   (void)UAVRangeSize;
   SetRenderTargets(hDevice, phRenderTargetView, NumRTVs, ClearSlots,
                    hDepthStencilView);
}

static void APIENTRY
RelocateDeviceFuncs11(D3D10DDI_HDEVICE hDevice, D3D11DDI_DEVICEFUNCS *pDeviceFuncs)
{
   (void)hDevice;
   FillDeviceFuncs11(pDeviceFuncs);
}

/*
 * Stream output gained multiple streams in D3D11.  At pipeline level 10_1 only
 * stream zero exists, so the declaration is forwarded entry by entry.
 */
static void
Translate11StreamOutputDecl(const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pIn,
                            D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pOut,
                            D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *pEntries,
                            UINT MaxEntries)
{
   memset(pOut, 0, sizeof *pOut);
   pOut->pShaderCode = pIn->pShaderCode;
   pOut->pOutputStreamDecl = pEntries;

   /*
    * D3D10 has a single stream and one stride; D3D11 has several of each.
    * Below 11_0 only stream zero can be declared, so take its entries and its
    * stride and drop the rest.
    */
   UINT written = 0;
   for (UINT i = 0; i < pIn->NumEntries && written < MaxEntries; ++i) {
      if (pIn->pOutputStreamDecl[i].Stream != 0) {
         LOG_UNSUPPORTED_ENTRYPOINT();
         continue;
      }
      pEntries[written].OutputSlot    = pIn->pOutputStreamDecl[i].OutputSlot;
      pEntries[written].RegisterIndex = pIn->pOutputStreamDecl[i].RegisterIndex;
      pEntries[written].RegisterMask  = pIn->pOutputStreamDecl[i].RegisterMask;
      ++written;
   }
   pOut->NumEntries = written;
   pOut->StreamOutputStrideInBytes =
      pIn->NumStrides > 0 ? pIn->BufferStridesInBytes[0] : 0;
}

#define D3D11_SO_MAX_ENTRIES 128

static SIZE_T APIENTRY
CalcPrivateGeometryShaderWithStreamOutput11(
   D3D10DDI_HDEVICE hDevice,
   __in const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pData,
   __in const D3D10DDIARG_STAGE_IO_SIGNATURES *pSignatures)
{
   D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT create;
   D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY entries[D3D11_SO_MAX_ENTRIES];
   Translate11StreamOutputDecl(pData, &create, entries, D3D11_SO_MAX_ENTRIES);
   return CalcPrivateGeometryShaderWithStreamOutput(hDevice, &create, pSignatures);
}

static void APIENTRY
CreateGeometryShaderWithStreamOutput11(
   D3D10DDI_HDEVICE hDevice,
   __in const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pData,
   D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRTShader,
   __in const D3D10DDIARG_STAGE_IO_SIGNATURES *pSignatures)
{
   D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT create;
   D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY entries[D3D11_SO_MAX_ENTRIES];
   Translate11StreamOutputDecl(pData, &create, entries, D3D11_SO_MAX_ENTRIES);
   CreateGeometryShaderWithStreamOutput(hDevice, &create, hShader, hRTShader,
                                        pSignatures);
}

/*
 * ----------------------------------------------------------------------
 *
 * Entries the runtime never reaches below pipeline level 11_0.  They exist so
 * the table carries no null pointer.
 *
 * ----------------------------------------------------------------------
 */

static VOID APIENTRY
Stub_DrawIndexedInstancedIndirect(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE resource, UINT offset)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)resource;
   (void)offset;
}
static VOID APIENTRY
Stub_DrawInstancedIndirect(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE resource, UINT offset)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)resource;
   (void)offset;
}
static VOID APIENTRY
Stub_CommandListExecute(D3D10DDI_HDEVICE device, D3D11DDI_HCOMMANDLIST commandList)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)commandList;
}
static VOID APIENTRY
Stub_HsSetShaderResources(D3D10DDI_HDEVICE device, UINT startSlot, UINT numViews, const D3D10DDI_HSHADERRESOURCEVIEW *views)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)startSlot;
   (void)numViews;
   (void)views;
}
static VOID APIENTRY
Stub_HsSetShader(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shader;
}
static VOID APIENTRY
Stub_HsSetSamplers(D3D10DDI_HDEVICE device, UINT startSlot, UINT numSamplers, const D3D10DDI_HSAMPLER *samplers)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)startSlot;
   (void)numSamplers;
   (void)samplers;
}
static VOID APIENTRY
Stub_HsSetConstantBuffers(D3D10DDI_HDEVICE device, UINT startSlot, UINT numBuffers, const D3D10DDI_HRESOURCE *buffers)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)startSlot;
   (void)numBuffers;
   (void)buffers;
}
static VOID APIENTRY
Stub_DsSetShaderResources(D3D10DDI_HDEVICE device, UINT startSlot, UINT numViews, const D3D10DDI_HSHADERRESOURCEVIEW *views)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)startSlot;
   (void)numViews;
   (void)views;
}
static VOID APIENTRY
Stub_DsSetShader(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shader;
}
static VOID APIENTRY
Stub_DsSetSamplers(D3D10DDI_HDEVICE device, UINT startSlot, UINT numSamplers, const D3D10DDI_HSAMPLER *samplers)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)startSlot;
   (void)numSamplers;
   (void)samplers;
}
static VOID APIENTRY
Stub_DsSetConstantBuffers(D3D10DDI_HDEVICE device, UINT startSlot, UINT numBuffers, const D3D10DDI_HRESOURCE *buffers)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)startSlot;
   (void)numBuffers;
   (void)buffers;
}
static VOID APIENTRY
Stub_CreateHullShader(D3D10DDI_HDEVICE device, const UINT *shaderCode, D3D10DDI_HSHADER shader, D3D10DDI_HRTSHADER runtimeShader, const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *signatures)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shaderCode;
   (void)shader;
   (void)runtimeShader;
   (void)signatures;
}
static VOID APIENTRY
Stub_CreateDomainShader(D3D10DDI_HDEVICE device, const UINT *shaderCode, D3D10DDI_HSHADER shader, D3D10DDI_HRTSHADER runtimeShader, const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *signatures)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shaderCode;
   (void)shader;
   (void)runtimeShader;
   (void)signatures;
}
static VOID APIENTRY
Stub_CheckDeferredContextHandleSizes(D3D10DDI_HDEVICE device, UINT *handleSizeArray, D3D11DDI_HANDLESIZE *handleSizes)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)handleSizeArray;
   (void)handleSizes;
}
static SIZE_T APIENTRY
Stub_CalcDeferredContextHandleSize(D3D10DDI_HDEVICE device, D3D11DDI_HANDLETYPE handleType, VOID *handleData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)handleType;
   (void)handleData;
   return 0;
}
static SIZE_T APIENTRY
Stub_CalcPrivateDeferredContextSize(D3D10DDI_HDEVICE device, const D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE *arguments)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)arguments;
   return 0;
}
static VOID APIENTRY
Stub_CreateDeferredContext(D3D10DDI_HDEVICE device, const D3D11DDIARG_CREATEDEFERREDCONTEXT *arguments)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)arguments;
}
static VOID APIENTRY
Stub_AbandonCommandList(D3D10DDI_HDEVICE device)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
}
static SIZE_T APIENTRY
Stub_CalcPrivateCommandListSize(D3D10DDI_HDEVICE device, const D3D11DDIARG_CREATECOMMANDLIST *arguments)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)arguments;
   return 0;
}
static VOID APIENTRY
Stub_CreateCommandList(D3D10DDI_HDEVICE device, const D3D11DDIARG_CREATECOMMANDLIST *arguments, D3D11DDI_HCOMMANDLIST commandList, D3D11DDI_HRTCOMMANDLIST runtimeCommandList)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)arguments;
   (void)commandList;
   (void)runtimeCommandList;
}
static VOID APIENTRY
Stub_DestroyCommandList(D3D10DDI_HDEVICE device, D3D11DDI_HCOMMANDLIST commandList)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)commandList;
}
static SIZE_T APIENTRY
Stub_CalcPrivateTessellationShaderSize(D3D10DDI_HDEVICE device, const UINT *shaderCode, const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *signatures)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shaderCode;
   (void)signatures;
   return 0;
}
static VOID APIENTRY
Stub_PsSetShaderWithIfaces(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader, UINT classInstanceCount, const UINT *classInstances, const D3D11DDIARG_POINTERDATA *interfacePointerData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shader;
   (void)classInstanceCount;
   (void)classInstances;
   (void)interfacePointerData;
}
static VOID APIENTRY
Stub_VsSetShaderWithIfaces(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader, UINT classInstanceCount, const UINT *classInstances, const D3D11DDIARG_POINTERDATA *interfacePointerData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shader;
   (void)classInstanceCount;
   (void)classInstances;
   (void)interfacePointerData;
}
static VOID APIENTRY
Stub_GsSetShaderWithIfaces(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader, UINT classInstanceCount, const UINT *classInstances, const D3D11DDIARG_POINTERDATA *interfacePointerData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shader;
   (void)classInstanceCount;
   (void)classInstances;
   (void)interfacePointerData;
}
static VOID APIENTRY
Stub_HsSetShaderWithIfaces(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader, UINT classInstanceCount, const UINT *classInstances, const D3D11DDIARG_POINTERDATA *interfacePointerData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shader;
   (void)classInstanceCount;
   (void)classInstances;
   (void)interfacePointerData;
}
static VOID APIENTRY
Stub_DsSetShaderWithIfaces(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader, UINT classInstanceCount, const UINT *classInstances, const D3D11DDIARG_POINTERDATA *interfacePointerData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shader;
   (void)classInstanceCount;
   (void)classInstances;
   (void)interfacePointerData;
}
static VOID APIENTRY
Stub_CsSetShaderWithIfaces(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader, UINT classInstanceCount, const UINT *classInstances, const D3D11DDIARG_POINTERDATA *interfacePointerData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shader;
   (void)classInstanceCount;
   (void)classInstances;
   (void)interfacePointerData;
}
static VOID APIENTRY
Stub_CreateComputeShader(D3D10DDI_HDEVICE device, const UINT *shaderCode, D3D10DDI_HSHADER shader, D3D10DDI_HRTSHADER runtimeShader)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shaderCode;
   (void)shader;
   (void)runtimeShader;
}
static VOID APIENTRY
Stub_CsSetShader(D3D10DDI_HDEVICE device, D3D10DDI_HSHADER shader)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)shader;
}
static VOID APIENTRY
Stub_CsSetShaderResources(D3D10DDI_HDEVICE device, UINT startSlot, UINT numberOfViews, const D3D10DDI_HSHADERRESOURCEVIEW *views)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)startSlot;
   (void)numberOfViews;
   (void)views;
}
static VOID APIENTRY
Stub_CsSetSamplers(D3D10DDI_HDEVICE device, UINT startSlot, UINT numberOfSamplers, const D3D10DDI_HSAMPLER *samplers)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)startSlot;
   (void)numberOfSamplers;
   (void)samplers;
}
static VOID APIENTRY
Stub_CsSetConstantBuffers(D3D10DDI_HDEVICE device, UINT startSlot, UINT numberOfBuffers, const D3D10DDI_HRESOURCE *resources)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)startSlot;
   (void)numberOfBuffers;
   (void)resources;
}
static SIZE_T APIENTRY
Stub_CalcPrivateUnorderedAccessViewSize(D3D10DDI_HDEVICE device, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *arguments)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)arguments;
   return 0;
}
static VOID APIENTRY
Stub_CreateUnorderedAccessView(D3D10DDI_HDEVICE device, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *arguments, D3D11DDI_HUNORDEREDACCESSVIEW view, D3D11DDI_HRTUNORDEREDACCESSVIEW runtimeView)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)arguments;
   (void)view;
   (void)runtimeView;
}
static VOID APIENTRY
Stub_DestroyUnorderedAccessView(D3D10DDI_HDEVICE device, D3D11DDI_HUNORDEREDACCESSVIEW view)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)view;
}
static VOID APIENTRY
Stub_ClearUnorderedAccessViewUint(D3D10DDI_HDEVICE device, D3D11DDI_HUNORDEREDACCESSVIEW view, const UINT values[4])
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)view;
   (void)values[4];
}
static VOID APIENTRY
Stub_ClearUnorderedAccessViewFloat(D3D10DDI_HDEVICE device, D3D11DDI_HUNORDEREDACCESSVIEW view, const FLOAT values[4])
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)view;
   (void)values[4];
}
static VOID APIENTRY
Stub_CsSetUnorderedAccessViews(D3D10DDI_HDEVICE device, UINT startSlot, UINT numViews, const D3D11DDI_HUNORDEREDACCESSVIEW *views, const UINT *initialCounts)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)startSlot;
   (void)numViews;
   (void)views;
   (void)initialCounts;
}
static VOID APIENTRY
Stub_Dispatch(D3D10DDI_HDEVICE device, UINT x, UINT y, UINT z)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)x;
   (void)y;
   (void)z;
}
static VOID APIENTRY
Stub_DispatchIndirect(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE resource, UINT offset)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)resource;
   (void)offset;
}
static VOID APIENTRY
Stub_SetResourceMinLOD(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE resource, FLOAT minLod)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)resource;
   (void)minLod;
}
static VOID APIENTRY
Stub_CopyStructureCount(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE destination, UINT destinationOffset, D3D11DDI_HUNORDEREDACCESSVIEW source)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)destination;
   (void)destinationOffset;
   (void)source;
}
static VOID APIENTRY
Stub_RecycleCommandList(D3D10DDI_HDEVICE device, D3D11DDI_HCOMMANDLIST commandList)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)commandList;
}
static HRESULT APIENTRY
Stub_RecycleCreateCommandList(D3D10DDI_HDEVICE device, const D3D11DDIARG_CREATECOMMANDLIST *arguments, D3D11DDI_HCOMMANDLIST commandList, D3D11DDI_HRTCOMMANDLIST runtimeCommandList)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)arguments;
   (void)commandList;
   (void)runtimeCommandList;
   return E_NOTIMPL;
}
static HRESULT APIENTRY
Stub_RecycleCreateDeferredContext(D3D10DDI_HDEVICE device, const D3D11DDIARG_CREATEDEFERREDCONTEXT *arguments)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)arguments;
   return E_NOTIMPL;
}
static VOID APIENTRY
Stub_RecycleDestroyCommandList(D3D10DDI_HDEVICE device, D3D11DDI_HCOMMANDLIST commandList)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   (void)device;
   (void)commandList;
}

/*
 * ----------------------------------------------------------------------
 *
 * FillDeviceFuncs11 --
 *
 *    Publish the D3D11 device callbacks.
 *
 * ----------------------------------------------------------------------
 */

void
FillDeviceFuncs11(D3D11DDI_DEVICEFUNCS *pDeviceFuncs)
{
   memset(pDeviceFuncs, 0, sizeof *pDeviceFuncs);

   pDeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP = ResourceUpdateSubResourceUP;
   pDeviceFuncs->pfnVsSetConstantBuffers = VsSetConstantBuffers;
   pDeviceFuncs->pfnPsSetShaderResources = PsSetShaderResources;
   pDeviceFuncs->pfnPsSetShader = PsSetShader;
   pDeviceFuncs->pfnPsSetSamplers = PsSetSamplers;
   pDeviceFuncs->pfnVsSetShader = VsSetShader;
   pDeviceFuncs->pfnDrawIndexed = DrawIndexed;
   pDeviceFuncs->pfnDraw = Draw;
   pDeviceFuncs->pfnDynamicIABufferMapNoOverwrite = ResourceMap;
   pDeviceFuncs->pfnDynamicIABufferUnmap = ResourceUnmap;
   pDeviceFuncs->pfnDynamicConstantBufferMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicIABufferMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicConstantBufferUnmap = ResourceUnmap;
   pDeviceFuncs->pfnPsSetConstantBuffers = PsSetConstantBuffers;
   pDeviceFuncs->pfnIaSetInputLayout = IaSetInputLayout;
   pDeviceFuncs->pfnIaSetVertexBuffers = IaSetVertexBuffers;
   pDeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP = ResourceUpdateSubResourceUP;
   pDeviceFuncs->pfnVsSetConstantBuffers = VsSetConstantBuffers;
   pDeviceFuncs->pfnPsSetShaderResources = PsSetShaderResources;
   pDeviceFuncs->pfnPsSetShader = PsSetShader;
   pDeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP = ResourceUpdateSubResourceUP;
   pDeviceFuncs->pfnVsSetConstantBuffers = VsSetConstantBuffers;
   pDeviceFuncs->pfnPsSetShaderResources = PsSetShaderResources;
   pDeviceFuncs->pfnPsSetShader = PsSetShader;
   pDeviceFuncs->pfnPsSetSamplers = PsSetSamplers;
   pDeviceFuncs->pfnVsSetShader = VsSetShader;
   pDeviceFuncs->pfnDrawIndexed = DrawIndexed;
   pDeviceFuncs->pfnDraw = Draw;
   pDeviceFuncs->pfnDynamicIABufferMapNoOverwrite = ResourceMap;
   pDeviceFuncs->pfnDynamicIABufferUnmap = ResourceUnmap;
   pDeviceFuncs->pfnDynamicConstantBufferMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicIABufferMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicConstantBufferUnmap = ResourceUnmap;
   pDeviceFuncs->pfnPsSetConstantBuffers = PsSetConstantBuffers;
   pDeviceFuncs->pfnIaSetInputLayout = IaSetInputLayout;
   pDeviceFuncs->pfnIaSetVertexBuffers = IaSetVertexBuffers;
   pDeviceFuncs->pfnIaSetIndexBuffer = IaSetIndexBuffer;
   pDeviceFuncs->pfnDrawIndexedInstanced = DrawIndexedInstanced;
   pDeviceFuncs->pfnDrawInstanced = DrawInstanced;
   pDeviceFuncs->pfnDynamicResourceMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicResourceUnmap = ResourceUnmap;
   pDeviceFuncs->pfnGsSetConstantBuffers = GsSetConstantBuffers;
   pDeviceFuncs->pfnGsSetShader = GsSetShader;
   pDeviceFuncs->pfnIaSetTopology = IaSetTopology;
   pDeviceFuncs->pfnStagingResourceMap = ResourceMap;
   pDeviceFuncs->pfnStagingResourceUnmap = ResourceUnmap;
   pDeviceFuncs->pfnVsSetShaderResources = VsSetShaderResources;
   pDeviceFuncs->pfnVsSetSamplers = VsSetSamplers;
   pDeviceFuncs->pfnGsSetShaderResources = GsSetShaderResources;
   pDeviceFuncs->pfnGsSetSamplers = GsSetSamplers;
   pDeviceFuncs->pfnSetRenderTargets = SetRenderTargets11;
   pDeviceFuncs->pfnShaderResourceViewReadAfterWriteHazard = ShaderResourceViewReadAfterWriteHazard;
   pDeviceFuncs->pfnResourceReadAfterWriteHazard = ResourceReadAfterWriteHazard;
   pDeviceFuncs->pfnSetBlendState = SetBlendState;
   pDeviceFuncs->pfnSetDepthStencilState = SetDepthStencilState;
   pDeviceFuncs->pfnSetRasterizerState = SetRasterizerState;
   pDeviceFuncs->pfnQueryEnd = QueryEnd;
   pDeviceFuncs->pfnQueryBegin = QueryBegin;
   pDeviceFuncs->pfnResourceCopyRegion = ResourceCopyRegion;
   pDeviceFuncs->pfnResourceUpdateSubresourceUP = ResourceUpdateSubResourceUP;
   pDeviceFuncs->pfnSoSetTargets = SoSetTargets;
   pDeviceFuncs->pfnDrawAuto = DrawAuto;
   pDeviceFuncs->pfnSetViewports = SetViewports;
   pDeviceFuncs->pfnSetScissorRects = SetScissorRects;
   pDeviceFuncs->pfnClearRenderTargetView = ClearRenderTargetView;
   pDeviceFuncs->pfnClearDepthStencilView = ClearDepthStencilView;
   pDeviceFuncs->pfnSetPredication = SetPredication;
   pDeviceFuncs->pfnQueryGetData = QueryGetData;
   pDeviceFuncs->pfnFlush = Flush;
   pDeviceFuncs->pfnGenMips = GenMips;
   pDeviceFuncs->pfnResourceCopy = ResourceCopy;
   pDeviceFuncs->pfnResourceResolveSubresource = ResourceResolveSubResource;
   pDeviceFuncs->pfnResourceMap = ResourceMap;
   pDeviceFuncs->pfnResourceUnmap = ResourceUnmap;
   pDeviceFuncs->pfnResourceIsStagingBusy = ResourceIsStagingBusy;
   pDeviceFuncs->pfnRelocateDeviceFuncs = RelocateDeviceFuncs11;
   pDeviceFuncs->pfnCalcPrivateResourceSize = CalcPrivateResourceSize11;
   pDeviceFuncs->pfnCalcPrivateOpenedResourceSize = CalcPrivateOpenedResourceSize;
   pDeviceFuncs->pfnCreateResource = CreateResource11;
   pDeviceFuncs->pfnOpenResource = OpenResource;
   pDeviceFuncs->pfnDestroyResource = DestroyResource;
   pDeviceFuncs->pfnCalcPrivateShaderResourceViewSize = CalcPrivateShaderResourceViewSize11;
   pDeviceFuncs->pfnCreateShaderResourceView = CreateShaderResourceView11;
   pDeviceFuncs->pfnDestroyShaderResourceView = DestroyShaderResourceView;
   pDeviceFuncs->pfnCalcPrivateRenderTargetViewSize = CalcPrivateRenderTargetViewSize;
   pDeviceFuncs->pfnCreateRenderTargetView = CreateRenderTargetView;
   pDeviceFuncs->pfnDestroyRenderTargetView = DestroyRenderTargetView;
   pDeviceFuncs->pfnCalcPrivateDepthStencilViewSize = CalcPrivateDepthStencilViewSize11;
   pDeviceFuncs->pfnCreateDepthStencilView = CreateDepthStencilView11;
   pDeviceFuncs->pfnDestroyDepthStencilView = DestroyDepthStencilView;
   pDeviceFuncs->pfnCalcPrivateElementLayoutSize = CalcPrivateElementLayoutSize;
   pDeviceFuncs->pfnCreateElementLayout = CreateElementLayout;
   pDeviceFuncs->pfnDestroyElementLayout = DestroyElementLayout;
   pDeviceFuncs->pfnCalcPrivateBlendStateSize = CalcPrivateBlendStateSize1;
   pDeviceFuncs->pfnCreateBlendState = CreateBlendState1;
   pDeviceFuncs->pfnDestroyBlendState = DestroyBlendState;
   pDeviceFuncs->pfnCalcPrivateDepthStencilStateSize = CalcPrivateDepthStencilStateSize;
   pDeviceFuncs->pfnCreateDepthStencilState = CreateDepthStencilState;
   pDeviceFuncs->pfnDestroyDepthStencilState = DestroyDepthStencilState;
   pDeviceFuncs->pfnCalcPrivateRasterizerStateSize = CalcPrivateRasterizerStateSize;
   pDeviceFuncs->pfnCreateRasterizerState = CreateRasterizerState;
   pDeviceFuncs->pfnDestroyRasterizerState = DestroyRasterizerState;
   pDeviceFuncs->pfnCalcPrivateShaderSize = CalcPrivateShaderSize;
   pDeviceFuncs->pfnCreateVertexShader = CreateVertexShader;
   pDeviceFuncs->pfnCreateGeometryShader = CreateGeometryShader;
   pDeviceFuncs->pfnCreatePixelShader = CreatePixelShader;
   pDeviceFuncs->pfnCalcPrivateGeometryShaderWithStreamOutput = CalcPrivateGeometryShaderWithStreamOutput11;
   pDeviceFuncs->pfnCreateGeometryShaderWithStreamOutput = CreateGeometryShaderWithStreamOutput11;
   pDeviceFuncs->pfnDestroyShader = DestroyShader;
   pDeviceFuncs->pfnCalcPrivateSamplerSize = CalcPrivateSamplerSize;
   pDeviceFuncs->pfnCreateSampler = CreateSampler;
   pDeviceFuncs->pfnDestroySampler = DestroySampler;
   pDeviceFuncs->pfnCalcPrivateQuerySize = CalcPrivateQuerySize;
   pDeviceFuncs->pfnCreateQuery = CreateQuery;
   pDeviceFuncs->pfnDestroyQuery = DestroyQuery;
   pDeviceFuncs->pfnCheckFormatSupport = CheckFormatSupport;
   pDeviceFuncs->pfnCheckMultisampleQualityLevels = CheckMultisampleQualityLevels;
   pDeviceFuncs->pfnCheckCounterInfo = CheckCounterInfo;
   pDeviceFuncs->pfnCheckCounter = CheckCounter;
   pDeviceFuncs->pfnDestroyDevice = DestroyDevice;
   pDeviceFuncs->pfnSetTextFilterSize = SetTextFilterSize;
   pDeviceFuncs->pfnResourceConvert = ResourceCopy;
   pDeviceFuncs->pfnResourceConvertRegion = ResourceCopyRegion;
   /* Mesa leaves these unassigned in the D3D10 table as well. */
   pDeviceFuncs->pfnDrawIndexedInstancedIndirect = Stub_DrawIndexedInstancedIndirect;
   pDeviceFuncs->pfnDrawInstancedIndirect = Stub_DrawInstancedIndirect;
   pDeviceFuncs->pfnCommandListExecute = Stub_CommandListExecute;
   pDeviceFuncs->pfnHsSetShaderResources = Stub_HsSetShaderResources;
   pDeviceFuncs->pfnHsSetShader = Stub_HsSetShader;
   pDeviceFuncs->pfnHsSetSamplers = Stub_HsSetSamplers;
   pDeviceFuncs->pfnHsSetConstantBuffers = Stub_HsSetConstantBuffers;
   pDeviceFuncs->pfnDsSetShaderResources = Stub_DsSetShaderResources;
   pDeviceFuncs->pfnDsSetShader = Stub_DsSetShader;
   pDeviceFuncs->pfnDsSetSamplers = Stub_DsSetSamplers;
   pDeviceFuncs->pfnDsSetConstantBuffers = Stub_DsSetConstantBuffers;
   pDeviceFuncs->pfnCreateHullShader = Stub_CreateHullShader;
   pDeviceFuncs->pfnCreateDomainShader = Stub_CreateDomainShader;
   pDeviceFuncs->pfnCheckDeferredContextHandleSizes = Stub_CheckDeferredContextHandleSizes;
   pDeviceFuncs->pfnCalcDeferredContextHandleSize = Stub_CalcDeferredContextHandleSize;
   pDeviceFuncs->pfnCalcPrivateDeferredContextSize = Stub_CalcPrivateDeferredContextSize;
   pDeviceFuncs->pfnCreateDeferredContext = Stub_CreateDeferredContext;
   pDeviceFuncs->pfnAbandonCommandList = Stub_AbandonCommandList;
   pDeviceFuncs->pfnCalcPrivateCommandListSize = Stub_CalcPrivateCommandListSize;
   pDeviceFuncs->pfnCreateCommandList = Stub_CreateCommandList;
   pDeviceFuncs->pfnDestroyCommandList = Stub_DestroyCommandList;
   pDeviceFuncs->pfnCalcPrivateTessellationShaderSize = Stub_CalcPrivateTessellationShaderSize;
   pDeviceFuncs->pfnPsSetShaderWithIfaces = Stub_PsSetShaderWithIfaces;
   pDeviceFuncs->pfnVsSetShaderWithIfaces = Stub_VsSetShaderWithIfaces;
   pDeviceFuncs->pfnGsSetShaderWithIfaces = Stub_GsSetShaderWithIfaces;
   pDeviceFuncs->pfnHsSetShaderWithIfaces = Stub_HsSetShaderWithIfaces;
   pDeviceFuncs->pfnDsSetShaderWithIfaces = Stub_DsSetShaderWithIfaces;
   pDeviceFuncs->pfnCsSetShaderWithIfaces = Stub_CsSetShaderWithIfaces;
   pDeviceFuncs->pfnCreateComputeShader = Stub_CreateComputeShader;
   pDeviceFuncs->pfnCsSetShader = Stub_CsSetShader;
   pDeviceFuncs->pfnCsSetShaderResources = Stub_CsSetShaderResources;
   pDeviceFuncs->pfnCsSetSamplers = Stub_CsSetSamplers;
   pDeviceFuncs->pfnCsSetConstantBuffers = Stub_CsSetConstantBuffers;
   pDeviceFuncs->pfnCalcPrivateUnorderedAccessViewSize = Stub_CalcPrivateUnorderedAccessViewSize;
   pDeviceFuncs->pfnCreateUnorderedAccessView = Stub_CreateUnorderedAccessView;
   pDeviceFuncs->pfnDestroyUnorderedAccessView = Stub_DestroyUnorderedAccessView;
   pDeviceFuncs->pfnClearUnorderedAccessViewUint = Stub_ClearUnorderedAccessViewUint;
   pDeviceFuncs->pfnClearUnorderedAccessViewFloat = Stub_ClearUnorderedAccessViewFloat;
   pDeviceFuncs->pfnCsSetUnorderedAccessViews = Stub_CsSetUnorderedAccessViews;
   pDeviceFuncs->pfnDispatch = Stub_Dispatch;
   pDeviceFuncs->pfnDispatchIndirect = Stub_DispatchIndirect;
   pDeviceFuncs->pfnSetResourceMinLOD = Stub_SetResourceMinLOD;
   pDeviceFuncs->pfnCopyStructureCount = Stub_CopyStructureCount;
   pDeviceFuncs->pfnRecycleCommandList = Stub_RecycleCommandList;
   pDeviceFuncs->pfnRecycleCreateCommandList = Stub_RecycleCreateCommandList;
   pDeviceFuncs->pfnRecycleCreateDeferredContext = Stub_RecycleCreateDeferredContext;
   pDeviceFuncs->pfnRecycleDestroyCommandList = Stub_RecycleDestroyCommandList;
}

#endif /* SUPPORT_D3D11 */
