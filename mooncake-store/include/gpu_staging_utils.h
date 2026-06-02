#pragma once

#include "cuda_alike.h"

#if defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
#include <acl/acl_rt.h>
#endif

#if defined(USE_SUNRISE)
#include <tang_runtime_api.h>
#include <sys/mman.h>
#include <cerrno>
#endif

#include <cstddef>
#include <glog/logging.h>

namespace mooncake {
namespace gpu_staging {

#if defined(USE_SUNRISE)
inline tangError_t QueryPointerAttrsBestEffort(const void* ptr,
                                               tangPointerAttributes* attr,
                                               int preferred_dev = -1) {
    if (!ptr || !attr) return tangErrorInvalidValue;
    if (preferred_dev < 0) {
        attr->type = tangMemoryTypeUnregistered;
        return tangErrorInvalidValue;
    }
    int saved_device = -1;
    if (tangGetDevice(&saved_device) != tangSuccess) {
        saved_device = -1;
    }
    tangError_t ret = tangSetDevice(preferred_dev);
    if (ret != tangSuccess) {
        if (saved_device >= 0) tangSetDevice(saved_device);
        return ret;
    }
    ret = tangPointerGetAttributes(attr, const_cast<void*>(ptr));
    if (saved_device >= 0) tangSetDevice(saved_device);
    return ret;
}
#endif

// Detect whether ptr resides in accelerator device memory.
// If so, writes the device ID to *out_device_id for subsequent SetDevice.
inline bool IsDevicePointer(const void* ptr, int* out_device_id) {
#if defined(USE_CUDA) || defined(USE_MUSA) || defined(USE_MACA) || \
    defined(USE_HYGON) || defined(USE_COREX)
    cudaPointerAttributes attr{};
    if (cudaPointerGetAttributes(&attr, ptr) == cudaSuccess &&
        attr.type == cudaMemoryTypeDevice) {
        if (out_device_id) *out_device_id = attr.device;
        return true;
    }
#elif defined(USE_HIP)
    hipPointerAttribute_t attr{};
    if (hipPointerGetAttributes(&attr, ptr) == hipSuccess &&
        attr.type == hipMemoryTypeDevice) {
        if (out_device_id) *out_device_id = attr.device;
        return true;
    }
#elif defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
    aclrtPtrAttributes attr{};
    if (aclrtPointerGetAttributes(const_cast<void*>(ptr), &attr) ==
            ACL_SUCCESS &&
        attr.location.type == ACL_MEM_LOCATION_TYPE_DEVICE) {
        if (out_device_id) *out_device_id = static_cast<int>(attr.location.id);
        return true;
    }
#elif defined(USE_SUNRISE)
    {
        tangPointerAttributes attr = {};
        tangError_t ret = QueryPointerAttrsBestEffort(ptr, &attr, 0);
        if (ret == tangSuccess && attr.type == tangMemoryTypeDevice) {
            if (out_device_id) *out_device_id = attr.device;
            return true;
        }
#ifdef MADV_POPULATE_WRITE
        uintptr_t page = reinterpret_cast<uintptr_t>(ptr) & ~4095ULL;
        errno = 0;
        int mr = madvise(reinterpret_cast<void*>(page), 4096,
                         MADV_POPULATE_WRITE);
        if (mr != 0 && errno == ENOMEM) {
            if (out_device_id) *out_device_id = 0;
            return true;
        }
#endif
    }
#endif
    (void)ptr;
    (void)out_device_id;
    return false;
}

// Copy device memory to host. Caller must have called SetDevice first.
inline bool CopyDeviceToHost(void* dst, const void* src, size_t size) {
#if defined(USE_CUDA) || defined(USE_MUSA) || defined(USE_MACA) || \
    defined(USE_HYGON) || defined(USE_COREX)
    return cudaMemcpy(dst, src, size, cudaMemcpyDeviceToHost) == cudaSuccess;
#elif defined(USE_HIP)
    return hipMemcpy(dst, src, size, hipMemcpyDeviceToHost) == hipSuccess;
#elif defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
    return aclrtMemcpy(dst, size, src, size, ACL_MEMCPY_DEVICE_TO_HOST) ==
           ACL_SUCCESS;
#elif defined(USE_SUNRISE)
    return tangMemcpy(dst, src, size, tangMemcpyDeviceToHost) == tangSuccess;
#else
    (void)dst;
    (void)src;
    (void)size;
    return false;
#endif
}

// Auto-direction copy: runtime determines the transfer direction from pointer
// attributes (cudaMemcpyDefault). Works for H2H, H2D, D2H, and D2D.
// Caller must have called SetDevice first when device memory is involved.
inline bool CopyAuto(void* dst, const void* src, size_t size) {
#if defined(USE_CUDA) || defined(USE_MUSA) || defined(USE_MACA) || \
    defined(USE_HYGON) || defined(USE_COREX)
    return cudaMemcpy(dst, src, size, cudaMemcpyDefault) == cudaSuccess;
#elif defined(USE_HIP)
    return hipMemcpy(dst, src, size, hipMemcpyDefault) == hipSuccess;
#elif defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
    aclrtPtrAttributes src_attr{}, dst_attr{};
    bool src_dev = aclrtPointerGetAttributes(const_cast<void*>(src),
                                             &src_attr) == ACL_SUCCESS &&
                   src_attr.location.type == ACL_MEM_LOCATION_TYPE_DEVICE;
    bool dst_dev = aclrtPointerGetAttributes(dst, &dst_attr) == ACL_SUCCESS &&
                   dst_attr.location.type == ACL_MEM_LOCATION_TYPE_DEVICE;
    aclrtMemcpyKind kind = ACL_MEMCPY_HOST_TO_HOST;
    if (src_dev && dst_dev)
        kind = ACL_MEMCPY_DEVICE_TO_DEVICE;
    else if (src_dev)
        kind = ACL_MEMCPY_DEVICE_TO_HOST;
    else if (dst_dev)
        kind = ACL_MEMCPY_HOST_TO_DEVICE;
    return aclrtMemcpy(dst, size, src, size, kind) == ACL_SUCCESS;
#elif defined(USE_SUNRISE)
    tangPointerAttributes src_attr = {}, dst_attr = {};
    tangError_t src_ret = QueryPointerAttrsBestEffort(
        const_cast<void*>(src), &src_attr, 0);
    tangError_t dst_ret = QueryPointerAttrsBestEffort(dst, &dst_attr, 0);
    bool src_dev = (src_ret == tangSuccess &&
                    src_attr.type == tangMemoryTypeDevice);
    bool dst_dev = (dst_ret == tangSuccess &&
                    dst_attr.type == tangMemoryTypeDevice);
    if (src_ret != tangSuccess || dst_ret != tangSuccess) {
#ifdef MADV_POPULATE_WRITE
        if (!src_dev) {
            uintptr_t src_page = reinterpret_cast<uintptr_t>(src) & ~4095ULL;
            errno = 0;
            src_dev = madvise(reinterpret_cast<void*>(src_page), 4096,
                              MADV_POPULATE_WRITE) != 0 &&
                     errno == ENOMEM;
        }
        if (!dst_dev) {
            uintptr_t dst_page = reinterpret_cast<uintptr_t>(dst) & ~4095ULL;
            errno = 0;
            dst_dev = madvise(reinterpret_cast<void*>(dst_page), 4096,
                              MADV_POPULATE_WRITE) != 0 &&
                     errno == ENOMEM;
        }
#else
        (void)src_dev;
        (void)dst_dev;
#endif
    }
    if (!src_dev && !dst_dev) {
        std::memcpy(dst, src, size);
        return true;
    }
    tangMemcpyKind kind = tangMemcpyHostToHost;
    if (src_dev && dst_dev)
        kind = tangMemcpyDeviceToDevice;
    else if (src_dev)
        kind = tangMemcpyDeviceToHost;
    else if (dst_dev)
        kind = tangMemcpyHostToDevice;
    return tangMemcpy(dst, src, size, kind) == tangSuccess;
#else
    (void)dst;
    (void)src;
    (void)size;
    return false;
#endif
}

// Bind the calling thread to the given device context.
inline void SetDevice(int device_id) {
    if (device_id < 0) return;
#if defined(USE_CUDA) || defined(USE_MUSA) || defined(USE_MACA) || \
    defined(USE_HYGON) || defined(USE_COREX)
    cudaSetDevice(device_id);
#elif defined(USE_HIP)
    hipSetDevice(device_id);
#elif defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
    aclrtSetDevice(device_id);
#elif defined(USE_SUNRISE)
    tangSetDevice(device_id);
#endif
}

// Copy host memory to device. Caller must have called SetDevice first.
inline bool CopyHostToDevice(void* dst, const void* src, size_t size) {
#if defined(USE_CUDA) || defined(USE_MUSA) || defined(USE_MACA) || \
    defined(USE_HYGON) || defined(USE_COREX)
    return cudaMemcpy(dst, src, size, cudaMemcpyHostToDevice) == cudaSuccess;
#elif defined(USE_HIP)
    return hipMemcpy(dst, src, size, hipMemcpyHostToDevice) == hipSuccess;
#elif defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
    return aclrtMemcpy(dst, size, src, size, ACL_MEMCPY_HOST_TO_DEVICE) ==
           ACL_SUCCESS;
#elif defined(USE_SUNRISE)
    return tangMemcpy(dst, src, size, tangMemcpyHostToDevice) == tangSuccess;
#else
    (void)dst;
    (void)src;
    (void)size;
    return false;
#endif
}

// Detect whether ptr resides in host (CPU) memory.
// Used together with IsDevicePointer for safe pointer-type dispatching:
//   if IsDevicePointer  -> CopyHostToDevice / CopyDeviceToHost
//   else if IsHostPointer -> memcpy
//   else                  -> reject (unknown type, e.g. non-standard allocator)
//
// Pageable host memory (not tracked by CUDA runtime) is treated as host.
inline bool IsHostPointer(const void* ptr) {
#if defined(USE_CUDA) || defined(USE_MUSA) || defined(USE_MACA) || \
    defined(USE_HYGON) || defined(USE_COREX)
    cudaPointerAttributes attr{};
    if (cudaPointerGetAttributes(&attr, ptr) != cudaSuccess) {
        // Query failed: pageable host memory not tracked by the runtime.
        cudaGetLastError();  // clear sticky error
        return true;
    }
    return attr.type != cudaMemoryTypeDevice;
#elif defined(USE_HIP)
    hipPointerAttribute_t attr{};
    if (hipPointerGetAttributes(&attr, ptr) != hipSuccess) {
        hipGetLastError();  // clear sticky error
        return true;
    }
    return attr.type != hipMemoryTypeDevice;
#elif defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
    aclrtPtrAttributes attr{};
    if (aclrtPointerGetAttributes(const_cast<void*>(ptr), &attr) !=
        ACL_SUCCESS) {
        // Query failed: likely pageable host memory not tracked by the runtime.
        return true;
    }
    return attr.location.type != ACL_MEM_LOCATION_TYPE_DEVICE;
#elif defined(USE_SUNRISE)
    {
        tangPointerAttributes attr = {};
        tangError_t ret = QueryPointerAttrsBestEffort(ptr, &attr, 0);
        if (ret == tangSuccess) {
            return attr.type != tangMemoryTypeDevice;
        }
    }
#ifdef MADV_POPULATE_WRITE
    {
        uintptr_t page = reinterpret_cast<uintptr_t>(ptr) & ~4095ULL;
        errno = 0;
        if (madvise(reinterpret_cast<void*>(page), 4096,
                    MADV_POPULATE_WRITE) != 0 &&
            errno == ENOMEM) {
            return false;
        }
    }
#endif
    return true;
#else
    (void)ptr;
    return true;  // CPU-only build: all pointers are host
#endif
}
}  // namespace gpu_staging
}  // namespace mooncake
