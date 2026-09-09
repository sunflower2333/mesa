#include "Debug.h"

#include <stdarg.h>
#include <stdio.h>


#if MESA_DEBUG

unsigned st_debug = 0;

static const
struct debug_named_value st_debug_flags[] = {
   {"newtexops", ST_DEBUG_NEW_TEX_OPS, "newtexops"},
   {"tgsi", ST_DEBUG_TGSI, "tgsi"},
   DEBUG_NAMED_VALUE_END
};
void
st_debug_parse(void)
{
   st_debug = debug_get_flags_option("ST_DEBUG", st_debug_flags, st_debug);
}

#endif


void
DebugPrintf(const char *format, ...)
{
    /* Every DDI entry point calls this, and the hot ones run hundreds of times
     * a frame.  OutputDebugString raises a debug exception and takes a
     * machine-wide mutex on each call whether or not anyone is listening, so
     * leaving it on cost far more than the frame it was tracing.  Decide once,
     * and format nothing at all unless tracing was asked for.
     *
     * OutputDebugString buffers are session scoped, so a capture started from a
     * remote shell never sees output from dwm.exe, which runs as SYSTEM. Append
     * to a file as well, so the last entry point before a crash can be read
     * back. */
    static int trace = -1;
    if (trace < 0) {
        char enabled[8];
        trace = GetEnvironmentVariableA("VIOGPU_UMD_TRACE", enabled, sizeof enabled) > 0 &&
                enabled[0] == '1';
    }
    if (!trace) {
        return;
    }

    char buf[4096];

    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);

    OutputDebugStringA(buf);

    {
        HANDLE log = CreateFileA("C:\\Users\\Public\\umd_trace.log",
                                 FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (log != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(log, buf, (DWORD)strlen(buf), &written, NULL);
            CloseHandle(log);
        }
    }
}


/**
 * Produce a human readable message from HRESULT.
 *
 * @sa http://msdn.microsoft.com/en-us/library/ms679351(VS.85).aspx
 */
void
CheckHResult(HRESULT hr, const char *function, unsigned line)
{
   if (FAILED(hr)) {
      LPSTR lpMessageBuffer = NULL;

      FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                     FORMAT_MESSAGE_FROM_SYSTEM,
                     NULL,
                     hr,
                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     (LPSTR)&lpMessageBuffer,
                     0,
                     NULL);

      DebugPrintf("%s: %u: 0x%08lX: %s", function, line, hr, lpMessageBuffer);

      LocalFree(lpMessageBuffer);
   }
}


void
AssertFail(const char *expr,
           const char *file,
           unsigned line,
           const char *function)
{
   DebugPrintf("%s:%u:%s: Assertion `%s' failed.\n", file, line, function, expr);
#if defined(__GNUC__)
   __asm("int3");
#elif defined(_MSC_VER)
   __debugbreak();
#else
   DebugBreak();
#endif
}
