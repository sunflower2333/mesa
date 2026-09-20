// SPDX-License-Identifier: MIT
#pragma once

/* File diagnostics are explicit and latched once per process. Even a sampled
 * CreateFile/WriteFile on DWM's render thread can block on NTFS/filters for
 * hundreds of milliseconds. Ordinary Present must not do that disk I/O.
 * Set the environment variable, or create the marker before starting DWM,
 * only for a diagnostic capture; those runs are not clean FPS baselines. */
static inline bool PresentDiagnosticsEnabled()
{
   static const bool enabled = [] {
      char value[2] = {};
      const DWORD length = GetEnvironmentVariableA("VIOGPU_PRESENT_TRACE", value, sizeof(value));
      if (length)
         return length == 1 && value[0] == '1';
      const DWORD attributes = GetFileAttributesA("C:\\ProgramData\\DroidVM\\viogpu-present-trace");
      return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
   }();
   return enabled;
}

/* QPC timestamps are comparable with ETW from the same guest. Keep incomplete
 * stages explicit: a failed prepare must not appear as a zero-cost callback. */
struct PresentTiming {
   LARGE_INTEGER frequency = {};
   LARGE_INTEGER stamps[5] = {};
   unsigned completed = 0;
   bool enabled;

   explicit PresentTiming(bool traceEnabled) : enabled(traceEnabled) {
      if (enabled) {
         QueryPerformanceFrequency(&frequency);
         QueryPerformanceCounter(&stamps[0]);
      }
   }
   void mark() {
      if (enabled && completed < 4)
         QueryPerformanceCounter(&stamps[++completed]);
   }
   LONGLONG usec(unsigned phase) const {
      if (frequency.QuadPart <= 0 || phase >= completed ||
          stamps[phase + 1].QuadPart < stamps[phase].QuadPart)
         return -1;
      const LONGLONG ticks = stamps[phase + 1].QuadPart - stamps[phase].QuadPart;
      return (ticks / frequency.QuadPart) * 1000000 +
             ((ticks % frequency.QuadPart) * 1000000) / frequency.QuadPart;
   }
   LONGLONG totalUsec() const {
      LONGLONG total = 0;
      for (unsigned i = 0; i < completed; ++i) {
         const LONGLONG phase = usec(i);
         if (phase < 0)
            return -1;
         total += phase;
      }
      return total;
   }
   bool sample(unsigned sequence, bool failed) const {
      return enabled && (failed || sequence <= 16 || sequence % 64 == 0 || totalUsec() >= 20000);
   }
};
