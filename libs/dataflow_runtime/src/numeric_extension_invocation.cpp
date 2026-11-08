#include "numeric_extension_state.h"
#include "frame_access.h"
#include <algorithm>

namespace dataflow_runtime::detail {
namespace {
bool same_coordinates(const DataFrameView& a,const DataFrameView& b) noexcept {
    const auto* x=std::get_if<NumericView>(&a.payload);const auto* y=std::get_if<NumericView>(&b.payload);
    if(!x || !y)return false;
    for(std::size_t axis=0;axis<2;++axis)if(!std::ranges::equal(x->coordinates.axes[axis],y->coordinates.axes[axis]))return false;
    return true;
}
}
PreparedNumericExtension::Impl::~Impl(){if(writer_lease)writer_lease->active=nullptr;}
void PreparedNumericExtension::Impl::prepare_invocation() {
    raw_inputs.resize(inputs.size());raw_outputs.resize(outputs.size());
    coordinate_storage.resize(outputs.size());coordinate_counts.resize(outputs.size());
    coordinate_groups.resize(inputs.size());input_clocks.resize(inputs.size());
    output_buffer_contracts.resize(outputs.size());
    authority=std::make_shared<NumericOperationAuthority>();writer_lease=std::make_shared<NumericOperationWriterLease>();
    for(std::size_t i=0;i<inputs.size();++i){
        const auto& clock=latches[i].contract.clock;
        input_clocks[i]=std::make_shared<NumericOperationClock>(NumericOperationClock{std::string(clock.domain),clock.tick_seconds,clock.sample_rate});
    }
    for(std::size_t i=0;i<outputs.size();++i)output_buffer_contracts[i]=NumericOperationBindingAccess::storage(output_contracts[i]);
}
void PreparedNumericExtension::Impl::bind_inputs() noexcept {
    for(std::size_t i=0;i<inputs.size();++i){
        inputs[i]=latches[i].input();raw_inputs[i]=latches[i].frame();coordinate_groups[i]=i;
        for(std::size_t before=0;before<i;++before)if(same_coordinates(raw_inputs[i],raw_inputs[before])){coordinate_groups[i]=coordinate_groups[before];break;}
    }
}
void PreparedNumericExtension::Impl::begin_invocation() noexcept {
    bind_inputs();
    invocation={raw_inputs,raw_outputs,inputs,outputs,input_clocks,coordinate_storage,coordinate_counts,coordinate_groups,authority,writer_lease,authority->next.fetch_add(1)};
    writer_lease->active=&invocation;
    for(std::size_t i=0;i<inputs.size();++i)if(latches[i].ready)inputs[i].origin=NumericOperationBindingAccess::origin(invocation,i);
}
void PreparedNumericExtension::Impl::targets(const PlanNode& node,State& state) noexcept {
    begin_invocation();
    for(std::size_t i=0;i<outputs.size();++i){
        raw_outputs[i]=mutable_output(node,state,PortId(i));raw_outputs[i].contract=&output_contracts[i];raw_outputs[i].coordinates={};
        coordinate_counts[i]={};
        for(std::size_t axis=0;axis<2;++axis)coordinate_storage[i][axis]=state.numeric_outputs[i].coordinates[axis];
        auto& target=outputs[i];target={};target.port=PortId(i);
        target.value={&output_buffer_contracts[i],raw_outputs[i].storage,raw_outputs[i].planes,raw_outputs[i].actual,{}};
        target.coordinates=NumericOperationBindingAccess::writer(invocation,i);
    }
}
} // namespace dataflow_runtime::detail
