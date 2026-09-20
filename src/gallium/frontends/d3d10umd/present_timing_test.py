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
#include <cstring>
using LONGLONG=long long;
using DWORD=unsigned long;
constexpr DWORD INVALID_FILE_ATTRIBUTES=~DWORD(0), FILE_ATTRIBUTE_DIRECTORY=16;
const char *env="";
DWORD attrs=INVALID_FILE_ATTRIBUTES;
unsigned reads=0, clocks=0;
DWORD GetEnvironmentVariableA(const char *, char *out, unsigned size) {
  ++reads;
  unsigned n=std::strlen(env);
  if (n<size) std::memcpy(out,env,n+1);
  return n<size ? n : n+1;
}
DWORD GetFileAttributesA(const char *) { ++reads; return attrs; }
struct LARGE_INTEGER { LONGLONG QuadPart; };
LONGLONG frequency=10000000, now=0;
void QueryPerformanceFrequency(LARGE_INTEGER *p) { ++clocks;p->QuadPart=frequency; }
void QueryPerformanceCounter(LARGE_INTEGER *p) { ++clocks;p->QuadPart=now; }
#include "PresentTiming.h"
int main(int argc, char **argv) {
  assert(argc==2);
  bool expected=false;
  if (!std::strcmp(argv[1],"marker")) { attrs=128;expected=true; }
  if (!std::strcmp(argv[1],"directory")) attrs=FILE_ATTRIBUTE_DIRECTORY;
  if (!std::strcmp(argv[1],"env1")) { env="1";expected=true; }
  if (!std::strcmp(argv[1],"env0")) { env="0";attrs=128; }
  if (!std::strcmp(argv[1],"invalid")) { env="yes";attrs=128; }
  assert(PresentDiagnosticsEnabled()==expected);
  const unsigned readCount=reads;
  env="1";attrs=128;
  for(unsigned i=0;i<1000;++i) assert(PresentDiagnosticsEnabled()==expected);
  assert(reads==readCount); // no environment/file probes on later Presents
  PresentTiming off(false);
  for(unsigned i=0;i<1000;++i) {
    off.mark();
    assert(!off.sample(i,false) && !off.sample(i,true));
  }
  assert(clocks==0 && off.completed==0);
  PresentTiming t(true);
  assert(t.completed==0 && t.usec(0)==-1 && t.totalUsec()==0);
  now=1230;t.mark();assert(t.usec(0)==123 && t.usec(1)==-1);
  now+=700000;t.mark();assert(t.usec(1)==70000 && t.totalUsec()==70123);
  assert(t.sample(17,false)); // unsampled sequence still records a long prepare
  now+=420;t.mark();now+=170;t.mark();
  assert(t.usec(2)==42 && t.usec(3)==17 && t.completed==4);
  t.mark();assert(t.completed==4 && t.totalUsec()==70182);
  now=0;PresentTiming fast(true);now=10;fast.mark();
  assert(fast.sample(16,false) && fast.sample(64,false));
  assert(!fast.sample(17,false) && fast.sample(17,true));
  now=0;PresentTiming boundary(true);now=199990;boundary.mark();
  assert(!boundary.sample(17,false));
  now=200000;boundary.mark();assert(boundary.sample(17,false));
  frequency=0;PresentTiming invalid(true);invalid.mark();assert(invalid.totalUsec()==-1);
  frequency=10000000;now=100;PresentTiming backwards(true);now=99;backwards.mark();
  assert(backwards.usec(0)==-1);
  frequency=19200000;now=900000000000LL;PresentTiming longClock(true);
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
    for mode in ('off', 'marker', 'directory', 'env1', 'env0', 'invalid'):
        subprocess.run([str(root/'test'), mode],check=True)
