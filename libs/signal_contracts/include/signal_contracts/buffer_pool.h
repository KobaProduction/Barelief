#pragma once
#include "frame.h"
#include <atomic>
#include <utility>

namespace signal_contracts {
struct BufferSlot { std::atomic<std::size_t> references{0}; };
class BufferPool;
// The pool, slots and storage must outlive every handle. A published buffer is immutable.
class BufferHandle {
public:
    BufferHandle() noexcept = default;
    BufferHandle(const BufferHandle &) noexcept;
    BufferHandle &operator=(const BufferHandle &) noexcept;
    BufferHandle(BufferHandle &&other) noexcept { swap(other); }
    BufferHandle &operator=(BufferHandle &&other) noexcept { if(this!=&other){reset(); swap(other);} return *this; }
    ~BufferHandle() { reset(); }
    void reset() noexcept;
    void swap(BufferHandle &other) noexcept {
        std::swap(pool_,other.pool_); std::swap(index_,other.index_); std::swap(size_,other.size_);
    }
    explicit operator bool() const noexcept { return pool_ != nullptr; }
    std::span<std::byte> bytes() noexcept;
    std::span<const std::byte> bytes() const noexcept;
private:
    friend class BufferPool;
    BufferPool *pool_ = nullptr;
    std::size_t index_ = 0, size_ = 0;
};
// No allocation: caller supplies aligned storage and initially unused slots.
// Acquire/release are thread safe; copying or writing the same handle needs synchronization.
class BufferPool {
public:
    BufferPool(std::span<std::byte> storage, std::span<BufferSlot> slot_storage, std::size_t block_bytes) noexcept
        : storage_(storage), slots_(slot_storage), block_bytes_(block_bytes) {}
    BufferPool(const BufferPool &) = delete;
    BufferPool &operator=(const BufferPool &) = delete;
    Status acquire(std::size_t bytes, BufferHandle &out) noexcept;
    std::size_t in_use() const noexcept;
private:
    friend class BufferHandle;
    std::span<std::byte> storage_;
    std::span<BufferSlot> slots_;
    std::size_t block_bytes_;
};
struct RetainedFrame {
    BufferHandle buffer;
    FrameView view;
};
// Copies samples AND stream_id into one pool block; out is unchanged on failure.
Status retain(const FrameView &, BufferPool &, RetainedFrame &) noexcept;
} // namespace signal_contracts
