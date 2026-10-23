#pragma once
#include <cstdlib>
#include <pthread.h>

// Paired with Tracy's pthread TLS fallback on MinGW. Its heap-owned thread
// data survives the CRT's emulated-TLS teardown. The CRT allocator also keeps
// allocations valid until the pthread key destructor finishes.
#define TRACY_HAS_CUSTOM_ALLOCATOR
namespace tracy {
inline void* PlatformMalloc(size_t size) { return std::malloc(size); }
inline void PlatformFree(void* ptr) { std::free(ptr); }
inline void* PlatformRealloc(void* ptr, size_t size) { return std::realloc(ptr, size); }
inline void PlatformAllocatorInit() {}
inline void PlatformAllocatorThreadInit() {}
inline void PlatformAllocatorFinalize() {}
inline void PlatformAllocatorThreadFinalize() {}
}
