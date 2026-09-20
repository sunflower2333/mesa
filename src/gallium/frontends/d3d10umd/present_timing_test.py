#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute the production QPC phase accounting with deterministic timestamps."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
fixture = r'''
#include <cassert>
#include <cstdio>
using LONGLONG=long long;
struct LARGE_INTEGER { LONGLONG QuadPart; };
LONGLONG frequency=10000000, now=0;
void QueryPerformanceFrequency(LARGE_INTEGER *p) { p->QuadPart=frequency; }
void QueryPerformanceCounter(LARGE_INTEGER *p) { p->QuadPart=now; }
#include "PresentTiming.h"
int main() {
  PresentTiming t;
  assert(t.completed==0 && t.usec(0)==-1 && t.totalUsec()==0);
  now=1230;t.mark();assert(t.usec(0)==123 && t.usec(1)==-1);
  now+=700000;t.mark();assert(t.usec(1)==70000 && t.totalUsec()==70123);
  assert(t.sample(17,false)); // unsampled sequence still records a long prepare
  now+=420;t.mark();now+=170;t.mark();
  assert(t.usec(2)==42 && t.usec(3)==17 && t.completed==4);
  t.mark();assert(t.completed==4 && t.totalUsec()==70182);
  now=0;PresentTiming fast;now=10;fast.mark();
  assert(fast.sample(16,false) && fast.sample(64,false));
  assert(!fast.sample(17,false) && fast.sample(17,true));
  now=0;PresentTiming boundary;now=199990;boundary.mark();
  assert(!boundary.sample(17,false));
  now=200000;boundary.mark();assert(boundary.sample(17,false));
  frequency=0;PresentTiming invalid;invalid.mark();assert(invalid.totalUsec()==-1);
  frequency=10000000;now=100;PresentTiming backwards;now=99;backwards.mark();
  assert(backwards.usec(0)==-1);
  frequency=19200000;now=900000000000LL;PresentTiming longClock;
  now+=19200000LL*10000;longClock.mark();assert(longClock.usec(0)==10000000000LL);
  puts("present QPC phases, partial failure, sampling, long wait and clock validation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='present-timing-') as temporary:
    root=Path(temporary)
    (root/'test.cpp').write_text(fixture)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                    '-fsanitize=undefined','-I',str(here),str(root/'test.cpp'),
                    '-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
