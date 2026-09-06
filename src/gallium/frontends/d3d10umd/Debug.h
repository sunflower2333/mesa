#pragma once

#ifdef __MINGW32__
#undef WIN32_LEAN_AND_MEAN /* for DEFINE_GUID macro */
#define _NO_OLDNAMES       /* avoid defining ssize_t */
#include <stdio.h>         /* for vsnprintf */
#undef fileno              /* we redefine this in vm_basic_defs.h */
#endif

#include <windows.h>
#include "util/u_debug.h"

#ifdef __cplusplus
extern "C" {
#endif


/* tgsi_to_nir() - the only consumer of this frontend's TGSI in the Zink/NIR
 * configuration - implements just the legacy TEX opcode family (TEX, TXP, TXB,
 * TXL, TXF, TXD, TG4).  It has no case at all for the SAMPLE family, and its
 * instruction switch ends in fprintf + abort(), so emitting SAMPLE terminates
 * the process that loaded the UMD.  The legacy path is therefore the default,
 * and this flag opts *in* to the SAMPLE path for debugging.  It only has an
 * effect in a MESA_DEBUG build, where st_debug is a real variable. */
#define ST_DEBUG_NEW_TEX_OPS   (1 <<  0)
#define ST_DEBUG_TGSI          (1 <<  1)


#if MESA_DEBUG
extern unsigned st_debug;
#else
#define st_debug 0
#endif


#if MESA_DEBUG
void st_debug_parse(void);
#else
#define st_debug_parse() ((void)0)
#endif


void
DebugPrintf(const char *format, ...);


void
CheckHResult(HRESULT hr, const char *function, unsigned line);


#define CHECK_NTSTATUS(status) \
   CheckNTStatus(status, __func__, __LINE__)


#define CHECK_HRESULT(hr) \
   CheckHResult(hr, __func__, __LINE__)


void
AssertFail(const char *expr, const char *file, unsigned line, const char *function);


#ifndef NDEBUG
#define ASSERT(expr) ((expr) ? (void)0 : AssertFail(#expr, __FILE__, __LINE__, __func__))
#else
#define ASSERT(expr) do { } while (0 && (expr))
#endif


#if 0 && !defined(NDEBUG)
#define LOG_ENTRYPOINT() DebugPrintf("%s\n", __func__)
#else
#define LOG_ENTRYPOINT() (void)0
#endif

#define LOG_UNSUPPORTED_ENTRYPOINT() DebugPrintf("%s XXX\n", __func__)

#define LOG_UNSUPPORTED(expr) \
   do { if (expr) DebugPrintf("%s:%d XXX %s\n", __func__, __LINE__, #expr); } while(0)


#ifdef __cplusplus
}
#endif

