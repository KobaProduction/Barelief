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
namespace {
struct ContractStorageShape {
    std::size_t bytes=0,channel_bytes=0,text_bytes=0,channel_count=0,first=0,second=0;
};
Status measure_contract_storage(const DataContract& source,std::size_t current,std::size_t maximum,ContractStorageShape& shape) {
    if(const auto* c=std::get_if<NumericContract>(&source)){shape.channel_count=c->channel_order.size();shape.first=c->clock.domain.size();}
    else{const auto& s=std::get<StructuredContract>(source);shape.first=s.schema.size();shape.second=s.encoding.size();}
    shape.text_bytes=shape.first;
    if(!add(shape.text_bytes,shape.second) || !mul(shape.channel_count,sizeof(std::uint32_t),shape.channel_bytes) ||
        !add(shape.bytes,shape.channel_bytes) || !add(shape.bytes,shape.text_bytes))return Status::overflow;
    auto peak=shape.bytes;
    if(!add(peak,current))return Status::overflow;
    return peak>maximum?Status::buffer_too_small:Status::ok;
}
template<class Storage> Status allocate_contract_storage(Storage& storage,const ContractStorageShape& shape) {
    if(shape.channel_count){storage.channels.reset(new(std::nothrow) std::uint32_t[shape.channel_count]);if(!storage.channels)return Status::exhausted;}
    if(shape.text_bytes){storage.text.reset(new(std::nothrow) char[shape.text_bytes]);if(!storage.text)return Status::exhausted;}
    storage.bytes=shape.bytes;storage.channel_count=shape.channel_count;
    storage.first_text=shape.first;storage.second_text=shape.second;
    return Status::ok;
}
template<class Storage> void copy_structured_storage(const StructuredContract& s,Storage& storage,const ContractStorageShape& shape) {
        if(shape.first)std::memcpy(storage.text.get(),s.schema.data(),shape.first);
        if(shape.second)std::memcpy(storage.text.get()+shape.first,s.encoding.data(),shape.second);
        auto& target=std::get<StructuredContract>(storage.value);target.schema={};target.encoding={};
}
Status checked_storage_shape(const DataContract& source,const FrameLimits& limits,std::size_t current,std::size_t maximum,ContractStorageShape& shape) {
    const auto status=validate_contract(source,limits);if(status!=Status::ok)return status;
    return measure_contract_storage(source,current,maximum,shape);
}
template<class Storage> void copy_contract_storage(const DataContract& source,Storage& storage,const ContractStorageShape& shape) {
    storage.value=source;
    if(const auto* c=std::get_if<NumericContract>(&source)) {
        std::copy(c->channel_order.begin(),c->channel_order.end(),storage.channels.get());
        if(shape.first)std::memcpy(storage.text.get(),c->clock.domain.data(),shape.first);
        auto& target=std::get<NumericContract>(storage.value);target.channel_order={};target.clock.domain={};
    }else{
        copy_structured_storage(std::get<StructuredContract>(source),storage,shape);
    }
}
template<class Storage> Status prepare_owned_storage(const DataContract& source,const ContractStorageShape& shape,std::unique_ptr<Storage>& storage) {
    storage.reset(new(std::nothrow) Storage);
    if(!storage)return Status::exhausted;
    const auto status=allocate_contract_storage(*storage,shape);if(status!=Status::ok)return status;
    copy_contract_storage(source,*storage,shape);return Status::ok;
}
}
Status own_processing_contract(const DataContract& source,const FrameLimits& limits,std::size_t maximum,OwnedProcessingContract& out) noexcept {
    ContractStorageShape shape;shape.bytes=sizeof(OwnedProcessingContract::Storage);
    const auto measured=checked_storage_shape(source,limits,out.owned_bytes(),maximum,shape);
    if(measured!=Status::ok)return measured;
    std::unique_ptr<OwnedProcessingContract::Storage> storage;
    const auto allocated=prepare_owned_storage(source,shape,storage);if(allocated!=Status::ok)return allocated;
    OwnedProcessingContract candidate;candidate.storage_=storage.release();out=std::move(candidate);return Status::ok;
}

}
