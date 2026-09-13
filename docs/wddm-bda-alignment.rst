WDDM buffer device address allocation alignment
==============================================

Turnip's WDDM backend implements
``VK_VALVE_buffer_device_address_allocation_alignment`` with a maximum request
of 65536 bytes. A nonzero power-of-two request on ``vkAllocateMemory`` selects
a 64 KiB aligned guest VMA reservation. The backing allocation size and KMT
page alignment remain unchanged, and the exact selected address travels in
``RequestedIova``. Existing failure, partial-create rollback and final-owner
paths retain responsibility for that reservation.

Absent or zero alignment requests keep the existing page alignment. The
feature must be enabled and ``VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`` must be
set for a nonzero request. Nonzero explicit opaque capture addresses cannot be
combined with nonzero alignment (VUID 12511). Capture/replay without an
alignment request retains its original address. WDDM does not advertise
sparse, lazy or external allocation paths; other backends do not advertise
this extension. This does not alter D3D12 feature-level admission.

Registry provenance
-------------------

Only the three extension structures, their ``VkStructureType`` values and the
extension XML block are backported from KhronosGroup/Vulkan-Headers commit
``ee2ec5fd83dafce291024683b50dc89219333076``. The remaining Vulkan header version
is unchanged. Official extension revision 1 is dated 2026-08-26. Its memory
valid-usage requirements are 12510 through 12514, and the minimum supported
maximum alignment is 65536 bytes. No private extension number is invented.

Validation
----------

``python src/freedreno/vulkan/tests/tu_wddm_allocation_status_test.py`` executes
the production request parser, WDDM BO allocator and rollback code against
controlled KMT peers, using Mesa's real ``src/util/vma.c`` allocator. It covers
fragmented VA ranges, requests from 1 through 65536, page-sized and odd-page
allocations, feature/flags/max/power-of-two rejection, original capture replay,
and exact backing/IOVA/rollback accounting. The
``--negative-control-alignment`` mode replaces only the actual VMA alignment
argument with the old page size and must detect misaligned results.

This is an allocator/transport test, not hardware acceptance. Root owns
installation and real Vulkan and D3D12 allocation/GPU readback validation.
