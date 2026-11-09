// SPDX-License-Identifier: AGPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
std::size_t allocations = 0;
bool fail_allocation = false;
void check(bool value) {
    if (!value) { std::fputs("allocation replacement family failed\n", stderr); std::abort(); }
}
void* allocate(std::size_t size) {
    ++allocations;
    if (fail_allocation) { throw std::bad_alloc{}; }
    if (auto* pointer = std::malloc(size ? size : 1)) { return pointer; }
    throw std::bad_alloc{};
}
void* allocate_aligned(std::size_t size, std::size_t alignment) {
    ++allocations;
    if (fail_allocation) { throw std::bad_alloc{}; }
#ifdef _WIN32
    auto* pointer = _aligned_malloc(size, alignment);
#else
    auto* pointer = std::aligned_alloc(alignment, size);
#endif
    if (!pointer) { throw std::bad_alloc{}; }
    return pointer;
}
void ordinary_and_array() {
    const auto before = allocations;
    auto* scalar = ::operator new(32, std::nothrow);
    auto* array = ::operator new[](32, std::nothrow);
    check(scalar && array && allocations == before + 2);
    ::operator delete(scalar);
    ::operator delete[](array);
}
void aligned_and_array() {
    const auto before = allocations;
    auto* scalar = ::operator new(64, std::align_val_t{64}, std::nothrow);
    auto* array = ::operator new[](64, std::align_val_t{64}, std::nothrow);
    check(scalar && array && allocations == before + 2);
    ::operator delete(scalar, std::align_val_t{64});
    ::operator delete[](array, std::align_val_t{64});
}
void allocation_failure() {
    fail_allocation = true;
    const auto before = allocations;
    check(!::operator new(32, std::nothrow));
    check(!::operator new[](32, std::nothrow));
    check(!::operator new(64, std::align_val_t{64}, std::nothrow));
    check(!::operator new[](64, std::align_val_t{64}, std::nothrow));
    fail_allocation = false;
    check(allocations == before + 4);
}
}
void* operator new(std::size_t size) { return allocate(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, std::size_t(alignment));
}
void operator delete(void* pointer, std::align_val_t) noexcept {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
int main() {
    ordinary_and_array();
    aligned_and_array();
    allocation_failure();
    std::puts("allocation replacement families PASS");
}
