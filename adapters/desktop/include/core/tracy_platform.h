#pragma once
#include <cstdlib>

// Paired with Tracy's pthread TLS fallback on MinGW. Its heap-owned thread
// data survives the CRT's emulated-TLS teardown. The CRT allocator also keeps
// allocations valid until the pthread key destructor finishes.
#define TRACY_HAS_CUSTOM_ALLOCATOR
namespace tracy {
void* PlatformMalloc(size_t size);
void PlatformFree(void* ptr);
void* PlatformRealloc(void* ptr, size_t size);
void PlatformAllocatorInit();
void PlatformAllocatorThreadInit();
void PlatformAllocatorFinalize();
void PlatformAllocatorThreadFinalize();
}
