#include "numeric_extension_state.h"
#include "preparation_support.h"
#include <algorithm>

namespace dataflow_runtime::detail {
namespace {
Status copy_row(const NumericView& source,MutableNumericView& destination,std::size_t plane,std::size_t row) noexcept {
    for(std::size_t column=0;column<source.actual[1];++column){
        std::uint64_t bits=0;auto status=load_bits(source,{plane,row,column},source.contract->dtype,bits);
        if(status!=Status::ok)return status;
        status=store_bits(destination,{plane,row,column},destination.contract->dtype,bits);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status copy_values(const NumericView& source,MutableNumericView& destination) noexcept {
    for(std::size_t plane=0;plane<source.planes.size();++plane)for(std::size_t row=0;row<source.actual[0];++row){
        const auto status=copy_row(source,destination,plane,row);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status validate_source(const NumericInputLatch& latch,const DataFrameView& frame,const FrameLimits& limits) noexcept {
    const auto* source=std::get_if<NumericView>(&frame.payload);
    if(!source || !source->contract || source->ring.capacity || !frame.stamp.available)return Status::unavailable;
    if(!same_physical_contract(latch.contract,*source->contract) || frame.stamp.stream_id.size()>latch.stream.size())return Status::incompatible_contract;
    const auto status=validate_view(frame,limits);if(status!=Status::ok)return status;
    return validate_values(*source,latch.rule.values,limits);
}
Status copy_coordinates(const NumericView& source,NumericBuffer& destination) noexcept {
    for(std::size_t axis=0;axis<2;++axis){
        const auto bytes=source.coordinates.axes[axis];
        if(bytes.size()>destination.coordinates[axis].size())return Status::incompatible_contract;
        std::copy(bytes.begin(),bytes.end(),destination.coordinates[axis].begin());
    }
    return Status::ok;
}
}
MutableNumericView NumericInputLatch::mutable_view() noexcept {
    MutableNumericView result{&contract,buffer.storage(),buffer.planes,buffer.actual,{},{}};
    for(std::size_t axis=0;axis<2;++axis)if(contract.coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE)
        result.coordinates.axes[axis]={buffer.coordinates[axis].data(),buffer.actual[axis]*sizeof(double)};
    return result;
}
DataFrameView NumericInputLatch::frame() const noexcept {
    NumericView view{&contract,buffer.storage(),buffer.planes,buffer.actual,{},{}};
    for(std::size_t axis=0;axis<2;++axis)if(contract.coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE)
        view.coordinates.axes[axis]={buffer.coordinates[axis].data(),buffer.actual[axis]*sizeof(double)};
    auto time=stamp;time.stream_id={stream.data(),stream_size};time.available=ready;
    if(!ready)view.storage={};
    return {time,view};
}
NumericOperationInput NumericInputLatch::input() const noexcept {
    NumericBufferView view{&storage_contract,buffer.storage(),buffer.planes,buffer.actual,{}};
    std::span<const float> contiguous;
    if(ready && contract.dtype==DType::Float32 && contract.representation.encoding==StorageEncoding::Native &&
       buffer.floats.size()>=buffer.actual[0]*buffer.actual[1] && buffer.planes.size()==1 && !buffer.planes[0].offset &&
       buffer.planes[0].stride_bytes[0]==buffer.actual[1]*sizeof(float) &&
       (contract.shape.rank!=Rank::Matrix || buffer.planes[0].stride_bytes[1]==sizeof(float)))
        contiguous={buffer.floats.data(),buffer.actual[0]*buffer.actual[1]};
    if(!ready)view.storage={};
    return {rule.port,view,{},ready?Status::ok:Status::unavailable,contiguous};
}
Status NumericInputLatch::retain(const DataFrameView& frame,const FrameLimits& limits) noexcept {
    const auto status=validate_source(*this,frame,limits);if(status!=Status::ok)return status;
    return store_frame(frame,std::get<NumericView>(frame.payload));
}
Status NumericInputLatch::store_frame(const DataFrameView& frame,const NumericView& source) noexcept {
    buffer.actual=source.actual;auto destination=mutable_view();
    auto status=copy_values(source,destination);if(status!=Status::ok)return status;
    status=copy_coordinates(source,buffer);if(status!=Status::ok)return status;
    std::copy(frame.stamp.stream_id.begin(),frame.stamp.stream_id.end(),stream.begin());stream_size=frame.stamp.stream_id.size();
    stamp=frame.stamp;stamp.stream_id={};ready=true;return Status::ok;
}
bool NumericInputLatch::copy_state_from(const NumericInputLatch& other) noexcept {
    if(!same_physical_contract(contract,other.contract) || buffer.storage().size()!=other.buffer.storage().size() || stream.size()!=other.stream.size())return false;
    std::copy(other.buffer.storage().begin(),other.buffer.storage().end(),buffer.storage().begin());
    for(std::size_t axis=0;axis<2;++axis){
        if(buffer.coordinates[axis].size()!=other.buffer.coordinates[axis].size())return false;
        std::copy(other.buffer.coordinates[axis].begin(),other.buffer.coordinates[axis].end(),buffer.coordinates[axis].begin());
    }
    std::copy(other.stream.begin(),other.stream.end(),stream.begin());stream_size=other.stream_size;
    buffer.actual=other.buffer.actual;stamp=other.stamp;stamp.stream_id={};ready=other.ready;return true;
}
} // namespace dataflow_runtime::detail
