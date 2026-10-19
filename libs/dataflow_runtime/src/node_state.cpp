#include "node_state.h"

namespace dataflow_runtime::detail {
void clear_node_state(const PlanNode& node,State& state) noexcept {
    state.clear();
    if(node.operation!=ProcessingOperation::History || state.numeric_outputs.empty())return;
    auto& buffer=state.numeric_outputs[0];
    const auto& contract=std::get<NumericContract>(node.numeric_outputs[0]);
    state.frames=buffer.actual[0];
    MutableNumericView view{&contract,buffer.storage(),buffer.planes,buffer.actual,{buffer.actual[0],0,{}},{}};
    for(std::size_t axis=0;axis<2;++axis)if(!buffer.coordinates[axis].empty()){
        std::fill(buffer.coordinates[axis].begin(),buffer.coordinates[axis].end(),std::byte{0});
        view.coordinates.axes[axis]={buffer.coordinates[axis].data(),buffer.actual[axis]*8};}
    (void)fill_silence(view,{buffer.planes.size(),SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX});
}
void stamp_state(State& state,const FrameStamp& stamp) noexcept {
    state.numeric_stamp=stamp;
    if(stamp.stream_id.size()>state.stream_storage.size()){state.numeric_stamp.available=false;state.numeric_stamp.stream_id={};return;}
    if(stamp.stream_id.empty())state.numeric_stamp.stream_id={};
    else {std::copy(stamp.stream_id.begin(),stamp.stream_id.end(),state.stream_storage.begin());
        state.numeric_stamp.stream_id={state.stream_storage.data(),stamp.stream_id.size()};}
}
std::size_t index(std::span<const PlanNode> nodes,NodeId id) noexcept {
    for(std::size_t i=0;i<nodes.size();++i)if(nodes[i].node.id==id)return i;
    return absent;
}
}
