# WDDM allocation errors after device loss

`tu_wddm_bo_init` previously mapped every failed zero-handle KMT creation to
`VK_ERROR_OUT_OF_DEVICE_MEMORY`. This included a closed native context after
device loss. A successful compensating destroy also cleared the original
CreateAllocation status, making later classification impossible.

The production path now preserves the initiating status after releasing a
partially created allocation, rejects already-lost devices before allocating,
and classifies failures as follows:

| Evidence | Vulkan result |
| --- | --- |
| Vulkan device already lost | DEVICE_LOST, no new KMT allocation |
| STATUS_DEVICE_NOT_READY (C00000A3), DEVICE_REMOVED (C00002B6), GRAPHICS_GPU_EXCEPTION_ON_DEVICE (C01E0200) | DEVICE_LOST and mark device lost |
| Existing context/fence or OS execution status check fails | DEVICE_LOST and mark device lost |
| Healthy device plus STATUS_NO_MEMORY (C0000017), INSUFFICIENT_RESOURCES (C000009A), COMMITMENT_LIMIT (C000012D), GRAPHICS_NO_VIDEO_MEMORY (C01E0100) | OUT_OF_DEVICE_MEMORY |
| Healthy device plus unexpected status, exhausted busy retry, or success without a handle | UNKNOWN with original NTSTATUS |
| Failed compensating destroy retains a live handle | Existing DEVICE_LOST behavior; preserve retry owner and VMA |

The installed KMD's native allocation path uses DEVICE_NOT_READY when context
snapshot/registration/reset admission fails. These are backend gates, not
memory-pressure evidence. The other status values match the Windows NTSTATUS
contract. Local descriptor allocation failures remain OUT_OF_HOST_MEMORY;
normal VMA/table capacity failures retain their existing results.

Only failure classification adds context/OS health queries; successful
allocations do not gain a query. There is no free-memory heuristic, retry-count
change, owner release change, context reopening, GPU reset or fence mutation.
Partial-create cleanup continues to leave AssumeNotInUse clear.

Run the actual production fixture:

```text
python src/freedreno/vulkan/tests/tu_wddm_allocation_status_test.py --sanitize
python src/freedreno/vulkan/tests/tu_wddm_allocation_status_test.py --negative-control-oom
python src/freedreno/vulkan/tests/tu_wddm_allocation_status_test.py --negative-control-rollback-status
```

Sanitizers are available on the Linux path. The fixture extracts the real
bo_init, allocation_create, descriptor validation, compensating destroy,
device status check and result mapping, using real Vulkan/private ABI headers
and controlled OS/allocator peers. It checks lost/gated/healthy states,
unknown and true pressure statuses, ordinary success, already-lost rejection,
rollback success and retained-owner failure. The existing SDK-compiled
fake-dispatch fixture also checks retained status after successful rollback.

The first semantic negative restores the old all-OOM call site and must fail
the device-loss oracle. The second removes status retention after rollback
and must fail the original-status oracle. Neither fixture operates a target.
This change prevents misleading post-loss OOM classification; it does not
repair the original long Hough dispatch, prove recovery, or make GB7 valid.
