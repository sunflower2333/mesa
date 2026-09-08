# WDDM shared-content validation

The ARM64 `d3d11_shared_test` selects only the 1af4:1050 hardware adapter,
requires the installed VIOGPU UMD to be loaded, and tests:

- `--local`: four 64x64 BGRA patterns, all pixels read back on one device.
- `--shared`: the same patterns opened and read by another device. The opened
  texture remains alive across all four producer updates.
- `--keyed`: producer/consumer ownership changes with keyed mutexes. Positive
  WAIT_TIMEOUT/WAIT_ABANDONED results are failures too.
- `--nt`: keyed sharing using CreateSharedHandle/OpenSharedResource1.
- `--dcomp`: actual surface clear, visual/window binding, EndDraw, Commit and
  WaitForCommitCompletion. An API PASS needs independent visible-pixel evidence.

Run each mode separately in the logged-in console with a 45-second process
timeout. Preserve crash logs and compare Application RecordId deltas, since
the guest clock can move. The SDK-only workflow builds these tests without
rebuilding the full Mesa driver. Compile success is not runtime validation.

## Measured pre-fix failures (2026-09-08)

Guest package 100.6.101.58293, UMD SHA256
`A0B91CC709DC856DD113941AEA9D23880E18FA2A8FCA126DCF5281642393323D`:

- Local pattern control: exit 0, all four frames have 0/4096 mismatches.
- Legacy sharing: producer correct; GetSharedHandle and OpenSharedResource
  return S_OK, but consumer returns zero for 4096/4096 pixels; exit 1.
- Keyed/NT modes: AcquireSync succeeds, producer correct, ReleaseSync returns
  E_INVALIDARG; exit 1. NT handle creation is not reached in this baseline.
- DComp API sequence: four commits complete, no device removal; visible output
  remains unverified. Basic CreateSurface/BeginDraw is not the failing call.

## Implementation contract

The frontend currently renders with Zink/Turnip; WDDM Turnip has no external
memory import/export. Shared BGRA8, single-level, single-layer, non-MSAA default
textures therefore use their WDDM allocation as inter-process storage and the
Gallium texture as a GPU cache. This is GPU rendering with CPU content transfer,
not zero-copy sharing.

Writes mark a cache dirty. Flush, Present and ResolveSharedResource publish
dirty GPU contents using synchronized readback and LockCb/UnlockCb. Before a
copy or draw consumes a clean shared resource, the frontend imports its current
WDDM contents. Partial destination updates import untouched pixels first.
Opened resources retain the existing allocation rather than inventing shared
contents. Failed maps/locks/publication are returned to the runtime.

Other shared layouts are refused instead of being mislabeled BGRA8. Required
follow-up validation includes cross-process ownership, partial updates,
shader sampling, producer destruction with a consumer still alive, sustained
DWM composition, and the original Vulkan/DirectX acceptance workloads. None
of these are implied by a four-frame sharing PASS.
