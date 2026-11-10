// SPDX-License-Identifier: AGPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <new>
#include <cstddef>
#include <cstdint>
#include <limits>
#include "test_aligned_allocation.h"

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
    if (auto* pointer = test_allocation::allocate_aligned(size, alignment)) return pointer;
    throw std::bad_alloc{};
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
void aligned_sizes() {
    const auto before = allocations;
    auto* zero = ::operator new(0, std::align_val_t{64}, std::nothrow);
    auto* odd = ::operator new[](65, std::align_val_t{64}, std::nothrow);
    check(zero && odd && allocations == before + 2);
    check(reinterpret_cast<std::uintptr_t>(zero) % 64 == 0);
    check(reinterpret_cast<std::uintptr_t>(odd) % 64 == 0);
    static_cast<std::byte*>(odd)[64] = std::byte{0x5a};
    check(static_cast<std::byte*>(odd)[64] == std::byte{0x5a});
    ::operator delete(zero, std::align_val_t{64});
    ::operator delete[](odd, std::align_val_t{64});
}
void aligned_overflow() {
    const auto before = allocations;
    check(!::operator new(std::numeric_limits<std::size_t>::max(), std::align_val_t{64}, std::nothrow));
    check(allocations == before + 1);
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
    test_allocation::deallocate_aligned(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
int main() {
    ordinary_and_array();
    aligned_and_array();
    aligned_sizes();
    aligned_overflow();
    allocation_failure();
    std::puts("allocation replacement families PASS");
}
