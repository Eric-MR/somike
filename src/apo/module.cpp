#include "module.h"

#include <atomic>

namespace somike::apo {

namespace {
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};
}  // namespace

void ModuleAddRef() noexcept { g_objects.fetch_add(1, std::memory_order_relaxed); }
void ModuleRelease() noexcept { g_objects.fetch_sub(1, std::memory_order_relaxed); }

void ModuleLock(bool lock) noexcept {
    if (lock) {
        g_locks.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_locks.fetch_sub(1, std::memory_order_relaxed);
    }
}

bool ModuleCanUnload() noexcept {
    return g_objects.load(std::memory_order_relaxed) == 0 &&
           g_locks.load(std::memory_order_relaxed) == 0;
}

}  // namespace somike::apo
