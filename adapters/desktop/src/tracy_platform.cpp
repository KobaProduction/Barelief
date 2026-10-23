#include <core/tracy_platform.h>

namespace tracy {
void* PlatformMalloc(size_t size) { return std::malloc(size); }
void PlatformFree(void* ptr) { std::free(ptr); }
void* PlatformRealloc(void* ptr, size_t size) { return std::realloc(ptr, size); }
void PlatformAllocatorInit() {}
void PlatformAllocatorThreadInit() {}
void PlatformAllocatorFinalize() {}
void PlatformAllocatorThreadFinalize() {}
}
