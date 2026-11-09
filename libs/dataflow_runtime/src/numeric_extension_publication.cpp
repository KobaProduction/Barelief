#include "numeric_extension_state.h"
#include "frame_access.h"
#include <algorithm>

namespace dataflow_runtime::detail {
namespace {
bool bounded_actual(const NumericOperationOutput& target,const NumericContract& contract) noexcept {
    const auto dimensions=contract.shape.rank==Rank::Scalar?0:contract.shape.rank==Rank::Vector?1:2;
    for(std::size_t axis=0;axis<2;++axis)
        if(target.value.actual[axis]<contract.shape.extents[axis].minimum || target.value.actual[axis]>contract.shape.extents[axis].maximum || (axis>=dimensions && target.value.actual[axis]!=1))return false;
    return true;
}
bool owns_buffer(const NumericOperationOutput& target,const NumericBuffer& buffer,const NumericBufferContract& contract) noexcept {
    return target.value.contract==&contract && target.value.storage.data()==buffer.storage().data() &&
        target.value.storage.size()==buffer.storage().size() && target.value.planes.data()==buffer.planes.data() &&
        target.value.planes.size()==buffer.planes.size() && !target.value.ring.capacity && !target.value.ring.oldest && target.value.ring.silence_mask.empty();
}
Status bind_coordinates(MutableNumericView& view,const std::array<std::span<std::byte>,2>& storage,
                        const std::array<std::size_t,2>& counts) noexcept {
    for(std::size_t axis=0;axis<2;++axis){
        if(view.contract->coordinates[axis].mode!=CoordinateMode::ExplicitFloat64LE){if(counts[axis])return Status::incompatible_contract;continue;}
        if(counts[axis]!=view.actual[axis] || counts[axis]>storage[axis].size()/sizeof(double))return Status::incompatible_contract;
        view.coordinates.axes[axis]=storage[axis].first(counts[axis]*sizeof(double));
    }
    return Status::ok;
}
}
Status PreparedNumericExtension::Impl::validate_output_binding(std::size_t i,const State& state) noexcept {
    auto& target=outputs[i];
    if(target.port!=i || !owns_buffer(target,state.numeric_outputs[i],output_buffer_contracts[i]) ||
       !NumericOperationBindingAccess::valid_writer(target.coordinates,invocation,i))return Status::incompatible_contract;
    if(target.status!=Status::ok)target.value.actual={std::size_t(output_contracts[i].shape.extents[0].minimum),std::size_t(output_contracts[i].shape.extents[1].minimum)};
    if(!bounded_actual(target,output_contracts[i]))return Status::incompatible_contract;
    raw_outputs[i].actual=target.value.actual;raw_outputs[i].coordinates={};output_stamps[i]={};output_stamps[i].available=false;
    if(target.status!=Status::ok)return Status::ok;
    return NumericOperationBindingAccess::stamp(target.origin,output_contracts[i].clock,authority,output_stamps[i]);
}
Status PreparedNumericExtension::Impl::validate_output_frame(std::size_t i,const FrameLimits& limits) noexcept {
    if(outputs[i].status!=Status::ok)return Status::ok;
    auto status=bind_coordinates(raw_outputs[i],coordinate_storage[i],coordinate_counts[i]);if(status!=Status::ok)return status;
    status=validate_view({output_stamps[i],raw_outputs[i].view()},limits);if(status!=Status::ok)return status;
    const auto values=descriptor->policy.output_values.empty()?ValueDomain::FiniteOnly:descriptor->policy.output_values[i];
    return validate_values(raw_outputs[i].view(),values,limits);
}
Status PreparedNumericExtension::Impl::validate_outputs(const State& state,const FrameLimits& limits) noexcept {
    for(std::size_t i=0;i<outputs.size();++i){const auto status=validate_output_binding(i,state);if(status!=Status::ok)return status;}
    auto status=numeric_operation_route_coordinates(invocation,descriptor->policy);if(status!=Status::ok)return status;
    status=validate_output_frames(limits);if(status!=Status::ok)return status;
    return numeric_operation_metadata_values(invocation,descriptor->policy,PortDirection::Output);
}
Status PreparedNumericExtension::Impl::validate_output_frames(const FrameLimits& limits) noexcept {
    for(std::size_t i=0;i<outputs.size();++i){const auto status=validate_output_frame(i,limits);if(status!=Status::ok)return status;}
    return Status::ok;
}
void PreparedNumericExtension::Impl::rebind_output(std::size_t i,State& state) const noexcept {
    const auto& buffer=state.numeric_outputs[i];auto stamp=output_stamps[i];stamp.stream_id={output_streams[i].data(),output_stream_sizes[i]};
    NumericView view{&output_contracts[i],buffer.storage(),buffer.planes,buffer.actual,{},{}};
    for(std::size_t axis=0;axis<2;++axis)if(stamp.available && output_contracts[i].coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE)
        view.coordinates.axes[axis]={buffer.coordinates[axis].data(),buffer.actual[axis]*sizeof(double)};
    if (!stamp.available) { view.storage = {}; }
    state.numeric_frames[i] = {stamp, view};
}
void PreparedNumericExtension::Impl::rebind(State& state) const noexcept {
    if(state.numeric_frames.size()!=output_contracts.size() || state.numeric_outputs.size()!=output_contracts.size())return;
    for(std::size_t i=0;i<output_contracts.size();++i)rebind_output(i,state);
}
void PreparedNumericExtension::Impl::publish(State& state) noexcept {
    for(std::size_t i=0;i<outputs.size();++i){
        output_statuses[i]=outputs[i].status;output_stamps[i].available=outputs[i].status==Status::ok;
        output_stamps[i].stream_id={};publish_stream(i);
        state.numeric_outputs[i].actual=outputs[i].value.actual;
    }
    rebind(state);
}
void PreparedNumericExtension::Impl::publish_stream(std::size_t output) noexcept {
    output_stream_sizes[output]=0;
    if(outputs[output].status!=Status::ok || !descriptor->policy.stream_input)return;
    for(const auto& latch:latches){
        if(latch.rule.port!=*descriptor->policy.stream_input || !latch.ready)continue;
        std::copy_n(latch.stream.begin(),latch.stream_size,output_streams[output].begin());
        output_stream_sizes[output]=latch.stream_size;return;
    }
}
bool PreparedNumericExtension::Impl::copy_publication(const Impl& other) noexcept {
    if(output_streams.size()!=other.output_streams.size())return false;
    for(std::size_t i=0;i<output_streams.size();++i)if(output_streams[i].size()<other.output_stream_sizes[i])return false;
    for(std::size_t i=0;i<output_streams.size();++i){
        std::copy_n(other.output_streams[i].begin(),other.output_stream_sizes[i],output_streams[i].begin());
        output_stream_sizes[i]=other.output_stream_sizes[i];output_stamps[i]=other.output_stamps[i];output_statuses[i]=other.output_statuses[i];
    }
    return true;
}
void PreparedNumericExtension::Impl::invalidate(State& state,Status status) noexcept {
    std::fill(output_statuses.begin(),output_statuses.end(),status);
    for(auto& stamp:output_stamps){stamp.available=false;stamp.discontinuity=true;}
    clear_borrowed();rebind(state);
}
void PreparedNumericExtension::rebind_outputs(State& state) const noexcept {value_->rebind(state);}
} // namespace dataflow_runtime::detail
