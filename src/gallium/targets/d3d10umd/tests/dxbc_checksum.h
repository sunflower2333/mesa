/* SPDX-License-Identifier: MIT
 * DXBC uses MD5 compression with a different final block layout. This small
 * test-only implementation is checked against the SDK compiler's checksum
 * before modifying any bytecode. All supported Windows targets are LE.
 */
#pragma once
#include <cstdint>
#include <cstring>

static void DxbcChecksum(const void *container, size_t bytes, uint32_t result[4])
{
   const uint32_t constants[64] = {
      0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
      0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
      0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
      0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
      0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
      0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
      0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
      0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
   };
   const unsigned shifts[4][4] = {{7,12,17,22},{5,9,14,20},{4,11,16,23},{6,10,15,21}};
   uint32_t state[4] = {0x67452301,0xefcdab89,0x98badcfe,0x10325476};
   auto block = [&](const unsigned char *data) {
      uint32_t words[16]; memcpy(words,data,sizeof(words));
      uint32_t a=state[0],b=state[1],c=state[2],d=state[3];
      for (unsigned i=0;i<64;i++) {
         uint32_t f; unsigned index;
         if(i<16) {f=(b&c)|(~b&d);index=i;}
         else if(i<32) {f=(d&b)|(~d&c);index=(5*i+1)%16;}
         else if(i<48) {f=b^c^d;index=(3*i+5)%16;}
         else {f=c^(b|~d);index=(7*i)%16;}
         const uint32_t sum=a+f+constants[i]+words[index];
         const unsigned shift=shifts[i/16][i%4];
         a=d;d=c;c=b;b+=(sum<<shift)|(sum>>(32-shift));
      }
      state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
   };
   const auto data=static_cast<const unsigned char *>(container)+20;
   const size_t length=bytes-20;
   const uint32_t bits=static_cast<uint32_t>(length*8);
   size_t offset=0;
   while(length-offset>=64) {block(data+offset);offset+=64;}
   unsigned char tail[64]={};
   const size_t remaining=length-offset;
   memcpy(tail,data+offset,remaining);tail[remaining]=0x80;
   if(remaining>=56) {block(tail);memset(tail,0,sizeof(tail));}
   else {memmove(tail+4,tail,remaining+1);}
   memcpy(tail,&bits,sizeof(bits));
   const uint32_t suffix=(bits>>2)|1;
   memcpy(tail+60,&suffix,sizeof(suffix));
   block(tail);memcpy(result,state,sizeof(state));
}
