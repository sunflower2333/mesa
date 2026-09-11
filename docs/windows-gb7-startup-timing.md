# Windows GB7 startup timing

Enable `TU_WDDM_PERF=1` before starting the interactive test process. The
default path does not query timing APIs or emit these startup records. Set
the variable only for the candidate application; preserve the desktop UMD.

The existing submission/allocation timing does not cover the approximately
90-second GB7 preparation interval observed with both 2 GiB and 4 GiB guests.
This candidate adds records for device creation, graphics/compute pipeline
creation, compute pipeline cache lookup, SPIR-V conversion, NIR lowering and
shader creation. Device and pipeline API calls always emit when enabled;
nested shader/cache operations emit only when wall time reaches 50 ms.

The `TU_WDDM_PERF` prefix and its existing fields remain parser-compatible.
Additional `qpc_start`, `tid` and `thread_cpu_ms` fields identify the interval
and its calling-thread CPU use. A negative CPU value means unavailable,
not zero CPU use. CPU time includes that thread's kernel and user execution,
excludes compiler workers, and may be quantized. It is not GPU busy time.
Nested intervals overlap and must not be added together. API scopes include
failed creation attempts and do not establish successful shader execution.

Correlate QPC start/end with the existing guest stage samples and host submit
retirement trace. A long API wall interval with little calling-thread CPU
only narrows attribution to waits, descheduling or other threads; it does
not prove which one occurred. If the startup interval lies outside these
scopes, inspect application-side preparation next. The timing itself does
not change shader compilation, caching, resources, submission or fences.

This branch is an independent depth-one clone of Mesa 6c3ddd22849. It retains
the prior bounded command-buffer batching and reference validation repairs.
Runtime startup attribution and any resulting optimization remain pending.

The compiler audit also found a separate failure-path defect: a compute
pipeline cache miss did not check a null result from `tu_spirv_to_nir`, unlike
the graphics path. Conversion failure could reach disassembly or NIR lowering
with a null pointer. The compute path now takes its existing failure cleanup,
returns `VK_ERROR_OUT_OF_HOST_MEMORY` consistently with the graphics caller,
and leaves the output pipeline null. The helper's pointer-only interface does
not distinguish underlying conversion error codes; that convention is retained.

The production cache-miss fault-injection regression covers failed conversion
with and without executable capture, shader-create failure, cache hit,
compile-required early return and successful compilation. Its pre-fix negative
control must fail. This source defect is not yet tied to a captured GB7 crash.
