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
void canonical_unavailable(NumericOperationOutput& target,const NumericContract& contract) noexcept {
    if(target.status==Status::ok && target.stamp.available)return;
    target.value.actual={std::size_t(contract.shape.extents[0].minimum),std::size_t(contract.shape.extents[1].minimum)};
    target.value.coordinates={};
}
bool owns_coordinates(const NumericOperationOutput& target) noexcept {
    for(std::size_t axis=0;axis<2;++axis){
        const auto view=target.value.coordinates.axes[axis];
        if(target.value.contract->coordinates[axis].mode!=CoordinateMode::ExplicitFloat64LE){if(!view.empty())return false;continue;}
        if(view.size()!=target.value.actual[axis]*sizeof(double) || view.size()>target.writable_coordinates[axis].size() || view.data()!=target.writable_coordinates[axis].data())return false;
    }
    return true;
}
Status valid_target(const NumericOperationOutput& target,const NumericBuffer& buffer,const NumericContract& contract,
                    ValueDomain values,std::size_t stream_capacity,const FrameLimits& limits) noexcept {
    if(target.value.contract!=&contract ||
       target.value.storage.data()!=buffer.storage().data() || target.value.storage.size()!=buffer.storage().size() ||
       target.value.planes.data()!=buffer.planes.data() || target.value.planes.size()!=buffer.planes.size() ||
       target.value.ring.capacity || target.stamp.stream_id.size()>stream_capacity || !bounded_actual(target,contract))return Status::incompatible_contract;
    for(std::size_t axis=0;axis<2;++axis)if(target.writable_coordinates[axis].data()!=buffer.coordinates[axis].data() || target.writable_coordinates[axis].size()!=buffer.coordinates[axis].size())return Status::incompatible_contract;
    if(target.status!=Status::ok || !target.stamp.available)return Status::ok;
    if(!owns_coordinates(target))return Status::incompatible_contract;
    const auto status=validate_view({target.stamp,target.value.view()},limits);if(status!=Status::ok)return status;
    return validate_values(target.value.view(),values,limits);
}
}
void PreparedNumericExtension::Impl::targets(const PlanNode& node,State& state) noexcept {
    for(std::size_t i=0;i<latches.size();++i)inputs[i]=latches[i].input();
    for(std::size_t i=0;i<outputs.size();++i){
        auto& target=outputs[i];target={};target.port=PortId(i);target.value=mutable_output(node,state,PortId(i));target.value.contract=&output_contracts[i];
        target.value.coordinates={};target.stamp.available=false;
        for(std::size_t axis=0;axis<2;++axis)target.writable_coordinates[axis]=state.numeric_outputs[i].coordinates[axis];
    }
}
Status PreparedNumericExtension::Impl::validate_outputs(const State& state,const FrameLimits& limits) noexcept {
    for(std::size_t i=0;i<outputs.size();++i){
        if(outputs[i].port!=i)return Status::incompatible_contract;
        canonical_unavailable(outputs[i],output_contracts[i]);
        const auto values=descriptor->policy.output_values.empty()?ValueDomain::FiniteOnly:descriptor->policy.output_values[i];
        const auto status=valid_target(outputs[i],state.numeric_outputs[i],output_contracts[i],values,output_streams[i].size(),limits);
        if(status!=Status::ok)return status;
    }
    return Status::ok;
}
void PreparedNumericExtension::Impl::rebind(State& state) const noexcept {
    if(state.numeric_frames.size()!=output_contracts.size() || state.numeric_outputs.size()!=output_contracts.size())return;
    for(std::size_t i=0;i<output_contracts.size();++i){
        const auto& buffer=state.numeric_outputs[i];auto stamp=output_stamps[i];stamp.stream_id={output_streams[i].data(),output_stream_sizes[i]};
        NumericView view{&output_contracts[i],buffer.storage(),buffer.planes,buffer.actual,{},{}};
        for(std::size_t axis=0;axis<2;++axis)if(stamp.available && output_contracts[i].coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE)
            view.coordinates.axes[axis]={buffer.coordinates[axis].data(),buffer.actual[axis]*sizeof(double)};
        if(!stamp.available)view.storage={};state.numeric_frames[i]={stamp,view};
    }
}
void PreparedNumericExtension::Impl::publish(State& state) noexcept {
    for(std::size_t i=0;i<outputs.size();++i){
        const auto& target=outputs[i];output_statuses[i]=target.status;output_stamps[i]=target.stamp;
        output_stamps[i].available=target.status==Status::ok && target.stamp.available;output_stamps[i].stream_id={};
        std::copy(target.stamp.stream_id.begin(),target.stamp.stream_id.end(),output_streams[i].begin());output_stream_sizes[i]=target.stamp.stream_id.size();
        state.numeric_outputs[i].actual=target.value.actual;
    }
    rebind(state);
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
