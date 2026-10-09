#include "signal_contracts/buffer_pool.h"
#include <cstring>
#include <limits>
namespace signal_contracts {
BufferHandle::BufferHandle(const BufferHandle &other) noexcept :pool_(other.pool_),index_(other.index_),size_(other.size_) {
    if(pool_)pool_->slots_[index_].references.fetch_add(1,std::memory_order_relaxed);
}
BufferHandle &BufferHandle::operator=(const BufferHandle &other) noexcept { BufferHandle copy(other);swap(copy);return *this; }
void BufferHandle::reset() noexcept {
    if(pool_)pool_->slots_[index_].references.fetch_sub(1,std::memory_order_release);
    pool_=nullptr; size_=0;
}
std::span<std::byte> BufferHandle::bytes() noexcept {
    return pool_?pool_->storage_.subspan(index_*pool_->block_bytes_,size_):std::span<std::byte>{};
}
std::span<const std::byte> BufferHandle::bytes() const noexcept {
    return pool_?pool_->storage_.subspan(index_*pool_->block_bytes_,size_):std::span<const std::byte>{};
}
Status BufferPool::acquire(std::size_t bytes, BufferHandle &out) noexcept {
    if(!block_bytes_ || slots_.size()>storage_.size()/block_bytes_)return Status::invalid_argument;
    if(bytes>block_bytes_)return Status::buffer_too_small;
    for(std::size_t i=0;i<slots_.size();++i) {
        std::size_t zero=0;
        if(slots_[i].references.compare_exchange_strong(zero,1,std::memory_order_acquire)) {
            BufferHandle fresh;fresh.pool_=this;fresh.index_=i;fresh.size_=bytes;out=std::move(fresh);return Status::ok;
        }
    }
    return Status::exhausted;
}
std::size_t BufferPool::in_use() const noexcept {
    std::size_t used=0;for(auto &s:slots_) used+=s.references.load(std::memory_order_relaxed)!=0;return used;
}
Status retain(const FrameView &f, BufferPool &pool, RetainedFrame &out) noexcept {
    auto status=validate(f); if(status!=Status::ok && status!=Status::unavailable)return status;
    const HistoryView history=f.type==FrameType::History && f.metadata.available?f.history:HistoryView{};
    std::size_t count=0;
    auto add=[&](std::size_t n){if(n>(std::numeric_limits<std::size_t>::max()-count))return false;count+=n;return true;};
    if(!add(f.left.size()) || !add(f.right.size()) || !add(f.values.size()) ||
       (history.bands && history.frames>std::numeric_limits<std::size_t>::max()/history.bands) ||
       !add(history.frames*history.bands) || f.packet.size()>std::numeric_limits<std::size_t>::max()-f.metadata.stream_id.size() ||
       count>(std::numeric_limits<std::size_t>::max()-f.metadata.stream_id.size()-f.packet.size())/sizeof(float))return Status::overflow;
    RetainedFrame x;status=pool.acquire(count*sizeof(float)+f.metadata.stream_id.size()+f.packet.size(),x.buffer);
    if(status!=Status::ok)return status;
    auto bytes=x.buffer.bytes();
    if(count && reinterpret_cast<std::uintptr_t>(bytes.data())%alignof(float))return Status::invalid_argument;
    x.view=f;std::size_t offset=0;
    auto copy=[&](std::span<const float> source){
        auto *ptr=reinterpret_cast<float *>(bytes.data()+offset); if(!source.empty())std::memcpy(ptr,source.data(),source.size_bytes());
        offset+=source.size_bytes();return std::span<const float>(ptr,source.size());};
    x.view.left=copy(f.left);x.view.right=copy(f.right);x.view.values=copy(f.values);
    x.view.history={};
    if(history.frames) {
        const auto start=offset;for(std::size_t i=0;i<history.frames;++i)copy(history.row(i));
        x.view.history={std::span<const float>(reinterpret_cast<const float *>(bytes.data()+start),history.frames*history.bands),
            history.bands,history.frames,history.frames,0};
    }
    // Retained history stores logical rows contiguously; capacity remains semantic metadata.
    if(!f.packet.empty())std::memcpy(bytes.data()+offset,f.packet.data(),f.packet.size());
    x.view.packet={reinterpret_cast<const std::uint8_t *>(bytes.data()+offset),f.packet.size()};offset+=f.packet.size();
    if(!f.metadata.stream_id.empty())std::memcpy(bytes.data()+offset,f.metadata.stream_id.data(),f.metadata.stream_id.size());
    x.view.metadata.stream_id={reinterpret_cast<const char *>(bytes.data()+offset),f.metadata.stream_id.size()};
    out=std::move(x);return Status::ok;
}
}
