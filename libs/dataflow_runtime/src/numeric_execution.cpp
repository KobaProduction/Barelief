#include "runtime_internal.h"
#include "preparation_support.h"
#include "signal_time.h"
#include "frame_access.h"
#include "mixer_execution.h"
#include "transform_execution.h"
#include <wled_protocol/wled_packet.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dataflow_runtime {
using namespace detail;
using namespace domain;
using namespace signal_contracts;
Status PreparedGraph::process_numeric_ports(NodeId ingress,const DataFrameView& input,RuntimeContext& c,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks) const noexcept {
    return process_numeric_impl(ingress,input,c,delivery,callbacks,nullptr,{},{},{});
}
Status PreparedGraph::process_numeric_ports(NodeId ingress,PortId port,const DataFrameView& input,RuntimeContext& c,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks) const noexcept {
    return process_numeric_impl(ingress,input,c,delivery,callbacks,nullptr,{},{},{},port);
}
Status PreparedGraph::process_numeric_impl(NodeId ingress,const DataFrameView& input,RuntimeContext& c,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks,
    const OwnedDataFrame* owned,std::optional<std::uint64_t> history_ordinal,std::shared_ptr<const void> retention_owner,std::optional<std::uint64_t> history_filled_count,PortId ingress_port) const noexcept {
    if(!impl_ || !impl_->numeric_graph || !c.impl_ || c.impl_->plan!=this)return Status::invalid_argument;
    const auto source_index=index(impl_->nodes,ingress);
    if(source_index==absent || (impl_->nodes[source_index].operation!=ProcessingOperation::Source && impl_->nodes[source_index].operation!=ProcessingOperation::Mixer))return Status::invalid_argument;
    const auto& source_node=impl_->nodes[source_index];auto& source_state=c.impl_->states[source_index];
    if(ingress_port>=source_node.numeric_outputs.size())return Status::invalid_argument;
    const auto& limits=impl_->numeric_profile.admission.frame;
    const auto actual=std::visit([](const auto& value)->std::optional<DataContract>{return value.contract?std::optional<DataContract>{*value.contract}:std::nullopt;},input.payload);
    if(!actual || !same_physical_contract(*actual,source_node.numeric_outputs[ingress_port]))return Status::incompatible_contract;
    const auto* history_input=std::get_if<NumericContract>(&*actual);
    const bool external_history=history_input && (std::get<NumericView>(input.payload).ring.capacity!=0 ||
        (history_ordinal && history_input->shape.rank==Rank::Matrix));
    if(external_history && (!owned || !retention_owner || !history_ordinal ||
        (input.stamp.available && !std::get<NumericView>(input.payload).ring.capacity)))return Status::invalid_argument;
    const auto real_filled=external_history?history_filled_count.value_or(input.stamp.available?std::get<NumericView>(input.payload).actual[0]:source_state.progress):0;
    if(external_history && (real_filled>(input.stamp.available?std::get<NumericView>(input.payload).actual[0]:history_input->shape.extents[0].maximum) || real_filled>*history_ordinal))return Status::invalid_argument;
    auto status=validate_view(input,limits);if(status!=Status::ok && status!=Status::unavailable)return status;
    if(input.stamp.available){
        if(const auto* value=std::get_if<NumericView>(&input.payload)){status=validate_values(*value,ValueDomain::AnyIeee,limits);if(status!=Status::ok)return status;}
        else {const auto& structured=std::get<StructuredView>(input.payload);if(packet_contract(*structured.contract) &&
            wled_protocol::validate_packet({reinterpret_cast<const std::uint8_t*>(structured.bytes.data()),structured.bytes.size()},{0,wled_protocol::packet_size})!=wled_protocol::Status::Ok)return Status::invalid_argument;}
    }
    if(source_state.source_markers[ingress_port]){const auto& old=source_state.source_stamps[ingress_port];
        if(external_history && source_state.history_origin_valid && input.stamp.producer_epoch==old.producer_epoch && input.stamp.generation==old.generation && !input.stamp.discontinuity){
            if(*history_ordinal<source_state.row_ordinal)return Status::invalid_argument;
            if(input.stamp.available){const auto& incoming=std::get<NumericView>(input.payload);const auto delta=*history_ordinal-source_state.row_ordinal;
                if(history_filled_count){
                    const auto expected=source_state.progress+std::min<std::uint64_t>(delta,incoming.ring.capacity-source_state.progress);
                    if(incoming.actual[0]!=incoming.ring.capacity || real_filled!=expected || incoming.ring.oldest!=(source_state.oldest+delta%incoming.ring.capacity)%incoming.ring.capacity)return Status::incompatible_contract;
                }else {
                const auto free=incoming.ring.capacity-source_state.frames;
                const auto overwritten=delta>free?delta-free:0;
                const auto expected=delta>=free?incoming.ring.capacity:source_state.frames+std::size_t(delta);
                if(incoming.actual[0]!=expected || incoming.ring.oldest!=(source_state.oldest+overwritten%incoming.ring.capacity)%incoming.ring.capacity)return Status::incompatible_contract;}}
        }
        if(input.stamp.producer_epoch==old.producer_epoch && (input.stamp.generation<old.generation ||
            (input.stamp.generation==old.generation && (input.stamp.available || input.stamp.sequence) && input.stamp.sequence<=old.sequence)))return Status::would_block;
        for(std::size_t i=0;i<source_state.retired_count;++i)if(source_state.retired[i]==input.stamp.producer_epoch)return Status::would_block;
        if(input.stamp.producer_epoch!=old.producer_epoch || input.stamp.generation!=old.generation || input.stamp.discontinuity){
            const auto old_epoch=old.producer_epoch;
            for(std::size_t i=0;i<impl_->nodes.size();++i)if(impl_->nodes[i].clock==source_index){
                // Each output lane has its own acceptance marker. Reset only
                // descendants of that lane, regardless of callback lane order.
                auto branch=i;
                while(impl_->nodes[branch].parent!=absent && impl_->nodes[branch].parent!=source_index)branch=impl_->nodes[branch].parent;
                if(impl_->nodes[branch].parent==source_index && impl_->nodes[branch].parent_port!=ingress_port)continue;
                if(impl_->nodes[i].operation==ProcessingOperation::History){c.impl_->states[i].pending_discontinuity=true;continue;}
                clear_node_state(impl_->nodes[i],c.impl_->states[i]);
            }
            if(input.stamp.producer_epoch!=old_epoch){source_state.retired[source_state.retired_next]=old_epoch;
                source_state.retired_next=(source_state.retired_next+1)%source_state.retired.size();
                source_state.retired_count=std::min(source_state.retired_count+1,source_state.retired.size());}
        }
    }
    if(external_history){
        source_state.history_origin_valid=true;
        if(!input.stamp.available){source_state.frames=history_input->shape.extents[0].maximum;source_state.progress=std::size_t(real_filled);source_state.row_ordinal=*history_ordinal;}
        if(*history_ordinal<source_state.row_ordinal)return Status::invalid_argument;
        auto previous_owner=source_state.ingress_owner;source_state.ingress_owner=std::move(retention_owner);source_state.owned_ingress=*owned;
    }
    if(!external_history && input.stamp.available && history_input && history_input->shape.rank==Rank::Matrix){
        source_state.owned_ingress={};source_state.ingress_owner.reset();source_state.history_origin_valid=false;
        source_state.frames=source_state.oldest=source_state.progress=0;source_state.row_ordinal=0;
    }
    source_state.numeric_marker=true;stamp_state(source_state,input.stamp);source_state.last_sequence=std::max(source_state.last_sequence,input.stamp.sequence);
    auto endpoint=[&](const PlanNode& n,PortDirection direction,PortId port,const DataFrameView& frame,Status s){
        const auto& cn=impl_->numeric_graph->nodes[n.configured];
        return NumericPortFrameView{impl_->numeric_graph->graph_id,{n.node.id,port},direction,cn.kind,
            direction==PortDirection::Input?n.numeric_input:n.numeric_outputs[port],frame,s,delivery};
    };
    auto gather=[&](const PlanNode& n,State& state,const DataFrameView& frame,NumericGatherOutput& out){
        const auto& plan=n.gather->gather;const auto& source=std::get<NumericView>(frame.payload);
        const auto actual=plan.input_contract().shape.rank==Rank::Matrix && plan.output_contract().shape.rank==Rank::Vector?std::array{source.actual[1],std::size_t{1}}:source.actual;
        NumericGatherDestination destination{{&plan.output_contract(),state.gather_payload,plan.output_planes(),actual,{},{}},state.gather_metadata};
        return gather_numeric_frame(plan,frame,{},destination,state.gather_scratch,out);
    };
    Status result=Status::ok;
    auto history_producer=[&](std::size_t at){
        while(at!=absent && impl_->nodes[at].operation!=ProcessingOperation::History && impl_->nodes[at].operation!=ProcessingOperation::Source &&
            (impl_->nodes[at].operation!=ProcessingOperation::Converter || impl_->nodes[at].identity_bypass)){
            const auto& parent=impl_->nodes[at];at=parent.parent!=absent?parent.parent:parent.numeric_clock_parent;}
        return at;};
    auto history_route=[&](State& state,const NumericView& source,std::size_t port,bool silence,bool seed,std::size_t producer,const FrameStamp& stamp){
        auto view=source;auto& mirror=state.route_masks[port];const auto capacity=source.ring.capacity;
        if(!capacity || mirror.size()!=capacity*2)return view;
        auto& continuity=state.route_state[port];
        const auto producer_id=producer==absent?NodeId{-1}:impl_->nodes[producer].node.id;
        const auto ordinal=producer==source_index && external_history?*history_ordinal:producer==absent?0:c.impl_->states[producer].row_ordinal;
        seed=seed || !continuity.valid || continuity.producer!=producer_id || continuity.epoch!=stamp.producer_epoch ||
            continuity.generation!=stamp.generation || stamp.discontinuity || ordinal<continuity.ordinal;
        const auto changed=seed?source.actual[0]:std::size_t(std::min(std::uint64_t(source.actual[0]),ordinal-continuity.ordinal));
        // Mirrored physical metadata publishes a contiguous logical mask without
        // shifting/copying the history payload or the mask on every row.
        for(std::size_t row=source.actual[0]-changed;row<source.actual[0];++row){const auto physical=(source.ring.oldest+row)%capacity;
            const auto masked=silence || source.semantic_silence || (!source.ring.silence_mask.empty() && source.ring.silence_mask[row]!=std::byte{0});
            mirror[physical]=mirror[physical+capacity]=masked?std::byte{1}:std::byte{0};}
        continuity={producer_id,ordinal,stamp.producer_epoch,stamp.generation,true};
        view.ring.silence_mask={mirror.data()+source.ring.oldest,capacity};view.semantic_silence=false;return view;
    };
    auto run=[&](auto&& self,std::size_t i,const DataFrameView& incoming,Status incoming_status=Status::ok,std::size_t selector_lane=0)->Status {
        const auto& n=impl_->nodes[i];auto& state=c.impl_->states[i];
        const auto& profile=impl_->numeric_profile;
        auto now=[&]() noexcept {return profile.monotonic_nanoseconds?profile.monotonic_nanoseconds(profile.measurement_clock):std::uint64_t{0};};
        const auto started=now();std::uint64_t child_time=0;
        struct Measurement {NodeStats& stats;const NumericRuntimeProfile& profile;std::uint64_t start;std::uint64_t& children;double deadline;std::uint64_t extra;
            ~Measurement(){if(!profile.monotonic_nanoseconds)return;const auto end=profile.monotonic_nanoseconds(profile.measurement_clock);const auto elapsed=end>=start?end-start:0;const auto self=(elapsed>=children?elapsed-children:0)+extra;stats.self_nanoseconds+=self;stats.max_self_nanoseconds=std::max(stats.max_self_nanoseconds,self);if(self>deadline)++stats.deadline_misses;std::size_t bin=0;for(auto v=self/1000;v>1 && bin<15;v>>=1)++bin;++stats.self_histogram[bin];}
        } measurement{state.stats,profile,started,child_time,1e9*double(impl_->numeric_graph->execution_interval.numerator)/double(impl_->numeric_graph->execution_interval.denominator),state.pending_self_nanoseconds};
        // Delivery adapters may retain/serialize a frame synchronously. That
        // work is outside the node's processing self-time, just like descendants.
        auto deliver=[&](auto callback,const auto& event){if(!callback)return;
            const auto begin=now();callback(callbacks.user,event);const auto end=now();
            if(end>=begin)child_time+=end-begin;};
        auto deliver_output=[&](const NumericPortFrameView& event){deliver(callbacks.output,event);};
        auto deliver_sink=[&](const NumericPortFrameView& event){deliver(callbacks.sink_input,event);};
        auto deliver_history=[&](const NumericHistoryRowView& event){deliver(callbacks.history,event);};
        state.pending_self_nanoseconds=0;
        state.stats.node=n.node.id;++state.stats.calls;
        const bool first=!state.numeric_marker;
        auto current=incoming;
        if(auto* numeric=std::get_if<NumericView>(&current.payload)){
            const auto& declaration=(n.operation==ProcessingOperation::Source||n.operation==ProcessingOperation::Mixer||n.operation==ProcessingOperation::ChannelComparison)?n.numeric_outputs[0]:n.numeric_input;
            if(n.operation!=ProcessingOperation::InputSelector&&!(n.operation==ProcessingOperation::Switch&&n.endpoint_plan))if(const auto* physical=std::get_if<NumericContract>(&declaration))numeric->contract=physical;
            if(n.operation==ProcessingOperation::Source && external_history && !current.stamp.available){
                numeric->actual[0]=history_input->shape.extents[0].maximum;
                numeric->ring={numeric->actual[0],state.oldest,{}};
            }
        }
        auto downstream_status=incoming_status;
        if(n.operation==ProcessingOperation::InputSelector&&!n.endpoint_plan){
            if(n.parent==absent){const auto& clock=std::get<NumericContract>(n.numeric_input).clock;
                current.stamp.available=!clock.domain.empty() && clock.sample_rate && clock.tick_seconds.numerator>0 && clock.tick_seconds.denominator;
                if(current.stamp.available)current.payload=zero_value(n.numeric_input,state.numeric_zero);
                else std::get<NumericView>(current.payload).storage={};
            }
            if(first || current.stamp.discontinuity ||
                current.stamp.producer_epoch!=state.numeric_stamp.producer_epoch || current.stamp.generation!=state.numeric_stamp.generation ||
                (current.stamp.available && !state.numeric_stamp.available))state.pending_discontinuity=true;
        }
        // Do not assemble a window from samples spanning a selector switch,
        // source restart or unavailable interval. Retained History stays intact.
        if(n.node.kind==NodeKind::Window && (current.stamp.discontinuity || !current.stamp.available ||
            (state.numeric_marker && !state.numeric_stamp.available && current.stamp.available))){
            state.progress=state.remainder=state.hop=0;
            std::fill(state.pcm.begin(),state.pcm.end(),0);
            state.pending_discontinuity=true;
        }
        if(n.node.kind==NodeKind::PcmResampler){
            auto& resampler=std::get<ResamplerKernel>(state.kernel).operation();
            if(current.stamp.discontinuity || !current.stamp.available || !resampler.input_contiguous(current.stamp.timestamp_ticks) ||
               (state.numeric_marker && (current.stamp.producer_epoch!=state.numeric_stamp.producer_epoch ||
                current.stamp.generation!=state.numeric_stamp.generation || (!state.numeric_stamp.available && current.stamp.available)))){
                resampler.reset();state.pending_discontinuity=true;
            }
        }
        const bool disconnected=n.parent==absent && n.numeric_clock_parent!=absent;
        if(disconnected){const auto* history=std::get_if<NumericView>(&current.payload);
            if(!history || !history->ring.capacity)current.payload=zero_value(n.numeric_input,state.numeric_zero);
        }
        if(!state.route_masks.empty() && std::get<NumericView>(current.payload).ring.capacity && (n.operation==ProcessingOperation::Sink || n.operation==ProcessingOperation::Reduce || n.operation==ProcessingOperation::Converter)){
            auto& value=std::get<NumericView>(current.payload);value=history_route(state,value,0,disconnected || (!n.node.enabled && !n.identity_bypass),first,
                history_producer(n.parent!=absent?n.parent:n.numeric_clock_parent),current.stamp);
        }
        if(n.parent!=absent){state.peak_hz=c.impl_->states[n.parent].peak_hz;state.magnitude=c.impl_->states[n.parent].magnitude;}
        // History owns continuity of its retained rows. Ancestor geometry can
        // restart without changing this History's contract/source clock. Real
        // source discontinuities clear its state and set pending_discontinuity.
        if(n.operation==ProcessingOperation::History)current.stamp.discontinuity=state.pending_discontinuity ||
            (n.clock!=absent && impl_->nodes[n.clock].operation==ProcessingOperation::InputSelector && incoming.stamp.discontinuity);
        if(state.pending_discontinuity && n.node.kind!=NodeKind::Window && n.node.kind!=NodeKind::PcmResampler){current.stamp.discontinuity=true;
            // An unavailable History marker does not append a row. Preserve the
            // boundary until the first retained row after recovery is emitted.
            if(n.operation!=ProcessingOperation::History || current.stamp.available)state.pending_discontinuity=false;}
        stamp_state(state,current.stamp);state.numeric_marker=true;
        if(!current.stamp.available || (std::holds_alternative<NumericView>(current.payload) && std::get<NumericView>(current.payload).semantic_silence))state.peak_hz=state.magnitude=0;
        if(n.operation==ProcessingOperation::Sink){
            if(current.stamp.available && !n.node.enabled && (!std::holds_alternative<NumericView>(current.payload) || !std::get<NumericView>(current.payload).ring.capacity)){
                auto silence=zero_value(n.numeric_input,state.numeric_zero);
                if(auto* numeric=std::get_if<NumericView>(&silence)){const auto& original=std::get<NumericView>(current.payload);
                    numeric->actual=original.actual;numeric->coordinates=original.coordinates;}
                else {auto& packet=std::get<StructuredView>(silence);packet.bytes=packet.bytes.first(std::get<StructuredView>(current.payload).bytes.size());}
                current.payload=silence;
            }
            const auto ep=endpoint(n,PortDirection::Input,0,current,current.stamp.available?Status::ok:incoming_status==Status::ok?Status::unavailable:incoming_status);
            if(callbacks.sink_input)deliver_sink(ep);
            if(n.gather && callbacks.history && std::get<NumericView>(current.payload).ring.capacity){const auto& source=std::get<NumericView>(current.payload);const auto producer=history_producer(n.parent!=absent?n.parent:n.numeric_clock_parent);
                const auto ordinal=producer==absent?std::uint64_t{0}:c.impl_->states[producer].row_ordinal;
                const auto filled=producer==absent?std::uint64_t{0}:std::uint64_t(c.impl_->states[producer].progress);
                const auto capacity=std::uint64_t(source.ring.capacity?source.ring.capacity:source.contract->shape.extents[0].maximum);
                auto event=!current.stamp.available?NumericHistoryEventKind::Unavailable:filled?NumericHistoryEventKind::Row:NumericHistoryEventKind::Empty;
                NumericGatherOutput compact;
                if(event==NumericHistoryEventKind::Row){const auto gathered=gather(n,state,current,compact);if(gathered!=Status::ok)return gathered;}
                deliver_history({event,ep,*source.contract,compact,filled,capacity,ordinal});
            }
            return Status::ok;
        }
        auto emit=[&](PortId p){auto& frame=state.numeric_frames[p];++state.stats.produced;
            if(n.operation==ProcessingOperation::InputSelector&&frame.stamp.available) {
                auto& source=std::get<NumericView>(frame.payload);auto destination=mutable_output(n,state,p);
                // A wider declared envelope needs its prepared backing storage.
                // Equal-capacity lanes remain borrowed; only the active samples
                // are copied when producer storage cannot cover that envelope.
                std::size_t required=0;const auto layout=required_storage(*destination.contract,source.planes,required);
                if(layout!=Status::ok||source.storage.size()<required) {
                    destination.actual=source.actual;destination.coordinates=source.coordinates;
                    Status copied=Status::ok;
                    for(std::size_t row=0;copied==Status::ok&&row<source.actual[0];++row){std::uint64_t bits=0;
                        copied=load_bits(source,{0,row,0},source.contract->dtype,bits);
                        if(copied==Status::ok)copied=store_bits(destination,{0,row,0},destination.contract->dtype,bits);}
                    if(copied!=Status::ok){result=copied;frame.stamp.available=false;downstream_status=copied;}
                    else source=destination.view();
                }
            }
            if(auto* numeric=std::get_if<NumericView>(&frame.payload))numeric->contract=&std::get<NumericContract>(n.numeric_outputs[p]);
            stamp_state(state,frame.stamp);
            if(frame.stamp.available){if(const auto* value=std::get_if<NumericView>(&frame.payload);value && value->contract->dtype==DType::Float32 &&
               (n.node.kind==NodeKind::StereoFFT || n.node.kind==NodeKind::SpectrumMerge || n.node.kind==NodeKind::SpectrumGain || n.node.kind==NodeKind::BarkWeighting) && value->contract->channel_order.size()==1){
                float peak=0;std::size_t peak_index=0;
                for(std::size_t j=0;j<value->actual[0];++j){float amplitude=0;if(load_element(*value,{0,j,0},amplitude)!=Status::ok)continue;
                    if(amplitude>peak){peak=amplitude;peak_index=j;}}
                state.magnitude=peak;state.peak_hz=0;const auto& axis=value->contract->coordinates[0];
                if(axis.mode==CoordinateMode::Regular && axis.unit==NumericUnit::Hertz)
                    state.peak_hz=float(double(axis.origin.numerator)/double(axis.origin.denominator)+double(peak_index)*double(axis.step.numerator)/double(axis.step.denominator));
            }}
            if(callbacks.output && !n.internal_conversion)deliver_output(endpoint(n,PortDirection::Output,p,frame,frame.stamp.available?Status::ok:downstream_status==Status::ok?Status::unavailable:downstream_status));
            for(const auto& sink:impl_->nodes)if(sink.scalar_view())for(const auto& input:sink.endpoint_plan->inputs)
                if((input.parent==i && input.parent_port==p) || (input.parent==absent && input.clock==i && p==0)){
                    auto delivered=frame;const auto& contract=std::get<NumericContract>(input.contract);
                    const bool silence=!sink.node.enabled || input.parent==absent;
                    alignas(std::max_align_t) std::array<std::byte,8> bytes{};const std::array<PlaneLayout,1> planes{{{0,{0,0}}}};
                    if(silence){MutableNumericView zero{&contract,bytes,planes,{1,1},{},{}};
                        if(input.parent==absent && !input.clock_known)delivered.stamp.available=false;
                        const auto filled=fill_silence(zero,limits);if(filled!=Status::ok){result=filled;continue;}auto value=zero.view();if(!delivered.stamp.available)value.storage={};delivered.payload=value;}
                    else if(auto* value=std::get_if<NumericView>(&delivered.payload))value->contract=&contract;
                    if(callbacks.sink_input)deliver_sink({impl_->numeric_graph->graph_id,{sink.node.id,input.port},PortDirection::Input,
                        impl_->numeric_graph->nodes[sink.configured].kind,input.contract,delivered,delivered.stamp.available?Status::ok:downstream_status==Status::ok?Status::unavailable:downstream_status,delivery});
                }
            for(const auto& route:n.mixer_destinations)if(route.output==p){
                auto send_consumer=[&](const DataFrameView& produced,Status status){return self(self,route.node,produced,status);};
                MixerDelivery delivery{&send_consumer,[](void* user,const DataFrameView& produced,Status status){
                    return (*static_cast<decltype(send_consumer)*>(user))(produced,status);
                },child_time};
                const auto accepted=accept_mixer_contribution(impl_->nodes[route.node],c.impl_->states[route.node],route.input,frame,*impl_->numeric_graph,profile,delivery);
                if(accepted!=Status::ok)result=accepted;
            }
            for(auto child:n.children)if(!impl_->nodes[child].scalar_view() && impl_->nodes[child].parent_port==p){const auto child_begin=now();const auto sent=self(self,child,frame,downstream_status);const auto child_end=now();if(child_end>=child_begin)child_time+=child_end-child_begin;if(sent!=Status::ok)result=sent;}
            for(const auto& route:n.selector_destinations)if(route.output==p){const auto child_begin=now();
                const auto sent=self(self,route.node,frame,downstream_status,route.input);const auto child_end=now();
                if(child_end>=child_begin)child_time+=child_end-child_begin;
                if(sent!=Status::ok)result=sent;}
        };
        if(n.operation==ProcessingOperation::Switch&&n.endpoint_plan){const auto& params=std::get<RouteProcessingParameters>(impl_->numeric_graph->nodes[n.configured].parameters);
            auto forward=[&](std::size_t lane,bool missing){for(std::size_t output=lane;output<n.numeric_outputs.size();output+=params.channels_per_stream){auto frame=current;
                frame.stamp.discontinuity=frame.stamp.discontinuity||!state.source_markers[output]||state.source_stamps[output].generation!=frame.stamp.generation||state.source_stamps[output].producer_epoch!=frame.stamp.producer_epoch;
                if(missing||!n.node.enabled||!params.open||output/params.channels_per_stream!=params.selected){auto zero=std::get<NumericView>(zero_value(n.numeric_outputs[output],state.numeric_outputs[output]));const auto& original=std::get<NumericView>(current.payload);zero.actual=original.actual;zero.coordinates=original.coordinates;frame.payload=zero;}
                state.numeric_frames[output]=frame;state.source_stamps[output]=frame.stamp;state.source_stamps[output].stream_id={};state.source_markers[output]=true;emit(PortId(output));}};
            forward(selector_lane,n.endpoint_plan->inputs[selector_lane].parent==absent);
            std::size_t anchor=0;while(anchor<n.endpoint_plan->inputs.size()&&n.endpoint_plan->inputs[anchor].parent==absent)++anchor;
            if(selector_lane==(anchor==n.endpoint_plan->inputs.size()?0:anchor))for(std::size_t lane=0;lane<n.endpoint_plan->inputs.size();++lane)if(lane!=selector_lane&&n.endpoint_plan->inputs[lane].parent==absent)forward(lane,true);
            return Status::ok;
        }
        if(n.operation==ProcessingOperation::InputSelector&&n.endpoint_plan) {
            auto forward=[&](std::size_t lane,bool missing){auto frame=current;
                const bool initial=!state.source_markers[lane];
                frame.stamp.discontinuity=frame.stamp.discontinuity||initial||
                    state.source_stamps[lane].producer_epoch!=frame.stamp.producer_epoch||
                    state.source_stamps[lane].generation!=frame.stamp.generation;
                if(missing||!n.node.enabled){auto zero=std::get<NumericView>(zero_value(n.numeric_outputs[lane],state.numeric_zero));
                    const auto& original=std::get<NumericView>(current.payload);zero.actual=original.actual;zero.coordinates=original.coordinates;frame.payload=zero;}
                if(missing){const auto& clock=std::get<NumericContract>(n.numeric_outputs[lane]).clock;
                    frame.stamp.available=!clock.domain.empty()&&clock.sample_rate;}
                state.numeric_frames[lane]=frame;state.source_stamps[lane]=frame.stamp;
                state.source_stamps[lane].stream_id={};state.source_markers[lane]=true;emit(PortId(lane));
            };
            const auto& input=n.endpoint_plan->inputs[selector_lane];
            forward(selector_lane,input.parent==absent);
            // Missing lanes follow one selected producer, never every ingress.
            std::size_t anchor=0;while(anchor<n.endpoint_plan->inputs.size()&&n.endpoint_plan->inputs[anchor].parent==absent)++anchor;
            if(selector_lane==(anchor==n.endpoint_plan->inputs.size()?0:anchor))
                for(std::size_t lane=0;lane<n.endpoint_plan->inputs.size();++lane)
                    if(lane!=selector_lane&&n.endpoint_plan->inputs[lane].parent==absent)forward(lane,true);
            return Status::ok;
        }
        auto marker_stamp=current.stamp;
        if(n.node.kind==NodeKind::PcmResampler && !convert_rate_ticks(current.stamp.timestamp_ticks,
            std::get<NumericContract>(n.numeric_input).clock.sample_rate,
            std::get<NumericContract>(n.numeric_outputs[0]).clock.sample_rate,marker_stamp.timestamp_ticks)){
            marker_stamp.timestamp_ticks=0;marker_stamp.available=false;downstream_status=Status::overflow;result=Status::overflow;
        }
        for(std::size_t p=0;p<state.numeric_frames.size();++p){auto& frame=state.numeric_frames[p];frame.stamp=marker_stamp;frame.stamp.available=false;
            if(const auto* structured=std::get_if<StructuredContract>(&n.numeric_outputs[p])){frame.payload=StructuredView{structured,{}};continue;}
            auto dst=mutable_output(n,state,PortId(p));
            if(n.operation==ProcessingOperation::History || state.history_origin_valid){dst.ring={dst.actual[0],state.oldest,{}};dst.actual[0]=state.frames;}
            auto unavailable=dst.view();unavailable.storage={};frame.payload=unavailable;
        }
        bool marker_sequenced=false;
        auto sequence_marker=[&](){
            if(n.node.kind!=NodeKind::PcmResampler || marker_sequenced)return true;
            if(state.output_sequence==UINT64_MAX)return false;
            marker_stamp.sequence=++state.output_sequence;
            if(!marker_stamp.source_sequence)marker_stamp.source_sequence=current.stamp.sequence;
            marker_stamp.discontinuity=marker_stamp.discontinuity || state.pending_discontinuity;
            marker_sequenced=true;
            for(auto& frame:state.numeric_frames){frame.stamp=marker_stamp;frame.stamp.available=false;}
            return true;
        };
        auto reject=[&](Status cause){++state.stats.errors;result=cause;downstream_status=cause;
            if(n.node.kind==NodeKind::PcmResampler){std::get<ResamplerKernel>(state.kernel).operation().reset();state.pending_discontinuity=true;}
            if(!sequence_marker())return Status::overflow;
            for(std::size_t p=0;p<state.numeric_frames.size();++p){auto& frame=state.numeric_frames[p];frame.stamp=marker_stamp;frame.stamp.available=false;
                if(auto* numeric=std::get_if<NumericView>(&frame.payload))numeric->storage={};else std::get<StructuredView>(frame.payload).bytes={};
                emit(PortId(p));}
            return Status::ok;
        };
        if(!current.stamp.available){
            if(!sequence_marker())return Status::overflow;
            if(n.operation==ProcessingOperation::History && callbacks.history){auto dst=mutable_output(n,state,0);dst.actual={state.frames,dst.actual[1]};dst.ring={state.numeric_outputs[0].actual[0],state.oldest,{}};
                auto unavailable=dst.view();unavailable.storage={};state.numeric_frames[0].payload=unavailable;
                deliver_history({NumericHistoryEventKind::Unavailable,endpoint(n,PortDirection::Output,0,state.numeric_frames[0],downstream_status==Status::ok?Status::unavailable:downstream_status),std::get<NumericContract>(n.numeric_outputs[0]),{},std::uint64_t(state.progress),std::uint64_t(dst.ring.capacity),state.row_ordinal});}
        }else if(n.identity_bypass){
            state.numeric_frames[0]=current;
            if(n.operation==ProcessingOperation::Converter && n.gather && std::get<NumericView>(current.payload).ring.capacity){const auto producer=history_producer(n.parent!=absent?n.parent:n.numeric_clock_parent);
                if(producer!=absent){state.row_ordinal=c.impl_->states[producer].row_ordinal;state.progress=c.impl_->states[producer].progress;}
                const auto& view=std::get<NumericView>(current.payload);state.frames=view.actual[0];state.oldest=view.ring.oldest;}
        }else if(n.operation==ProcessingOperation::Mixer || n.operation==ProcessingOperation::ChannelComparison){state.numeric_frames[0]=current;
        }else if(n.operation==ProcessingOperation::Extract){
            const auto& source=std::get<NumericView>(current.payload);auto output=mutable_output(n,state,0);std::uint64_t bits=0;
            if(n.node.enabled){const auto& index=n.endpoint_plan->indices;status=load_bits(source,{index[0],index[1],index[2]},source.contract->dtype,bits);if(status!=Status::ok)return reject(status);
                status=store_bits(output,{0,0,0},output.contract->dtype,bits);
            }else status=fill_silence(output,limits);
            if(status!=Status::ok)return reject(status);
            state.numeric_frames[0]={current.stamp,output.view()};
        }else if(n.operation==ProcessingOperation::InputSelector){
            state.numeric_frames[0]=current;
            if(!n.node.enabled){auto silence=std::get<NumericView>(zero_value(n.numeric_outputs[0],state.numeric_outputs[0]));
                const auto& original=std::get<NumericView>(current.payload);silence.actual=original.actual;silence.coordinates=original.coordinates;
                state.numeric_frames[0].payload=silence;}
        }else if(n.operation==ProcessingOperation::Source || n.operation==ProcessingOperation::Gate || n.operation==ProcessingOperation::Switch || n.operation==ProcessingOperation::Splitter){
            bool open=impl_->numeric_graph->nodes[n.configured].enabled;std::uint16_t selected=0;
            if(n.operation!=ProcessingOperation::Source){const auto& p=std::get<RouteProcessingParameters>(impl_->numeric_graph->nodes[n.configured].parameters);open=open&&p.open;selected=p.selected;}
            for(std::size_t p=0;p<state.numeric_frames.size();++p){auto& out=state.numeric_frames[p];out=current;
                if(!state.route_masks.empty() && std::get<NumericView>(current.payload).ring.capacity){
                    out.payload=history_route(state,std::get<NumericView>(current.payload),p,disconnected || !open || (n.operation==ProcessingOperation::Switch && p!=selected),first,
                        history_producer(n.operation==ProcessingOperation::Source?i:n.parent!=absent?n.parent:n.numeric_clock_parent),current.stamp);
                    continue;
                }
                if(!open || (n.operation==ProcessingOperation::Switch && p!=selected)){
                    out.payload=zero_value(n.numeric_outputs[p],state.numeric_outputs[p]);
                    if(auto* silenced=std::get_if<NumericView>(&out.payload)){const auto& input_value=std::get<NumericView>(current.payload);
                        silenced->actual=input_value.actual;silenced->coordinates=input_value.coordinates;}
                }}
        }else if(n.operation==ProcessingOperation::Converter){const auto& src=std::get<NumericView>(current.payload);auto dst=mutable_output(n,state,0);dst.actual=src.actual;dst.coordinates=src.coordinates;dst.ring=src.ring;
            if(!src.ring.capacity){state.history_origin_valid=false;state.frames=state.oldest=state.progress=0;state.row_ordinal=0;}
            if(src.ring.capacity){
                const auto producer=history_producer(n.parent!=absent?n.parent:n.numeric_clock_parent);
                if(producer==absent)return reject(Status::unavailable);
                const auto ordinal=c.impl_->states[producer].row_ordinal;
                const auto origin=impl_->nodes[producer].node.id;
                const bool rebase=!state.history_origin_valid || state.history_origin!=origin || current.stamp.discontinuity || ordinal<state.row_ordinal;
                const auto changed=rebase?src.actual[0]:std::size_t(std::min(std::uint64_t(src.actual[0]),ordinal-state.row_ordinal));
                const auto& plan=n.input_gather->gather;auto row_contract=n.row_conversion->output.contract();
                status=Status::ok;
                for(std::size_t logical=src.actual[0]-changed;logical<src.actual[0];++logical){
                    NumericGatherOutput row;NumericGatherDestination destination{{&plan.output_contract(),state.input_gather_payload,plan.output_planes(),{src.actual[1],1},{},{}},state.input_gather_metadata};
                    status=gather_numeric_frame(plan,current,{false,logical},destination,state.input_gather_scratch,row);if(status!=Status::ok)return reject(status);
                    const auto physical=(src.ring.oldest+logical)%src.ring.capacity;
                    for(std::size_t plane=0;plane<dst.planes.size();++plane)state.row_planes[plane]={dst.planes[plane].offset+physical*dst.planes[plane].stride_bytes[0],{dst.planes[plane].stride_bytes[1],0}};
                    MutableNumericView row_output{&row_contract,dst.storage,state.row_planes,{src.actual[1],1},{},std::get<NumericView>(row.frame.payload).coordinates};
                    status=n.node.enabled?numeric_kernel_status(dsp_kernels::convert(*n.row_conversion,std::get<NumericView>(row.frame.payload),row_output,state.numeric_scratch).status):fill_silence(row_output,limits);
                    if(status!=Status::ok){result=status;downstream_status=status;break;}
                    if(!src.coordinates.axes[0].empty()){auto& coords=state.numeric_outputs[0].coordinates[0];
                        const auto coordinate=src.coordinates.axes[0].subspan(logical*8,8);
                        std::copy(coordinate.begin(),coordinate.end(),coords.begin()+physical*8);
                        std::copy(coordinate.begin(),coordinate.end(),coords.begin()+(physical+src.ring.capacity)*8);}
                }
                if(status==Status::ok){state.frames=src.actual[0];state.oldest=src.ring.oldest;state.row_ordinal=ordinal;state.progress=c.impl_->states[producer].progress;state.history_origin=origin;state.history_origin_valid=true;}dst.ring.silence_mask={};
                if(!src.coordinates.axes[0].empty())dst.coordinates.axes[0]={state.numeric_outputs[0].coordinates[0].data()+state.oldest*8,state.frames*8};
            }else status=n.node.enabled?numeric_kernel_status(dsp_kernels::convert(*n.conversion,src,dst,state.numeric_scratch).status):fill_silence(dst,limits);
            const auto feature=src.ring.capacity?1u:0u;
            if(!src.coordinates.axes[feature].empty()){auto& coords=state.numeric_outputs[0].coordinates[feature];
                std::copy(src.coordinates.axes[feature].begin(),src.coordinates.axes[feature].end(),coords.begin());dst.coordinates.axes[feature]={coords.data(),src.coordinates.axes[feature].size()};}
            if(status==Status::ok)state.numeric_frames[0]={current.stamp,dst.view()};else {result=status;downstream_status=status;}
        }else if(n.operation==ProcessingOperation::Transform){
            auto emit_transform=[&](){emit(0);};
            struct Delivery {decltype(emit_transform)& emit;decltype(reject)& reject_frame;};
            Delivery delivery{emit_transform,reject};
            const auto transformed=execute_transform(n,state,current,limits,{&delivery,
                [](void* user){static_cast<Delivery*>(user)->emit();},
                [](void* user,Status status){return static_cast<Delivery*>(user)->reject_frame(status);}});
            if(transformed.completed)return transformed.status;
            if(transformed.status!=Status::ok){++state.stats.errors;result=transformed.status;downstream_status=transformed.status;}
        }else if(n.operation==ProcessingOperation::WLEDEncoder){const auto& source=std::get<NumericView>(current.payload);auto& packet=state.numeric_outputs[0];
            if(!n.node.enabled){const auto silent=wled_protocol::silence(0,{reinterpret_cast<std::uint8_t*>(packet.bytes.data()),packet.bytes.size()},{0,wled_protocol::packet_size});
                status=silent.status==wled_protocol::Status::Ok?Status::ok:Status::invalid_argument;}
            else {const auto count=source.actual[0];for(std::size_t j=0;j<count;++j){status=load_element(source,{0,j,0},state.values[j]);if(status!=Status::ok)return reject(status);}
                const auto encoded=wled_protocol::encode_mel({state.values.data(),count},state.peak_hz,state.magnitude,n.gain,0,
                    {reinterpret_cast<std::uint8_t*>(packet.bytes.data()),packet.bytes.size()},{state.values.size(),wled_protocol::packet_size});
                status=encoded.status==wled_protocol::Status::Ok?Status::ok:encoded.status==wled_protocol::Status::NonFiniteInput ||
                    encoded.status==wled_protocol::Status::NonFiniteResult?Status::overflow:Status::invalid_argument;
            }
            if(status==Status::ok){auto& output=state.numeric_frames[0];output.stamp=current.stamp;
                output.payload=StructuredView{&std::get<StructuredContract>(n.numeric_outputs[0]),packet.storage()};}
            else {result=status;downstream_status=status;}
        }else if(n.operation==ProcessingOperation::Reduce){NumericGatherOutput compact;status=n.node.enabled?gather(n,state,current,compact):Status::ok;
            auto dst=mutable_output(n,state,0);
            if(status==Status::ok)status=n.node.enabled?numeric_kernel_status(dsp_kernels::reduce(*n.reduction,std::get<NumericView>(compact.frame.payload),dst,state.numeric_scratch).status):fill_silence(dst,limits);
            if(status==Status::ok)state.numeric_frames[0]={current.stamp,dst.view()};else {downstream_status=status;if(status!=Status::unavailable)result=status;}
        }else if(n.operation==ProcessingOperation::History){auto dst=mutable_output(n,state,0);const auto& src=std::get<NumericView>(current.payload);const auto capacity=dst.actual[0];
            dst.ring={capacity,0,{}}; // physical write view; publication below uses logical oldest.
            if(state.row_ordinal==UINT64_MAX || state.output_sequence==UINT64_MAX){result=Status::overflow;downstream_status=Status::overflow;}
            else {
                const auto physical=state.oldest;
                const auto zero=std::get<NumericView>(zero_value(n.numeric_input,state.numeric_zero));
                for(std::size_t p=0;p<dst.planes.size();++p)for(std::size_t col=0;col<src.actual[0];++col){std::uint64_t bits=0;
                    status=load_bits(n.node.enabled?src:zero,{p,col,0},src.contract->dtype,bits);if(status!=Status::ok)return reject(status);
                    status=store_bits(dst,{p,physical,col},dst.contract->dtype,bits);if(status!=Status::ok)return reject(status);}
                state.frames=capacity;state.progress=std::min(state.progress+1,capacity);state.oldest=(state.oldest+1)%capacity;
                ++state.row_ordinal;dst.actual={state.frames,src.actual[0]};dst.ring={capacity,state.oldest,{}};
                auto& buffer=state.numeric_outputs[0];
                if(dst.contract->coordinates[1].mode==CoordinateMode::ExplicitFloat64LE){
                    std::copy(src.coordinates.axes[0].begin(),src.coordinates.axes[0].end(),buffer.coordinates[1].begin());
                    dst.coordinates.axes[1]={buffer.coordinates[1].data(),src.actual[0]*8};}
                if(dst.contract->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE){
                    const double coordinate=dst.contract->coordinates[0].unit==NumericUnit::Frames?double(state.row_ordinal):
                        double(current.stamp.timestamp_ticks)*double(src.contract->clock.tick_seconds.numerator)/double(src.contract->clock.tick_seconds.denominator);
                    if(!std::isfinite(coordinate))return reject(Status::overflow);
                    std::uint64_t bits=0;std::memcpy(&bits,&coordinate,8);
                    for(std::size_t byte=0;byte<8;++byte)buffer.coordinates[0][physical*8+byte]=buffer.coordinates[0][(physical+capacity)*8+byte]=std::byte((bits>>(byte*8))&255);
                    dst.coordinates.axes[0]={buffer.coordinates[0].data()+state.oldest*8,state.frames*8};}
                auto stamp=current.stamp;stamp.sequence=++state.output_sequence;
                state.numeric_frames[0]={stamp,dst.view()};auto count=mutable_output(n,state,1);
                status=store_element(count,{0,0,0},std::uint64_t(state.progress));if(status!=Status::ok)return reject(status);
                state.numeric_frames[1]={stamp,count.view()};
                if(callbacks.history){NumericGatherOutput compact;status=gather(n,state,state.numeric_frames[0],compact);
                    if(status!=Status::ok)return reject(status);
                    deliver_history({NumericHistoryEventKind::Row,endpoint(n,PortDirection::Output,0,state.numeric_frames[0],Status::ok),std::get<NumericContract>(n.numeric_outputs[0]),compact,std::uint64_t(state.progress),std::uint64_t(capacity),state.row_ordinal});}
            }
        }
        if(n.operation==ProcessingOperation::Source && n.gather && current.stamp.available && external_history){
            const auto& value=std::get<NumericView>(state.numeric_frames[0].payload);
            state.row_ordinal=*history_ordinal;state.frames=value.actual[0];state.progress=std::size_t(real_filled);state.oldest=value.ring.oldest;
        }
        if(n.gather && !n.internal_conversion && (n.operation==ProcessingOperation::Source || n.operation==ProcessingOperation::Converter) && callbacks.history && std::get<NumericView>(state.numeric_frames[0].payload).ring.capacity){
            const auto& frame=state.numeric_frames[0];const auto& value=std::get<NumericView>(frame.payload);
            auto event=!frame.stamp.available?NumericHistoryEventKind::Unavailable:state.progress?NumericHistoryEventKind::Row:NumericHistoryEventKind::Empty;
            NumericGatherOutput compact;
            if(event==NumericHistoryEventKind::Row){status=gather(n,state,frame,compact);if(status!=Status::ok)return status;}
            deliver_history({event,endpoint(n,PortDirection::Output,0,frame,frame.stamp.available?Status::ok:Status::unavailable),*value.contract,compact,std::uint64_t(state.progress),std::uint64_t(value.ring.capacity),state.row_ordinal});
        }
        for(std::size_t p=0;p<state.numeric_frames.size();++p)if(n.operation!=ProcessingOperation::Source || p==ingress_port)emit(PortId(p));
        return Status::ok;
    };
    source_state.source_stamps[ingress_port]=input.stamp;source_state.source_stamps[ingress_port].stream_id={};source_state.source_markers[ingress_port]=true;
    status=run(run,source_index,input);if(status!=Status::ok)return status;
    for(std::size_t i=0;i<impl_->nodes.size();++i){const auto& n=impl_->nodes[i];if(n.operation==ProcessingOperation::Clock || n.operation==ProcessingOperation::Mixer || n.operation==ProcessingOperation::ChannelComparison || n.scalar_view() || n.operation==ProcessingOperation::Source || n.parent!=absent || n.numeric_clock_parent!=absent ||
        (n.clock!=source_index && n.clock!=absent))continue;
        const auto& zero=c.impl_->states[i].numeric_zero;
        DataFrameView current{input.stamp,zero_value(n.numeric_input,zero)};
        if(n.clock==absent || n.operation==ProcessingOperation::InputSelector){current.stamp.available=false;
            if(auto* numeric=std::get_if<NumericView>(&current.payload)){numeric->storage={};if(numeric->ring.capacity)numeric->actual[0]=0;}
            else std::get<StructuredView>(current.payload).bytes={};}
        status=run(run,i,current);if(status!=Status::ok)return status;
    }
    return result;
}
Status PreparedGraph::process_numeric_owned_ports(NodeId ingress,const OwnedDataFrame& owned,RuntimeContext& context,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks,
    std::optional<std::uint64_t> history_ordinal,std::shared_ptr<const void> retention_owner,std::optional<std::uint64_t> history_filled_count) const noexcept {
    if(!owned || !context.impl_ || context.impl_->plan!=this)return Status::invalid_argument;
    const auto at=index(impl_->nodes,ingress);if(at==absent || impl_->nodes[at].operation!=ProcessingOperation::Source)return Status::invalid_argument;
    return process_numeric_impl(ingress,owned.view(),context,delivery,callbacks,&owned,history_ordinal,std::move(retention_owner),history_filled_count);
}

} // namespace dataflow_runtime
