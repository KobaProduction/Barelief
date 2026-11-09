// SPDX-License-Identifier: AGPL-3.0-or-later
#include <new>

// Complete the test-local counting replacements. Forwarding through their
// throwing overloads keeps allocation/deallocation families visible to ASan.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); }
    catch (const std::bad_alloc&) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new[](size); }
    catch (const std::bad_alloc&) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    ::operator delete(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    ::operator delete[](pointer);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new(size, alignment); }
    catch (const std::bad_alloc&) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new[](size, alignment); }
    catch (const std::bad_alloc&) { return nullptr; }
}
void operator delete(void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete[](pointer, alignment);
}
