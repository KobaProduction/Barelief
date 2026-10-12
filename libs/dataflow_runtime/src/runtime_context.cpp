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
Status PreparedGraph::history(NodeId id,std::uint16_t port,const RuntimeContext& context,DataFrameView& out,std::uint64_t* filled_count,std::uint64_t* ordinal) const noexcept {
    if(!context.impl_ || context.impl_->plan!=this)return Status::invalid_argument;
    auto at=index(impl_->nodes,id);if(at==absent)return Status::invalid_argument;
    const auto& requested=impl_->nodes[at];
    const auto contract=port_contract(id,requested.operation==ProcessingOperation::Sink?PortDirection::Input:PortDirection::Output,port);
    if(!contract || !std::holds_alternative<NumericContract>(*contract) || std::get<NumericContract>(*contract).shape.rank!=Rank::Matrix)return Status::incompatible_contract;
    const State* mask_state=nullptr;std::size_t mask_port=port;
    while(impl_->nodes[at].operation!=ProcessingOperation::History && impl_->nodes[at].operation!=ProcessingOperation::Source &&
        (impl_->nodes[at].operation!=ProcessingOperation::Converter || impl_->nodes[at].identity_bypass)){const auto& routed=impl_->nodes[at];const auto& candidate=context.impl_->states[at];
        if(!mask_state && port<candidate.route_masks.size()){mask_state=&candidate;mask_port=port;}
        const auto parent=routed.parent!=absent?routed.parent:routed.numeric_clock_parent;
        if(parent==absent)return Status::unavailable;
        port=routed.parent_port;at=parent;
    }
    const auto& node=impl_->nodes[at];const auto& state=context.impl_->states[at];
    if(filled_count)*filled_count=state.progress;
    if(ordinal)*ordinal=state.row_ordinal;
    if(port>=state.numeric_outputs.size())return Status::unsupported_version;
    if(node.operation==ProcessingOperation::Source){
        if(!state.owned_ingress)return state.history_origin_valid?Status::unavailable:Status::incompatible_contract;
        auto frame=state.owned_ingress.view();auto& view=std::get<NumericView>(frame.payload);
        if(!view.ring.capacity){
            if(frame.stamp.available || !state.history_origin_valid)return Status::incompatible_contract;
            view.actual[0]=std::get<NumericContract>(node.numeric_outputs[port]).shape.extents[0].maximum;
            view.ring={view.actual[0],state.oldest,{}};
        }
        view.contract=&std::get<NumericContract>(node.numeric_outputs[port]);
        const auto* routed_mask=mask_state?mask_state:&state;const auto routed_port=mask_state?mask_port:port;
        if(routed_port<routed_mask->route_masks.size() && routed_mask->route_masks[routed_port].size()==view.ring.capacity*2)
            view.ring.silence_mask={routed_mask->route_masks[routed_port].data()+view.ring.oldest,view.ring.capacity};
        frame.stamp=context.impl_->states[index(impl_->nodes,id)].numeric_stamp;
        if(!frame.stamp.available)view.storage={};
        out=frame;return frame.stamp.available?Status::ok:Status::unavailable;
    }
    if(node.operation==ProcessingOperation::Converter && !state.history_origin_valid)return Status::incompatible_contract;
    const auto& value=state.numeric_outputs[port];const auto& declared=std::get<NumericContract>(node.numeric_outputs[port]);
    auto stamp=context.impl_->states[index(impl_->nodes,id)].numeric_stamp;
    const bool initialized_empty=node.operation==ProcessingOperation::History && !state.numeric_marker && state.progress==0 &&
        !declared.clock.domain.empty() && declared.clock.tick_seconds.numerator>0 && declared.clock.tick_seconds.denominator>0;
    stamp.available=initialized_empty || (context.impl_->states[index(impl_->nodes,id)].numeric_marker&&stamp.available);
    if(initialized_empty){stamp.sequence=stamp.source_sequence=0;stamp.timestamp_ticks=0;stamp.discontinuity=true;}
    NumericView view{&declared,value.storage(),value.planes,{state.frames,value.actual[1]},{value.actual[0],state.oldest,{}},{}};
    for(std::size_t axis=0;axis<2;++axis)if(!value.coordinates[axis].empty()){
        const auto offset=axis==0?state.oldest*8:0;
        view.coordinates.axes[axis]={value.coordinates[axis].data()+offset,view.actual[axis]*8};}
    if(mask_state && mask_state->route_masks[mask_port].size()==value.actual[0]*2)view.ring.silence_mask={mask_state->route_masks[mask_port].data()+state.oldest,value.actual[0]};
    if(!stamp.available)view.storage={};
    out={stamp,view};return stamp.available?Status::ok:Status::unavailable;
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
void PreparedGraph::refresh_state(RuntimeContext &c, const PreparedGraph &previous,
                                  const RuntimeContext &old_context) const noexcept {
    if(impl_->numeric_graph){
        if(!previous.impl_->numeric_graph || old_context.impl_->plan!=&previous)return;
        for(std::size_t i=0;i<impl_->nodes.size();++i){const auto& n=impl_->nodes[i];
            const auto old=index(previous.impl_->nodes,n.node.id);if(old==absent)continue;
            auto& dst=c.impl_->states[i];const auto& src=old_context.impl_->states[old];
            if(n.reset){
                // Rebuilding window geometry discards its payload, not the
                // schedule or producer sequence of an unchanged source clock.
                if(!n.clock_reset){dst.output_sequence=src.output_sequence;
                    if(n.node.kind==NodeKind::Window && impl_->numeric_source_graph->execution_interval==previous.impl_->numeric_source_graph->execution_interval){
                        dst.progress=src.progress;dst.remainder=src.remainder;dst.hop=src.hop;}}
                continue;
            }
            dst.frames=src.frames;dst.oldest=src.oldest;dst.row_ordinal=src.row_ordinal;
            dst.history_origin=src.history_origin;dst.history_origin_valid=src.history_origin_valid;
            dst.numeric_marker=src.numeric_marker;stamp_state(dst,src.numeric_stamp);dst.last_sequence=src.last_sequence;
            dst.retired=src.retired;dst.retired_count=src.retired_count;dst.retired_next=src.retired_next;
            dst.progress=src.progress;dst.remainder=src.remainder;dst.hop=src.hop;dst.output_sequence=src.output_sequence;
            if(dst.route_state.size()==src.route_state.size())std::copy(src.route_state.begin(),src.route_state.end(),dst.route_state.begin());
            dst.pending_discontinuity=src.pending_discontinuity;dst.stats=src.stats;
            if(dst.source_stamps.size()==src.source_stamps.size()){std::copy(src.source_stamps.begin(),src.source_stamps.end(),dst.source_stamps.begin());std::copy(src.source_markers.begin(),src.source_markers.end(),dst.source_markers.begin());}
            if(n.node.kind==NodeKind::SignalGenerator){*std::get<OscillatorKernel>(dst.kernel).value=*std::get<OscillatorKernel>(src.kernel).value;
                (void)std::get<OscillatorKernel>(dst.kernel).value->update(std::get<SignalGeneratorProcessingParameters>(impl_->numeric_graph->nodes[n.configured].parameters).oscillators);}
            if((n.operation==ProcessingOperation::Mixer || n.operation==ProcessingOperation::ChannelComparison) && dst.mixer_queues.size()==src.mixer_queues.size())for(std::size_t q=0;q<dst.mixer_queues.size();++q){auto& next=dst.mixer_queues[q];
                const auto& incoming=n.endpoint_plan->inputs[q];const auto& old_inputs=previous.impl_->nodes[old].endpoint_plan->inputs;
                const auto found=std::find_if(old_inputs.begin(),old_inputs.end(),[&](const auto& input){return input.port==incoming.port;});
                auto producer_id=[](const auto& nodes,std::size_t parent){return parent==absent?NodeId(-1):nodes[parent].node.id;};
                if(found==old_inputs.end() || incoming.parent_port!=found->parent_port ||
                   producer_id(impl_->nodes,incoming.parent)!=producer_id(previous.impl_->nodes,found->parent) ||
                   producer_id(impl_->nodes,incoming.clock)!=producer_id(previous.impl_->nodes,found->clock) ||
                   (incoming.parent!=absent && impl_->nodes[incoming.parent].clock_reset)){
                    next.discontinuity=true;continue;}
                const auto& before=src.mixer_queues[std::size_t(found-old_inputs.begin())];
                next.last_input=before.last_input;next.last_input.stream_id={};next.input_marker=before.input_marker;next.boundary=before.boundary;next.boundary.stream_id={};next.boundary_marker=before.boundary_marker;next.count=std::min(next.samples.size(),before.count);next.head=0;next.dropped=before.dropped;next.underrun=before.underrun;next.discontinuity=before.discontinuity;next.stamps=before.stamps;next.stamp_head=before.stamp_head;next.stamp_count=before.stamp_count;for(std::size_t slot=0;slot<2;++slot)if(next.coordinates[slot].size()==before.coordinates[slot].size())std::copy(before.coordinates[slot].begin(),before.coordinates[slot].end(),next.coordinates[slot].begin());
                next.aligned_start_watermark=before.aligned_start_watermark;next.aligned_start_valid=before.aligned_start_valid;
                for(std::size_t k=0;k<next.count;++k){const auto slot=(before.head+before.count-next.count+k)%before.samples.size();next.samples[k]=before.samples[slot];if(!next.aligned.empty()&&!before.aligned.empty())next.aligned[k]=before.aligned[slot];
                    if(!next.provenance.empty())next.provenance[k]=before.provenance[slot];}}

            if(n.node.kind==NodeKind::PcmResampler)
                std::get<ResamplerKernel>(dst.kernel).operation().copy_state_from(std::get<ResamplerKernel>(src.kernel).operation());
            if(n.selector_changed){dst.pending_discontinuity=true;
                if(n.operation==ProcessingOperation::InputSelector&&n.endpoint_plan)
                    std::fill(dst.source_markers.begin(),dst.source_markers.end(),false);}
            dst.peak_hz=src.peak_hz;dst.magnitude=src.magnitude;
            auto previous_owner=dst.ingress_owner;dst.ingress_owner=src.ingress_owner;dst.owned_ingress=src.owned_ingress;
            for(std::size_t p=0;p<dst.route_masks.size();++p)if(p<src.route_masks.size() && dst.route_masks[p].size()==src.route_masks[p].size())
                std::copy(src.route_masks[p].begin(),src.route_masks[p].end(),dst.route_masks[p].begin());
            // Route masks refer to physical ring rows. Resize with the producer's
            // logical newest suffix; never unmask previously bypassed rows.
            for(std::size_t p=0;p<dst.route_masks.size() && p<src.route_masks.size();++p)
                if(!dst.route_masks[p].empty() && dst.route_masks[p].size()!=src.route_masks[p].size()){
                    auto producer=old;
                    while(previous.impl_->nodes[producer].operation!=ProcessingOperation::History &&
                          previous.impl_->nodes[producer].operation!=ProcessingOperation::Source &&
                          previous.impl_->nodes[producer].operation!=ProcessingOperation::Converter){
                        const auto& routed=previous.impl_->nodes[producer];
                        const auto parent=routed.parent!=absent?routed.parent:routed.numeric_clock_parent;
                        if(parent==absent)break;
                        producer=parent;
                    }
                    const auto& retained=old_context.impl_->states[producer];
                    const auto new_capacity=dst.route_masks[p].size()/2,old_capacity=src.route_masks[p].size()/2;
                    const auto kept=std::min(retained.progress,new_capacity);
                    if(old_capacity && retained.frames>=kept)for(std::size_t row=0;row<kept;++row){
                        const auto from=(retained.oldest+retained.frames-kept+row)%old_capacity,to=new_capacity-kept+row;
                        dst.route_masks[p][to]=dst.route_masks[p][to+new_capacity]=src.route_masks[p][from];}
                }
            if(dst.pcm.size()==src.pcm.size())std::copy(src.pcm.begin(),src.pcm.end(),dst.pcm.begin());
            if(n.operation==ProcessingOperation::Mixer && impl_->numeric_source_graph->execution_interval!=previous.impl_->numeric_source_graph->execution_interval){dst.progress=dst.hop=0;dst.remainder=0;dst.pending_discontinuity=true;}
            if(n.node.kind==NodeKind::Window){
                if(impl_->numeric_source_graph->execution_interval!=previous.impl_->numeric_source_graph->execution_interval){
                    dst.progress=dst.hop=0;dst.remainder=0;dst.pending_discontinuity=true;
                }
                if(!n.node.enabled || n.node.enabled!=previous.impl_->nodes[old].node.enabled){
                    std::fill(dst.pcm.begin(),dst.pcm.end(),0);dst.pending_discontinuity=true;
                }
            }
            if((n.operation==ProcessingOperation::History || (n.operation==ProcessingOperation::Converter && !n.identity_bypass && src.history_origin_valid)) && !dst.numeric_outputs.empty() && std::get<NumericContract>(n.numeric_outputs[0]).shape.rank==Rank::Matrix && dst.numeric_outputs[0].actual[0]!=src.numeric_outputs[0].actual[0]){
                auto& next=dst.numeric_outputs[0];const auto& before=src.numeric_outputs[0];
                const auto& contract=std::get<NumericContract>(n.numeric_outputs[0]);
                const auto& old_contract=std::get<NumericContract>(previous.impl_->nodes[old].numeric_outputs[0]);
                const auto kept=std::min(src.progress,next.actual[0]);
                NumericView source{&old_contract,before.storage(),before.planes,{src.frames,before.actual[1]},{before.actual[0],src.oldest,{}},{}};
                MutableNumericView target{&contract,next.storage(),next.planes,next.actual,{next.actual[0],0,{}},{}};
                for(std::size_t p=0;p<next.planes.size();++p)for(std::size_t row=0;row<kept;++row)
                    for(std::size_t col=0;col<next.actual[1];++col){std::uint64_t bits=0;
                        (void)load_bits(source,{p,src.frames-kept+row,col},contract.dtype,bits);
                        (void)store_bits(target,{p,next.actual[0]-kept+row,col},contract.dtype,bits);}
                if(!next.coordinates[0].empty())for(std::size_t row=0;row<kept;++row){
                    const auto physical=(src.oldest+src.frames-kept+row)%before.actual[0];
                    const auto to=next.actual[0]-kept+row;
                    std::copy_n(before.coordinates[0].begin()+physical*8,8,next.coordinates[0].begin()+to*8);
                    std::copy_n(before.coordinates[0].begin()+physical*8,8,next.coordinates[0].begin()+(to+next.actual[0])*8);}
                if(next.coordinates[1].size()==before.coordinates[1].size())std::copy(before.coordinates[1].begin(),before.coordinates[1].end(),next.coordinates[1].begin());
                dst.frames=next.actual[0];dst.oldest=0;dst.progress=kept;
                if(n.operation==ProcessingOperation::History){auto& count=dst.numeric_outputs[1];const auto& count_contract=std::get<NumericContract>(n.numeric_outputs[1]);
                    MutableNumericView count_view{&count_contract,count.storage(),count.planes,count.actual,{},{}};
                    (void)store_element(count_view,{0,0,0},std::uint64_t(kept));}
                continue;
            }
            for(std::size_t p=0;p<dst.numeric_outputs.size();++p)
                if(p<src.numeric_outputs.size() && dst.numeric_outputs[p].storage().size()==src.numeric_outputs[p].storage().size())
                    {std::copy(src.numeric_outputs[p].storage().begin(),src.numeric_outputs[p].storage().end(),dst.numeric_outputs[p].storage().begin());
                    for(std::size_t axis=0;axis<2;++axis)if(dst.numeric_outputs[p].coordinates[axis].size()==src.numeric_outputs[p].coordinates[axis].size())
                        std::copy(src.numeric_outputs[p].coordinates[axis].begin(),src.numeric_outputs[p].coordinates[axis].end(),dst.numeric_outputs[p].coordinates[axis].begin());}
        }
        return;
    }
}
std::vector<NodeId> PreparedGraph::reset_nodes() const {
    std::vector<NodeId> result;
    for (const auto &n : impl_->nodes)
        if (n.reset)
            result.push_back(n.node.id);
    return result;
}

} // namespace dataflow_runtime
