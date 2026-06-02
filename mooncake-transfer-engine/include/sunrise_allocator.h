#pragma once

#include <cstddef>
#include <cstdlib>
#include <glog/logging.h>

#if defined(USE_SUNRISE)
#include <tang_runtime_api.h>
#include <mutex>
#include <unordered_set>
#endif

#if defined(USE_SUNRISE)
namespace mooncake {
namespace sunrise_alloc_detail {
inline std::unordered_set<void*>& tangAllocatedSet() {
    static std::unordered_set<void*> s;
    return s;
}
inline std::mutex& tangAllocMutex() {
    static std::mutex m;
    return m;
}
}  // namespace sunrise_alloc_detail
}  // namespace mooncake
#endif

inline void* sunrise_allocate_memory(size_t total_size) {
#if defined(USE_SUNRISE)
    void* ptr = nullptr;
    tangError_t ret = tangHostAlloc(&ptr, total_size, 0);
    if (ret == tangSuccess && ptr) {
        {
            std::lock_guard<std::mutex> guard(
                mooncake::sunrise_alloc_detail::tangAllocMutex());
            mooncake::sunrise_alloc_detail::tangAllocatedSet().insert(ptr);
        }
        return ptr;
    }
    LOG(WARNING) << "tangHostAlloc failed (" << ret
                 << "), falling back to posix_memalign for size="
                 << total_size;
#endif
    void* fallback = nullptr;
    if (posix_memalign(&fallback, 64, total_size) != 0) return nullptr;
    return fallback;
}

inline void sunrise_free_memory(void* ptr) {
    if (!ptr) return;
#if defined(USE_SUNRISE)
    {
        std::lock_guard<std::mutex> guard(
            mooncake::sunrise_alloc_detail::tangAllocMutex());
        if (mooncake::sunrise_alloc_detail::tangAllocatedSet().count(ptr)) {
            mooncake::sunrise_alloc_detail::tangAllocatedSet().erase(ptr);
            tangError_t ret = tangFreeHost(ptr);
            if (ret != tangSuccess) {
                LOG(ERROR) << "tangFreeHost failed for tracked pointer: " << ret;
            }
            return;
        }
    }
#endif
    free(ptr);
}
