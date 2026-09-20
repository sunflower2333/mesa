// SPDX-License-Identifier: MIT
#pragma once

/* QPC timestamps are comparable with ETW from the same guest. Keep incomplete
 * stages explicit: a failed prepare must not appear as a zero-cost callback. */
struct PresentTiming {
   LARGE_INTEGER frequency = {};
   LARGE_INTEGER stamps[5] = {};
   unsigned completed = 0;

   PresentTiming() {
      QueryPerformanceFrequency(&frequency);
      QueryPerformanceCounter(&stamps[0]);
   }
   void mark() {
      if (completed < 4)
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
      return failed || sequence <= 16 || sequence % 64 == 0 || totalUsec() >= 20000;
   }
};
