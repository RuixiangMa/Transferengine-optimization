# Sunrise Store Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring mooncake-store's sunrise_link support to feature parity with Ascend, enabling multi-process C2C via Tang IPC.

**Architecture:** Use Direct TENT IPC — the TENT transport layer already handles `tangIpcGetMemHandle`/`tangIpcOpenMemHandle` via segment metadata exchange. Store layer adds memory management (Sunrise-specific allocator/deleter), agent mode support (dummy-real buffer registration RPC), and process-level runtime lifecycle fixes.

**Tech Stack:** C++17, Tang runtime API (`tang_runtime_api.h`, `ptml.h`), TENT transport framework, mooncake-store RPC framework.

---

## Task 1: Process-Level Runtime Lifecycle

**Rationale:** `uninstall()` calls `dlclose()` on Tang/PTML libraries. In multi-process agent mode, the real client may create/destroy engines, and re-loading libraries after `dlclose` can fail due to stale static state. This is a prerequisite for all other tasks.

**Files:**
- Modify: `mooncake-transfer-engine/tent/src/transport/sunrise_link/sunrise_link_transport.cpp`
- Modify: `mooncake-transfer-engine/tent/include/tent/transport/sunrise_link/sunrise_link_transport.h`

- [ ] **Step 1: Add process-level runtime state to the .cpp file**

In `sunrise_link_transport.cpp`, add file-local process-shared state before the `SunriseLinkTransport::SunriseLinkTransport()` constructor (around line 250):

```cpp
namespace {

struct ProcessRuntimeState {
    void* ptml_handle = nullptr;
    void* tang_handle = nullptr;
    bool initialized = false;
    std::once_flag init_flag;
};

static ProcessRuntimeState& processRuntime() {
    static ProcessRuntimeState state;
    return state;
}

}
```

- [ ] **Step 2: Rewrite `initSunriseLink()` to use process-level state**

Replace the body of `initSunriseLink()` (lines 297-345) to initialize once via `std::call_once`:

```cpp
Status SunriseLinkTransport::initSunriseLink() {
    auto& rt = processRuntime();
    std::call_once(rt.init_flag, [&rt]() {
        rt.ptml_handle =
            dlopen(TangRtSharedObjectPath("libptml_shared.so").c_str(), RTLD_NOW);
        if (!rt.ptml_handle) {
            char* error = dlerror();
            LOG(ERROR) << "Failed to load libptml_shared.so: "
                       << (error ? error : "unknown error");
            return;
        }

        LOAD_PTML_SYM(rt.ptml_handle, "ptmlInit", ptmlInit);
        LOAD_PTML_SYM(rt.ptml_handle, "ptmlPtlinkEnableAll", ptmlPtlinkEnableAll);
        LOAD_PTML_SYM(rt.ptml_handle, "ptmlPtlinkPhytopoDetect",
                      ptmlPtlinkPhytopoDetect);
        LOAD_PTML_SYM(rt.ptml_handle, "ptmlDeviceGetCount", ptmlDeviceGetCount);

        ptmlReturn_t ret = ptmlInit();
        if (ret != PTML_SUCCESS) {
            LOG(ERROR) << "ptmlInit failed, error code: " << ret;
            return;
        }

        ret = ptmlPtlinkEnableAll();
        if (ret != PTML_SUCCESS) {
            LOG(WARNING) << "ptmlPtlinkEnableAll failed, error code: " << ret;
        }

        rt.tang_handle =
            dlopen(TangRtSharedObjectPath("libtangrt_shared.so").c_str(), RTLD_NOW);
        if (!rt.tang_handle) {
            char* error = dlerror();
            LOG(ERROR) << "Failed to load libtangrt_shared.so: "
                       << (error ? error : "unknown error");
            return;
        }

        rt.initialized = true;
        LOG(INFO) << "SunriseLink process-level runtime initialized (PTML + Tang)";
    });

    if (!rt.ptml_handle || !rt.tang_handle) {
        return Status::InternalError("SunriseLink runtime initialization failed");
    }

    ptml_handle_ = rt.ptml_handle;
    runtime_lib_handle_ = rt.tang_handle;
    return Status::OK();
}
```

- [ ] **Step 3: Remove `dlclose` from `uninstall()`**

In `uninstall()` (lines 1041-1085), remove the `dlclose` blocks (lines 1061-1064 and 1075-1078). The library handles are now process-lifetime and should not be closed. Keep all other cleanup (IPC handle closing, memory deregistration):

```cpp
Status SunriseLinkTransport::uninstall() {
    if (installed_) {
        std::vector<void*> reg_addrs;
        {
            std::lock_guard<std::mutex> guard(registered_memory_mutex_);
            reg_addrs.reserve(registered_memory_.size());
            for (auto& entry : registered_memory_) {
                reg_addrs.push_back(entry.first);
            }
        }
        for (void* addr : reg_addrs) {
            auto status = deregisterMemory(addr);
            if (!status.ok()) return status;
        }
        {
            std::lock_guard<std::mutex> guard(registered_memory_mutex_);
            registered_memory_.clear();
            registered_memory_gpu_id_.clear();
        }

        for (auto& seg_entry : relocate_map_) {
            for (auto& entry : seg_entry.second) {
                if (entry.second.dev_ptr) {
                    tangIpcCloseMemHandle(entry.second.dev_ptr);
                }
            }
        }
        relocate_map_.clear();

        // Do NOT dlclose ptml_handle_ or runtime_lib_handle_ — they are
        // process-lifetime resources managed by ProcessRuntimeState.

        metadata_.reset();
        installed_ = false;
        LOG(INFO) << "SunriseLink transport uninstalled";
    }
    return Status::OK();
}
```

- [ ] **Step 4: Remove the member handles from the header if desired, or keep as aliases**

In `sunrise_link_transport.h`, the members `runtime_lib_handle_` (line 108) and `ptml_handle_` (line 110) can remain as aliases pointing to the process-level state. They are still useful for the LOAD_PTML_SYM macro which writes to file-local function pointers. No header change needed.

- [ ] **Step 5: Build and verify existing tests still pass**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build && cmake --build . --target tent_xport_sunrise_link 2>&1 | tail -20
```
Expected: Build succeeds with no errors.

- [ ] **Step 6: Commit**

```bash
git add mooncake-transfer-engine/tent/src/transport/sunrise_link/sunrise_link_transport.cpp
git commit -m "fix(sunrise_link): move Tang/PTML runtime to process-lifetime, stop dlclose on uninstall"
```

---

## Task 2: Sunrise Memory Allocator and Segment Deleter

**Rationale:** Store segments need Tang-compatible memory allocation (tangHostAlloc) instead of plain malloc, so the Tang runtime can perform DMA on these buffers. This mirrors `ascend_allocate_memory` / `ascend_free_memory`.

**Files:**
- Create: `mooncake-transfer-engine/include/sunrise_allocator.h`
- Modify: `mooncake-store/src/utils.cpp`
- Modify: `mooncake-store/include/real_client.h`
- Modify: `mooncake-store/src/real_client.cpp`
- Modify: `mooncake-store/src/aligned_client_buffer.cpp`

- [ ] **Step 1: Create `sunrise_allocator.h`**

Create `mooncake-transfer-engine/include/sunrise_allocator.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdlib>
#include <glog/logging.h>

#if defined(USE_SUNRISE)
#include <tang_runtime_api.h>
#endif

inline void* sunrise_allocate_memory(size_t total_size) {
#if defined(USE_SUNRISE)
    void* ptr = nullptr;
    tangError_t ret = tangHostAlloc(&ptr, total_size, 0);
    if (ret != tangSuccess || !ptr) {
        LOG(WARNING) << "tangHostAlloc failed (" << ret
                     << "), falling back to aligned_alloc for size="
                     << total_size;
        void* fallback = nullptr;
        if (posix_memalign(&fallback, 64, total_size) != 0) return nullptr;
        return fallback;
    }
    return ptr;
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, 64, total_size) != 0) return nullptr;
    return ptr;
#endif
}

inline void sunrise_free_memory(void* ptr) {
    if (!ptr) return;
#if defined(USE_SUNRISE)
    tangError_t ret = tangFreeHost(ptr);
    if (ret != tangSuccess) {
        LOG(WARNING) << "tangFreeHost failed (" << ret << "), trying free()";
        free(ptr);
    }
#else
    free(ptr);
#endif
}
```

- [ ] **Step 2: Add sunrise_link branch to `allocate_buffer_allocator_memory()` in `utils.cpp`**

In `mooncake-store/src/utils.cpp`, add the include at the top (around line 45, after the ascend includes):

```cpp
#if defined(USE_SUNRISE)
#include "sunrise_allocator.h"
#endif
```

In `allocate_buffer_allocator_memory()` (around line 116), add after the ascend block:

```cpp
#if defined(USE_SUNRISE)
    if (protocol == "sunrise_link") {
        return sunrise_allocate_memory(total_size);
    }
#endif
```

- [ ] **Step 3: Add sunrise_link branch to `free_memory()` in `utils.cpp`**

In `free_memory()` (around line 375), add after the ascend block:

```cpp
#if defined(USE_SUNRISE)
    if (protocol == "sunrise_link") {
        return sunrise_free_memory(ptr);
    }
#endif
```

- [ ] **Step 4: Add `SunriseSegmentDeleter` to `real_client.h`**

In `mooncake-store/include/real_client.h`, add after `AscendSegmentDeleter` (after line 767):

```cpp
struct SunriseSegmentDeleter {
    void operator()(void* ptr) {
        if (ptr) {
            sunrise_free_memory(ptr);
        }
    }
};
```

Also add the include at the top (near the ascend includes):
```cpp
#if defined(USE_SUNRISE)
#include "sunrise_allocator.h"
#endif
```

And add the member (near `ascend_segment_ptrs_` around line 783):
```cpp
std::vector<std::unique_ptr<void, SunriseSegmentDeleter>>
    sunrise_segment_ptrs_;
```

- [ ] **Step 5: Use `SunriseSegmentDeleter` in `real_client.cpp`**

In `mooncake-store/src/real_client.cpp`, find the segment deleter section (around line 850 where `ascend_segment_ptrs_` is used). Add after it:

```cpp
else if (this->protocol == "sunrise_link") {
    sunrise_segment_ptrs_.emplace_back(ptr, SunriseSegmentDeleter{});
}
```

Also in `tearDownAll_internal()`, add cleanup for `sunrise_segment_ptrs_` (near where `ascend_segment_ptrs_` is cleared):

```cpp
sunrise_segment_ptrs_.clear();
```

- [ ] **Step 6: Add `sunrise_link` to `UseProtocolAllocator()` in `aligned_client_buffer.cpp`**

In `mooncake-store/src/aligned_client_buffer.cpp`, modify `UseProtocolAllocator()` (line 16-18):

```cpp
bool UseProtocolAllocator(const std::string& protocol) {
    return protocol == kAscendProtocol || protocol == kUbshmemProtocol ||
           protocol == "sunrise_link";
}
```

- [ ] **Step 7: Build and verify**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build && cmake --build . --target mooncake_store mooncake_client 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 8: Commit**

```bash
git add mooncake-transfer-engine/include/sunrise_allocator.h mooncake-store/src/utils.cpp mooncake-store/include/real_client.h mooncake-store/src/real_client.cpp mooncake-store/src/aligned_client_buffer.cpp
git commit -m "feat(store): add Sunrise memory allocator, segment deleter, and protocol allocator support"
```

---

## Task 3: Agent Mode Flag and MappedShm Extension

**Rationale:** The `sunrise_agent_mode` flag is needed for the dummy-real architecture. `MappedShm::is_sunrise` distinguishes Sunrise IPC regions from POSIX mmap regions during cleanup.

**Files:**
- Modify: `mooncake-transfer-engine/include/config.h`
- Modify: `mooncake-store/include/real_client.h`

- [ ] **Step 1: Add `sunrise_agent_mode` to `GlobalConfig` in `config.h`**

In `mooncake-transfer-engine/include/config.h`, in the `GlobalConfig` struct (after `ascend_agent_mode` at line 80):

```cpp
bool sunrise_agent_mode = false;
```

- [ ] **Step 2: Add `is_sunrise` to `MappedShm` in `real_client.h`**

In `mooncake-store/include/real_client.h`, in the `MappedShm` struct (after `is_ascend` at line 799):

```cpp
bool is_sunrise = false;
```

- [ ] **Step 3: Commit**

```bash
git add mooncake-transfer-engine/include/config.h mooncake-store/include/real_client.h
git commit -m "feat: add sunrise_agent_mode flag and MappedShm::is_sunrise"
```

---

## Task 4: Store-Layer Runtime Initialization

**Rationale:** When the store runs in agent mode with sunrise_link, Tang runtime must be initialized and a default device context set before any memory operations. This mirrors `setup_ascend_internal()`.

**Files:**
- Modify: `mooncake-store/include/real_client.h`
- Modify: `mooncake-store/src/real_client.cpp`

- [ ] **Step 1: Declare `setup_sunrise_internal()` in `real_client.h`**

In `mooncake-store/include/real_client.h`, near `setup_ascend_internal()` declaration (around line 891):

```cpp
tl::expected<void, ErrorCode> setup_sunrise_internal(size_t local_buffer_size);
```

- [ ] **Step 2: Implement `setup_sunrise_internal()` in `real_client.cpp`**

In `mooncake-store/src/real_client.cpp`, add after `setup_ascend_internal()` (around line 626):

```cpp
tl::expected<void, ErrorCode> RealClient::setup_sunrise_internal(
    size_t local_buffer_size) {
#if defined(USE_SUNRISE)
    int device_count = 0;
    tangError_t ret = tangGetDeviceCount(&device_count);
    if (ret != tangSuccess || device_count <= 0) {
        LOG(ERROR) << "tangGetDeviceCount failed or no devices found, ret="
                   << ret << " count=" << device_count;
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    ret = tangSetDevice(0);
    if (ret != tangSuccess) {
        LOG(ERROR) << "tangSetDevice(0) failed: " << ret << " "
                   << tangGetErrorString(ret);
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    LOG(INFO) << "Sunrise runtime initialized with " << device_count
              << " device(s)";
#endif
    return {};
}
```

- [ ] **Step 3: Call `setup_sunrise_internal()` from `setup_internal()`**

In `mooncake-store/src/real_client.cpp`, in `setup_internal()` (around line 636, after the `setup_ascend_internal` block under `USE_ASCEND_DIRECT`), add:

```cpp
#if defined(USE_SUNRISE)
    if (protocol == "sunrise_link" && globalConfig().sunrise_agent_mode) {
        auto sunrise_setup = setup_sunrise_internal(local_buffer_size);
        if (!sunrise_setup) return sunrise_setup;
    }
#endif
```

- [ ] **Step 4: Build and verify**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build && cmake --build . --target mooncake_store mooncake_client 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add mooncake-store/include/real_client.h mooncake-store/src/real_client.cpp
git commit -m "feat(store): add setup_sunrise_internal for Tang runtime init in agent mode"
```

---

## Task 5: SHM Helper Adaptation

**Rationale:** When `sunrise_agent_mode` is true, shared memory should use `tangHostAlloc` instead of `memfd_create` + `mmap`, so the Tang runtime can DMA to/from these buffers. This mirrors the Ascend VMM allocation path.

**Files:**
- Modify: `mooncake-store/src/shm_helper.cpp`

- [ ] **Step 1: Add Sunrise allocation path in `ShmHelper::allocate()`**

In `mooncake-store/src/shm_helper.cpp`, in the `allocate()` method (around line 79, after the ascend agent mode block), add:

```cpp
#if defined(USE_SUNRISE)
    if (globalConfig().sunrise_agent_mode) {
        void* ptr = sunrise_allocate_memory(alloc_size);
        if (!ptr) {
            LOG(ERROR) << "sunrise_allocate_memory failed for size="
                       << alloc_size;
            return nullptr;
        }
        shm->base_addr = reinterpret_cast<uintptr_t>(ptr);
        shm->fd = -1;
        LOG(INFO) << "Allocated Sunrise shm buffer size=" << alloc_size
                  << " addr=" << shm->base_addr;
        return shm;
    }
#endif
```

This requires adding the include at the top:
```cpp
#if defined(USE_SUNRISE)
#include "sunrise_allocator.h"
#endif
```

- [ ] **Step 2: Add Sunrise free path in `ShmHelper::cleanup()` and `ShmHelper::free()`**

In `cleanup()` (around line 56, after the ascend block), add:

```cpp
#if defined(USE_SUNRISE)
        if (globalConfig().sunrise_agent_mode) {
            sunrise_free_memory(reinterpret_cast<void*>(shm->base_addr));
            continue;
        }
#endif
```

In `free()` (around line 151, after the ascend block), add:

```cpp
#if defined(USE_SUNRISE)
    if (globalConfig().sunrise_agent_mode) {
        sunrise_free_memory(reinterpret_cast<void*>((*it)->base_addr));
        it = segments_.erase(it);
        continue;
    }
#endif
```

- [ ] **Step 3: Build and verify**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build && cmake --build . --target mooncake_store 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add mooncake-store/src/shm_helper.cpp
git commit -m "feat(store): use tangHostAlloc for shm in sunrise agent mode"
```

---

## Task 6: Sunrise SHM RPC Handlers (Real Client)

**Rationale:** The real client needs RPC handlers so the dummy client can register its device memory regions. The handler receives a `tangIpcMemHandle_t`, opens it in the real client's process, and registers the memory with the transfer engine.

**Files:**
- Modify: `mooncake-store/include/real_client.h`
- Modify: `mooncake-store/src/real_client.cpp`

- [ ] **Step 1: Declare Sunrise RPC methods in `real_client.h`**

In `mooncake-store/include/real_client.h`, near the ascend RPC declarations (around line 485), add:

```cpp
#if defined(USE_SUNRISE)
    tl::expected<void, ErrorCode> sunrise_shm_internal(
        uint64_t dummy_base_addr, size_t mem_size, bool is_local_buffer,
        const std::string& ipc_handle_bytes, int32_t device_id,
        const UUID& client_id);

    tl::expected<void, ErrorCode> sunrise_unmap_shm_internal(
        const UUID& client_id);
#endif
```

- [ ] **Step 2: Implement `sunrise_shm_internal()` in `real_client.cpp`**

In `mooncake-store/src/real_client.cpp`, add after `teardown_ascend_shm_buffer()` (around line 2372):

```cpp
#if defined(USE_SUNRISE)
tl::expected<void, ErrorCode> RealClient::sunrise_shm_internal(
    uint64_t dummy_base_addr, size_t mem_size, bool is_local_buffer,
    const std::string& ipc_handle_bytes, int32_t device_id,
    const UUID& client_id) {
    if (ipc_handle_bytes.size() != sizeof(tangIpcMemHandle_t)) {
        LOG(ERROR) << "Invalid Sunrise IPC handle size: "
                   << ipc_handle_bytes.size()
                   << " expected: " << sizeof(tangIpcMemHandle_t);
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }

    tangIpcMemHandle_t handle;
    memcpy(&handle, ipc_handle_bytes.data(), sizeof(handle));

    int saved_dev = -1;
    tangGetDevice(&saved_dev);
    tangError_t ret = tangSetDevice(device_id);
    if (ret != tangSuccess) {
        LOG(ERROR) << "tangSetDevice(" << device_id
                   << ") failed in sunrise_shm_internal: " << ret;
        return tl::unexpected(ErrorCode::INTERNAL_ERROR);
    }

    void* opened_ptr = nullptr;
    ret = tangIpcOpenMemHandle(&opened_ptr, handle,
                               tangIpcMemLazyEnablePeerAccess);
    if (saved_dev >= 0) tangSetDevice(saved_dev);
    if (ret != tangSuccess || !opened_ptr) {
        LOG(ERROR) << "tangIpcOpenMemHandle failed: " << ret << " "
                   << tangGetErrorString(ret);
        return tl::unexpected(ErrorCode::INTERNAL_ERROR);
    }

    std::string location = "cuda:" + std::to_string(device_id);
    if (is_local_buffer) {
        auto reg_result = client_->RegisterLocalMemory(opened_ptr, mem_size,
                                                       location, false, true);
        if (!reg_result) {
            LOG(ERROR) << "RegisterLocalMemory failed for Sunrise shm";
            tangIpcCloseMemHandle(opened_ptr);
            return tl::unexpected(reg_result.error());
        }
    }

    MappedShm shm;
    shm.shm_name = "sunrise_ipc_" + std::to_string(dummy_base_addr);
    shm.shm_addr_offset = 0;
    shm.shm_buffer = opened_ptr;
    shm.shm_size = mem_size;
    shm.dummy_base_addr = dummy_base_addr;
    shm.is_sunrise = true;
    shm.is_ipc = true;
    shm.device_id = device_id;

    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        auto& ctx = shm_contexts_[client_id];
        ctx.mapped_shms.push_back(std::move(shm));
    }

    LOG(INFO) << "Sunrise shm registered: dev=" << device_id
              << " size=" << mem_size
              << " ptr=" << opened_ptr;
    return {};
}
#endif
```

- [ ] **Step 3: Implement `teardown_sunrise_shm_buffer()` helper**

Add near the `sunrise_shm_internal` implementation:

```cpp
#if defined(USE_SUNRISE)
void RealClient::teardown_sunrise_shm_buffer(MappedShm& shm) {
    if (shm.shm_size > 0 && client_) {
        auto res = client_->unregisterLocalMemory(shm.shm_buffer, true);
        if (!res) {
            LOG(WARNING) << "Failed to unregister Sunrise local memory: "
                         << shm.shm_name
                         << ", error: " << toString(res.error());
        }
    }
    if (shm.shm_buffer) {
        tangError_t ret = tangIpcCloseMemHandle(shm.shm_buffer);
        if (ret != tangSuccess) {
            LOG(WARNING) << "tangIpcCloseMemHandle failed: " << ret;
        }
        shm.shm_buffer = nullptr;
    }
    LOG(INFO) << "Sunrise shm torn down: " << shm.shm_name;
}
#endif
```

Declare this in `real_client.h` near `teardown_ascend_shm_buffer` (around line 889):

```cpp
#if defined(USE_SUNRISE)
    void teardown_sunrise_shm_buffer(MappedShm& shm);
#endif
```

- [ ] **Step 4: Implement `sunrise_unmap_shm_internal()`**

```cpp
#if defined(USE_SUNRISE)
tl::expected<void, ErrorCode> RealClient::sunrise_unmap_shm_internal(
    const UUID& client_id) {
    std::lock_guard<std::mutex> lock(context_mutex_);
    auto it = shm_contexts_.find(client_id);
    if (it == shm_contexts_.end()) return {};

    auto& mapped_shms = it->second.mapped_shms;
    for (auto shm_it = mapped_shms.begin(); shm_it != mapped_shms.end();) {
        if (shm_it->is_sunrise) {
            teardown_sunrise_shm_buffer(*shm_it);
            shm_it = mapped_shms.erase(shm_it);
        } else {
            ++shm_it;
        }
    }
    if (mapped_shms.empty()) {
        shm_contexts_.erase(it);
    }
    return {};
}
#endif
```

- [ ] **Step 5: Add Sunrise branch in existing teardown paths**

In `tearDownAll_internal()`, where mapped_shms are cleaned up (near line 1100-1137), add a Sunrise check alongside the existing Ascend check:

```cpp
if (shm.is_sunrise) {
    teardown_sunrise_shm_buffer(shm);
} else if (shm.is_ascend) {
    teardown_ascend_shm_buffer(shm);
} else {
    // POSIX mmap path
}
```

Similarly in `unregister_shm_buffer_internal()` (near line 2451).

In the dummy client monitor (near line 5174), add Sunrise handling:

```cpp
if (globalConfig().sunrise_agent_mode) {
    (void)real_client_rpc_->call<&RealClient::sunrise_unmap_shm_internal>(client_id_);
}
```

- [ ] **Step 6: Build and verify**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build && cmake --build . --target mooncake_store mooncake_client 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 7: Commit**

```bash
git add mooncake-store/include/real_client.h mooncake-store/src/real_client.cpp
git commit -m "feat(store): add Sunrise SHM RPC handlers for multi-process C2C"
```

---

## Task 7: Dummy Client Integration

**Rationale:** The dummy client needs to detect Sunrise device memory, export IPC handles, and register them with the real client via RPC. This is the critical path for multi-process C2C.

**Files:**
- Modify: `mooncake-store/src/dummy_client.cpp`
- Modify: `mooncake-store/src/real_client_main.cpp`

- [ ] **Step 1: Add `register_sunrise_shm()` to `dummy_client.cpp`**

In `mooncake-store/src/dummy_client.cpp`, add after `register_ascend_shm()` (around line 371):

```cpp
#if defined(USE_SUNRISE)
void DummyClient::register_sunrise_shm(ShmSegment* shm) {
    uint64_t dummy_base_addr = shm->base_addr;
    bool already_mapped = false;
    auto check_result = real_client_rpc_->call<&RealClient::is_shm_mapped_internal>(
        dummy_base_addr, client_id_);
    if (check_result && *check_result) {
        already_mapped = true;
    }

    void* buffer = reinterpret_cast<void*>(shm->base_addr);
    int device_id = -1;

    if (gpu_staging::IsDevicePointer(buffer, &device_id)) {
        if (already_mapped) return;

        tangSetDevice(device_id);
        tangIpcMemHandle_t handle;
        tangError_t ret = tangIpcGetMemHandle(&handle, buffer);
        if (ret != tangSuccess) {
            LOG(ERROR) << "tangIpcGetMemHandle failed: " << ret << " "
                       << tangGetErrorString(ret);
            return;
        }

        std::string handle_bytes(reinterpret_cast<const char*>(&handle),
                                 sizeof(handle));
        auto result =
            real_client_rpc_->call<&RealClient::sunrise_shm_internal>(
                dummy_base_addr, shm->size, true, handle_bytes, device_id,
                client_id_);
        if (!result) {
            LOG(ERROR) << "sunrise_shm_internal RPC failed for device buffer";
            return;
        }
        LOG(INFO) << "Registered Sunrise device shm: dev=" << device_id
                  << " size=" << shm->size;
    } else {
        if (already_mapped) return;
        register_shm_via_ipc(shm);
    }
}
#endif
```

Declare in the header if needed (check `dummy_client.h` for the pattern).

- [ ] **Step 2: Add Sunrise branch in `register_buffer()`**

In `dummy_client.cpp` `register_buffer()` (around line 645-704), add a Sunrise branch before the Ascend branch:

```cpp
#if defined(USE_SUNRISE)
    if (globalConfig().sunrise_agent_mode) {
        int device_id = -1;
        if (gpu_staging::IsDevicePointer(buffer, &device_id)) {
            register_device_buffer_for_reconnect(buffer, size, device_id);
            return;
        }
    }
#endif
```

- [ ] **Step 3: Add Sunrise branch in `setup_dummy()` and `unregister_shm()`**

In `setup_dummy()` (around line 504-518), add Sunrise registration path:

```cpp
#if defined(USE_SUNRISE)
    if (globalConfig().sunrise_agent_mode) {
        register_sunrise_shm(shm.get());
        continue;
    }
#endif
```

In `unregister_shm()` (around line 565-576), add Sunrise unmap:

```cpp
#if defined(USE_SUNRISE)
    if (globalConfig().sunrise_agent_mode) {
        (void)real_client_rpc_->call<&RealClient::sunrise_unmap_shm_internal>(client_id_);
        return;
    }
#endif
```

- [ ] **Step 4: Add reconnection support for Sunrise device buffers**

In the reconnection path (around line 1295-1310), add after the Ascend re-registration:

```cpp
#if defined(USE_SUNRISE)
    if (globalConfig().sunrise_agent_mode) {
        for (auto& [buffer, info] : registered_device_buffers_) {
            ShmSegment tmp_shm;
            tmp_shm.base_addr = reinterpret_cast<uintptr_t>(buffer);
            tmp_shm.size = info.size;
            register_sunrise_shm(&tmp_shm);
        }
    }
#endif
```

- [ ] **Step 5: Register Sunrise RPC handlers in `real_client_main.cpp`**

In `mooncake-store/src/real_client_main.cpp`, add after the ascend handler registrations (around line 68):

```cpp
#if defined(USE_SUNRISE)
    server.register_handler<&RealClient::sunrise_shm_internal>(&real_client);
    server.register_handler<&RealClient::sunrise_unmap_shm_internal>(&real_client);
#endif
```

And add the agent mode flag (around line 108):

```cpp
#if defined(USE_SUNRISE)
    globalConfig().sunrise_agent_mode = true;
#endif
```

- [ ] **Step 6: Build and verify**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build && cmake --build . --target mooncake_store mooncake_client 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 7: Commit**

```bash
git add mooncake-store/src/dummy_client.cpp mooncake-store/src/real_client_main.cpp
git commit -m "feat(store): add dummy client Sunrise integration for multi-process C2C"
```

---

## Task 8: Transport Selector same_machine Constraint

**Rationale:** `tangIpcOpenMemHandle` is machine-local. The transport selector should not route cross-node transfers to sunrise_link. This prevents silent failures when a cross-node transfer is incorrectly routed to sunrise_link.

**Files:**
- Modify: `mooncake-transfer-engine/tent/src/transport/sunrise_link/sunrise_link_transport.cpp`
- Modify: `mooncake-transfer-engine/tent/src/runtime/transport_selector.cpp`

- [ ] **Step 1: Add `same_machine_only` cap to SunriseLinkTransport constructor**

In `sunrise_link_transport.cpp`, in the constructor (line 251-258), add:

```cpp
SunriseLinkTransport::SunriseLinkTransport() : installed_(false) {
    caps.dram_to_dram = true;
    caps.dram_to_gpu = true;
    caps.gpu_to_dram = true;
    caps.gpu_to_gpu = true;
    caps.same_machine_only = true;
}
```

- [ ] **Step 2: Check `same_machine_only` in `isTransportAvailable()`**

In `transport_selector.cpp`, in `isTransportAvailable()` (line 319), add `SUNRISE_LINK` to the same_machine check:

```cpp
if ((type == NVLINK || type == SHM || type == SUNRISE_LINK) &&
    !context.same_machine) {
    return false;
}
```

- [ ] **Step 3: Build and verify**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build && cmake --build . --target tent_xport_sunrise_link tent_runtime 2>&1 | tail -20
```
Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add mooncake-transfer-engine/tent/src/transport/sunrise_link/sunrise_link_transport.cpp mooncake-transfer-engine/tent/src/runtime/transport_selector.cpp
git commit -m "fix(sunrise_link): add same_machine_only constraint to prevent cross-node routing"
```

---

## Task 9: CMake Integration Verification

**Rationale:** Verify that the CMake configuration already has the correct Sunrise dependencies and that the new files compile correctly.

**Files:**
- Modify: `mooncake-store/src/CMakeLists.txt` (if needed)

- [ ] **Step 1: Verify existing CMake configuration**

The `USE_SUNRISE` block already exists in `mooncake-store/src/CMakeLists.txt` (lines 353-364) with:
- `USE_SUNRISE` compile definitions for both targets
- Include directories for `/usr/local/tangrt/include`
- Link against `libtangrt_shared.so`, `libptml_shared.so`, and `dl`

This should be sufficient. Verify the `sunrise_allocator.h` include path is accessible. The header is in `mooncake-transfer-engine/include/` which should already be in the include path.

If the include path is not set, add to the `USE_SUNRISE` block:

```cmake
if(USE_SUNRISE)
    target_compile_definitions(mooncake_store PRIVATE USE_SUNRISE)
    target_compile_definitions(mooncake_client PRIVATE USE_SUNRISE)
    target_include_directories(mooncake_store PRIVATE /usr/local/tangrt/include)
    target_include_directories(mooncake_client PRIVATE /usr/local/tangrt/include)
    target_link_libraries(mooncake_store PRIVATE
        /usr/local/tangrt/lib/libtangrt_shared.so
        /usr/local/tangrt/lib/libptml_shared.so dl)
    target_link_libraries(mooncake_client PRIVATE
        /usr/local/tangrt/lib/libtangrt_shared.so
        /usr/local/tangrt/lib/libptml_shared.so dl)
endif()
```

- [ ] **Step 2: Full build test**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build && cmake --build . -j$(nproc) 2>&1 | tail -30
```
Expected: Full build succeeds with no errors.

- [ ] **Step 3: Commit (if CMakeLists changed)**

```bash
git add mooncake-store/src/CMakeLists.txt
git commit -m "build: verify/update Sunrise CMake integration"
```

---

## Task 10: Integration Testing

**Rationale:** Verify the full multi-process C2C path works end-to-end. Test the dummy-real architecture with sunrise_link, agent mode lifecycle, and existing pybind tests.

**Files:**
- Modify: `mooncake-store/tests/pybind_client_test.cpp`

- [ ] **Step 1: Verify existing SunriseLink pybind tests still pass**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build
LD_LIBRARY_PATH=/usr/local/tangrt/lib ./mooncake-store/tests/pybind_client_test --gtest_filter="*SunriseLink*"
```
Expected: All existing SunriseLink tests pass.

- [ ] **Step 2: Add multi-process C2C test**

In `mooncake-store/tests/pybind_client_test.cpp`, add a fork-based multi-process test:

```cpp
#if defined(USE_SUNRISE)
TEST_F(RealClientTest, SunriseLinkMultiProcessC2C) {
    const std::string key = "sunrise_mp_key";
    const std::string value = "sunrise_mp_value";

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        close(pipefd[0]);
        auto client = std::make_unique<Client>();
        auto setup_result = client->setup_internal(
            "server_host", "tcp://127.0.0.1:50051",
            1 << 30, 1 << 24, "sunrise_link", "", master_address_);
        if (!setup_result) _exit(1);

        auto put_result = client->put(key, value);
        if (!put_result) _exit(2);

        char done = 'D';
        (void)write(pipefd[1], &done, 1);
        close(pipefd[1]);

        std::this_thread::sleep_for(std::chrono::seconds(2));
        _exit(0);
    }

    close(pipefd[1]);
    char done = 0;
    (void)read(pipefd[0], &done, 1);
    close(pipefd[0]);

    auto client = std::make_unique<Client>();
    auto setup_result = client->setup_internal(
        "client_host", "tcp://127.0.0.1:50051",
        1 << 30, 1 << 24, "sunrise_link", "", master_address_);
    ASSERT_TRUE(setup_result.has_value());

    auto get_result = client->get(key);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(*get_result, value);

    int wstatus = 0;
    (void)waitpid(pid, &wstatus, 0);
    ASSERT_TRUE(WIFEXITED(wstatus));
    ASSERT_EQ(WEXITSTATUS(wstatus), 0);
}
#endif
```

- [ ] **Step 3: Run all SunriseLink tests**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build
LD_LIBRARY_PATH=/usr/local/tangrt/lib ./mooncake-store/tests/pybind_client_test --gtest_filter="*SunriseLink*"
```
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add mooncake-store/tests/pybind_client_test.cpp
git commit -m "test(store): add multi-process C2C test for sunrise_link"
```

---

## Task 11: Final Verification

- [ ] **Step 1: Run full store test suite (non-Sunrise) to verify no regression**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build
LD_LIBRARY_PATH=/usr/local/tangrt/lib ./mooncake-store/tests/pybind_client_test
```
Expected: All 50+ tests pass, no regressions.

- [ ] **Step 2: Run TENT transport tests**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build
ctest --test-dir . --output-on-failure -R SunriseLink
```
Expected: All SunriseLink transport tests pass.

- [ ] **Step 3: Verify full build is clean**

Run:
```bash
cd /data/ruixiangma/Transferengine-optimization/build && cmake --build . -j$(nproc) 2>&1 | grep -E "(error|warning)" | head -20
```
Expected: No errors, minimal warnings.
