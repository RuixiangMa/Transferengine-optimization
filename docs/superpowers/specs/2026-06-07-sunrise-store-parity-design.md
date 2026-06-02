# Mooncake Store Sunrise Link Parity Design

**Date:** 2026-06-07
**Status:** Draft
**Goal:** Bring mooncake-store's sunrise_link support to feature parity with Ascend, enabling multi-process C2C via Tang IPC.

## 1. Background

Sunrise Link is the C2C (chip-to-chip) transport for Sunrise AI accelerators, using the Tang runtime and PTML library. It is already partially integrated into mooncake-store:

- Protocol admission via TENT backend (`protocolRequiresTentBackend("sunrise_link") == true`)
- Host and device data paths (`put/get` and `put_from/get_into`) work
- ConfigDict whitelist includes `sunrise_link`
- GPU staging utilities (`gpu_staging_utils.h`) support Tang pointer detection and memcpy
- Pinned buffer pool uses `tangHostAlloc` / `tangFreeHost`
- CMake links against `libtangrt_shared.so`

However, compared to Ascend (the reference for NPU transport integration), sunrise_link is missing critical store-layer capabilities, especially for multi-process C2C.

## 2. Gap Analysis (vs Ascend)

| Capability | Ascend | sunrise_link Current | Status |
|---|---|---|---|
| Protocol admission | `client_service.cpp` ascend branch | TENT auto-route | Done |
| ConfigDict whitelist | Not included | Included | Done |
| Segment allocator/deleter | `AscendSegmentDeleter` + `ascend_allocate/free_memory` | None | **Missing** |
| Store-layer runtime init | `setup_ascend_internal()` + `ContextManager` | None | **Missing** |
| SHM IPC import (real client) | `ascend_shm_internal()` + `ascend_ipc_shm_internal()` | None | **Missing** |
| SHM teardown | `teardown_ascend_shm_buffer()` | None | **Missing** |
| Dummy client integration | `register_ascend_shm()` + device buffer tracking + reconnect | None | **Missing** |
| RPC handler registration | 3 handlers in `real_client_main.cpp` | None | **Missing** |
| Agent mode flag | `ascend_agent_mode` | None | **Missing** |
| Aligned buffer allocator | `UseProtocolAllocator("ascend")` | Not included | **Missing** |
| shm_helper VMM allocation | `ascend_allocate_vmm_memory_direct` | None | **Missing** |
| MappedShm extension | `is_ascend`, `is_ipc`, `vmm_handle`, `ipc_key_data` | None | **Missing** |
| Device pointer detection | `aclrtPointerGetAttributes` | `madvise` + `tangPointerGetAttributes` | Done |
| H2D/D2H/D2D copy | `aclrtMemcpy` | `tangMemcpy` | Done |
| Pinned buffer | `aclrtMallocHost/FreeHost` | `tangHostAlloc/FreeHost` | Done |
| Transport same_machine constraint | N/A (Ascend has fabric path) | Not set | **Missing** |
| Process-level runtime lifecycle | N/A | `dlclose` in `uninstall()` | **Missing** |
| Store-level tests | None | None (partial pybind tests) | **Missing** |

## 3. Architecture Decision: Direct TENT IPC (Not Dummy-Real IPC Relay)

### Options Considered

**Option A: Dummy-Real IPC Relay (align with Ascend)**
Dummy client detects device memory → exports `tangIpcMemHandle` via RPC → real client calls `tangIpcOpenMemHandle` → registers with transfer engine.

Pros: Architecture aligned with Ascend. Cons: Duplicates IPC mechanism already handled by TENT transport layer (`addMemoryBuffer` → `tangIpcGetMemHandle` → `shm_path` serialization → `relocateRemoteAddress` → `tangIpcOpenMemHandle`).

**Option B: Direct TENT IPC (chosen)**
Leverage TENT's existing segment metadata exchange which already carries `tangIpcMemHandle_t` in `shm_path`. The transport layer handles IPC open/close automatically. Store layer only needs memory management, agent mode support, and SHM lifecycle.

Pros: Reuses existing mechanism, less code, no RPC overhead for IPC handle transfer, consistent with TENT's transport-autonomous design. Cons: Different pattern from Ascend (but justified by TENT-only path).

### Rationale

Sunrise Link is TENT-only (`protocolRequiresTentBackend == true`). TENT's transport already implements full IPC handle lifecycle. Adding a parallel RPC-based IPC relay would be redundant. The store layer should focus on memory management and agent mode orchestration, not re-implement transport-level IPC.

### Dummy-Real Role Clarification

Despite choosing "Direct TENT IPC" over "Dummy-Real IPC Relay," sunrise_link still needs the Dummy-Real architecture for agent mode. The distinction:

- **IPC handle exchange**: Handled by TENT transport automatically. When a dummy client's segment is registered with the transfer engine, `addMemoryBuffer()` calls `tangIpcGetMemHandle` and stores the handle in `desc.shm_path`. When the real client's transport needs to access that memory, `relocateRemoteAddress()` automatically calls `tangIpcOpenMemHandle`. No store-layer RPC is needed for this.

- **Buffer registration and lifecycle**: The dummy client still needs RPC to tell the real client about its memory regions (address, size, device_id). The real client must `tangIpcOpenMemHandle` in its own process context so the opened pointer can be registered with the real client's transfer engine. This is the `sunrise_shm_internal` RPC handler.

- **Key difference from Ascend**: Ascend needs two RPC paths (VMM Fabric Handle + IPC Key) because its segment metadata exchange does not automatically carry IPC handles. Sunrise only needs one RPC path (buffer registration + IPC handle) because the TENT transport handles the actual data transfer IPC internally.

## 4. Design

### 4.1 Segment Memory Allocation/Deallocation

**Files:** `mooncake-store/src/utils.cpp`, `mooncake-store/include/real_client.h`, `mooncake-store/src/real_client.cpp`, `mooncake-store/src/aligned_client_buffer.cpp`

#### 4.1.1 Memory allocator in `utils.cpp`

Add `sunrise_link` branch to `allocate_buffer_allocator_memory()` and `free_memory()`:

```
#if defined(USE_SUNRISE)
if (protocol == "sunrise_link") {
    return sunrise_allocate_memory(total_size);
}
#endif
```

```
#if defined(USE_SUNRISE)
if (protocol == "sunrise_link") {
    return sunrise_free_memory(ptr);
}
#endif
```

New helper functions in a `sunrise_allocator.h` (or inline in utils.cpp):

- `sunrise_allocate_memory(size_t total_size)`: Uses `tangHostAlloc` for host-accessible pinned memory. This is the same allocator used by the pinned buffer pool. `tangHostAlloc` returns host-visible memory that is also accessible by the Tang runtime for DMA operations. Sets `tangSetDevice(0)` before allocation. Falls back to `aligned_alloc` if `tangHostAlloc` fails.
- `sunrise_free_memory(void* ptr)`: Uses `tangFreeHost` to free. Only valid for pointers allocated by `tangHostAlloc`.

#### 4.1.2 SunriseSegmentDeleter in `real_client.h`

```cpp
struct SunriseSegmentDeleter {
    void operator()(void* ptr) {
        if (ptr) {
            sunrise_free_memory(ptr);
        }
    }
};
```

In `real_client.cpp`, when `protocol == "sunrise_link"`, use `SunriseSegmentDeleter` for segment pointer lifecycle (similar to `AscendSegmentDeleter` at line 850).

#### 4.1.3 Aligned buffer allocator

In `aligned_client_buffer.cpp`, add `"sunrise_link"` to `UseProtocolAllocator()`:

```cpp
bool UseProtocolAllocator(const std::string& protocol) {
    return protocol == "ascend" || protocol == "ubshmem" || protocol == "sunrise_link";
}
```

### 4.2 Agent Mode and Multi-Process Support

**Files:** `mooncake-transfer-engine/include/config.h`, `mooncake-store/include/real_client.h`, `mooncake-store/src/real_client.cpp`, `mooncake-store/src/dummy_client.cpp`, `mooncake-store/src/real_client_main.cpp`

#### 4.2.1 Agent mode flag

In `config.h`, add:

```cpp
bool sunrise_agent_mode = false;
```

In `real_client_main.cpp`, under `USE_SUNRISE` guard:

```cpp
globalConfig().sunrise_agent_mode = true;
```

#### 4.2.2 MappedShm extension

In `real_client.h` `MappedShm` struct, add:

```cpp
bool is_sunrise = false;
```

This distinguishes Sunrise IPC-mapped memory from POSIX mmap regions. Unlike Ascend which needs both VMM and IPC paths (`is_ipc`, `vmm_handle`, `ipc_key_data`), Sunrise only has the IPC path via `tangIpcMemHandle_t`, so `is_sunrise` is sufficient — the IPC handle lifecycle is managed by the TENT transport layer, not the store layer.

#### 4.2.3 Sunrise SHM registration (real client)

In `real_client.cpp`, add `sunrise_shm_internal()` RPC handler:

1. Receive serialized `tangIpcMemHandle_t` + `device_id` + `size` from dummy client
2. Call `tangSetDevice(device_id)` then `tangIpcOpenMemHandle(&opened_ptr, handle, tangIpcMemLazyEnablePeerAccess)`
3. Register opened memory with `client_->RegisterLocalMemory(opened_ptr, size, "cuda:" + device_id)`
4. Store in `MappedShm{is_sunrise=true, shm_buffer=opened_ptr, shm_size=size, device_id=device_id}`
5. Return the mapped address offset to the dummy client

Add `teardown_sunrise_shm_buffer()`:

1. Unregister from transfer engine: `client_->unregisterLocalMemory(shm.shm_buffer)`
2. Close IPC handle: `tangIpcCloseMemHandle(shm.shm_buffer)`

Add `sunrise_unmap_shm_internal()` to iterate and tear down sunrise shm regions.

#### 4.2.4 Sunrise SHM registration (dummy client)

In `dummy_client.cpp`, add `register_sunrise_shm()`:

1. Detect if the buffer is device memory via `gpu_staging::IsDevicePointer()`
2. If device memory: call `tangIpcGetMemHandle(&handle, buffer)` to export the IPC handle
3. Serialize the handle and send via RPC: `invoke_rpc<&RealClient::sunrise_shm_internal>(handle_bytes, device_id, size)`
4. Track the buffer for reconnection in `registered_device_buffers_`

For non-device (host) memory: use the standard POSIX shm path (memfd_create + mmap).

#### 4.2.5 RPC handler registration

In `real_client_main.cpp`, register:

```cpp
server.register_handler<&RealClient::sunrise_shm_internal>(&real_client);
server.register_handler<&RealClient::sunrise_unmap_shm_internal>(&real_client);
```

#### 4.2.6 Dummy client register_buffer integration

In `dummy_client.cpp` `register_buffer()`, add a sunrise branch:

```cpp
#if defined(USE_SUNRISE)
if (gpu_staging::IsDevicePointer(buffer, &device_id)) {
    register_sunrise_shm(buffer, size, device_id);
    register_device_buffer_for_reconnect(buffer, size, device_id);
    return;
}
#endif
```

#### 4.2.7 Reconnection support

In `dummy_client.cpp` reconnection path, re-register all sunrise device buffers by calling `register_sunrise_shm()` for each entry in `registered_device_buffers_`.

### 4.3 Store-Layer Runtime Initialization

**Files:** `mooncake-store/src/real_client.cpp`

Add `setup_sunrise_internal()`:

1. Verify Tang runtime is available (check `tangInit()` result or equivalent)
2. Set default device: `tangSetDevice(0)`
3. Log device count

Called from `setup_internal()` when `protocol == "sunrise_link"` and `sunrise_agent_mode` is true.

### 4.4 SHM Helper Adaptation

**Files:** `mooncake-store/src/shm_helper.cpp`

When `sunrise_agent_mode` is true, use `tangHostAlloc` for shared memory allocation instead of `memfd_create` + `mmap`. On free, use `tangFreeHost`.

This mirrors how Ascend uses `ascend_allocate_vmm_memory_direct` when `ascend_agent_mode && ascend_use_fabric_mem`.

### 4.5 Process-Level Runtime Lifecycle

**File:** `mooncake-transfer-engine/tent/src/transport/sunrise_link/sunrise_link_transport.cpp`

**Problem:** `uninstall()` calls `dlclose()` on PTML and Tang runtime libraries. If a new `TransferEngine` is created after destruction, `dlopen` may not reinitialize correctly (static state from the previous load may be stale).

**Solution** (from existing design doc `2026-06-04-sunrise-link-process-runtime-design.md`):

- Move Tang/PTML library handles and runtime initialization state into file-local process-shared storage
- `initSunriseLink()` should initialize that shared state once (using `std::call_once` or similar)
- Transport destruction should no longer call `dlclose()`
- Instance-owned cleanup only for IPC mappings (`relocate_map_`) and registered memory

This is a prerequisite for reliable multi-process operation where the real client's engine may be torn down and recreated.

### 4.6 Transport Selector same_machine Constraint

**File:** `mooncake-transfer-engine/tent/src/runtime/transport_selector.cpp`

Add `same_machine=true` capability to `SunriseLinkTransport` or enforce it in the transport selector. Currently, NVLink and SHM have `same_machine` constraints (transport_selector.cpp line 319), but sunrise_link does not. Since `tangIpcOpenMemHandle` is fundamentally machine-local, the selector should not route cross-node transfers to sunrise_link.

Implementation: Add `caps.same_machine_only = true` to `SunriseLinkTransport` constructor, and check it in `isTransportAvailable()`.

### 4.7 CMake Integration

**File:** `mooncake-store/src/CMakeLists.txt`

Add Sunrise-specific link dependencies under `USE_SUNRISE` guard:

```cmake
if(USE_SUNRISE)
    target_link_libraries(mooncake_store PRIVATE tangrt_shared ptml_shared)
    target_link_libraries(mooncake_client PRIVATE tangrt_shared ptml_shared)
endif()
```

### 4.8 Testing

**Files:** `mooncake-store/tests/pybind_client_test.cpp`, new test files

1. **Multi-process C2C test**: Fork-based test (similar to `sunrise_link_transport_test.cpp`) but at the store level. Two processes, each with a store client, one puts data, the other gets it via sunrise_link.

2. **Dummy-Real integration test**: Test the dummy client → real client path with sunrise_link. Verify device buffer registration, shm mapping, and data transfer.

3. **Agent mode lifecycle test**: Verify that creating/destroying/re-creating store clients with sunrise_link works correctly (relies on process-level runtime fix from 4.5).

4. **Existing pybind tests**: Verify that existing `SunriseLinkHostPutGetRemoveWithTent` and `SunriseLinkDevicePutFromAndGetInto` still pass.

## 5. Implementation Order

1. Process-level runtime lifecycle (4.5) — prerequisite for reliable multi-process
2. Segment memory allocator/deleter (4.1) — foundational
3. Agent mode flag + MappedShm extension (4.2.1, 4.2.2) — data structure prep
4. Store-layer runtime init (4.3) — initialization path
5. SHM helper adaptation (4.4) — memory management
6. Sunrise SHM RPC handlers (4.2.3, 4.2.5) — real client side
7. Dummy client integration (4.2.4, 4.2.6, 4.2.7) — dummy client side
8. Transport selector same_machine constraint (4.6) — routing safety
9. CMake integration (4.7) — build system
10. Testing (4.8) — verification

## 6. Files Modified

| File | Change |
|---|---|
| `mooncake-transfer-engine/tent/src/transport/sunrise_link/sunrise_link_transport.cpp` | Process-level runtime (no dlclose), same_machine cap |
| `mooncake-transfer-engine/tent/include/tent/transport/sunrise_link/sunrise_link_transport.h` | same_machine cap flag |
| `mooncake-transfer-engine/tent/src/runtime/transport_selector.cpp` | Check same_machine for sunrise_link |
| `mooncake-transfer-engine/include/config.h` | Add `sunrise_agent_mode` flag |
| `mooncake-store/src/utils.cpp` | sunrise_link allocator/free branches |
| `mooncake-store/include/real_client.h` | `SunriseSegmentDeleter`, `MappedShm::is_sunrise`, `sunrise_shm_internal`, `teardown_sunrise_shm_buffer` declarations |
| `mooncake-store/src/real_client.cpp` | `setup_sunrise_internal`, `sunrise_shm_internal`, `teardown_sunrise_shm_buffer`, `sunrise_unmap_shm_internal`, segment deleter integration |
| `mooncake-store/src/dummy_client.cpp` | `register_sunrise_shm`, `register_buffer` sunrise branch, reconnect support |
| `mooncake-store/src/real_client_main.cpp` | RPC handler registration, `sunrise_agent_mode = true` |
| `mooncake-store/src/aligned_client_buffer.cpp` | Add `sunrise_link` to `UseProtocolAllocator` |
| `mooncake-store/src/shm_helper.cpp` | `tangHostAlloc` path for `sunrise_agent_mode` |
| `mooncake-store/src/CMakeLists.txt` | `USE_SUNRISE` link dependencies |
| `mooncake-store/tests/pybind_client_test.cpp` | Multi-process C2C test additions |
