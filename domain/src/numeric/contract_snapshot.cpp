#include <domain/numeric/contract_snapshot.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>
namespace domain {
using namespace signal_contracts;
namespace {
bool add(std::size_t& a,std::size_t b) noexcept {
    if(b>std::numeric_limits<std::size_t>::max()-a)return false;
    a+=b;return true;
}
bool mul(std::size_t a,std::size_t b,std::size_t& out) noexcept {
    if(a && b>std::numeric_limits<std::size_t>::max()/a)return false;
    out=a*b;return true;
}
}
struct OwnedProcessingContract::Storage {
    std::atomic<std::size_t> references{1};DataContract value;
    std::unique_ptr<std::uint32_t[]> channels;std::unique_ptr<char[]> text;
    std::size_t channel_count=0,first_text=0,second_text=0,bytes=0;
};
OwnedProcessingContract::OwnedProcessingContract(const OwnedProcessingContract& other) noexcept:storage_(other.storage_) {
    if(storage_)storage_->references.fetch_add(1,std::memory_order_relaxed);
}
OwnedProcessingContract& OwnedProcessingContract::operator=(const OwnedProcessingContract& other) noexcept {
    if(this!=&other){OwnedProcessingContract candidate(other);std::swap(storage_,candidate.storage_);}return *this;
}
OwnedProcessingContract::OwnedProcessingContract(OwnedProcessingContract&& other) noexcept:storage_(std::exchange(other.storage_,nullptr)) {}
OwnedProcessingContract& OwnedProcessingContract::operator=(OwnedProcessingContract&& other) noexcept {
    if(this!=&other){release();storage_=std::exchange(other.storage_,nullptr);}return *this;
}
OwnedProcessingContract::~OwnedProcessingContract(){release();}
void OwnedProcessingContract::release() noexcept {
    if(storage_ && storage_->references.fetch_sub(1,std::memory_order_acq_rel)==1)delete storage_;
    storage_=nullptr;
}
DataContract OwnedProcessingContract::view() const noexcept {
    if(!storage_)return NumericContract{};
    auto value=storage_->value;
    if(auto* c=std::get_if<NumericContract>(&value)) {
        c->channel_order={storage_->channels.get(),storage_->channel_count};c->clock.domain={storage_->text.get(),storage_->first_text};
    }else{
        auto& s=std::get<StructuredContract>(value);s.schema={storage_->text.get(),storage_->first_text};
        s.encoding={storage_->text.get()+storage_->first_text,storage_->second_text};
    }return value;
}
std::size_t OwnedProcessingContract::owned_bytes() const noexcept {return storage_?storage_->bytes:0;}
Status own_processing_contract(const DataContract& source,const FrameLimits& limits,std::size_t maximum,OwnedProcessingContract& out) noexcept {
    const auto status=validate_contract(source,limits);if(status!=Status::ok)return status;
    std::size_t bytes=sizeof(OwnedProcessingContract::Storage),channel_bytes=0,text_bytes=0,channel_count=0,first=0,second=0;
    if(const auto* c=std::get_if<NumericContract>(&source)){channel_count=c->channel_order.size();first=c->clock.domain.size();}
    else{const auto& s=std::get<StructuredContract>(source);first=s.schema.size();second=s.encoding.size();}
    text_bytes=first;
    if(!add(text_bytes,second) || !mul(channel_count,sizeof(std::uint32_t),channel_bytes) || !add(bytes,channel_bytes) || !add(bytes,text_bytes))return Status::overflow;
    std::size_t peak=bytes;if(!add(peak,out.owned_bytes()))return Status::overflow;
    if(peak>maximum)return Status::buffer_too_small;
    std::unique_ptr<OwnedProcessingContract::Storage> storage(new(std::nothrow) OwnedProcessingContract::Storage);
    if(!storage)return Status::exhausted;
    if(channel_count){storage->channels.reset(new(std::nothrow) std::uint32_t[channel_count]);if(!storage->channels)return Status::exhausted;}
    if(text_bytes){storage->text.reset(new(std::nothrow) char[text_bytes]);if(!storage->text)return Status::exhausted;}
    storage->value=source;storage->bytes=bytes;storage->channel_count=channel_count;storage->first_text=first;storage->second_text=second;
    if(const auto* c=std::get_if<NumericContract>(&source)) {
        std::copy(c->channel_order.begin(),c->channel_order.end(),storage->channels.get());
        if(first)std::memcpy(storage->text.get(),c->clock.domain.data(),first);
        auto& target=std::get<NumericContract>(storage->value);target.channel_order={};target.clock.domain={};
    }else{
        const auto& s=std::get<StructuredContract>(source);
        if(first)std::memcpy(storage->text.get(),s.schema.data(),first);
        if(second)std::memcpy(storage->text.get()+first,s.encoding.data(),second);
        auto& target=std::get<StructuredContract>(storage->value);target.schema={};target.encoding={};
    }
    OwnedProcessingContract candidate;candidate.storage_=storage.release();out=std::move(candidate);return Status::ok;
}

}
