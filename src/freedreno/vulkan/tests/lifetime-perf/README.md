# WDDM deferred retirement and persistent submit staging

This experiment changes the Turnip UMD, not the Native Context wire ABI. It adds
an opt-in final-BO-release queue, a per-device CPU submit scratch allocation and
aggregate ownership/retry counters. No GPU allocation reuse cache is introduced.

## Lifetime policy

`TU_WDDM_DEFERRED_BO_DESTROY=1` opts in before creating the Vulkan device. The
option is sampled once per device. With it unset, the existing synchronous BO
release policy and its wait behavior remain in place. CPU submit scratch reuse
is independent of this switch and is enabled in either mode.

Final BO release records the last submitted context fence and reset generation.
It keeps one internal owner reference, the sparse token, KMT allocation, metadata,
rounded VMA extent and heap charge. Pending owners are excluded from NEW residency
snapshots, maps and explicit-reference admission. Existing submissions still own
their original KMD references. Vulkan object/dump pointers are detached immediately.

No background thread or per-submit drain is added. Opportunistic passes run on
BO release and before submission (16 slots) and allocation (up to the existing
1024-BO limit). A rotating cursor avoids a permanently busy first entry starving
later entries. Each pass queries completion at most once for unlatched fences.
A reached fence is latched; zero is a never-submitted sentinel, not evidence that
a nonzero fence completed across wrap.

Each eligible owner gets one Unlock (when mapped) and one DestroyAllocation2
attempt. The reaper adds no sleeps or polling/retry loop; an OS thunk itself can
still block. Both busy statuses preserve ownership for a later pass. A hard
failure, failed query/unlock, or mismatched reset generation marks the device
lost and retains the graph rather than releasing its IOVA. Only exact
STATUS_SUCCESS releases UMD bookkeeping. AssumeNotInUse remains zero.

There are TWO lifetime boundaries. A successful KMT destroy is not evidence
that the host has detached its native range. The KMD's existing overlapping-range
ownership check continues to return DEVICE_BUSY until that detach. The existing
CreateAllocation guarded busy retry remains mandatory. This patch neither
invents host completion nor weakens that check or changes the ABI.

Live plus retired owners share the existing 1024-slot cap. Quarantine therefore
can make a new allocation fail at capacity while old GPU work is pending; it
does not grow an unbounded queue or silently evict a live owner. No explicit
reaper thread wakes after the workload stops. Final device teardown drains all
remaining owners. In experimental mode its preliminary wait is limited to the
existing 250 ms advisory budget; a timeout retains the complete outer graph
(process-lifetime leak rather than use-after-free). Subsequent legacy KMT teardown
retries retain their separate bounds, so 250 ms is NOT a total teardown deadline.

## Persistent CPU scratch

The first valid Render lazily allocates one device-owned scratch block containing
64 KiB of packet storage and 1024 render-reference entries (104 KiB total on a
64-bit target with 40-byte references). Later calls reuse it while wddm_mutex
serializes packet construction and the KMT Render call. Only used prefixes are
cleared. No pointer into this scratch is handed to the host; the existing
transport copies into the KMT-provided command/allocation/patch buffers.

Allocation failure returns out-of-host-memory without publishing a partial
scratch owner. Render failure retains scratch for safe final cleanup. Parent
teardown failures retain it with the rest of the device. Successful teardown
releases it once. This is CPU scratch reuse, not unsafe BO/IOVA recycling.

## Tests and diagnostics

```sh
python src/freedreno/vulkan/tests/lifetime-perf/run.py
python src/freedreno/vulkan/tests/lifetime-perf/run.py --negative-control fence
python src/freedreno/vulkan/tests/lifetime-perf/run.py --negative-control busy
python src/freedreno/vulkan/tests/lifetime-perf/run.py --negative-control residency
python src/freedreno/vulkan/tests/lifetime-perf/run.py --negative-control scratch
python src/freedreno/vulkan/tests/check_tu_wddm_policy.py
```

Tests extract and compile actual production functions, with fake OS/allocator
boundaries. GCC runs AddressSanitizer/UndefinedBehaviorSanitizer and MSVC uses
/W4 /WX. Coverage includes busy/status/epoch/query/unlock faults, duplicate final
release, wrap, rotating scans, teardown failures, independent devices and 64
randomized retirement schedules. 10,000 Render calls exercise changing packet
sizes and injected Render failures with one allocation and 9,999 reuses.
Negative controls must fail at their specific intended assertion. The harness
checks lock boundaries but is NOT a model of real kernel/GPU concurrency.

`TU_WDDM_DIAGNOSTICS=1` enables the existing diagnostic sink. The new lifetime
summary is multiline, aggregated and emitted on successful device teardown.
It reports queued/reaped/pending/peak, completion queries/pending observations,
destroy busy, failure reasons, and scratch allocations/reuses. Observation
counts are not nanoseconds, GPU execution time or FPS. A retained failed teardown
does not fabricate a final success summary. Existing failure diagnostics remain.

## Target acceptance still required

Use a recoverable VM and the same KMD/Mesa/DXVK/VKD3D and shader-cache settings.
Compare the opt-in switch off/on separately from pipeline window changes. Run
allocation churn, mapped uploads, buffer-device-address workloads, memory-budget
pressure, slow fences, repeated device create/destroy, and reset/TDR injection.
Check no black frames, stale data, premature address reuse or retained-owner
growth. Measure CPU time, frame-time distribution and throughput on hardware.
A clean build and these fixtures prove neither a speedup nor target acceptance.
