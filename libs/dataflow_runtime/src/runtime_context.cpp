#include "runtime_internal.h"
#include <algorithm>

namespace dataflow_runtime {
using namespace detail;
using namespace domain;
using namespace signal_contracts;
RuntimeContext::RuntimeContext() : impl_(std::make_unique<Impl>()) {}
RuntimeContext::~RuntimeContext() = default;
RuntimeContext::RuntimeContext(const RuntimeContext &c) : impl_(std::make_unique<Impl>(*c.impl_)) {
    for(auto& state:impl_->states)if(!state.numeric_stamp.stream_id.empty())state.numeric_stamp.stream_id={state.stream_storage.data(),state.numeric_stamp.stream_id.size()};
}
RuntimeContext &RuntimeContext::operator=(const RuntimeContext &c) {
    if (this != &c)
        {impl_ = std::make_unique<Impl>(*c.impl_);
        for(auto& state:impl_->states)if(!state.numeric_stamp.stream_id.empty())state.numeric_stamp.stream_id={state.stream_storage.data(),state.numeric_stamp.stream_id.size()};}
    return *this;
}
RuntimeContext::RuntimeContext(RuntimeContext &&) noexcept = default;
RuntimeContext &RuntimeContext::operator=(RuntimeContext &&) noexcept = default;
namespace {
class HistoryQuery {
    std::span<const PlanNode> nodes;
    std::span<const State> states;
    const std::size_t requested;
    std::size_t at;
    PortId port,mask_port;
    const State* mask_state=nullptr;
    static bool forwarding(ProcessingOperation operation) noexcept {
        return operation==ProcessingOperation::Sink || operation==ProcessingOperation::Gate || operation==ProcessingOperation::Switch || operation==ProcessingOperation::Splitter || operation==ProcessingOperation::InputSelector || operation==ProcessingOperation::Converter;
    }
    bool producer() const noexcept {
        return nodes[at].operation==ProcessingOperation::History || nodes[at].operation==ProcessingOperation::Source || (nodes[at].operation==ProcessingOperation::Converter && !nodes[at].identity_bypass);
    }
    Status resolve_producer() noexcept {
        while(!producer()){
            const auto& routed=nodes[at];const auto& candidate=states[at];if(!forwarding(routed.operation))return Status::incompatible_contract;
            if(!mask_state && port<candidate.route_masks.size()){mask_state=&candidate;mask_port=port;}
            const auto parent=routed.parent!=absent?routed.parent:routed.numeric_clock_parent;if(parent==absent)return Status::unavailable;
            port=routed.parent_port;at=parent;
        }
        return Status::ok;
    }
    void source_mask(NumericView& value,const State& state) const noexcept {
        const auto* routed=mask_state?mask_state:&state;const auto routed_port=mask_state?mask_port:port;
        if(routed_port<routed->route_masks.size() && routed->route_masks[routed_port].size()==value.ring.capacity*2)value.ring.silence_mask={routed->route_masks[routed_port].data()+value.ring.oldest,value.ring.capacity};
    }
    Status source_value(const PlanNode& node,const State& state,DataFrameView& frame) const noexcept {
        auto& view=std::get<NumericView>(frame.payload);
        if(!view.ring.capacity) {
            if(frame.stamp.available || !state.history_origin_valid)return Status::incompatible_contract;
            view.actual[0]=std::get<NumericContract>(node.numeric_outputs[port]).shape.extents[0].maximum;
            view.ring={view.actual[0],state.oldest,{}};
        }
        view.contract=&std::get<NumericContract>(node.numeric_outputs[port]);
        source_mask(view,state);
        return Status::ok;
    }
    Status source(DataFrameView& output) const noexcept {
        const auto& state=states[at];const auto& node=nodes[at];
        if(!state.owned_ingress)return state.history_origin_valid?Status::unavailable:Status::incompatible_contract;
        auto frame=state.owned_ingress.view();auto& view=std::get<NumericView>(frame.payload);
        const auto status=source_value(node,state,frame);if(status!=Status::ok)return status;
        frame.stamp=states[requested].numeric_stamp;
        if(!frame.stamp.available)view.storage={};
        output=frame;return frame.stamp.available?Status::ok:Status::unavailable;
    }
    FrameStamp prepared_stamp(const NumericContract& contract,const State& state) const noexcept {
        auto stamp=states[requested].numeric_stamp;
        const bool empty=nodes[at].operation==ProcessingOperation::History && !state.numeric_marker && state.progress==0 && !contract.clock.domain.empty() && contract.clock.tick_seconds.numerator>0 && contract.clock.tick_seconds.denominator>0;
        stamp.available=empty || (states[requested].numeric_marker && stamp.available);
        if(empty){stamp.sequence=stamp.source_sequence=0;stamp.timestamp_ticks=0;stamp.discontinuity=true;}
        return stamp;
    }
    void prepared_coordinates(NumericView& view,const NumericBuffer& buffer,const State& state) const noexcept {
        for(std::size_t axis=0;axis<2;++axis)if(!buffer.coordinates[axis].empty()){
            const auto offset=axis==0?state.oldest*8:0;view.coordinates.axes[axis]={buffer.coordinates[axis].data()+offset,view.actual[axis]*8};
        }
        if(mask_state && mask_state->route_masks[mask_port].size()==buffer.actual[0]*2)view.ring.silence_mask={mask_state->route_masks[mask_port].data()+state.oldest,buffer.actual[0]};
    }
    Status prepared(DataFrameView& output) const noexcept {
        const auto& node=nodes[at];const auto& state=states[at];
        if(node.operation==ProcessingOperation::Converter && !state.history_origin_valid)return Status::incompatible_contract;
        const auto& buffer=state.numeric_outputs[port];const auto& contract=std::get<NumericContract>(node.numeric_outputs[port]);const auto stamp=prepared_stamp(contract,state);
        NumericView view{&contract,buffer.storage(),buffer.planes,{state.frames,buffer.actual[1]},{buffer.actual[0],state.oldest,{}},{}};
        prepared_coordinates(view,buffer,state);if(!stamp.available)view.storage={};output={stamp,view};return stamp.available?Status::ok:Status::unavailable;
    }
public:
    HistoryQuery(std::span<const PlanNode> plan,std::span<const State> context,std::size_t node,PortId requested_port) noexcept:nodes(plan),states(context),requested(node),at(node),port(requested_port),mask_port(port){}
    Status read(DataFrameView& output,std::uint64_t* filled,std::uint64_t* ordinal) noexcept {
        const auto status=resolve_producer();if(status!=Status::ok)return status;
        const auto& state=states[at];if(filled)*filled=state.progress;if(ordinal)*ordinal=state.row_ordinal;
        if(port>=state.numeric_outputs.size())return Status::unsupported_version;
        return nodes[at].operation==ProcessingOperation::Source?source(output):prepared(output);
    }
};
const RoutingAlias* history_alias(std::span<const RoutingAlias> aliases,NodeId id) noexcept {
    for(const auto& alias:aliases)
        if(alias.node==id && alias.kind==NodeKind::Reroute)return &alias;
    return nullptr;
}
}
Status PreparedGraph::history(NodeId id,std::uint16_t port,const RuntimeContext& context,DataFrameView& output,std::uint64_t* filled,std::uint64_t* ordinal) const noexcept {
    if(!context.impl_ || context.impl_->plan!=this)return Status::invalid_argument;
    if(const auto* alias=history_alias(impl_->routing_aliases,id)) {
        if(port!=0)return Status::invalid_argument;
        return alias->source<0?Status::unavailable:history(alias->source,alias->source_port,context,output,filled,ordinal);
    }
    const auto at=index(impl_->nodes,id);if(at==absent)return Status::invalid_argument;
    const auto direction=impl_->nodes[at].operation==ProcessingOperation::Sink?PortDirection::Input:PortDirection::Output;const auto contract=port_contract(id,direction,port);
    if(!contract || !std::holds_alternative<NumericContract>(*contract) || (std::get<NumericContract>(*contract).shape.rank!=Rank::Matrix && std::get<NumericContract>(*contract).semantic!=Semantic::History))return Status::incompatible_contract;
    return HistoryQuery(impl_->nodes,context.impl_->states,at,port).read(output,filled,ordinal);
}
std::vector<NodeId> PreparedGraph::clear_ingress(NodeId id,RuntimeContext& context) const {
    std::vector<NodeId> changed;if(!context.impl_ || context.impl_->plan!=this)return changed;
    const auto root=index(impl_->nodes,id);if(root==absent || impl_->nodes[root].operation!=ProcessingOperation::Source)return changed;
    for(std::size_t i=0;i<impl_->nodes.size();++i)if(impl_->nodes[i].clock==root){clear_node_state(impl_->nodes[i],context.impl_->states[i]);changed.push_back(impl_->nodes[i].node.id);}
    return changed;
}
void PreparedGraph::reset_history(NodeId id,RuntimeContext& context) const noexcept {
    if(!context.impl_ || context.impl_->plan!=this)return;
    const auto at=index(impl_->nodes,id);if(at==absent || impl_->nodes[at].operation!=ProcessingOperation::History)return;
    clear_node_state(impl_->nodes[at],context.impl_->states[at]);
}
bool PreparedGraph::marker_current(NodeId id,const FrameStamp& stamp,const RuntimeContext& context) const noexcept {
    if(!context.impl_ || context.impl_->plan!=this)return false;
    const auto at=index(impl_->nodes,id);if(at==absent)return false;
    const auto& state=context.impl_->states[at];
    return state.numeric_marker&&stamp.producer_epoch==state.numeric_stamp.producer_epoch&&stamp.generation==state.numeric_stamp.generation;
}
void PreparedGraph::rebase_local_producers(RuntimeContext& context,std::uint64_t previous,std::uint64_t next,std::uint64_t generation) const noexcept {
    if(!context.impl_ || context.impl_->plan!=this)return;
    for(auto& state:context.impl_->states)if(state.numeric_marker&&state.numeric_stamp.producer_epoch==previous){state.numeric_stamp.producer_epoch=next;state.numeric_stamp.generation=generation;}
}
namespace {
class StateRefresh {
    const std::span<const PlanNode> nodes,prior_nodes;
    const std::span<State> states;
    const std::span<const State> prior_states;
    const NumericProcessingGraph& graph;
    const bool same_interval;
    struct Frame {const PlanNode& node;const PlanNode& prior;State& next;const State& before;std::size_t prior_index;};
    static NodeId producer_id(std::span<const PlanNode> nodes,std::size_t parent) noexcept {return parent==absent?NodeId(-1):nodes[parent].node.id;}
    void reset_frame(const Frame& frame) const noexcept {
        if(frame.node.clock_reset)return;
        frame.next.output_sequence=frame.before.output_sequence;
        if(frame.node.node.kind==NodeKind::Window && same_interval){frame.next.progress=frame.before.progress;frame.next.remainder=frame.before.remainder;frame.next.hop=frame.before.hop;}
    }
    void copy_identity(const Frame& frame) const noexcept {
        auto& next=frame.next;const auto& before=frame.before;
        next.frames=before.frames;next.oldest=before.oldest;next.row_ordinal=before.row_ordinal;
        next.history_origin=before.history_origin;next.history_origin_valid=before.history_origin_valid;
        next.numeric_marker=before.numeric_marker;stamp_state(next,before.numeric_stamp);next.last_sequence=before.last_sequence;
        next.retired=before.retired;next.retired_count=before.retired_count;next.retired_next=before.retired_next;
    }
    void copy_schedule(const Frame& frame) const noexcept {
        auto& next=frame.next;const auto& before=frame.before;
        next.progress=before.progress;next.remainder=before.remainder;next.hop=before.hop;next.output_sequence=before.output_sequence;
        next.pending_discontinuity=before.pending_discontinuity;next.stats=before.stats;
        if(next.route_state.size()==before.route_state.size())std::copy(before.route_state.begin(),before.route_state.end(),next.route_state.begin());
        if(next.source_stamps.size()==before.source_stamps.size()){
            std::copy(before.source_stamps.begin(),before.source_stamps.end(),next.source_stamps.begin());std::copy(before.source_markers.begin(),before.source_markers.end(),next.source_markers.begin());
        }
    }
    void copy_generator(const Frame& frame) const noexcept {
        if(frame.node.node.kind!=NodeKind::SignalGenerator)return;
        auto& oscillator=*std::get<OscillatorKernel>(frame.next.kernel).value;oscillator=*std::get<OscillatorKernel>(frame.before.kernel).value;
        (void)oscillator.update(std::get<SignalGeneratorProcessingParameters>(graph.nodes[frame.node.configured].parameters).oscillators);
    }
    void copy_kernels(const Frame& frame) const noexcept {
        copy_generator(frame);
        if(frame.node.operation==ProcessingOperation::MusicTransform)std::get<PreparedOperationKernel>(frame.next.kernel).copy_state_from(std::get<PreparedOperationKernel>(frame.before.kernel));
        if(frame.node.node.kind==NodeKind::PcmResampler)std::get<ResamplerKernel>(frame.next.kernel).operation().copy_state_from(std::get<ResamplerKernel>(frame.before.kernel).operation());
        if(frame.node.selector_changed){frame.next.pending_discontinuity=true;
            if(frame.node.operation==ProcessingOperation::InputSelector && frame.node.endpoint_plan)std::fill(frame.next.source_markers.begin(),frame.next.source_markers.end(),false);}
    }
    bool mixer_input_matches(const PlanNode::ScalarInput& next,const PlanNode::ScalarInput& before) const noexcept {
        return next.parent_port==before.parent_port && producer_id(nodes,next.parent)==producer_id(prior_nodes,before.parent) && producer_id(nodes,next.clock)==producer_id(prior_nodes,before.clock) && (next.parent==absent || !nodes[next.parent].clock_reset);
    }
    static void copy_queue_markers(MixerQueue& next,const MixerQueue& before) noexcept {
        next.last_input=before.last_input;next.last_input.stream_id={};next.input_marker=before.input_marker;
        next.boundary=before.boundary;next.boundary.stream_id={};next.boundary_marker=before.boundary_marker;
        next.count=std::min(next.samples.size(),before.count);next.head=0;next.dropped=before.dropped;next.underrun=before.underrun;next.discontinuity=before.discontinuity;
        next.aligned_start_watermark=before.aligned_start_watermark;next.aligned_start_valid=before.aligned_start_valid;
    }
    static void copy_queue_stamps(MixerQueue& next,const MixerQueue& before) noexcept {
        next.stamps=before.stamps;next.stamp_head=before.stamp_head;next.stamp_count=before.stamp_count;
        for(std::size_t slot=0;slot<2;++slot)if(next.coordinates[slot].size()==before.coordinates[slot].size())std::copy(before.coordinates[slot].begin(),before.coordinates[slot].end(),next.coordinates[slot].begin());
    }
    static void copy_queue_samples(MixerQueue& next,const MixerQueue& before) noexcept {
        for(std::size_t index=0;index<next.count;++index){
            const auto slot=(before.head+before.count-next.count+index)%before.samples.size();next.samples[index]=before.samples[slot];
            if(!next.aligned.empty() && !before.aligned.empty())next.aligned[index]=before.aligned[slot];
            if(!next.provenance.empty())next.provenance[index]=before.provenance[slot];
        }
    }
    void copy_mixer_lane(const Frame& frame,std::size_t lane) const noexcept {
        auto& next=frame.next.mixer_queues[lane];const auto& incoming=frame.node.endpoint_plan->inputs[lane];const auto& old_inputs=frame.prior.endpoint_plan->inputs;
        const auto found=std::find_if(old_inputs.begin(),old_inputs.end(),[&](const auto& input){return input.port==incoming.port;});
        if(found==old_inputs.end() || !mixer_input_matches(incoming,*found)){next.discontinuity=true;return;}
        const auto& before=frame.before.mixer_queues[std::size_t(found-old_inputs.begin())];copy_queue_markers(next,before);copy_queue_stamps(next,before);copy_queue_samples(next,before);
    }
    void copy_mixer(const Frame& frame) const noexcept {
        if((frame.node.operation!=ProcessingOperation::Mixer && frame.node.operation!=ProcessingOperation::ChannelComparison) || frame.next.mixer_queues.size()!=frame.before.mixer_queues.size())return;
        for(std::size_t lane=0;lane<frame.next.mixer_queues.size();++lane)copy_mixer_lane(frame,lane);
    }
    std::size_t history_producer(std::size_t at) const noexcept {
        while(prior_nodes[at].operation!=ProcessingOperation::History && prior_nodes[at].operation!=ProcessingOperation::Source && prior_nodes[at].operation!=ProcessingOperation::Converter){
            const auto& routed=prior_nodes[at];const auto parent=routed.parent!=absent?routed.parent:routed.numeric_clock_parent;if(parent==absent)break;at=parent;
        }
        return at;
    }
    void resize_mask(const Frame& frame,std::size_t port) const noexcept {
        auto& next=frame.next.route_masks[port];const auto& before=frame.before.route_masks[port];const auto& retained=prior_states[history_producer(frame.prior_index)];
        const auto capacity=next.size()/2,prior_capacity=before.size()/2,kept=std::min(retained.progress,capacity);
        if(!prior_capacity || retained.frames<kept)return;
        for(std::size_t row=0;row<kept;++row){const auto from=(retained.oldest+retained.frames-kept+row)%prior_capacity,to=capacity-kept+row;next[to]=next[to+capacity]=before[from];}
    }
    void copy_masks(const Frame& frame) const noexcept {
        auto& next=frame.next;const auto& before=frame.before;
        for(std::size_t port=0;port<next.route_masks.size() && port<before.route_masks.size();++port){
            if(next.route_masks[port].size()==before.route_masks[port].size())std::copy(before.route_masks[port].begin(),before.route_masks[port].end(),next.route_masks[port].begin());
            else if(!next.route_masks[port].empty())resize_mask(frame,port);
        }
    }
    void copy_ingress(const Frame& frame) const noexcept {
        auto previous_owner=frame.next.ingress_owner;frame.next.ingress_owner=frame.before.ingress_owner;frame.next.owned_ingress=frame.before.owned_ingress;
        copy_masks(frame);
    }
    void copy_window(const Frame& frame) const noexcept {
        auto& next=frame.next;const auto& before=frame.before;
        if(next.pcm.size()==before.pcm.size())std::copy(before.pcm.begin(),before.pcm.end(),next.pcm.begin());
        if((frame.node.operation==ProcessingOperation::Mixer || frame.node.node.kind==NodeKind::Window) && !same_interval){next.progress=next.hop=0;next.remainder=0;next.pending_discontinuity=true;}
        if(frame.node.node.kind==NodeKind::Window && (!frame.node.node.enabled || frame.node.node.enabled!=frame.prior.node.enabled)){std::fill(next.pcm.begin(),next.pcm.end(),0);next.pending_discontinuity=true;}
    }
    static bool needs_history_resize(const Frame& frame) noexcept {
        return (frame.node.operation==ProcessingOperation::History || (frame.node.operation==ProcessingOperation::Converter && !frame.node.identity_bypass && frame.before.history_origin_valid)) && !frame.next.numeric_outputs.empty() && std::get<NumericContract>(frame.node.numeric_outputs[0]).semantic==Semantic::History && frame.next.numeric_outputs[0].actual[0]!=frame.before.numeric_outputs[0].actual[0];
    }
    static void copy_history_row(const NumericView& source,MutableNumericView& target,DType dtype,
                                 std::size_t plane,std::size_t from,std::size_t to,std::size_t columns) noexcept {
        for(std::size_t column=0;column<columns;++column) {
            std::uint64_t bits=0;
            const bool vector=source.contract->shape.rank==Rank::Vector;
            (void)load_bits(source,{plane,from,vector?0:column},dtype,bits);
            (void)store_bits(target,{plane,to,vector?0:column},dtype,bits);
        }
    }
    static void resize_history_values(const Frame& frame,std::size_t kept) noexcept {
        auto& next=frame.next.numeric_outputs[0];const auto& before=frame.before.numeric_outputs[0];const auto& contract=std::get<NumericContract>(frame.node.numeric_outputs[0]);
        const auto& old_contract=std::get<NumericContract>(frame.prior.numeric_outputs[0]);
        NumericView source{&old_contract,before.storage(),before.planes,{frame.before.frames,before.actual[1]},{before.actual[0],frame.before.oldest,{}},{}};
        MutableNumericView target{&contract,next.storage(),next.planes,next.actual,{next.actual[0],0,{}},{}};
        for(std::size_t plane=0;plane<next.planes.size();++plane)
            for(std::size_t row=0;row<kept;++row)
                copy_history_row(source,target,contract.dtype,plane,frame.before.frames-kept+row,next.actual[0]-kept+row,next.actual[1]);
    }
    static void resize_history_coordinates(const Frame& frame,std::size_t kept) noexcept {
        auto& next=frame.next.numeric_outputs[0];const auto& before=frame.before.numeric_outputs[0];
        if(!next.coordinates[0].empty())for(std::size_t row=0;row<kept;++row){
            const auto physical=(frame.before.oldest+frame.before.frames-kept+row)%before.actual[0],to=next.actual[0]-kept+row;
            std::copy_n(before.coordinates[0].begin()+physical*8,8,next.coordinates[0].begin()+to*8);std::copy_n(before.coordinates[0].begin()+physical*8,8,next.coordinates[0].begin()+(to+next.actual[0])*8);
        }
        if(next.coordinates[1].size()==before.coordinates[1].size())std::copy(before.coordinates[1].begin(),before.coordinates[1].end(),next.coordinates[1].begin());
    }
    static void resize_history_progress(const Frame& frame,std::size_t kept) noexcept {
        frame.next.frames=frame.next.numeric_outputs[0].actual[0];frame.next.oldest=0;frame.next.progress=kept;
        if(frame.node.operation!=ProcessingOperation::History)return;
        auto& count=frame.next.numeric_outputs[1];const auto& contract=std::get<NumericContract>(frame.node.numeric_outputs[1]);
        MutableNumericView output{&contract,count.storage(),count.planes,count.actual,{},{}};(void)store_element(output,{0,0,0},std::uint64_t(kept));
    }
    static void resize_history(const Frame& frame) noexcept {
        const auto kept=std::min(frame.before.progress,frame.next.numeric_outputs[0].actual[0]);
        resize_history_values(frame,kept);resize_history_coordinates(frame,kept);resize_history_progress(frame,kept);
    }
    static void copy_output(const Frame& frame,std::size_t port) noexcept {
        auto& next=frame.next.numeric_outputs[port];const auto& before=frame.before.numeric_outputs[port];
        if(next.storage().size()!=before.storage().size())return;
        std::copy(before.storage().begin(),before.storage().end(),next.storage().begin());
        // MIDI coordinates belong to the new prepared range/tuning.
        for(std::size_t axis=0;axis<2;++axis)if(!(frame.node.node.kind==NodeKind::MusicalSpectrogram && port==3 && axis==0) && next.coordinates[axis].size()==before.coordinates[axis].size())std::copy(before.coordinates[axis].begin(),before.coordinates[axis].end(),next.coordinates[axis].begin());
    }
    static void copy_outputs(const Frame& frame) noexcept {
        if(needs_history_resize(frame)){resize_history(frame);return;}
        // New immutable banks retain coefficients from the new declaration.
        if(frame.node.node.kind==NodeKind::WindowFunction || frame.node.operation==ProcessingOperation::ArrayFill)return;
        for(std::size_t port=0;port<frame.next.numeric_outputs.size() && port<frame.before.numeric_outputs.size();++port)copy_output(frame,port);
    }
    void copy_frame(const Frame& frame) const noexcept {
        if(frame.node.reset){reset_frame(frame);return;}
        copy_identity(frame);copy_schedule(frame);copy_mixer(frame);copy_kernels(frame);copy_ingress(frame);copy_window(frame);copy_outputs(frame);
    }
public:
    StateRefresh(std::span<const PlanNode> prepared,std::span<State> context,const NumericProcessingGraph& definition,Ratio interval,std::span<const PlanNode> prior,std::span<const State> before,Ratio prior_interval) noexcept
        :nodes(prepared),prior_nodes(prior),states(context),prior_states(before),graph(definition),same_interval(interval==prior_interval){}
    void copy() const noexcept {
        for(std::size_t at=0;at<nodes.size();++at){const auto old=index(prior_nodes,nodes[at].node.id);if(old!=absent)copy_frame({nodes[at],prior_nodes[old],states[at],prior_states[old],old});}
    }
};
}
void PreparedGraph::refresh_state(RuntimeContext& context,const PreparedGraph& previous,const RuntimeContext& old_context) const noexcept {
    if(!impl_->numeric_graph || !previous.impl_->numeric_graph || old_context.impl_->plan!=&previous)return;
    StateRefresh{impl_->nodes,context.impl_->states,*impl_->numeric_graph,impl_->numeric_source_graph->execution_interval,previous.impl_->nodes,old_context.impl_->states,previous.impl_->numeric_source_graph->execution_interval}.copy();
}
std::vector<NodeId> PreparedGraph::reset_nodes() const {
    std::vector<NodeId> result;
    for (const auto &n : impl_->nodes)
        if (n.reset)
            result.push_back(n.node.id);
    return result;
}

} // namespace dataflow_runtime
