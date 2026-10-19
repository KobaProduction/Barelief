#include "signal_contracts/buffer_pool.h"
#include <cstring>
#include <limits>
namespace signal_contracts {
namespace {
bool add_count(std::size_t& count,std::size_t amount) noexcept {
    if(amount>std::numeric_limits<std::size_t>::max()-count)return false;
    count+=amount;return true;
}
Status retained_size(const FrameView& frame,const HistoryView& history,std::size_t& count,std::size_t& bytes) noexcept {
    const auto maximum=std::numeric_limits<std::size_t>::max();
    if(!add_count(count,frame.left.size()) || !add_count(count,frame.right.size()) || !add_count(count,frame.values.size()))return Status::overflow;
    if(history.bands && history.frames>maximum/history.bands)return Status::overflow;
    if(!add_count(count,history.frames*history.bands) || frame.packet.size()>maximum-frame.metadata.stream_id.size())return Status::overflow;
    const auto tail=frame.metadata.stream_id.size()+frame.packet.size();
    if(count>(maximum-tail)/sizeof(float))return Status::overflow;
    bytes=count*sizeof(float)+tail;return Status::ok;
}
struct RetainedWriter {
    std::span<std::byte> bytes;
    std::size_t offset=0;
    std::span<const float> floats(std::span<const float> source) noexcept {
        auto* result=reinterpret_cast<float*>(bytes.data()+offset);
        if(!source.empty())std::memcpy(result,source.data(),source.size_bytes());
        offset+=source.size_bytes();return {result,source.size()};
    }
    HistoryView history(const HistoryView& source) noexcept {
        if(!source.frames)return {};
        const auto start=offset;
        for(std::size_t row=0;row<source.frames;++row)floats(source.row(row));
        return {std::span<const float>(reinterpret_cast<const float*>(bytes.data()+start),source.frames*source.bands),source.bands,source.frames,source.frames,0};
    }
    void tail(const FrameView& source,FrameView& destination) noexcept {
        if(!source.packet.empty())std::memcpy(bytes.data()+offset,source.packet.data(),source.packet.size());
        destination.packet={reinterpret_cast<const std::uint8_t*>(bytes.data()+offset),source.packet.size()};offset+=source.packet.size();
        if(!source.metadata.stream_id.empty())std::memcpy(bytes.data()+offset,source.metadata.stream_id.data(),source.metadata.stream_id.size());
        destination.metadata.stream_id={reinterpret_cast<const char*>(bytes.data()+offset),source.metadata.stream_id.size()};
    }
};
Status copy_retained_payload(const FrameView& source,const HistoryView& history,std::size_t count,RetainedFrame& destination) noexcept {
    auto bytes=destination.buffer.bytes();
    if(count && reinterpret_cast<std::uintptr_t>(bytes.data())%alignof(float))return Status::invalid_argument;
    destination.view=source;RetainedWriter writer{bytes};
    destination.view.left=writer.floats(source.left);
    destination.view.right=writer.floats(source.right);
    destination.view.values=writer.floats(source.values);
    // Retained history stores logical rows contiguously; capacity remains semantic metadata.
    destination.view.history=writer.history(history);writer.tail(source,destination.view);return Status::ok;
}
struct RetentionPlan {HistoryView history;std::size_t samples=0,bytes=0;};
Status plan_retention(const FrameView& frame,RetentionPlan& plan) noexcept {
    const auto status=validate(frame);if(status!=Status::ok && status!=Status::unavailable)return status;
    plan.history=frame.type==FrameType::History && frame.metadata.available?frame.history:HistoryView{};
    return retained_size(frame,plan.history,plan.samples,plan.bytes);
}
}
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
    RetentionPlan plan;auto status=plan_retention(f,plan);if(status!=Status::ok)return status;
    RetainedFrame x;status=pool.acquire(plan.bytes,x.buffer);
    if(status!=Status::ok)return status;
    status=copy_retained_payload(f,plan.history,plan.samples,x);if(status!=Status::ok)return status;
    out=std::move(x);return Status::ok;
}
}
