// SPDX-License-Identifier: MIT
// Execute actual UMD query/flush entry points with reset at backend boundaries.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using UINT = unsigned;
using UINT64 = uint64_t;
using BOOL = int;
using HRESULT = int32_t;
#define APIENTRY
#ifndef __out_bcount_full_opt
#define __out_bcount_full_opt(x)
#endif
#define LOG_ENTRYPOINT() ((void)0)
#define LOG_UNSUPPORTED(x) ((void)(x))
#define DebugPrintf(...) ((void)0)
#define FAILED(x) ((x) < 0)
constexpr HRESULT D3DDDIERR_DEVICEREMOVED = -7, DXGI_DDI_ERR_WASSTILLDRAWING = -8;
constexpr UINT D3D10_DDI_GET_DATA_DO_NOT_FLUSH = 1;
enum pipe_reset_status { PIPE_NO_RESET, PIPE_GUILTY_CONTEXT_RESET, PIPE_UNKNOWN_CONTEXT_RESET };
enum { D3D10DDI_QUERY_EVENT, D3D10DDI_QUERY_OCCLUSIONPREDICATE, D3D10DDI_QUERY_STREAMOVERFLOWPREDICATE,
       D3D10DDI_QUERY_OCCLUSION, D3D10DDI_QUERY_TIMESTAMP, D3D10DDI_QUERY_TIMESTAMPDISJOINT,
       D3D10DDI_QUERY_PIPELINESTATS, D3D10DDI_QUERY_STREAMOUTPUTSTATS };
struct D3D10_DDI_QUERY_DATA_TIMESTAMP_DISJOINT { uint64_t Frequency; bool Disjoint; };
struct D3D10_DDI_QUERY_DATA_PIPELINE_STATISTICS {
   uint64_t IAVertices, IAPrimitives, VSInvocations, GSInvocations, GSPrimitives, CInvocations, CPrimitives, PSInvocations;
};
struct D3D10_DDI_QUERY_DATA_SO_STATISTICS { uint64_t NumPrimitivesWritten, PrimitivesStorageNeeded; };
union pipe_query_result {
   bool b;
   uint64_t u64;
   struct { uint64_t frequency; bool disjoint; } timestamp_disjoint;
   struct { uint64_t ia_vertices, ia_primitives, vs_invocations, gs_invocations, gs_primitives,
                      c_invocations, c_primitives, ps_invocations; } pipeline_statistics;
   struct { uint64_t num_primitives_written, primitives_storage_needed; } so_statistics;
};
struct pipe_context;
struct pipe_fence_handle {};
struct pipe_query {};
struct pipe_screen { bool (*fence_finish)(pipe_screen*, pipe_context*, pipe_fence_handle*, uint64_t); };
struct pipe_context {
   pipe_screen* screen;
   pipe_reset_status (*get_device_reset_status)(pipe_context*);
   bool (*get_query_result)(pipe_context*, pipe_query*, bool, pipe_query_result*);
   void (*flush)(pipe_context*, pipe_fence_handle**, unsigned);
};
struct Device { pipe_context* pipe; int LastFinishedQuerySeqNo = 0; };
struct Query { pipe_query* handle; pipe_fence_handle* completion_fence; int Type = D3D10DDI_QUERY_EVENT;
               int SeqNo = 9; uint32_t GetDataCount = 0; };
using D3D10DDI_HDEVICE = Device*;
using D3D10DDI_HQUERY = Query*;
Device* CastDevice(Device* d) { return d; }
pipe_context* CastPipeContext(Device* d) { return d->pipe; }
Query* CastQuery(Query* q) { return q; }
enum class Loss { None, Before, Fence, Result, Flush, Publish };
struct Scenario {
   Loss loss = Loss::None;
   pipe_reset_status reset = PIPE_NO_RESET;
   bool fenceReady = true, resultReady = true;
   unsigned fenceCalls = 0, resultCalls = 0, flushCalls = 0, publishCalls = 0;
   pipe_context* fenceContext = nullptr;
   HRESULT error = 0, publishError = 0;
} scenario;
void SetError(Device*, HRESULT hr) { scenario.error = hr; }
pipe_reset_status Reset(pipe_context*) { return scenario.reset; }
bool Fence(pipe_screen*, pipe_context* ctx, pipe_fence_handle*, uint64_t timeout) {
   assert(timeout == 0); ++scenario.fenceCalls; scenario.fenceContext = ctx;
   if (scenario.loss == Loss::Fence) scenario.reset = PIPE_UNKNOWN_CONTEXT_RESET;
   return scenario.fenceReady;
}
bool Result(pipe_context*, pipe_query*, bool wait, pipe_query_result* result) {
   assert(!wait); ++scenario.resultCalls;
   if (scenario.loss == Loss::Result) scenario.reset = PIPE_GUILTY_CONTEXT_RESET;
   result->b = true; return scenario.resultReady;
}
void BackendFlush(pipe_context*, pipe_fence_handle**, unsigned) {
   ++scenario.flushCalls;
   if (scenario.loss == Loss::Flush) scenario.reset = PIPE_GUILTY_CONTEXT_RESET;
}
HRESULT PublishSharedResources(Device*) {
   ++scenario.publishCalls;
   if (scenario.loss == Loss::Publish) scenario.reset = PIPE_UNKNOWN_CONTEXT_RESET;
   return scenario.publishError;
}
// PRODUCTION_FUNCTIONS
int main() {
   unsigned checks = 0, failures = 0;
   auto check = [&](bool pass, const char* label) {
      ++checks; if (!pass) { ++failures; std::printf("FAIL %s\n", label); }
   };
   pipe_screen screen{Fence};
   pipe_context pipe{&screen, Reset, Result, BackendFlush};
   pipe_query handle;
   pipe_fence_handle fence;
   for (unsigned flags : {0u, D3D10_DDI_GET_DATA_DO_NOT_FLUSH}) {
      for (Loss loss : {Loss::Before, Loss::Fence, Loss::Result, Loss::None}) {
         for (bool ready : {false, true}) {
            scenario = {}; scenario.loss = loss;
            if (loss == Loss::Before) scenario.reset = PIPE_GUILTY_CONTEXT_RESET;
            if (loss == Loss::Fence) scenario.fenceReady = ready;
            else scenario.resultReady = ready;
            Device device{&pipe}; Query query{&handle, &fence}; BOOL data = 0x12345678;
            QueryGetData(&device, &query, &data, sizeof(data), flags);
            bool lost = loss != Loss::None;
            HRESULT expected = lost ? D3DDDIERR_DEVICEREMOVED : ready ? 0 : DXGI_DDI_ERR_WASSTILLDRAWING;
            check(scenario.error == expected, "query distinguishes loss from pending/success");
            check(data == (!lost && ready ? 1 : 0x12345678), "lost/pending query preserves caller data");
            check(device.LastFinishedQuerySeqNo == (!lost && ready ? 9 : 0), "only successful query advances completion");
            if (loss == Loss::Before) check(!scenario.fenceCalls && !scenario.resultCalls, "already-lost query avoids backend work");
            else check(scenario.fenceContext == (flags ? nullptr : &pipe), "DO_NOT_FLUSH never supplies a context");
         }
      }
      scenario = {}; scenario.fenceReady = false;
      Device device{&pipe}; Query query{&handle, &fence};
      QueryGetData(&device, &query, nullptr, 0, flags);
      check(scenario.error == DXGI_DDI_ERR_WASSTILLDRAWING && !scenario.resultCalls,
            "ordinary pending fence remains nonblocking");
   }
   for (Loss loss : {Loss::Before, Loss::Flush, Loss::Publish, Loss::None}) {
      scenario = {}; scenario.loss = loss;
      if (loss == Loss::Before) scenario.reset = PIPE_GUILTY_CONTEXT_RESET;
      Device device{&pipe}; Flush(&device);
      check(scenario.error == (loss == Loss::None ? 0 : D3DDDIERR_DEVICEREMOVED), "flush propagates observed loss");
      check(scenario.flushCalls == (loss == Loss::Before ? 0u : 1u), "lost device does not submit again");
      check(scenario.publishCalls == (loss == Loss::Before || loss == Loss::Flush ? 0u : 1u),
            "flush cannot publish pixels after observing loss");
   }
   scenario = {}; scenario.publishError = D3DDDIERR_DEVICEREMOVED;
   Device device{&pipe}; Flush(&device);
   check(scenario.error == D3DDDIERR_DEVICEREMOVED, "kernel shared publication removal is retained");
   scenario = {}; pipe.get_device_reset_status = nullptr; Flush(&device);
   check(!scenario.error && scenario.flushCalls == 1 && scenario.publishCalls == 1,
         "backends without reset query preserve normal flush");
   std::printf("UMD device loss: %u/%u checks passed\n", checks - failures, checks);
   return failures ? 1 : 0;
}
