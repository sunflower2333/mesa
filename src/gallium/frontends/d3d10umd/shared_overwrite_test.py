#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the production overwrite predicate against pixel coverage and guards."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
source = (here / 'Resource.cpp').read_text()
start = source.index('\nSharedWriteCoversResource(')
begin = source.index('{', start)
end = source.index('\n}', begin) + 2
function = 'bool ' + source[start:end]
update_start = source.index('\nResourceUpdateSubResourceUP(')
update_begin = source.index('{', update_start)
depth = 1
update_end = update_begin + 1
while depth:
    depth += (source[update_end] == '{') - (source[update_end] == '}')
    update_end += 1
update_function = 'void ' + source[update_start:update_end]
fixture = r'''
#include <cstdio>
#include <set>
enum { PIPE_TEXTURE_2D, PIPE_TEXTURE_2D_ARRAY, PIPE_BUFFER, PIPE_TEXTURE_3D };
struct pipe_resource {
 unsigned target, last_level, array_size, depth0, width0, height0;
};
struct pipe_box { int x,y,z,width,height,depth; };
FUNCTION
int main() {
 unsigned failures=0, cases=0;
 auto check=[&](bool actual, bool expected) { ++cases; failures += actual!=expected; };
 pipe_resource t{PIPE_TEXTURE_2D,0,1,1,4,3};
 // The oracle enumerates destination pixels independently of the predicate.
 // Include one-pixel strips, shifted full-size rectangles and empty boxes.
 for(unsigned x=0;x<=4;++x) for(unsigned y=0;y<=3;++y)
 for(int w=0;w<=4;++w) for(int h=0;h<=3;++h) {
   pipe_box b{7,9,0,w,h,1}; // Source origin does not affect destination coverage.
   std::set<unsigned> touched;
   bool valid=true;
   for(int row=0;row<h;++row) for(int col=0;col<w;++col) {
     if(x+col>=4 || y+row>=3) valid=false;
     else touched.insert((y+row)*4+x+col);
   }
   check(SharedWriteCoversResource(&t,0,x,y,0,&b), valid && touched.size()==12);
 }
 pipe_box full{0,0,0,4,3,1};
 check(SharedWriteCoversResource(&t,0,0,0,0,&full),true);
 check(SharedWriteCoversResource(&t,1,0,0,0,&full),false);
 check(SharedWriteCoversResource(&t,0,0,0,1,&full),false);
 for(unsigned target: {PIPE_TEXTURE_2D_ARRAY,PIPE_BUFFER,PIPE_TEXTURE_3D}) {
   t.target=target; check(SharedWriteCoversResource(&t,0,0,0,0,&full),false);
 }
 t.target=PIPE_TEXTURE_2D;
 t.last_level=1; check(SharedWriteCoversResource(&t,0,0,0,0,&full),false); t.last_level=0;
 t.array_size=2; check(SharedWriteCoversResource(&t,0,0,0,0,&full),false); t.array_size=1;
 t.depth0=2; check(SharedWriteCoversResource(&t,0,0,0,0,&full),false); t.depth0=1;
 for(int depth: {-1,0,2}) {
   full.depth=depth; check(SharedWriteCoversResource(&t,0,0,0,0,&full),false);
 }
 full.depth=1; full.width=-1;
 check(SharedWriteCoversResource(&t,0,0,0,0,&full),false);
 full.width=4; full.height=-1;
 check(SharedWriteCoversResource(&t,0,0,0,0,&full),false);
 std::printf("shared overwrite: %u cases, %u failures\n",cases,failures);
 return failures ? 1 : 0;
}
'''
variants = {
    'production': function,
    'partial-width': function.replace('(unsigned)box->width == texture->width0', 'true'),
    'partial-height': function.replace('(unsigned)box->height == texture->height0', 'true'),
    'offset': function.replace('x == 0 && y == 0 && z == 0', '((void)x, (void)y, (void)z, true)'),
    'array': function.replace('texture->array_size == 1', 'true'),
    'mip': function.replace('texture->last_level == 0', 'true'),
}
with tempfile.TemporaryDirectory(prefix='shared-overwrite-') as tmp:
    tmp = Path(tmp)
    for name, body in variants.items():
        if name != 'production' and body == function:
            raise RuntimeError('Negative-control anchor missing: ' + name)
        cpp = tmp / (name + '.cpp')
        exe = tmp / name
        cpp.write_text(fixture.replace('FUNCTION', body))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        print(name + ': ' + result.stdout.strip())
        if result.stderr or (result.returncode != (0 if name == 'production' else 1)):
            raise RuntimeError(name + ': unexpected result: ' + result.stderr)

    # Execute the real UpdateSubresource callback, including a failed map.
    callback_fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
using UINT=unsigned; using HRESULT=int;
constexpr int PIPE_TEXTURE_2D=1, PIPE_NO_RESET=0, PIPE_MAP_WRITE=1, PIPE_MAP_DISCARD_RANGE=2;
constexpr int E_OUTOFMEMORY=-1, D3DDDIERR_DEVICEREMOVED=-2;
#define FAILED(x) ((x)<0)
#define __in_opt
#define __in
#define LOG_ENTRYPOINT() ((void)0)
struct pipe_resource { unsigned target=1,last_level=0,array_size=1,depth0=1,width0=4,height0=3; int format=0; };
struct pipe_box { int x,y,z,width,height,depth; };
struct pipe_transfer { unsigned stride=16,layer_stride=48; };
struct D3D10_DDI_BOX { unsigned left,top,front,right,bottom,back; };
struct pipe_context;
struct Resource { pipe_resource* resource; bool buffer=false,dirty=false; };
struct Device { pipe_context* pipe; };
using D3D10DDI_HDEVICE=Device*; using D3D10DDI_HRESOURCE=Resource*;
Device* CastDevice(Device* d){return d;} Resource* CastResource(Resource* r){return r;}
bool CheckPredicate(Device*){return true;}
int error,refreshes,maps,refreshResult,resetStatus; bool failMap;
uint32_t backing[12],cache[12]; pipe_transfer transfer;
struct pipe_context {
 void* (*texture_map)(pipe_context*,pipe_resource*,unsigned,unsigned,const pipe_box*,pipe_transfer**);
 void* (*buffer_map)(pipe_context*,pipe_resource*,unsigned,unsigned,const pipe_box*,pipe_transfer**);
 int (*get_device_reset_status)(pipe_context*);
};
int reset(pipe_context*){return resetStatus;}
void* map(pipe_context*,pipe_resource*,unsigned,unsigned,const pipe_box* b,pipe_transfer** t){
 ++maps; *t=&transfer; return failMap?nullptr:cache+b->y*4+b->x;
}
void SetError(Device*,int e){error=e;}
HRESULT RefreshSharedResource(Device*,Resource*){
 ++refreshes; if(!refreshResult) std::memcpy(cache,backing,sizeof(cache)); return refreshResult;
}
void MarkSharedResourceWritten(Device*,pipe_resource*);
Resource* active;
void MarkSharedResourceWritten(Device*,pipe_resource*){active->dirty=true;}
void subResourceBox(pipe_resource*,unsigned,unsigned* level,pipe_box* b){*level=0;*b={0,0,0,4,3,1};}
void pipe_buffer_unmap(pipe_context*,pipe_transfer*){}
void pipe_texture_unmap(pipe_context*,pipe_transfer*){}
void util_copy_rect(uint8_t* dst,int,unsigned stride,int,int,int w,int h,const uint8_t* src,unsigned pitch,int,int){
 for(int row=0;row<h;++row) std::memcpy(dst+row*stride,src+row*pitch,w*4);
}
FUNCTION
UPDATE
int main(){
 pipe_resource texture; Resource resource{&texture}; active=&resource;
 pipe_context pipe{map,map,reset}; Device device{&pipe};
 uint32_t input[12]; for(unsigned i=0;i<12;++i) input[i]=100+i;
 auto setup=[&](){for(unsigned i=0;i<12;++i){backing[i]=20+i;cache[i]=0xdead;}
   error=refreshes=maps=refreshResult=resetStatus=0;failMap=false;resource.dirty=false;};
 setup(); ResourceUpdateSubResourceUP(&device,&resource,0,nullptr,input,16,48);
 assert(!error && !refreshes && maps==1 && resource.dirty);
 assert(!std::memcmp(input,cache,sizeof(cache)));
 setup(); D3D10_DDI_BOX partial{1,1,0,3,2,1};
 ResourceUpdateSubResourceUP(&device,&resource,0,&partial,input,16,48);
 assert(!error && refreshes==1 && resource.dirty);
 for(unsigned i=0;i<12;++i) assert(cache[i]==(i==5?100:i==6?101:backing[i]));
 setup(); refreshResult=-3;
 ResourceUpdateSubResourceUP(&device,&resource,0,&partial,input,16,48);
 assert(error==-3 && !maps && !resource.dirty);
 for(bool dirty: {false,true}) for(int lost: {0,1}) {
   setup(); failMap=true; resource.dirty=dirty; resetStatus=lost;
   ResourceUpdateSubResourceUP(&device,&resource,0,nullptr,input,16,48);
   assert(error==(lost?D3DDDIERR_DEVICEREMOVED:E_OUTOFMEMORY) && resource.dirty==dirty);
   assert(!refreshes && maps==1 && cache[0]==0xdead);
 }
 std::puts("production UpdateSubresource: full/partial pixels, refresh failure and four map failures PASS");
}
'''
    cpp = tmp / 'update.cpp'
    exe = tmp / 'update'
    cpp.write_text('#include <initializer_list>\n' + callback_fixture.replace(
        'FUNCTION', function).replace('UPDATE', update_function))
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
