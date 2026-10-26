#include "runtime_internal.h"
#include "preparation_support.h"
#include "signal_time.h"
#include "frame_access.h"
#include "mixer_execution.h"
#include "transform_execution.h"
#include "matrix_execution.h"
#include "value_execution.h"
#include "history_execution.h"
#include "conversion_execution.h"
#include "ingress_acceptance.h"
#include "port_delivery.h"
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
namespace {
struct RuntimeDispatch {
    std::span<const PlanNode> nodes;
    std::span<State> states;
    const NumericProcessingGraph& graph;
    const NumericRuntimeProfile& profile;
    std::span<const RoutingAlias> aliases;
    DeliveryStamp delivery;
    NumericRuntimeCallbacks callbacks;
    const DataFrameView& input;
    const std::size_t source_index;
    const PortId ingress_port;
    const NumericContract* history_input;
    const bool external_history;
    const std::optional<std::uint64_t> history_ordinal;
    const std::uint64_t real_filled;
    Status result=Status::ok;
    NumericPortFrameView endpoint(const PlanNode& n,PortDirection direction,PortId port,const DataFrameView& frame,Status s) const noexcept {
        const auto& cn=graph.nodes[n.configured];
        const DataContract* contract=direction==PortDirection::Input?&n.numeric_input:&n.numeric_outputs[port];
        if(direction==PortDirection::Input && n.endpoint_plan)
            for(const auto& input:n.endpoint_plan->inputs)if(input.port==port){contract=&input.contract;break;}
        return NumericPortFrameView{graph.graph_id,{n.node.id,port},direction,cn.kind,*contract,frame,s,delivery};
    }
    Status gather(const PlanNode& n,State& state,const DataFrameView& frame,NumericGatherOutput& out) const noexcept {
        const auto& plan=n.gather->gather;const auto& source=std::get<NumericView>(frame.payload);
        const auto actual=plan.input_contract().shape.rank==Rank::Matrix && plan.output_contract().shape.rank==Rank::Vector?std::array{source.actual[1],std::size_t{1}}:source.actual;
        NumericGatherDestination destination{{&plan.output_contract(),state.gather_payload,plan.output_planes(),actual,{},{}},state.gather_metadata};
        return gather_numeric_frame(plan,frame,{},destination,state.gather_scratch,out);

    }
    std::size_t history_producer(std::size_t at) const noexcept {
        while(at!=absent && nodes[at].operation!=ProcessingOperation::History && nodes[at].operation!=ProcessingOperation::Source &&
            (nodes[at].operation!=ProcessingOperation::Converter || nodes[at].identity_bypass)){
            const auto& parent=nodes[at];at=parent.parent!=absent?parent.parent:parent.numeric_clock_parent;}
        return at;
    }
    std::uint64_t history_ordinal_for(std::size_t producer) const noexcept {
        return producer==source_index && external_history?*history_ordinal:
            producer==absent?0:states[producer].row_ordinal;
    }
    void update_history_mask(std::span<std::byte> mirror,const NumericView& source,bool silence,
                             std::size_t changed) const noexcept {
        const auto capacity=source.ring.capacity;
        for(std::size_t row=source.actual[0]-changed;row<source.actual[0];++row) {
            const auto physical=(source.ring.oldest+row)%capacity;
            const auto masked=silence || source.semantic_silence ||
                (!source.ring.silence_mask.empty() && source.ring.silence_mask[row]!=std::byte{0});
            mirror[physical]=mirror[physical+capacity]=masked?std::byte{1}:std::byte{0};
        }
    }
    NumericView history_route(State& state,const NumericView& source,std::size_t port,bool silence,bool seed,std::size_t producer,const FrameStamp& stamp) const noexcept {
        auto view=source;auto& mirror=state.route_masks[port];const auto capacity=source.ring.capacity;
        if(!capacity || mirror.size()!=capacity*2)return view;
        auto& continuity=state.route_state[port];
        const auto producer_id=producer==absent?NodeId{-1}:nodes[producer].node.id;
        const auto ordinal=history_ordinal_for(producer);
        seed=seed || !continuity.valid || continuity.producer!=producer_id || continuity.epoch!=stamp.producer_epoch ||
            continuity.generation!=stamp.generation || stamp.discontinuity || ordinal<continuity.ordinal;
        const auto changed=seed?source.actual[0]:std::size_t(std::min(std::uint64_t(source.actual[0]),ordinal-continuity.ordinal));
        // Mirrored physical metadata publishes a contiguous logical mask without
        // shifting/copying the history payload or the mask on every row.
        update_history_mask(mirror,source,silence,changed);
        continuity={producer_id,ordinal,stamp.producer_epoch,stamp.generation,true};
        view.ring.silence_mask={mirror.data()+source.ring.oldest,capacity};view.semantic_silence=false;return view;

    }
    Status run(std::size_t,const DataFrameView&,Status=Status::ok,std::size_t=0) noexcept;
    bool needs_clock_update(const PlanNode& node) const noexcept {
        return node.operation!=ProcessingOperation::Clock && node.operation!=ProcessingOperation::Mixer && node.operation!=ProcessingOperation::ChannelComparison && !node.scalar_view() && node.operation!=ProcessingOperation::Source && node.parent==absent && node.numeric_clock_parent==absent && (node.clock==source_index || node.clock==absent);
    }
    Status process_disconnected(std::size_t at) noexcept {
        const auto& node=nodes[at];DataFrameView frame{input.stamp,zero_value(node.numeric_input,states[at].numeric_zero)};
        if(node.clock==absent || node.operation==ProcessingOperation::InputSelector){
            frame.stamp.available=false;
            if(auto* value=std::get_if<NumericView>(&frame.payload)){value->storage={};if(value->ring.capacity)value->actual[0]=0;}
            else std::get<StructuredView>(frame.payload).bytes={};
        }
        return run(at,frame);
    }
    Status process() noexcept {
        auto& source=states[source_index];source.source_stamps[ingress_port]=input.stamp;source.source_stamps[ingress_port].stream_id={};source.source_markers[ingress_port]=true;
        {const auto status=run(source_index,input);if(status!=Status::ok)return status;}
        for(std::size_t at=0;at<nodes.size();++at)if(needs_clock_update(nodes[at])){const auto status=process_disconnected(at);if(status!=Status::ok)return status;}
        return result;
    }

};
class NodeRun {
    RuntimeDispatch& runtime;
    const std::size_t i;
    const PlanNode& n;
    State& state;
    const NumericRuntimeProfile& profile;
    const FrameLimits& limits;
    const NumericRuntimeCallbacks callbacks;
    const DeliveryStamp delivery;
    Status& result;
    const PortId ingress_port;
    const NumericContract* history_input;
    const bool external_history;
    const std::optional<std::uint64_t> history_ordinal;
    const std::uint64_t real_filled;
    const DataFrameView& incoming;
    const std::size_t selector_lane;
    const bool first;
    DataFrameView current;
    Status downstream_status,status=Status::ok;
    std::uint64_t child_time=0;
    PortDelivery port_delivery;
    bool disconnected=false,marker_sequenced=false;
    FrameStamp marker_stamp;
    std::uint64_t now() const noexcept {return measurement_now(profile);}
    NumericPortFrameView endpoint(const PlanNode& node,PortDirection direction,PortId port,const DataFrameView& frame,Status status) const noexcept {return runtime.endpoint(node,direction,port,frame,status);}
    Status gather(const PlanNode& node,State& state,const DataFrameView& frame,NumericGatherOutput& out) const noexcept {return runtime.gather(node,state,frame,out);}
    std::size_t history_producer(std::size_t at) const noexcept {return runtime.history_producer(at);}
    NumericView history_route(State& state,const NumericView& source,std::size_t port,bool silence,bool seed,std::size_t producer,const FrameStamp& stamp) const noexcept {return runtime.history_route(state,source,port,silence,seed,producer,stamp);}
    Status copy_selector_values(const NumericView& source,MutableNumericView& destination) const noexcept {
        for(std::size_t row=0;row<source.actual[0];++row){
            std::uint64_t bits=0;auto status=load_bits(source,{0,row,0},source.contract->dtype,bits);if(status!=Status::ok)return status;
            status=store_bits(destination,{0,row,0},destination.contract->dtype,bits);if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
    void selector_storage(DataFrameView& frame,PortId port) noexcept {
        if(n.operation!=ProcessingOperation::InputSelector || !frame.stamp.available)return;
        auto& source=std::get<NumericView>(frame.payload);auto destination=mutable_output(n,state,port);
        std::size_t required=0;const auto layout=required_storage(*destination.contract,source.planes,required);
        if(layout==Status::ok && source.storage.size()>=required)return;
        destination.actual=source.actual;destination.coordinates=source.coordinates;
        const auto copied=copy_selector_values(source,destination);
        if(copied!=Status::ok){result=copied;frame.stamp.available=false;downstream_status=copied;}
        else source=destination.view();
    }
    Status fill_scalar_silence(const PlanNode::ScalarInput& input,const NumericContract& contract,
                               std::array<std::byte,8>& bytes,const std::array<PlaneLayout,1>& planes,
                               DataFrameView& delivered) const noexcept {
        MutableNumericView zero{&contract,bytes,planes,{1,1},{},{}};
        if(input.parent==absent && !input.clock_known)delivered.stamp.available=false;
        const auto filled=fill_silence(zero,limits);if(filled!=Status::ok)return filled;
        auto value=zero.view();if(!delivered.stamp.available)value.storage={};
        delivered.payload=value;
        return Status::ok;
    }
    void deliver_scalar_input(const PlanNode& sink,const PlanNode::ScalarInput& input,const DataFrameView& frame) noexcept {
        auto delivered=frame;const auto& contract=std::get<NumericContract>(input.contract);
        alignas(std::max_align_t) std::array<std::byte,8> bytes{};
        const std::array<PlaneLayout,1> planes{{{0,{0,0}}}};
        if(!sink.node.enabled || input.parent==absent) {
            const auto filled=fill_scalar_silence(input,contract,bytes,planes,delivered);
            if(filled!=Status::ok){result=filled;return;}
        }else if(auto* value=std::get_if<NumericView>(&delivered.payload))value->contract=&contract;
        if(callbacks.sink_input)port_delivery.sink({runtime.graph.graph_id,{sink.node.id,input.port},PortDirection::Input,
            runtime.graph.nodes[sink.configured].kind,input.contract,delivered,
            delivered.stamp.available?Status::ok:Status::unavailable,delivery});
    }
    void deliver_scalar_views(PortId p,const DataFrameView& frame) noexcept {
        for(const auto& sink:runtime.nodes)if(sink.scalar_view())for(const auto& input:sink.endpoint_plan->inputs)
            if((input.parent==i && input.parent_port==p) || (input.parent==absent && input.clock==i && p==0))deliver_scalar_input(sink,input,frame);
    }
    void deliver_mixers(PortId p,const DataFrameView& frame) noexcept {
            for(const auto& route:n.mixer_destinations)if(route.output==p){
                auto send_consumer=[&](const DataFrameView& produced,Status status){return runtime.run(route.node,produced,status);};
                MixerDelivery delivery{&send_consumer,[](void* user,const DataFrameView& produced,Status status){
                    return (*static_cast<decltype(send_consumer)*>(user))(produced,status);
                },child_time};
                const auto accepted=accept_mixer_contribution(runtime.nodes[route.node],runtime.states[route.node],route.input,frame,runtime.graph,profile,delivery);
                if(accepted!=Status::ok)result=accepted;
            }
    }
    void send_child(std::size_t child,const DataFrameView& frame,Status status,std::size_t lane=0) noexcept {
        const auto begin=now();const auto sent=runtime.run(child,frame,status,lane);const auto end=now();
        if(end>=begin)child_time+=end-begin;
        if(sent!=Status::ok)result=sent;
    }
    void deliver_children(PortId port,const DataFrameView& frame) noexcept {
        const auto status=frame.stamp.available?Status::ok:Status::unavailable;
        for(auto child:n.children)if(!runtime.nodes[child].scalar_view() && runtime.nodes[child].parent_port==port)send_child(child,frame,status);
        for(const auto& route:n.selector_destinations)if(route.output==port)send_child(route.node,frame,status,route.input);
    }
    void emit(PortId port) noexcept {
        auto& frame=state.numeric_frames[port];++state.stats.produced;selector_storage(frame,port);
        if(auto* numeric=std::get_if<NumericView>(&frame.payload))numeric->contract=&std::get<NumericContract>(n.numeric_outputs[port]);
        stamp_state(state,frame.stamp);
        if(callbacks.output || callbacks.sink_input)port_delivery.output(endpoint(n,PortDirection::Output,port,frame,frame.stamp.available?Status::ok:downstream_status==Status::ok?Status::unavailable:downstream_status),n.internal_conversion);
        deliver_scalar_views(port,frame);deliver_mixers(port,frame);deliver_children(port,frame);
    }
    bool sequence_marker() noexcept {
            if(n.node.kind!=NodeKind::PcmResampler || marker_sequenced)return true;
            if(state.output_sequence==UINT64_MAX)return false;
            marker_stamp.sequence=++state.output_sequence;
            if(!marker_stamp.source_sequence)marker_stamp.source_sequence=current.stamp.sequence;
            marker_stamp.discontinuity=marker_stamp.discontinuity || state.pending_discontinuity;
            marker_sequenced=true;
            for(auto& frame:state.numeric_frames){frame.stamp=marker_stamp;frame.stamp.available=false;}
            return true;

    }
    void emit_unavailable_frames() noexcept {
        for(std::size_t port=0;port<state.numeric_frames.size();++port) {
            auto& frame=state.numeric_frames[port];frame.stamp=marker_stamp;frame.stamp.available=false;
            if(auto* numeric=std::get_if<NumericView>(&frame.payload))numeric->storage={};
            else std::get<StructuredView>(frame.payload).bytes={};
            emit(PortId(port));
        }
    }
    Status reject(Status cause,bool transient=false) noexcept {
        if(!transient){++state.stats.errors;result=cause;}
        downstream_status=cause;
        if(n.node.kind==NodeKind::PcmResampler) {
            std::get<ResamplerKernel>(state.kernel).operation().reset();
            state.pending_discontinuity=true;
        }
        if(!sequence_marker())return Status::overflow;
        emit_unavailable_frames();
        return Status::ok;
    }
    std::optional<Status> process_unavailable() noexcept {
            if(!sequence_marker())return Status::overflow;
            if(n.operation==ProcessingOperation::History && callbacks.history){auto dst=mutable_output(n,state,0);dst.actual={state.frames,dst.actual[1]};dst.ring={state.numeric_outputs[0].actual[0],state.oldest,{}};
                auto unavailable=dst.view();unavailable.storage={};state.numeric_frames[0].payload=unavailable;
                port_delivery.history({NumericHistoryEventKind::Unavailable,endpoint(n,PortDirection::Output,0,state.numeric_frames[0],downstream_status==Status::ok?Status::unavailable:downstream_status),std::get<NumericContract>(n.numeric_outputs[0]),{},std::uint64_t(state.progress),std::uint64_t(dst.ring.capacity),state.row_ordinal});}

        return {};
    }
    std::optional<Status> process_bypass() noexcept {
            state.numeric_frames[0]=current;
            if(n.operation==ProcessingOperation::Converter && n.gather && std::get<NumericView>(current.payload).ring.capacity){const auto producer=history_producer(n.parent!=absent?n.parent:n.numeric_clock_parent);
                if(producer!=absent){state.row_ordinal=runtime.states[producer].row_ordinal;state.progress=runtime.states[producer].progress;}
                const auto& view=std::get<NumericView>(current.payload);state.frames=view.actual[0];state.oldest=view.ring.oldest;}

        return {};
    }
    std::optional<Status> process_mixture() noexcept {state.numeric_frames[0]=current;

        return {};
    }
    std::optional<Status> process_paired() noexcept {
            status=accept_fourier(n,state,selector_lane,current,limits);
            if(status==Status::would_block)return Status::ok;
            if(status!=Status::ok)return reject(status,status==Status::unavailable);

        return {};
    }
    std::optional<Status> process_matrix() noexcept {
            status=execute_matrix(n,state,current,limits,runtime.graph.execution_interval);
            if(status!=Status::ok)return reject(status);

        return {};
    }
    std::optional<Status> process_matrix_row() noexcept {
            status=execute_matrix_row(n,state,current,limits);
            if(status!=Status::ok)return reject(status,status==Status::unavailable);

        return {};
    }
    std::optional<Status> process_array_fill() noexcept {
            status=execute_array_fill(n,state,current,limits);
            if(status!=Status::ok)return reject(status,false);

        return {};
    }
    std::optional<Status> process_metadata() noexcept {
            status=execute_metadata(n,state,current,limits);
            if(status!=Status::ok)return reject(status,n.node.kind==NodeKind::SampleRate && status==Status::unavailable);

        return {};
    }
    std::optional<Status> process_extract() noexcept {
            status=execute_extract(n,state,current,limits);
            if(status!=Status::ok)return reject(status,false);

        return {};
    }
    std::optional<Status> process_selector() noexcept {
            state.numeric_frames[0]=current;
            if(!n.node.enabled){auto silence=std::get<NumericView>(zero_value(n.numeric_outputs[0],state.numeric_outputs[0]));
                const auto& original=std::get<NumericView>(current.payload);silence.actual=original.actual;silence.coordinates=original.coordinates;
                state.numeric_frames[0].payload=silence;}

        return {};
    }
    void route_output(std::size_t p,bool open,std::uint16_t selected) noexcept {
auto& out=state.numeric_frames[p];out=current;
                if(!state.route_masks.empty() && std::get<NumericView>(current.payload).ring.capacity){
                    out.payload=history_route(state,std::get<NumericView>(current.payload),p,disconnected || !open || (n.operation==ProcessingOperation::Switch && p!=selected),first,
                        history_producer(n.operation==ProcessingOperation::Source?i:n.parent!=absent?n.parent:n.numeric_clock_parent),current.stamp);
                    return;
                }
                if(!open || (n.operation==ProcessingOperation::Switch && p!=selected)){
                    out.payload=zero_value(n.numeric_outputs[p],state.numeric_outputs[p]);
                    if(auto* silenced=std::get_if<NumericView>(&out.payload)){const auto& input_value=std::get<NumericView>(current.payload);
                        silenced->actual=input_value.actual;silenced->coordinates=input_value.coordinates;}
                }    }
    std::optional<Status> process_routing() noexcept {
        bool open=runtime.graph.nodes[n.configured].enabled;std::uint16_t selected=0;
        if(n.operation!=ProcessingOperation::Source){const auto& parameters=std::get<RouteProcessingParameters>(runtime.graph.nodes[n.configured].parameters);open=open && parameters.open;selected=parameters.selected;}
        for(std::size_t port=0;port<state.numeric_frames.size();++port)route_output(port,open,selected);
        return {};
    }
    std::optional<Status> process_conversion() noexcept {
        std::optional<ConversionHistoryOrigin> origin;
        if(std::get<NumericView>(current.payload).ring.capacity){
            const auto producer=history_producer(n.parent!=absent?n.parent:n.numeric_clock_parent);
            if(producer!=absent)origin=ConversionHistoryOrigin{runtime.nodes[producer].node.id,runtime.states[producer].row_ordinal,runtime.states[producer].progress};
        }
        const auto converted=execute_conversion(n,state,current,limits,origin);
        if(converted.status!=Status::ok){if(converted.reject)return reject(converted.status);result=downstream_status=converted.status;}
        return {};
    }
    std::optional<Status> process_music() noexcept {
            status=execute_music(n,state,current,limits);if(status!=Status::ok)return reject(status);

        return {};
    }
    std::optional<Status> process_transform() noexcept {
            const auto transformed=execute_transform(n,state,current,limits,{this,
                [](void* user){static_cast<NodeRun*>(user)->emit(0);},
                [](void* user,Status status){return static_cast<NodeRun*>(user)->reject(status);}});
            if(transformed.completed)return transformed.status;
            if(transformed.status!=Status::ok){++state.stats.errors;result=transformed.status;downstream_status=transformed.status;}

        return {};
    }
    Status load_wled_values(const NumericView& source) noexcept {
        if(source.actual[0]>state.values.size() || source.ring.capacity)return Status::incompatible_contract;
        for(std::size_t index=0;index<source.actual[0];++index){
            const auto status=load_element(source,{0,index,0},state.values[index]);if(status!=Status::ok)return status;
            if(!std::isfinite(state.values[index]))return Status::overflow;
        }
        return Status::ok;
    }
    Status retain_wled_mel(const NumericView& source) noexcept {
        auto& join=state.wled_inputs.front();
        if(current.stamp.stream_id.size()>join.stream.size())return Status::exhausted;
        if(n.node.enabled){const auto loaded=load_wled_values(source);if(loaded!=Status::ok)return loaded;}
        std::copy(current.stamp.stream_id.begin(),current.stamp.stream_id.end(),join.stream.begin());
        join.stream_size=current.stamp.stream_id.size();join.count=source.actual[0];
        join.retain(0,current.stamp);return Status::ok;
    }
    Status retain_wled_scalar(const NumericView& source) noexcept {
        double value=0;Status loaded=Status::incompatible_contract;
        if(source.contract->dtype==DType::Float64)loaded=load_element(source,{},value);
        else if(source.contract->dtype==DType::Float32){float scalar=0;loaded=load_element(source,{},scalar);value=scalar;}
        if(loaded!=Status::ok)return loaded;
        if(!std::isfinite(value) || value<0 || value>std::numeric_limits<float>::max())return Status::overflow;
        auto& join=state.wled_inputs.front();join.scalars[selector_lane-1]=float(value);
        join.retain(selector_lane,current.stamp);return Status::ok;
    }
    float wled_scalar(std::size_t lane,float fallback) const noexcept {
        return n.endpoint_plan->inputs[lane].parent==absent?fallback:state.wled_inputs.front().scalars[lane-1];
    }
    void publish_wled(NumericBuffer& packet) noexcept {
        auto& join=state.wled_inputs.front();auto stamp=join.stamps[0];
        stamp.stream_id={join.stream.data(),join.stream_size};
        state.numeric_frames[0]={stamp,StructuredView{&std::get<StructuredContract>(n.numeric_outputs[0]),packet.storage()}};
        join.emitted=join.stamps[0];join.published=true;join.pending={};
    }

    Status encode_wled(NumericBuffer& packet) noexcept {
        const auto destination=std::span(reinterpret_cast<std::uint8_t*>(packet.bytes.data()),packet.bytes.size());
        if(!n.node.enabled)return wled_protocol::silence(0,destination,{0,wled_protocol::packet_size}).status==wled_protocol::Status::Ok?Status::ok:Status::invalid_argument;
        const auto& join=state.wled_inputs.front();
        const auto encoded=wled_protocol::encode_mel({state.values.data(),join.count},wled_scalar(1,n.wled_peak_hz),wled_scalar(2,n.wled_magnitude),n.gain,0,destination,{state.values.size(),wled_protocol::packet_size});
        if(encoded.status==wled_protocol::Status::Ok)return Status::ok;
        return encoded.status==wled_protocol::Status::NonFiniteInput || encoded.status==wled_protocol::Status::NonFiniteResult?Status::overflow:Status::invalid_argument;
    }
std::optional<Status> publish_wled_ready() noexcept {
        auto& join=state.wled_inputs.front();
        if(n.node.enabled && !join.ready(n))return Status::ok;
        auto& packet=state.numeric_outputs[0];
        const auto encoded=encode_wled(packet);
        if(encoded!=Status::ok){join.pending={};return reject(encoded);}
        publish_wled(packet);
        return {};
    }

    std::optional<Status> process_wled() noexcept {
        auto& join=state.wled_inputs.front();
        if(!n.node.enabled && selector_lane!=0)return Status::ok;
        if(!current.stamp.available){join.pending={};return process_unavailable();}
        if(join.already_published(current.stamp))return Status::ok;
        const auto& source=std::get<NumericView>(current.payload);
        const auto retained=retain_wled_input(source);
        if(retained!=Status::ok){join.pending={};return reject(retained);}
        return publish_wled_ready();
    }
    Status retain_wled_input(const NumericView& source) noexcept {
        return selector_lane==0?retain_wled_mel(source):retain_wled_scalar(source);
    }
    std::optional<Status> process_reduction() noexcept {NumericGatherOutput compact;status=n.node.enabled?gather(n,state,current,compact):Status::ok;
            auto dst=mutable_output(n,state,0);
            if(status==Status::ok)status=n.node.enabled?numeric_kernel_status(dsp_kernels::reduce(*n.reduction,std::get<NumericView>(compact.frame.payload),dst,state.numeric_scratch).status):fill_silence(dst,limits);
            if(status==Status::ok)state.numeric_frames[0]={current.stamp,dst.view()};else {downstream_status=status;if(status!=Status::unavailable)result=status;}

        return {};
    }
    void compact_scalar_history(const NumericContract& history,NumericGatherOutput& compact) noexcept {
            compact.frame={state.numeric_frames[0].stamp,
                n.node.enabled?current.payload:zero_value(n.numeric_input,state.numeric_zero)};
            const auto capacity=state.numeric_outputs[0].actual[0];
            compact.row.history=true;
            compact.row.silenced=!n.node.enabled || std::get<NumericView>(compact.frame.payload).semantic_silence;
            compact.row.logical_row=capacity-1;
            compact.row.physical_row=(state.oldest+capacity-1)%capacity;
            compact.row.filled_rows=capacity;
            compact.row.capacity=capacity;
            compact.row.time_axis=history.coordinates[0];
            const auto time=std::get<NumericView>(state.numeric_frames[0].payload).coordinates.axes[0];
            if(!time.empty())compact.row.time_coordinate=time.subspan((capacity-1)*sizeof(double),sizeof(double));
    }
    std::optional<Status> process_history() noexcept {
        const auto appended=append_history(n,state,current,limits);
        if(appended.status!=Status::ok){
            if(appended.reject)return reject(appended.status);
            result=downstream_status=appended.status;return {};
        }
        if(!callbacks.history)return {};
        return deliver_history();
    }
    std::optional<Status> deliver_history() noexcept {
        NumericGatherOutput compact;
        const auto& history=std::get<NumericContract>(n.numeric_outputs[0]);
        if(history.shape.rank==Rank::Matrix){
            status=gather(n,state,state.numeric_frames[0],compact);
            if(status!=Status::ok)return reject(status);
        }else{
            compact_scalar_history(history,compact);
        }
        const auto capacity=state.numeric_outputs[0].actual[0];
        port_delivery.history({NumericHistoryEventKind::Row,
            endpoint(n,PortDirection::Output,0,state.numeric_frames[0],Status::ok),
            history,compact,std::uint64_t(state.progress),std::uint64_t(capacity),state.row_ordinal});
        return {};
    }
    const DataContract& declared_contract() const noexcept {
        const bool lane_input=(paired_processing(n.operation) ||
            n.operation==ProcessingOperation::WLEDEncoder) && n.endpoint_plan;
        if(lane_input)return n.endpoint_plan->inputs[selector_lane].contract;
        if(n.operation==ProcessingOperation::Source || n.operation==ProcessingOperation::Mixer ||
           n.operation==ProcessingOperation::ChannelComparison)return n.numeric_outputs[0];
        return n.numeric_input;
    }
    void bind_contract() noexcept {
        auto* numeric=std::get_if<NumericView>(&current.payload);
        if(!numeric)return;
        if(n.operation!=ProcessingOperation::InputSelector &&
           !(n.operation==ProcessingOperation::Switch && n.endpoint_plan))
            if(const auto* physical=std::get_if<NumericContract>(&declared_contract()))numeric->contract=physical;
        if(n.operation==ProcessingOperation::Source && external_history && !current.stamp.available){
            numeric->actual[0]=history_input->shape.extents[0].maximum;
            numeric->ring={numeric->actual[0],state.oldest,{}};
        }
    }
    void selector_continuity() noexcept {
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
    }
    void kernel_continuity() noexcept {
        // Do not assemble a window from samples spanning a selector switch,
        // source restart or unavailable interval. Retained History stays intact.
        if(n.node.kind==NodeKind::Window && (current.stamp.discontinuity || !current.stamp.available ||
            (state.numeric_marker && !state.numeric_stamp.available && current.stamp.available))){
            state.progress=state.remainder=state.hop=0;
            std::fill(state.pcm.begin(),state.pcm.end(),0);
            state.pending_discontinuity=true;
        }
        if(n.operation==ProcessingOperation::ArrayProduct && !current.stamp.available)
            std::get<PreparedOperationKernel>(state.kernel).invalidate_product_lane(selector_lane);
        if((n.operation==ProcessingOperation::MusicTransform || complex_processing(n.operation)) && !current.stamp.available)
            std::get<PreparedOperationKernel>(state.kernel).reset();
        resampler_continuity();
    }
    void resampler_continuity() noexcept {
        if(n.node.kind==NodeKind::PcmResampler){
            auto& resampler=std::get<ResamplerKernel>(state.kernel).operation();
            if(current.stamp.discontinuity || !current.stamp.available || !resampler.input_contiguous(current.stamp.timestamp_ticks) ||
               (state.numeric_marker && (current.stamp.producer_epoch!=state.numeric_stamp.producer_epoch ||
                current.stamp.generation!=state.numeric_stamp.generation || (!state.numeric_stamp.available && current.stamp.available)))){
                resampler.reset();state.pending_discontinuity=true;
            }
        }
    }

    void route_disconnected() noexcept {
        if(n.operation==ProcessingOperation::WLEDEncoder && selector_lane!=0)return;
        disconnected=n.parent==absent && n.numeric_clock_parent!=absent;
        if(disconnected){const auto* history=std::get_if<NumericView>(&current.payload);
            if(!history || !history->ring.capacity)current.payload=zero_value(n.numeric_input,state.numeric_zero);
        }
        if(!state.route_masks.empty() && std::get<NumericView>(current.payload).ring.capacity && (n.operation==ProcessingOperation::Sink || n.operation==ProcessingOperation::Reduce || n.operation==ProcessingOperation::Converter || n.operation==ProcessingOperation::MatrixTranspose || n.operation==ProcessingOperation::MatrixWindowFunction || n.operation==ProcessingOperation::MatrixFFT || n.operation==ProcessingOperation::MatrixRow || n.operation==ProcessingOperation::MatrixCenter)){
            auto& value=std::get<NumericView>(current.payload);value=history_route(state,value,0,disconnected || (!n.node.enabled && !n.identity_bypass),first,
                history_producer(n.parent!=absent?n.parent:n.numeric_clock_parent),current.stamp);
        }
    }
    void retain_stamp() noexcept {
        // History owns continuity of its retained rows. Ancestor geometry can
        // restart without changing this History's contract/source clock. Real
        // source discontinuities clear its state and set pending_discontinuity.
        if(n.operation==ProcessingOperation::History)current.stamp.discontinuity=state.pending_discontinuity ||
            (n.clock!=absent && runtime.nodes[n.clock].operation==ProcessingOperation::InputSelector && incoming.stamp.discontinuity);
        if(state.pending_discontinuity && n.node.kind!=NodeKind::Window && n.node.kind!=NodeKind::PcmResampler){current.stamp.discontinuity=true;
            // An unavailable History marker does not append a row. Preserve the
            // boundary until the first retained row after recovery is emitted.
            if(n.operation!=ProcessingOperation::History || current.stamp.available)state.pending_discontinuity=false;}
        stamp_state(state,current.stamp);state.numeric_marker=true;
    }
    void silence_sink() noexcept {
        if(!current.stamp.available || n.node.enabled || (std::holds_alternative<NumericView>(current.payload) && std::get<NumericView>(current.payload).ring.capacity))return;

                auto silence=zero_value(n.numeric_input,state.numeric_zero);
                if(auto* numeric=std::get_if<NumericView>(&silence)){const auto& original=std::get<NumericView>(current.payload);
                    numeric->actual=original.actual;numeric->coordinates=original.coordinates;}
                else {auto& packet=std::get<StructuredView>(silence);packet.bytes=packet.bytes.first(std::get<StructuredView>(current.payload).bytes.size());}
                current.payload=silence;
                }
    void scalar_sink_row_info(const NumericView& source,NumericGatherOutput& compact) noexcept {
        const auto logical=source.actual[0]-1;
        compact.row.history=true;compact.row.silenced=std::get<NumericView>(compact.frame.payload).semantic_silence;
        compact.row.logical_row=logical;compact.row.physical_row=(source.ring.oldest+logical)%source.ring.capacity;
        compact.row.filled_rows=source.actual[0];compact.row.capacity=source.ring.capacity;
        compact.row.time_axis=source.contract->coordinates[0];
        const auto time=source.coordinates.axes[0];
        if(!time.empty())compact.row.time_coordinate=time.subspan(logical*sizeof(double),sizeof(double));
    }
    Status gather_sink_history(NumericGatherOutput& compact,NumericContract& contract) noexcept {
        if(n.gather)return gather(n,state,current,compact);
        const auto& source=std::get<NumericView>(current.payload);NumericView row;
        const auto status=history_row(source,source.actual[0]-1,row,contract,state.row_planes);
        if(status!=Status::ok)return status;
        compact.frame={current.stamp,row};scalar_sink_row_info(source,compact);
        return Status::ok;
    }
    Status deliver_sink_history(const NumericPortFrameView& ep) noexcept {
        if(!((n.gather || !state.row_planes.empty()) && callbacks.history && std::get<NumericView>(current.payload).ring.capacity))return Status::ok;
const auto& source=std::get<NumericView>(current.payload);const auto producer=history_producer(n.parent!=absent?n.parent:n.numeric_clock_parent);
                const auto ordinal=producer==absent?std::uint64_t{0}:runtime.states[producer].row_ordinal;
                const auto filled=producer==absent?std::uint64_t{0}:std::uint64_t(runtime.states[producer].progress);
                const auto capacity=std::uint64_t(source.ring.capacity?source.ring.capacity:source.contract->shape.extents[0].maximum);
                auto event=!current.stamp.available?NumericHistoryEventKind::Unavailable:filled?NumericHistoryEventKind::Row:NumericHistoryEventKind::Empty;
                NumericGatherOutput compact;NumericContract row_contract;
                if(event==NumericHistoryEventKind::Row){const auto gathered=gather_sink_history(compact,row_contract);if(gathered!=Status::ok)return gathered;}
                port_delivery.history({event,ep,*source.contract,compact,filled,capacity,ordinal});
                    return Status::ok;
    }
    Status process_sink() noexcept {
        silence_sink();const auto event=endpoint(n,PortDirection::Input,0,current,current.stamp.available?Status::ok:Status::unavailable);
        if(callbacks.sink_input)port_delivery.sink(event);
        return deliver_sink_history(event);
    }
    void publish_lane(std::size_t lane,const DataFrameView& frame) noexcept {
        state.numeric_frames[lane]=frame;state.source_stamps[lane]=frame.stamp;state.source_stamps[lane].stream_id={};state.source_markers[lane]=true;emit(PortId(lane));
    }
    void forward_switch_output(std::size_t output,bool missing,const RouteProcessingParameters& parameters) noexcept {
        auto frame=current;
        frame.stamp.discontinuity=frame.stamp.discontinuity || !state.source_markers[output] || state.source_stamps[output].generation!=frame.stamp.generation || state.source_stamps[output].producer_epoch!=frame.stamp.producer_epoch;
        if(missing || !n.node.enabled || !parameters.open || output/parameters.channels_per_stream!=parameters.selected){
            auto zero=std::get<NumericView>(zero_value(n.numeric_outputs[output],state.numeric_outputs[output]));const auto& original=std::get<NumericView>(current.payload);
            zero.actual=original.actual;zero.coordinates=original.coordinates;frame.payload=zero;
        }
        publish_lane(output,frame);
    }
    void forward_switch_lane(std::size_t lane,bool missing) noexcept {
        const auto& parameters=std::get<RouteProcessingParameters>(runtime.graph.nodes[n.configured].parameters);
        for(std::size_t output=lane;output<n.numeric_outputs.size();output+=parameters.channels_per_stream)forward_switch_output(output,missing,parameters);
    }
    void silence_selector_lane(DataFrameView& frame,std::size_t lane) const noexcept {
        auto zero=std::get<NumericView>(zero_value(n.numeric_outputs[lane],state.numeric_outputs[lane]));const auto& original=std::get<NumericView>(current.payload);
        if(zero.contract->shape.rank==original.contract->shape.rank){zero.actual=original.actual;zero.coordinates=original.coordinates;}
        frame.payload=zero;
    }
    void forward_selector_lane(std::size_t lane,bool missing) noexcept {
        auto frame=current;const bool initial=!state.source_markers[lane];
        frame.stamp.discontinuity=frame.stamp.discontinuity || initial || state.source_stamps[lane].producer_epoch!=frame.stamp.producer_epoch || state.source_stamps[lane].generation!=frame.stamp.generation;
        if(missing || !n.node.enabled)silence_selector_lane(frame,lane);
        if(missing){const auto& clock=std::get<NumericContract>(n.numeric_outputs[lane]).clock;frame.stamp.available=!clock.domain.empty() && clock.sample_rate;}
        publish_lane(lane,frame);
    }
    void forward_missing_lanes(bool switched) noexcept {
        std::size_t anchor=0;while(anchor<n.endpoint_plan->inputs.size() && n.endpoint_plan->inputs[anchor].parent==absent)++anchor;
        if(selector_lane!=(anchor==n.endpoint_plan->inputs.size()?0:anchor))return;
        for(std::size_t lane=0;lane<n.endpoint_plan->inputs.size();++lane)if(lane!=selector_lane && n.endpoint_plan->inputs[lane].parent==absent){
            if(switched)forward_switch_lane(lane,true);else forward_selector_lane(lane,true);
        }
    }
    Status process_switch() noexcept {
        forward_switch_lane(selector_lane,n.endpoint_plan->inputs[selector_lane].parent==absent);forward_missing_lanes(true);return Status::ok;
    }
    Status process_selector_group() noexcept {
        forward_selector_lane(selector_lane,n.endpoint_plan->inputs[selector_lane].parent==absent);forward_missing_lanes(false);return Status::ok;
    }
    void initialize_marker(PortId port) noexcept {
        auto& frame=state.numeric_frames[port];frame.stamp=marker_stamp;frame.stamp.available=false;
        if(const auto* structured=std::get_if<StructuredContract>(&n.numeric_outputs[port])){frame.payload=StructuredView{structured,{}};return;}
        auto output=mutable_output(n,state,port);
        if(n.operation==ProcessingOperation::History || state.history_origin_valid){output.ring={output.actual[0],state.oldest,{}};output.actual[0]=state.frames;}
        auto unavailable=output.view();unavailable.storage={};frame.payload=unavailable;
    }
    void initialize_markers() noexcept {
        marker_stamp=current.stamp;
        if(n.node.kind==NodeKind::PcmResampler && !convert_rate_ticks(current.stamp.timestamp_ticks,std::get<NumericContract>(n.numeric_input).clock.sample_rate,std::get<NumericContract>(n.numeric_outputs[0]).clock.sample_rate,marker_stamp.timestamp_ticks)){
            marker_stamp.timestamp_ticks=0;marker_stamp.available=false;downstream_status=Status::overflow;result=Status::overflow;
        }
        for(std::size_t port=0;port<state.numeric_frames.size();++port)initialize_marker(PortId(port));
    }
    void retain_source_history() noexcept {
        if(!(n.operation==ProcessingOperation::Source && n.gather && current.stamp.available && external_history))return;

            const auto& value=std::get<NumericView>(state.numeric_frames[0].payload);
            state.row_ordinal=*history_ordinal;state.frames=value.actual[0];state.progress=std::size_t(real_filled);state.oldest=value.ring.oldest;
            }
    Status publish_existing_history() noexcept {
        if(!(n.gather && !n.internal_conversion && (n.operation==ProcessingOperation::Source || n.operation==ProcessingOperation::Converter) && callbacks.history && std::get<NumericView>(state.numeric_frames[0].payload).ring.capacity))return Status::ok;

            const auto& frame=state.numeric_frames[0];const auto& value=std::get<NumericView>(frame.payload);
            auto event=!frame.stamp.available?NumericHistoryEventKind::Unavailable:state.progress?NumericHistoryEventKind::Row:NumericHistoryEventKind::Empty;
            NumericGatherOutput compact;
            if(event==NumericHistoryEventKind::Row){status=gather(n,state,frame,compact);if(status!=Status::ok)return status;}
            port_delivery.history({event,endpoint(n,PortDirection::Output,0,frame,frame.stamp.available?Status::ok:Status::unavailable),*value.contract,compact,std::uint64_t(state.progress),std::uint64_t(value.ring.capacity),state.row_ordinal});
                return Status::ok;
    }
    Status finalize() noexcept {
        retain_source_history();const auto status=publish_existing_history();if(status!=Status::ok)return status;
        for(std::size_t port=0;port<state.numeric_frames.size();++port)if(n.operation!=ProcessingOperation::Source || port==ingress_port)emit(PortId(port));
        return Status::ok;
    }
    std::optional<Status> process_auxiliary() noexcept {
        switch(n.operation){
        case ProcessingOperation::InputSelector:return process_selector();
        case ProcessingOperation::Source:case ProcessingOperation::Gate:case ProcessingOperation::Switch:case ProcessingOperation::Splitter:return process_routing();
        case ProcessingOperation::Converter:return process_conversion();
        case ProcessingOperation::MusicTransform:return process_music();
        default:return process_output();
        }
    }
    std::optional<Status> process_output() noexcept {
        switch(n.operation){
        case ProcessingOperation::Transform:return process_transform();
        case ProcessingOperation::WLEDEncoder:return process_wled();
        case ProcessingOperation::Reduce:return process_reduction();
        case ProcessingOperation::History:return process_history();
        default:return {};
        }
    }
    std::optional<Status> process_spectrum_peak() noexcept {
        const auto executed=execute_spectrum_peak(n,state,current,limits);
        if(executed!=Status::ok)return reject(executed);
        return {};
    }

    std::optional<Status> process_values() noexcept {
        switch(n.operation){
        case ProcessingOperation::MatrixRow:return process_matrix_row();
        case ProcessingOperation::ArrayFill:return process_array_fill();
        case ProcessingOperation::Metadata:return process_metadata();
        case ProcessingOperation::Extract:return process_extract();
        case ProcessingOperation::SpectrumPeak:return process_spectrum_peak();
        default:return process_auxiliary();
        }
    }
    std::optional<Status> process_operation() noexcept {
        if(n.operation==ProcessingOperation::WLEDEncoder)return process_wled();
        if(!current.stamp.available)return process_unavailable();
        if(n.identity_bypass && !((n.operation==ProcessingOperation::MatrixCenter || n.operation==ProcessingOperation::MatrixWindowFunction) && std::get<NumericView>(current.payload).ring.capacity))return process_bypass();
        if(n.operation==ProcessingOperation::Mixer || n.operation==ProcessingOperation::ChannelComparison)return process_mixture();
        if(paired_processing(n.operation))return process_paired();
        if(n.operation==ProcessingOperation::MatrixTranspose || n.operation==ProcessingOperation::MatrixWindowFunction || n.operation==ProcessingOperation::MatrixFFT || n.operation==ProcessingOperation::MatrixCenter)return process_matrix();
        return process_values();
    }
    void prepare_current() noexcept {
        bind_contract();selector_continuity();kernel_continuity();route_disconnected();retain_stamp();
    }
    Status dispatch_prepared() noexcept {
        if(n.operation==ProcessingOperation::Sink)return process_sink();
        if(n.operation==ProcessingOperation::Switch && n.endpoint_plan)return process_switch();
        if(n.operation==ProcessingOperation::InputSelector && n.endpoint_plan)return process_selector_group();
        initialize_markers();
        if(const auto completed=process_operation())return *completed;
        return finalize();
    }
public:
    NodeRun(RuntimeDispatch& execution,std::size_t node,const DataFrameView& frame,Status input_status,std::size_t lane) noexcept
        :runtime(execution),i(node),n(runtime.nodes[i]),state(runtime.states[i]),profile(runtime.profile),limits(profile.admission.frame),callbacks(runtime.callbacks),delivery(runtime.delivery),result(runtime.result),ingress_port(runtime.ingress_port),history_input(runtime.history_input),external_history(runtime.external_history),history_ordinal(runtime.history_ordinal),real_filled(runtime.real_filled),incoming(frame),selector_lane(lane),first(!state.numeric_marker),current(incoming),downstream_status(input_status),port_delivery(profile,runtime.aliases,callbacks,child_time){}
    Status process() noexcept {
        // Auxiliary factors cannot change or invalidate an identity bypass.
        if(n.operation==ProcessingOperation::ArrayProduct && !n.node.enabled && selector_lane!=n.product_frame_input)return Status::ok;
        ExecutionMeasurement measurement{state.stats,profile,runtime.graph.execution_interval,child_time,state.pending_self_nanoseconds};
        state.pending_self_nanoseconds=0;state.stats.node=n.node.id;++state.stats.calls;
        prepare_current();
        return dispatch_prepared();
    }
};
Status RuntimeDispatch::run(std::size_t index,const DataFrameView& frame,Status status,std::size_t lane) noexcept {
    return NodeRun(*this,index,frame,status,lane).process();
}
bool valid_source_ingress(std::span<const PlanNode> nodes,std::size_t source_index,
                          PortId port) noexcept {
    if(source_index==absent)return false;
    const auto& source=nodes[source_index];
    if(source.operation!=ProcessingOperation::Source &&
       source.operation!=ProcessingOperation::Mixer)return false;
    return port<source.numeric_outputs.size();
}

void advance_source_watermark(State& source,const FrameStamp& stamp) noexcept {
    source.numeric_marker=true;
    stamp_state(source,stamp);
    source.last_sequence=std::max(source.last_sequence,stamp.sequence);
}
}
Status PreparedGraph::process_numeric_impl(NodeId ingress,const DataFrameView& supplied,RuntimeContext& c,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks,
    const OwnedDataFrame* owned,std::optional<std::uint64_t> history_ordinal,std::shared_ptr<const void> retention_owner,std::optional<std::uint64_t> history_filled_count,PortId ingress_port) const noexcept {
    auto input=supplied; // Stamp may acquire a detected packet discontinuity; payload stays borrowed.
    if(!impl_ || !impl_->numeric_graph || !c.impl_ || c.impl_->plan!=this)return Status::invalid_argument;
    const auto source_index=index(impl_->nodes,ingress);
    if(!valid_source_ingress(impl_->nodes,source_index,ingress_port))return Status::invalid_argument;
    auto& source_state=c.impl_->states[source_index];
    const auto& limits=impl_->numeric_profile.admission.frame;
    IngressAcceptance acceptance{impl_->nodes,c.impl_->states,source_index,ingress_port,input,limits,owned,history_ordinal,retention_owner,history_filled_count};
    auto status=acceptance.accept();if(status!=Status::ok)return status;
    advance_source_watermark(source_state,input.stamp);
    return RuntimeDispatch{impl_->nodes,c.impl_->states,*impl_->numeric_graph,impl_->numeric_profile,impl_->routing_aliases,delivery,callbacks,input,source_index,ingress_port,acceptance.history_input,acceptance.external_history,history_ordinal,acceptance.real_filled}.process();
}
Status PreparedGraph::process_numeric_owned_ports(NodeId ingress,const OwnedDataFrame& owned,RuntimeContext& context,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks,
    std::optional<std::uint64_t> history_ordinal,std::shared_ptr<const void> retention_owner,std::optional<std::uint64_t> history_filled_count) const noexcept {
    if(!owned || !context.impl_ || context.impl_->plan!=this)return Status::invalid_argument;
    const auto at=index(impl_->nodes,ingress);if(at==absent || impl_->nodes[at].operation!=ProcessingOperation::Source)return Status::invalid_argument;
    return process_numeric_impl(ingress,owned.view(),context,delivery,callbacks,&owned,history_ordinal,std::move(retention_owner),history_filled_count);
}

} // namespace dataflow_runtime
