#include <dataflow_runtime/runtime.h>
#include <dsp_kernels/dsp.h>
#include <dsp_kernels/pcm_resampler.h>
#include <dsp_kernels/scalar_reduce.h>
#include <dsp_kernels/numeric_convert.h>
#include <dsp_kernels/numeric_reduce.h>
#include <wled_protocol/wled_packet.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <numeric>
namespace dataflow_runtime {
namespace {
using namespace domain;
using namespace signal_contracts;
constexpr std::size_t absent = std::numeric_limits<std::size_t>::max();
constexpr std::size_t fixed_plan_reservation=4096;
struct WideUnsigned {std::uint64_t high=0,low=0;};
WideUnsigned multiply_wide(std::uint64_t a,std::uint64_t b) noexcept {
    const std::uint64_t al=a&0xffffffffULL,ah=a>>32,bl=b&0xffffffffULL,bh=b>>32;
    const auto first=al*bl,second=ah*bl+(first>>32),third=al*bh+(second&0xffffffffULL);
    return {ah*bh+(second>>32)+(third>>32),(third<<32)|(first&0xffffffffULL)};
}
// Exact floor of two products, without compiler-specific 128-bit integers.
// The division is bounded to 128 steps and performs no allocation.
bool divide_products(std::uint64_t a,std::uint64_t b,std::uint64_t c,std::uint64_t d,std::uint64_t& out,std::uint64_t* fraction=nullptr) noexcept {
    const auto numerator=multiply_wide(a,b),denominator=multiply_wide(c,d);
    if(!denominator.high && !denominator.low)return false;
    WideUnsigned remainder;out=0;
    for(int bit=127;bit>=0;--bit){const bool carry=(remainder.high>>63)!=0;
        remainder.high=(remainder.high<<1)|(remainder.low>>63);
        remainder.low=(remainder.low<<1)|((bit>=64?numerator.high:numerator.low)>>(bit%64)&1);
        if(carry || remainder.high>denominator.high || (remainder.high==denominator.high && remainder.low>=denominator.low)){
            const bool borrow=remainder.low<denominator.low;remainder.low-=denominator.low;remainder.high-=denominator.high+std::uint64_t(borrow);
            if(bit>=64)return false;out|=std::uint64_t{1}<<bit;}}
    if(fraction){if(remainder.high)return false;*fraction=remainder.low;}return true;
}
bool convert_rate_ticks(std::int64_t ticks,std::uint32_t input,std::uint32_t output,std::int64_t& converted) noexcept {
    const auto magnitude=ticks<0 ? std::uint64_t(-(ticks+1))+1 : std::uint64_t(ticks);
    std::uint64_t value=0,fraction=0;
    if(!divide_products(magnitude,output,input,1,value,&fraction))return false;
    if(ticks<0 && fraction){if(value==UINT64_MAX)return false;++value;}
    const auto negative_limit=std::uint64_t(INT64_MAX)+1;
    if(value>std::uint64_t(INT64_MAX)){
        if(ticks>=0 || value!=negative_limit)return false;
        converted=INT64_MIN;
    }else converted=ticks<0 ? -std::int64_t(value) : std::int64_t(value);
    return true;
}
// Metadata pool is stable for the lifetime of its immutable prepared gather.
struct GatherOwner {
    std::vector<std::byte> metadata;
    BufferSlot slot;
    BufferPool pool;
    PreparedNumericGather gather;
    explicit GatherOwner(std::size_t size):metadata(size),pool(metadata,{&slot,1},size){}
};
struct PlanNode {
    struct ScalarInput {DataContract contract;std::size_t parent=absent,clock=absent;PortId port=0,parent_port=0;bool clock_known=false;};
    struct EndpointPlan {std::vector<ScalarInput> inputs;std::array<std::size_t,3> indices{};};
    std::unique_ptr<EndpointPlan> endpoint_plan;
    std::size_t parent = absent, clock = absent;
    struct {NodeId id=-1;NodeKind kind=NodeKind::NetworkInput;bool enabled=true;} node;
    std::uint16_t parent_port=0,selected_input=0;
    // Internal flags only; this object is never a serialized wire layout.
    bool reset:1 = true;
    bool clock_reset:1 = true;
    bool selector_changed:1=false;
    bool internal_conversion:1=false;
    bool identity_bypass:1=false;
    bool scalar_view() const noexcept {return node.kind==NodeKind::ScalarView && endpoint_plan!=nullptr;}
    std::vector<std::size_t> children;
    ProcessingOperation operation=ProcessingOperation::Source;
    std::size_t configured=absent;
    std::size_t numeric_clock_parent=absent;
    DataContract numeric_input;
    std::vector<DataContract> numeric_outputs;
    std::unique_ptr<dsp_kernels::PreparedConversion> conversion;
    std::unique_ptr<dsp_kernels::PreparedConversion> row_conversion;
    std::unique_ptr<dsp_kernels::PreparedReduction> reduction;
    std::shared_ptr<const GatherOwner> gather;
    std::shared_ptr<const GatherOwner> input_gather;
    float gain=1;
    unsigned merge_mode=2;
    dsp_kernels::WindowFunction window_function=dsp_kernels::WindowFunction::Hann;
    std::size_t hop_base=0;
    std::uint64_t hop_fraction=0,hop_denominator=1;
};
struct NumericBuffer {
    std::vector<std::byte> bytes;
    std::vector<float> floats; // Native Float32 objects; no byte-to-float type punning.
    std::vector<PlaneLayout> planes;
    std::shared_ptr<const std::vector<std::byte>> silence;
    std::size_t silence_bytes=0;
    std::array<std::vector<std::byte>,2> coordinates;
    std::array<std::size_t,2> actual{1,1};
    std::span<std::byte> storage() noexcept {return floats.empty()?std::span(bytes):std::as_writable_bytes(std::span(floats));}
    std::span<const std::byte> storage() const noexcept {return silence?std::span<const std::byte>(*silence).first(silence_bytes):floats.empty()?std::span<const std::byte>(bytes):std::as_bytes(std::span(floats));}
};
struct HistoryRouteState {
    NodeId producer=-1;
    std::uint64_t ordinal=0,epoch=0,generation=0;
    bool valid=false;
};
// Cold-owned optional operation: adding a resampler must not inflate every
// existing node's inline state on memory-constrained targets. Context snapshots
// deep-copy mutable FIR state; refresh_state copies into prepared storage instead.
struct ResamplerKernel {
    std::unique_ptr<dsp_kernels::PcmResampler> value=std::make_unique<dsp_kernels::PcmResampler>();
    ResamplerKernel()=default;
    ResamplerKernel(const ResamplerKernel& other):value(std::make_unique<dsp_kernels::PcmResampler>(*other.value)){}
    ResamplerKernel& operator=(const ResamplerKernel& other){if(this!=&other)value=std::make_unique<dsp_kernels::PcmResampler>(*other.value);return *this;}
    ResamplerKernel(ResamplerKernel&&) noexcept=default;
    ResamplerKernel& operator=(ResamplerKernel&&) noexcept=default;
    dsp_kernels::PcmResampler& operation() noexcept {return *value;}
    const dsp_kernels::PcmResampler& operation() const noexcept {return *value;}
};
struct State {
    // A node owns only its prepared numerical operation; contexts copy independently.
    std::variant<std::monostate,dsp_kernels::FFT,dsp_kernels::MelFilterBank,ResamplerKernel> kernel;
    std::vector<float> left, values, coefficients, pcm;
    std::vector<double> mel_boundaries;
    std::size_t frames = 0, oldest = 0, progress = 0, hop = 0;
    std::uint64_t remainder=0;
    std::uint64_t output_sequence = 0;
    std::array<std::uint64_t, 16> retired{};
    std::size_t retired_count = 0, retired_next = 0;
    bool pending_discontinuity = false;
    std::vector<NumericBuffer> numeric_outputs;
    std::vector<std::vector<std::byte>> route_masks;
    std::vector<HistoryRouteState> route_state;
    NumericBuffer numeric_zero;
    std::vector<std::byte> numeric_scratch,gather_payload,gather_metadata,gather_scratch;
    std::vector<std::byte> input_gather_payload,input_gather_metadata,input_gather_scratch;
    std::vector<PlaneLayout> row_planes;
    std::vector<DataFrameView> numeric_frames;
    std::vector<char> stream_storage;
    std::shared_ptr<const void> ingress_owner; // Declared before handles: released after them.
    OwnedDataFrame owned_ingress;
    FrameStamp numeric_stamp;
    std::uint64_t last_sequence=0;
    std::uint64_t row_ordinal=0;
    NodeId history_origin=-1;
    bool history_origin_valid=false;
    bool numeric_marker=false;
    float peak_hz=0,magnitude=0;
    void clear() noexcept {
        frames = oldest = progress = remainder = hop = 0;
        if(auto* resampler=std::get_if<ResamplerKernel>(&kernel))resampler->operation().reset();
        std::fill(pcm.begin(),pcm.end(),0);
        pending_discontinuity = true;
        numeric_marker=false;
        owned_ingress={};
        ingress_owner.reset();
        last_sequence=0;numeric_stamp.stream_id={};
        row_ordinal=0;
        history_origin_valid=false;
        for(auto& route:route_state)route={};
        peak_hz=magnitude=0;
        for(auto& mask:route_masks)std::fill(mask.begin(),mask.end(),std::byte{0});
    }
};
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
std::size_t index(const std::vector<PlanNode>& nodes,NodeId id) noexcept {
    for(std::size_t i=0;i<nodes.size();++i)if(nodes[i].node.id==id)return i;
    return absent;
}
} // namespace
struct RuntimeContext::Impl {
    std::vector<State> states;
    const PreparedGraph *plan = nullptr;
};
struct PreparedGraph::Impl {
    application::MemoryRequirements memory;
    std::vector<PlanNode> nodes;
    std::shared_ptr<const NumericProcessingGraph> numeric_graph;
    std::shared_ptr<const NumericProcessingGraph> numeric_source_graph;
    std::shared_ptr<const std::vector<std::byte>> silence;
    NumericRuntimeProfile numeric_profile{};
    std::optional<ProcessingGraphAdmission> numeric_admission;
};
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
PreparedGraph::PreparedGraph() : impl_(std::make_unique<Impl>()) {}
PreparedGraph::~PreparedGraph() = default;
application::MemoryRequirements PreparedGraph::memory_requirements() const noexcept {return impl_->memory;}
std::optional<DataContract> PreparedGraph::port_contract(NodeId id,PortDirection direction,std::uint16_t port) const noexcept {
    if(!impl_->numeric_graph)return {};
    const auto at=index(impl_->nodes,id);if(at==absent)return {};
    const auto& node=impl_->numeric_graph->nodes[impl_->nodes[at].configured];
    const auto& ports=direction==PortDirection::Input?node.inputs:node.outputs;
    for(const auto& candidate:ports)if(candidate.id==port)return candidate.contract.view();
    return {};
}
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
            dst.pending_discontinuity=src.pending_discontinuity;
            if(n.node.kind==NodeKind::PcmResampler)
                std::get<ResamplerKernel>(dst.kernel).operation().copy_state_from(std::get<ResamplerKernel>(src.kernel).operation());
            if(n.selector_changed)dst.pending_discontinuity=true;
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
                        if(parent==absent)break;producer=parent;
                    }
                    const auto& retained=old_context.impl_->states[producer];
                    const auto new_capacity=dst.route_masks[p].size()/2,old_capacity=src.route_masks[p].size()/2;
                    const auto kept=std::min(retained.progress,new_capacity);
                    if(old_capacity && retained.frames>=kept)for(std::size_t row=0;row<kept;++row){
                        const auto from=(retained.oldest+retained.frames-kept+row)%old_capacity,to=new_capacity-kept+row;
                        dst.route_masks[p][to]=dst.route_masks[p][to+new_capacity]=src.route_masks[p][from];}
                }
            if(dst.pcm.size()==src.pcm.size())std::copy(src.pcm.begin(),src.pcm.end(),dst.pcm.begin());
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
namespace {
bool numeric_add(std::size_t& a,std::size_t b) noexcept {
    if(b>SIZE_MAX-a)return false;
    a+=b;return true;
}
bool numeric_mul(std::size_t a,std::size_t b,std::size_t& out) noexcept {
    if(a && b>SIZE_MAX/a)return false;
    out=a*b;return true;
}
const ProcessingPort* numeric_port(std::span<const ProcessingPort> ports,PortId id) noexcept {
    for(const auto& p:ports)if(p.id==id)return &p;
    return nullptr;
}
bool numeric_geometry(const NumericContract& c,std::size_t& bytes,std::size_t& stride,
    std::size_t& plane,std::array<std::size_t,2>& shape) noexcept {
    std::size_t width=0;
    if(storage_width(c,width)!=Status::ok || c.representation.alignment>alignof(std::max_align_t))return false;
    const auto alignment=c.representation.alignment;
    if(!alignment || width>SIZE_MAX-(alignment-1))return false;
    const auto lane=(width+alignment-1)/alignment*alignment;
    shape={1,1};if(c.shape.rank!=Rank::Scalar)shape[0]=std::size_t(c.shape.extents[0].maximum);
    if(c.shape.rank==Rank::Matrix)shape[1]=std::size_t(c.shape.extents[1].maximum);
    std::size_t elements=0;if(!numeric_mul(shape[0],shape[1],elements))return false;
    if(c.representation.layout==ChannelLayout::Interleaved){
        if(width%alignment || !numeric_mul(width,c.channel_order.size(),stride))return false;
        plane=width;return numeric_mul(elements,stride,bytes);
    }
    stride=lane;if(!numeric_mul(elements,lane,plane))return false;
    return numeric_mul(plane,c.channel_order.size(),bytes);
}
bool zero_bytes(const NumericContract& c) noexcept {
    const auto* dtype=dtype_descriptor(c.dtype);
    return dtype && c.representation.normalization!=Normalization::UnsignedUnit;
}
bool direct_window(const NumericContract& c) noexcept {
    return c.dtype==DType::Float32 && c.shape.rank==Rank::Vector &&
        c.representation.encoding==StorageEncoding::Native && c.representation.layout==ChannelLayout::Planar &&
        c.representation.alignment<=alignof(float) && !c.representation.valid_bits &&
        (c.representation.normalization==Normalization::None || c.representation.normalization==Normalization::FloatUnit);
}
Status numeric_buffer(const NumericContract& c,NumericBuffer& b,const std::shared_ptr<const std::vector<std::byte>>& shared={}) {
    std::size_t size=0,stride=0,plane=0;
    if(!numeric_geometry(c,size,stride,plane,b.actual))return Status::invalid_argument;
    if(shared && zero_bytes(c)){if(size>shared->size())return Status::exhausted;b.silence=shared;b.silence_bytes=size;}
    else if(c.dtype==DType::Float32 && c.representation.encoding==StorageEncoding::Native)b.floats.resize(size/sizeof(float));
    else b.bytes.resize(size);
    b.planes.resize(c.channel_order.size());
    for(std::size_t i=0;i<b.planes.size();++i)b.planes[i]={i*plane,c.shape.rank==Rank::Matrix?std::array{stride*b.actual[1],stride}:std::array{stride,std::size_t{0}}};
    MutableNumericView v{&c,b.storage(),b.planes,b.actual,{},{}};
    for(std::size_t axis=0;axis<2;++axis)if(c.coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE){
        b.coordinates[axis].resize(b.actual[axis]*8*(c.shape.rank==Rank::Matrix && axis==0?2:1));
        v.coordinates.axes[axis]={b.coordinates[axis].data(),b.actual[axis]*8};}
    if(b.silence){v.storage={};auto value=v.view();value.storage=std::as_const(b).storage();value.semantic_silence=true;
        return validate_view({{},value},{b.planes.size(),SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX});}
    return fill_silence(v,{b.planes.size(),SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX});
}
bool packet_contract(const StructuredContract& c) noexcept {
    return c.schema=="wled-audio-sync" && c.version==1 && c.encoding=="audio-sync-44-le" &&
        c.payload_bytes.minimum==wled_protocol::packet_size && c.payload_bytes.maximum==wled_protocol::packet_size;
}
Status numeric_buffer(const DataContract& contract,NumericBuffer& buffer,const std::shared_ptr<const std::vector<std::byte>>& shared={}) {
    if(const auto* numeric=std::get_if<NumericContract>(&contract))return numeric_buffer(*numeric,buffer,shared);
    const auto& structured=std::get<StructuredContract>(contract);buffer.bytes.resize(std::size_t(structured.payload_bytes.maximum));
    if(packet_contract(structured)){
        const auto result=wled_protocol::silence(0,{reinterpret_cast<std::uint8_t*>(buffer.bytes.data()),buffer.bytes.size()},{0,wled_protocol::packet_size});
        if(result.status!=wled_protocol::Status::Ok)return Status::invalid_argument;
    }
    return Status::ok;
}
NumericGatherRequest numeric_gather_request(const ResolvedProcessingNode& n,const NumericRuntimeProfile& p) noexcept {
    const auto* in=numeric_port(n.operation==ProcessingOperation::History || n.operation==ProcessingOperation::Source || n.operation==ProcessingOperation::Converter?n.outputs:n.inputs,0);
    auto contract=std::get<NumericContract>(in->contract.view());std::span<const std::uint32_t> selected;
    if(n.operation==ProcessingOperation::Reduce){const auto& r=std::get<ReduceProcessingParameters>(*n.parameters);
        if(r.channels==ReductionChannels::SelectedChannels)selected=r.selected_channels;}
    const bool row=contract.shape.rank==Rank::Matrix && (n.operation!=ProcessingOperation::Reduce || std::get<ReduceProcessingParameters>(*n.parameters).rows==ReductionRows::LatestHistoryRow);
    return {contract,selected,{p.admission.frame,p.admission.frame.max_payload_bytes,p.admission.frame.max_metadata_bytes,p.kernels.max_scratch_bytes},row};
}
dsp_kernels::ScalarReduction numeric_reduction(NumericReduction op) noexcept {
    switch(op){case NumericReduction::MaxAbs:return dsp_kernels::ScalarReduction::MaximumAbsolute;
    case NumericReduction::SignedMean:return dsp_kernels::ScalarReduction::SignedMean;
    case NumericReduction::RMS:return dsp_kernels::ScalarReduction::Rms;}
    return dsp_kernels::ScalarReduction::MaximumAbsolute;
}
NumericContract numeric_projection(NumericContract c,std::span<const std::uint32_t> selected,bool matrix_row=false) noexcept {
    if(matrix_row){c.semantic=c.history_element;c.history_element=Semantic::Numeric;c.shape.rank=Rank::Vector;c.shape.axes={c.shape.axes[1],Axis::None};
        c.shape.extents={c.shape.extents[1],ExtentBounds{1,1}};c.coordinates={c.coordinates[1],AxisCoordinates{}};}
    if(!selected.empty())c.channel_order=selected;
    return c;
}
bool numeric_float_dsp(const ResolvedProcessingNode&,ProcessingResources&) noexcept;
bool numeric_parameter(const ResolvedProcessingNode&,std::string_view,double&) noexcept;
ProcessingSupport numeric_support(const void* context,const ResolvedProcessingNode& n,const ProcessingResourceProfile&) noexcept {
    ProcessingSupport out;if(!context)return out;
    const auto& p=*static_cast<const NumericRuntimeProfile*>(context);
    out.resources.plan_bytes=sizeof(PlanNode)+sizeof(State)+2*sizeof(std::size_t);
    if((n.kind=="ScalarView" || n.operation==ProcessingOperation::Extract) && !numeric_add(out.resources.plan_bytes,sizeof(PlanNode::EndpointPlan)))return out;
    if(n.kind=="ScalarView" && !numeric_add(out.resources.plan_bytes,n.inputs.size()*sizeof(PlanNode::ScalarInput)))return out;
    if(!numeric_add(out.resources.payload_bytes,p.admission.frame.max_string_bytes))return out;
    // Reference reservation for bounded callback/dispatch depth, not a native ABI measurement.
    if(!numeric_mul(p.admission.max_nodes,1536,out.resources.stack_bytes) || !numeric_add(out.resources.stack_bytes,4096))return out;
    for(auto ports:{n.inputs,n.outputs})for(const auto& port:ports){
        const auto contract=port.contract.view();const auto* c=std::get_if<NumericContract>(&contract);
        if(!c){const auto& structured=std::get<StructuredContract>(contract);
            if(n.operation!=ProcessingOperation::Source && n.operation!=ProcessingOperation::Sink && n.operation!=ProcessingOperation::Gate &&
               n.operation!=ProcessingOperation::Switch && n.operation!=ProcessingOperation::Splitter && n.operation!=ProcessingOperation::WLEDEncoder)return out;
            if(structured.payload_bytes.maximum>p.admission.frame.max_payload_bytes ||
               !numeric_add(out.resources.payload_bytes,std::size_t(structured.payload_bytes.maximum)) ||
               (port.direction==PortDirection::Output && !numeric_add(out.resources.plan_bytes,sizeof(DataContract)+sizeof(DataFrameView)+sizeof(NumericBuffer))))return out;
            continue;
        }
        if(!p.history_enabled && n.operation==ProcessingOperation::History)return out;
        std::size_t metadata=c->channel_order.size()*(sizeof(PlaneLayout)+sizeof(std::uint32_t));
        if(!numeric_add(metadata,c->clock.domain.size()) || !numeric_add(metadata,p.admission.frame.max_string_bytes))return out;
        if(c->shape.rank==Rank::Matrix && !n.inputs.empty() && n.operation!=ProcessingOperation::History &&
           !numeric_add(metadata,std::size_t(c->shape.extents[0].maximum)))return out;
        std::size_t coordinates=0;
        for(std::size_t axis=0;axis<2;++axis)if(c->coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE){std::size_t bytes=0;
            if(!numeric_mul(std::size_t(c->shape.extents[axis].maximum),8,bytes) || !numeric_add(coordinates,bytes))return out;}
        if(coordinates>p.admission.frame.max_coordinate_bytes || !numeric_add(metadata,coordinates) || metadata>p.admission.frame.max_metadata_bytes ||
           !numeric_add(out.resources.payload_bytes,coordinates))return out;
        if(c->shape.rank==Rank::Matrix && c->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE &&
           !numeric_add(out.resources.payload_bytes,std::size_t(c->shape.extents[0].maximum)*8))return out;
        std::size_t bytes=0,stride=0,plane=0;std::array<std::size_t,2> shape;
        if(!numeric_geometry(*c,bytes,stride,plane,shape) || bytes>p.admission.frame.max_payload_bytes)return out;
        const bool shared_zero=zero_bytes(*c) && (port.direction==PortDirection::Input || n.operation==ProcessingOperation::Source ||
            (n.operation==ProcessingOperation::Transform && n.kind=="Window" && direct_window(*c)));
        // Input contract and zero-buffer objects are inline in PlanNode/State,
        // already charged above. Only output objects occupy the three resized
        // vectors; every numeric buffer still owns its dynamic plane layouts.
        if((!shared_zero && !numeric_add(out.resources.payload_bytes,bytes)) ||
           !numeric_add(out.resources.plan_bytes,c->channel_order.size()*sizeof(PlaneLayout)) ||
           (port.direction==PortDirection::Output && !numeric_add(out.resources.plan_bytes,sizeof(DataContract)+sizeof(DataFrameView)+sizeof(NumericBuffer))))return out;
    }
    if(n.operation==ProcessingOperation::WLEDEncoder){
        const auto input=numeric_port(n.inputs,0)->contract.view();const auto* c=std::get_if<NumericContract>(&input);double gain=0;
        if(!c || c->dtype!=DType::Float32 || c->representation.encoding!=StorageEncoding::Native ||
           c->representation.normalization!=Normalization::None || c->channel_order.size()!=1 ||
           c->shape.rank!=Rank::Vector || c->shape.extents[0].maximum>wled_protocol::formatted_mel_count ||
           !numeric_parameter(n,"gain",gain) || !numeric_add(out.resources.payload_bytes,std::size_t(c->shape.extents[0].maximum)*sizeof(float)))return out;
    }
    if(n.operation==ProcessingOperation::Transform && !numeric_float_dsp(n,out.resources))return out;
    const auto primary=[](std::span<const ProcessingPort> ports){const auto* zero=numeric_port(ports,0);return zero?zero:(ports.empty()?nullptr:&ports.front());};
    auto get=[primary](std::span<const ProcessingPort> ports){return std::get<NumericContract>(primary(ports)->contract.view());};
    auto history=[primary](std::span<const ProcessingPort> ports){const auto* port=primary(ports);if(!port)return false;const auto contract=port->contract.view();const auto* c=std::get_if<NumericContract>(&contract);return c && c->shape.rank==Rank::Matrix;};
    if((!n.inputs.empty() && history(n.inputs) && n.operation!=ProcessingOperation::History) || (n.operation==ProcessingOperation::Source && history(n.outputs))){
        const auto rows=get(n.operation==ProcessingOperation::Source?n.outputs:n.inputs).shape.extents[0].maximum;std::size_t mask_bytes=0;
        if(!numeric_mul(std::size_t(rows),2*std::max(std::size_t{1},n.outputs.size()),mask_bytes) ||
           !numeric_add(out.resources.payload_bytes,mask_bytes) || !numeric_add(out.resources.plan_bytes,std::max(std::size_t{1},n.outputs.size())*(sizeof(std::vector<std::byte>)+sizeof(HistoryRouteState))))return out;
    }
    if(n.operation==ProcessingOperation::Source && history(n.outputs)){
        std::size_t bytes=0,stride=0,plane=0;std::array<std::size_t,2> shape;
        if(!numeric_geometry(get(n.outputs),bytes,stride,plane,shape) || !numeric_add(out.resources.payload_bytes,bytes) ||
           !numeric_add(out.resources.plan_bytes,p.admission.frame.max_metadata_bytes+sizeof(OwnedDataFrame)))return out;}
    if(n.operation==ProcessingOperation::History){const auto c=get(n.inputs);
        const auto h=get(n.outputs);
        if(c.shape.extents[0].minimum!=c.shape.extents[0].maximum)return out;
        if(h.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE && h.coordinates[0].unit!=NumericUnit::Seconds && h.coordinates[0].unit!=NumericUnit::Frames)return out;}
    if(n.operation==ProcessingOperation::Converter){const auto& policy=std::get<ConverterProcessingParameters>(*n.parameters).policy;
        const auto s=dsp_kernels::conversion_support({get(n.inputs),get(n.outputs),policy,p.kernels,p.admission.frame},p.capabilities);
        if(s.status!=dsp_kernels::NumericStatus::Ok)return out;
        out.resources.scratch_bytes=s.requirements.scratch_bytes;if(!numeric_add(out.resources.stack_bytes,s.requirements.stack_bytes))return out;
        if(!numeric_add(out.resources.plan_bytes,s.requirements.plan_bytes))return out;
        if(history(n.inputs)){NumericGatherRequirements req;const NumericGatherRequest request{get(n.inputs),{},
                {p.admission.frame,p.admission.frame.max_payload_bytes,p.admission.frame.max_metadata_bytes,p.kernels.max_scratch_bytes},true};
            if(numeric_gather_requirements(request,req)!=Status::ok || !numeric_add(out.resources.plan_bytes,req.plan_metadata_bytes+sizeof(GatherOwner)+sizeof(dsp_kernels::PreparedConversion)) ||
               !numeric_add(out.resources.payload_bytes,req.payload_bytes+req.output_metadata_bytes+get(n.outputs).channel_order.size()*sizeof(PlaneLayout)) ||
               !numeric_add(out.resources.scratch_bytes,req.scratch_bytes))return out;}}
    if(n.operation==ProcessingOperation::History || n.operation==ProcessingOperation::Reduce ||
       (n.operation==ProcessingOperation::Source && history(n.outputs)) ||
       (n.operation==ProcessingOperation::Converter && history(n.outputs)) ||
       (n.operation==ProcessingOperation::Sink && history(n.inputs))){
        const auto request=numeric_gather_request(n,p);NumericGatherRequirements r;
        if(numeric_gather_requirements(request,r)!=Status::ok)return out;
        if(!numeric_add(out.resources.plan_bytes,r.plan_metadata_bytes+sizeof(GatherOwner)) ||
           !numeric_add(out.resources.payload_bytes,r.payload_bytes+r.output_metadata_bytes) || !numeric_add(out.resources.scratch_bytes,r.scratch_bytes))return out;
        if(n.operation==ProcessingOperation::Reduce){const auto& reduction=std::get<ReduceProcessingParameters>(*n.parameters);
            const auto input=numeric_projection(request.input,request.selected_channels,request.matrix_row);
            const auto s=dsp_kernels::reduction_support({input,get(n.outputs),numeric_reduction(reduction.operation),reduction.precision_loss,reduction.rounding,p.kernels,p.admission.frame},p.capabilities);
            if(s.status!=dsp_kernels::NumericStatus::Ok)return out;
            if(!numeric_add(out.resources.scratch_bytes,s.requirements.scratch_bytes) || !numeric_add(out.resources.plan_bytes,s.requirements.plan_bytes))return out;
            if(!numeric_add(out.resources.stack_bytes,s.requirements.stack_bytes))return out;
        }
    }
    out.supported=true;return out;
}
Status numeric_kernel_status(dsp_kernels::NumericStatus s) noexcept {
    if(s==dsp_kernels::NumericStatus::Ok)return Status::ok;
    if(s==dsp_kernels::NumericStatus::EmptyPopulation)return Status::unavailable;
    if(s==dsp_kernels::NumericStatus::BufferTooSmall)return Status::buffer_too_small;
    if(s==dsp_kernels::NumericStatus::Overflow)return Status::overflow;
    return Status::invalid_argument;
}
bool numeric_compatible_node(const ConfiguredProcessingNode& a,const ConfiguredProcessingNode& b) noexcept {
    if(a.kind!=b.kind)return false;
    if(a.kind=="InputSelector"){
        // Admission requires an exact common contract at every selector port.
        // Adding/removing unused inputs does not change its logical output clock.
        const auto* input=&a.inputs.front();const auto* old_input=&b.inputs.front();
        const auto* output=numeric_port(a.outputs,0);const auto* old_output=numeric_port(b.outputs,0);
        return input && old_input && output && old_output &&
            same_physical_contract(input->contract.view(),old_input->contract.view()) &&
            same_physical_contract(output->contract.view(),old_output->contract.view());
    }
    if(a.inputs.size()!=b.inputs.size())return false;
    for(auto direction:{PortDirection::Input,PortDirection::Output}){
        const auto& x=direction==PortDirection::Input?a.inputs:a.outputs;const auto& y=direction==PortDirection::Input?b.inputs:b.outputs;
        for(const auto& port:x){const auto* other=numeric_port(y,port.id);
            if(!other){if(direction==PortDirection::Input)return false;continue;}
            if(port.direction!=other->direction)return false;
            auto current=port.contract.view(),prior=other->contract.view();
            if(a.kind!="NetworkInput" && a.kind!="AudioSource" && a.kind!="TestSignal"){
                // Capacity is retention policy, not a change of signal identity.
                auto* value=std::get_if<NumericContract>(&current);const auto* before=std::get_if<NumericContract>(&prior);
                if(value && before && value->shape.rank==Rank::Matrix && before->shape.rank==Rank::Matrix)value->shape.extents[0]=before->shape.extents[0];
            }
            if(!same_physical_contract(current,prior))return false;}}
    // Immutable kernels/coefficients are replaced during preparation. Compatible
    // clocks/port geometry preserve running history across value/policy edits.
    return true;
}
bool numeric_parameter(const ResolvedProcessingNode& n,std::string_view key,double& out) noexcept {
    ProcessingParameterView value;if(resolve_processing_parameter(n,key,value)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&value);if(!literal)return false;
    // Registered DSP sizes/gain are bounded small fields, never payload integers.
    out=std::visit([](auto x){return static_cast<double>(x);},*literal);return std::isfinite(out);
}
bool frequency_axis(const AxisCoordinates& axis,std::uint32_t rate,std::size_t samples) noexcept {
    if(axis.mode==CoordinateMode::ExplicitFloat64LE)return axis.unit==NumericUnit::Hertz;
    if(axis.mode!=CoordinateMode::Regular || axis.unit!=NumericUnit::Hertz || axis.origin.numerator ||
       axis.step.numerator<=0 || !axis.step.denominator || !samples)return false;
    auto numerator=std::uint64_t(axis.step.numerator),denominator=axis.step.denominator;
    const auto divisor=std::gcd(numerator,denominator);numerator/=divisor;denominator/=divisor;
    const auto expected=std::gcd(std::uint64_t(rate),std::uint64_t(samples));
    return numerator==std::uint64_t(rate)/expected && denominator==std::uint64_t(samples)/expected;
}
bool numeric_float_dsp(const ResolvedProcessingNode& n,ProcessingResources& resources) noexcept {
    const auto input=std::get<NumericContract>(numeric_port(n.inputs,0)->contract.view());
    const auto output=std::get<NumericContract>(numeric_port(n.outputs,0)->contract.view());
    if(input.dtype!=DType::Float32 || output.dtype!=DType::Float32 || input.representation.encoding!=StorageEncoding::Native ||
       input.representation.layout!=ChannelLayout::Planar || input.representation.valid_bits || (input.representation.normalization!=Normalization::None && input.representation.normalization!=Normalization::FloatUnit))return false;
    if(n.kind!="PcmResampler" && output.shape.extents[0].minimum!=output.shape.extents[0].maximum)return false;
    const auto columns=std::size_t(input.shape.extents[0].maximum),result=std::size_t(output.shape.extents[0].maximum),planes=input.channel_order.size();
    auto kind=node_kind(n.kind);if(!kind)return false;
    double parameter=0;
    switch(*kind){
    case NodeKind::PcmResampler:
        if(input.shape.rank!=Rank::Vector || !planes || planes>2 || !columns || columns>8192 || result>65536 ||
            output.shape.extents[0].minimum!=1 || !dsp_kernels::PcmResampler::supported(input.clock.sample_rate,output.clock.sample_rate) ||
            result<dsp_kernels::PcmResampler::maximum_output(columns,input.clock.sample_rate,output.clock.sample_rate))return false;
        return numeric_add(resources.payload_bytes,sizeof(dsp_kernels::PcmResampler)) &&
            numeric_add(resources.payload_bytes,(columns+result)*planes*sizeof(float)) &&
            numeric_add(resources.payload_bytes,dsp_kernels::PcmResampler::storage_bytes(planes,input.clock.sample_rate,output.clock.sample_rate));
    case NodeKind::Window:
        if(output.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE && input.coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE)return false;
        if(input.shape.rank!=Rank::Vector || !planes || !input.clock.sample_rate || !numeric_parameter(n,"size",parameter) || parameter!=double(result))return false;
        if(result<512 || result>dsp_kernels::max_fft_samples || (result&(result-1)))return false;
        return numeric_add(resources.payload_bytes,result*planes*sizeof(float));
    case NodeKind::WindowFunction: {
        ProcessingParameterView value;if(resolve_processing_parameter(n,"function",value)!=Status::ok || !std::holds_alternative<std::string_view>(value))return false;
        const auto function=std::get<std::string_view>(value);
        if(function!="hann" && function!="hamming" && function!="blackman" && function!="rectangular")return false;
        return columns==result && planes && numeric_add(resources.payload_bytes,columns*sizeof(float));}
    case NodeKind::StereoFFT: {
        if(!planes || columns!=result*2 || columns<512 || columns>dsp_kernels::max_fft_samples || (columns&(columns-1)))return false;
        if(output.coordinates[0].mode!=CoordinateMode::None && !frequency_axis(output.coordinates[0],input.clock.sample_rate,columns))return false;
        auto memory=dsp_kernels::estimate_memory(columns,0);if(!memory)return false;
        if(!numeric_add(resources.scratch_bytes,memory->fft_scratch_bytes+memory->shared_radix2_tables_bytes))return false;
        break;}
    case NodeKind::BarkWeighting:
        if(!input.clock.sample_rate || columns!=result || !planes)return false;
        if(!frequency_axis(input.coordinates[0],input.clock.sample_rate,columns*2))return false;
        if(!numeric_add(resources.payload_bytes,columns*sizeof(float)))return false;
        break;
    case NodeKind::SpectrumMerge:
        if(planes!=2 || columns!=result || !numeric_parameter(n,"merge",parameter))return false;
        break;
    case NodeKind::SpectrumGain:
        if(planes!=1 || columns!=result || !numeric_parameter(n,"gain",parameter))return false;
        break;
    case NodeKind::Mel:
        if(output.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE && output.coordinates[0].unit!=NumericUnit::Hertz && output.coordinates[0].unit!=NumericUnit::Values)return false;
        if(planes!=1 || !input.clock.sample_rate || columns>dsp_kernels::max_fft_samples/2 || result>dsp_kernels::max_mel_bands ||
           !numeric_parameter(n,"bands",parameter) || parameter!=double(result))return false;
        if(!frequency_axis(input.coordinates[0],input.clock.sample_rate,columns*2))return false;
        if(!numeric_add(resources.scratch_bytes,(result+2)*sizeof(double)) || !numeric_add(resources.payload_bytes,(result+2)*sizeof(double)))return false;
        break;
    default:return false;
    }
    if(*kind==NodeKind::Mel)return numeric_add(resources.payload_bytes,(columns+result)*sizeof(float));
    return true;
}
}
const NumericProcessingGraph* PreparedGraph::numeric_definition() const noexcept {return impl_->numeric_source_graph?&*impl_->numeric_source_graph:nullptr;}
std::shared_ptr<const NumericProcessingGraph> PreparedGraph::numeric_snapshot() const noexcept {return impl_->numeric_source_graph;}
std::optional<ProcessingGraphAdmission> PreparedGraph::numeric_admission() const noexcept {return impl_->numeric_admission;}
NumericDeviceCapabilities PreparedGraph::numeric_capabilities(const NumericRuntimeProfile& p) noexcept {
    auto resources=p.resources;
    if(!numeric_add(resources.retained_working_bytes,fixed_plan_reservation) || !numeric_add(resources.retained_working_bytes,p.admission.frame.max_payload_bytes))resources.retained_working_bytes=SIZE_MAX;
    return {&p,numeric_support,resources};
}
Status PreparedGraph::prepare_numeric(const NumericProcessingGraph& original,const NumericRuntimeProfile& profile,
    std::shared_ptr<const PreparedGraph>& result,std::unique_ptr<RuntimeContext>& context,const PreparedGraph* previous,const RuntimeContext* old_context) {
#if defined(__cpp_exceptions)
    try {
#endif
    NumericProcessingGraph lowered;
    if(!valid_execution_interval(original.execution_interval))return Status::invalid_argument;
    std::size_t preflight=0;const auto footprint=processing_graph_owned_bytes(original,profile.admission,preflight);
    if(footprint!=Status::ok)return footprint;
    const auto lowering=lower_dtype_edges(original,profile.admission,lowered);
    if(!lowering)return lowering.error==ProcessingError::Limit?Status::exhausted:Status::invalid_argument;
    const auto& g=lowered;
    if((previous && (!old_context || !old_context->impl_ || old_context->impl_->plan!=previous)) || (!previous && old_context))return Status::invalid_argument;
    if(g.nodes.size()>profile.admission.max_nodes || g.nodes.size()>SIZE_MAX/2)return Status::exhausted;
    std::vector<std::size_t> order(g.nodes.size()*2);ProcessingGraphAdmission admission;auto checked_profile=profile;
    std::size_t source_bytes=0;
    if(g.nodes.size()!=original.nodes.size()){
        const auto footprint=processing_graph_owned_bytes(original,profile.admission,source_bytes);if(footprint!=Status::ok)return footprint;
        if(!numeric_add(checked_profile.resources.retained_working_bytes,source_bytes))return Status::overflow;
    }
    if(previous && old_context){const auto mem=previous->memory_requirements();std::size_t old=mem.state_bytes;
        if(!numeric_add(old,mem.buffer_bytes) || !numeric_add(old,mem.scratch_bytes))return Status::overflow;
        if(previous->impl_->silence && previous->impl_->silence->size()==profile.admission.frame.max_payload_bytes)old-=previous->impl_->silence->size();
        if(!numeric_add(checked_profile.resources.retained_working_bytes,old))return Status::overflow;}
    const auto check=admit_processing_graph(g,profile.admission,numeric_capabilities(checked_profile),order,admission);
    if(!check)return check.error==ProcessingError::ResourceLimit || check.error==ProcessingError::Limit?Status::exhausted:Status::invalid_argument;
    // Domain peak already includes DTO ownership and topology workspace. Publish
    // retained plan ownership too, so a later Prepare cannot omit the old DTO.
    const auto metadata_and_plan=admission.peak_bytes-checked_profile.resources.retained_working_bytes-order.size()*sizeof(std::size_t)-
        admission.resources.payload_bytes-admission.resources.scratch_bytes-admission.resources.stack_bytes-profile.admission.frame.max_payload_bytes;
    auto plan=std::shared_ptr<PreparedGraph>(new PreparedGraph);auto c=std::make_unique<RuntimeContext>();
    plan->impl_->numeric_graph=std::make_shared<const NumericProcessingGraph>(g);plan->impl_->numeric_profile=profile;
    plan->impl_->numeric_source_graph=source_bytes?std::make_shared<const NumericProcessingGraph>(original):plan->impl_->numeric_graph;
    plan->impl_->silence=previous && previous->impl_->silence && previous->impl_->silence->size()==profile.admission.frame.max_payload_bytes?
        previous->impl_->silence:std::make_shared<const std::vector<std::byte>>(profile.admission.frame.max_payload_bytes);
    plan->impl_->memory={metadata_and_plan+source_bytes,admission.resources.payload_bytes+profile.admission.frame.max_payload_bytes,admission.resources.scratch_bytes};
    plan->impl_->numeric_admission=admission;
    plan->impl_->nodes.reserve(g.nodes.size());c->impl_->states.resize(g.nodes.size());
    for(std::size_t i=0;i<g.nodes.size();++i){const auto configured=order[g.nodes.size()+i];const auto& cn=plan->impl_->numeric_graph->nodes[configured];
        ResolvedProcessingNode resolved;if(!resolve_processing_node(cn,profile.admission,resolved))return Status::invalid_argument;
        PlanNode n;n.configured=configured;n.node.id=cn.id;n.operation=resolved.operation;n.node.enabled=cn.enabled;
        n.internal_conversion=std::none_of(original.nodes.begin(),original.nodes.end(),[&](const auto& node){return node.id==cn.id;});
        if(auto kind=node_kind(cn.kind))n.node.kind=*kind;
        if(!cn.inputs.empty())n.numeric_input=(resolved.operation==ProcessingOperation::InputSelector || cn.kind=="ScalarView"?&cn.inputs.front():numeric_port(cn.inputs,0))->contract.view();
        if(n.operation==ProcessingOperation::Extract){
            n.endpoint_plan=std::make_unique<PlanNode::EndpointPlan>();
            for(std::size_t axis=0;axis<3;++axis){double value=0;if(!numeric_parameter(resolved,std::array{"channel_index","index","column"}[axis],value))return Status::invalid_argument;n.endpoint_plan->indices[axis]=std::size_t(value);}
        }
        if(cn.kind=="ScalarView"){
            n.endpoint_plan=std::make_unique<PlanNode::EndpointPlan>();n.endpoint_plan->inputs.reserve(cn.inputs.size());
            for(const auto& port:cn.inputs){PlanNode::ScalarInput input;input.contract=port.contract.view();input.port=port.id;
                for(const auto& edge:g.edges)if(edge.to==cn.id && edge.input_port==port.id){input.parent=index(plan->impl_->nodes,edge.from);input.parent_port=edge.output_port;if(input.parent==absent)return Status::invalid_argument;input.clock=plan->impl_->nodes[input.parent].clock;}
                n.endpoint_plan->inputs.push_back(std::move(input));}
        }
        n.numeric_outputs.resize(cn.outputs.size());for(const auto& port:cn.outputs)n.numeric_outputs[port.id]=port.contract.view();
        n.identity_bypass=!cn.enabled && !cn.inputs.empty() && cn.outputs.size()==1 &&
            (n.operation==ProcessingOperation::Converter || n.operation==ProcessingOperation::Transform || n.operation==ProcessingOperation::Reduce) &&
            same_physical_contract(n.numeric_input,n.numeric_outputs[0]);
        auto& state=c->impl_->states[i];state.numeric_outputs.resize(cn.outputs.size());state.numeric_frames.resize(cn.outputs.size());state.stream_storage.resize(profile.admission.frame.max_string_bytes);
        for(std::size_t j=0;j<cn.outputs.size();++j){const auto* output=std::get_if<NumericContract>(&n.numeric_outputs[j]);
            const bool shared=n.operation==ProcessingOperation::Source || (n.operation==ProcessingOperation::Transform && n.node.kind==NodeKind::Window && output && direct_window(*output));
            const auto status=numeric_buffer(n.numeric_outputs[j],state.numeric_outputs[j],shared?plan->impl_->silence:nullptr);if(status!=Status::ok)return status;}
        if(!cn.inputs.empty()){
            if(numeric_buffer(n.numeric_input,state.numeric_zero,plan->impl_->silence)!=Status::ok)return Status::invalid_argument;
            if(const auto* input=std::get_if<NumericContract>(&n.numeric_input);input && input->shape.rank==Rank::Matrix && n.operation!=ProcessingOperation::History){
                state.route_masks.resize(std::max(std::size_t{1},cn.outputs.size()));
                state.route_state.resize(state.route_masks.size());for(auto& mask:state.route_masks)mask.resize(std::size_t(input->shape.extents[0].maximum)*2);
            }
        }
        if(n.operation==ProcessingOperation::InputSelector){double selected=0;
            if(!numeric_parameter(resolved,"selected_input",selected))return Status::invalid_argument;
            n.selected_input=std::uint16_t(selected);
        }
        for(const auto& edge:g.edges)if(!n.scalar_view() && edge.to==cn.id &&
            (n.operation!=ProcessingOperation::InputSelector || edge.input_port==n.selected_input)){
            n.parent=index(plan->impl_->nodes,edge.from);n.parent_port=edge.output_port;
            if(n.parent==absent)return Status::invalid_argument;
            n.clock=plan->impl_->nodes[n.parent].clock;}
        // A selector owns the continuity of its compatible output clock, even
        // when the physical producer of its chosen input changes.
        if(n.operation==ProcessingOperation::Source || n.operation==ProcessingOperation::InputSelector)n.clock=i;
        const auto old=previous&&previous->impl_->numeric_graph&&previous->impl_->numeric_graph->graph_id==g.graph_id?index(previous->impl_->nodes,cn.id):absent;
        n.reset=old==absent || !numeric_compatible_node(cn,previous->impl_->numeric_graph->nodes[previous->impl_->nodes[old].configured]);
        if(n.operation==ProcessingOperation::InputSelector && old!=absent){const auto& prior=previous->impl_->nodes[old];
            n.selector_changed=n.selected_input!=prior.selected_input || n.parent_port!=prior.parent_port ||
                (n.parent==absent)!=(prior.parent==absent) ||
                (n.parent!=absent && prior.parent!=absent && plan->impl_->nodes[n.parent].node.id!=previous->impl_->nodes[prior.parent].node.id);
        }
        if(!n.reset && old_context->impl_->states[old].numeric_stamp.stream_id.size()>state.stream_storage.size())return Status::exhausted;
        if(n.clock==absent && !n.reset){const auto old_clock=previous->impl_->nodes[old].clock;
            if(old_clock!=absent)n.clock=index(plan->impl_->nodes,previous->impl_->nodes[old_clock].node.id);}
        if(n.operation==ProcessingOperation::Converter){n.conversion=std::make_unique<dsp_kernels::PreparedConversion>();
            auto status=dsp_kernels::prepare_conversion({std::get<NumericContract>(n.numeric_input),std::get<NumericContract>(n.numeric_outputs[0]),
                std::get<ConverterProcessingParameters>(cn.parameters).policy,profile.kernels,profile.admission.frame},profile.capabilities,*n.conversion);
            if(status!=dsp_kernels::NumericStatus::Ok)return numeric_kernel_status(status);
            state.numeric_scratch.resize(n.conversion->requirements.scratch_bytes);
            if(std::get<NumericContract>(n.numeric_input).shape.rank==Rank::Matrix){
                const NumericGatherRequest request{std::get<NumericContract>(n.numeric_input),{},
                    {profile.admission.frame,profile.admission.frame.max_payload_bytes,profile.admission.frame.max_metadata_bytes,profile.kernels.max_scratch_bytes},true};
                NumericGatherRequirements req;if(numeric_gather_requirements(request,req)!=Status::ok)return Status::invalid_argument;
                auto owner=std::make_shared<GatherOwner>(req.plan_metadata_bytes);
                if(prepare_numeric_gather(request,owner->pool,owner->gather)!=Status::ok)return Status::invalid_argument;
                n.input_gather=std::move(owner);state.input_gather_payload.resize(req.payload_bytes);state.input_gather_metadata.resize(req.output_metadata_bytes);state.input_gather_scratch.resize(req.scratch_bytes);
                state.row_planes.resize(std::get<NumericContract>(n.numeric_outputs[0]).channel_order.size());
                n.row_conversion=std::make_unique<dsp_kernels::PreparedConversion>();
                status=dsp_kernels::prepare_conversion({n.input_gather->gather.output_contract(),numeric_projection(std::get<NumericContract>(n.numeric_outputs[0]),{},true),
                    std::get<ConverterProcessingParameters>(cn.parameters).policy,profile.kernels,profile.admission.frame},profile.capabilities,*n.row_conversion);
                if(status!=dsp_kernels::NumericStatus::Ok)return numeric_kernel_status(status);
            }}
        if(n.operation==ProcessingOperation::WLEDEncoder){double gain=0;if(!numeric_parameter(resolved,"gain",gain))return Status::invalid_argument;
            n.gain=float(gain);state.values.resize(std::size_t(std::get<NumericContract>(n.numeric_input).shape.extents[0].maximum));}
        if(n.operation==ProcessingOperation::Transform){const auto& in=std::get<NumericContract>(n.numeric_input);const auto& out=std::get<NumericContract>(n.numeric_outputs[0]);
            const auto columns=std::size_t(in.shape.extents[0].maximum),count=std::size_t(out.shape.extents[0].maximum);
            switch(n.node.kind){
            case NodeKind::PcmResampler:
                state.left.resize(columns*in.channel_order.size());state.values.resize(count*in.channel_order.size());
                state.kernel.emplace<ResamplerKernel>();
                if(!std::get<ResamplerKernel>(state.kernel).operation().prepare(in.clock.sample_rate,out.clock.sample_rate,in.channel_order.size()))return Status::invalid_argument;
                break;
            case NodeKind::Window:{
                const auto numerator=std::uint64_t(original.execution_interval.numerator),denominator=original.execution_interval.denominator;
                std::uint64_t hop=0;
                if(!divide_products(numerator,in.clock.sample_rate,denominator,1,hop,&n.hop_fraction))return Status::overflow;
                if(!hop || hop>SIZE_MAX || (hop==SIZE_MAX && n.hop_fraction))return Status::invalid_argument;
                n.hop_base=std::size_t(hop);n.hop_denominator=denominator;
                const auto emissions=columns/n.hop_base+(columns%n.hop_base!=0);
                if(emissions>profile.max_windows_per_ingress || (emissions && g.nodes.size()>profile.max_callback_deliveries_per_ingress/emissions) ||
                   (emissions && g.nodes.size() && profile.admission.max_ports_per_node>profile.max_callback_deliveries_per_ingress/emissions/g.nodes.size()))return Status::exhausted;
                state.pcm.resize(count*in.channel_order.size());break;}
            case NodeKind::WindowFunction: {
                ProcessingParameterView value;resolve_processing_parameter(resolved,"function",value);const auto function=std::get<std::string_view>(value);
                if(function=="hamming")n.window_function=dsp_kernels::WindowFunction::Hamming;
                else if(function=="blackman")n.window_function=dsp_kernels::WindowFunction::Blackman;
                else if(function=="rectangular")n.window_function=dsp_kernels::WindowFunction::Rectangular;
                state.coefficients.resize(columns);dsp_kernels::window(state.coefficients,n.window_function);break;}
            case NodeKind::BarkWeighting:state.coefficients.resize(columns);dsp_kernels::bark_weights(state.coefficients,float(in.clock.sample_rate)/float(columns*2));break;
            case NodeKind::StereoFFT:state.kernel.emplace<dsp_kernels::FFT>();if(!std::get<dsp_kernels::FFT>(state.kernel).prepare(columns))return Status::unavailable;break;
            case NodeKind::Mel:state.left.resize(columns);state.values.resize(count);state.kernel.emplace<dsp_kernels::MelFilterBank>();if(!std::get<dsp_kernels::MelFilterBank>(state.kernel).prepare(columns,in.clock.sample_rate,count))return Status::unavailable;
                state.mel_boundaries.resize(count+2);
                for(std::size_t band=0;band<count+2;++band){const double mel_max=2595.*std::log10(1.+double(in.clock.sample_rate)*.5/700.);
                    state.mel_boundaries[band]=700.*std::expm1(mel_max*double(band)/double(count+1)/2595.*std::log(10.));}break;
            case NodeKind::SpectrumMerge: {double merge_value=0;if(!numeric_parameter(resolved,"merge",merge_value))return Status::invalid_argument;n.merge_mode=unsigned(merge_value);break;}
            case NodeKind::SpectrumGain: {double gain=0;if(!numeric_parameter(resolved,"gain",gain))return Status::invalid_argument;n.gain=float(gain);break;}
            default:return Status::invalid_argument;
            }
        }
        if(n.operation==ProcessingOperation::Source && std::holds_alternative<NumericContract>(n.numeric_outputs[0]) &&
           std::get<NumericContract>(n.numeric_outputs[0]).shape.rank==Rank::Matrix){
            state.route_masks.resize(cn.outputs.size());
            state.route_state.resize(state.route_masks.size());for(auto& mask:state.route_masks)mask.resize(std::size_t(std::get<NumericContract>(n.numeric_outputs[0]).shape.extents[0].maximum)*2);}
        if(n.operation==ProcessingOperation::History || n.operation==ProcessingOperation::Reduce ||
           ((n.operation==ProcessingOperation::Source || n.operation==ProcessingOperation::Converter) && std::holds_alternative<NumericContract>(n.numeric_outputs[0]) && std::get<NumericContract>(n.numeric_outputs[0]).shape.rank==Rank::Matrix) ||
           (n.operation==ProcessingOperation::Sink && std::holds_alternative<NumericContract>(n.numeric_input) && std::get<NumericContract>(n.numeric_input).shape.rank==Rank::Matrix)){
            const auto request=numeric_gather_request(resolved,profile);NumericGatherRequirements req;
            auto status=numeric_gather_requirements(request,req);if(status!=Status::ok)return status;
            auto owner=std::make_shared<GatherOwner>(req.plan_metadata_bytes);status=prepare_numeric_gather(request,owner->pool,owner->gather);if(status!=Status::ok)return status;
            n.gather=std::move(owner);state.gather_payload.resize(req.payload_bytes);state.gather_metadata.resize(req.output_metadata_bytes);state.gather_scratch.resize(req.scratch_bytes);
            if(n.operation==ProcessingOperation::Reduce){const auto& r=std::get<ReduceProcessingParameters>(cn.parameters);
                n.reduction=std::make_unique<dsp_kernels::PreparedReduction>();
                auto k=dsp_kernels::prepare_reduction({n.gather->gather.output_contract(),std::get<NumericContract>(n.numeric_outputs[0]),numeric_reduction(r.operation),r.precision_loss,r.rounding,profile.kernels,profile.admission.frame},profile.capabilities,*n.reduction);
                if(k!=dsp_kernels::NumericStatus::Ok)return numeric_kernel_status(k);
                state.numeric_scratch.resize(n.reduction->requirements.scratch_bytes);}}
        plan->impl_->nodes.push_back(std::move(n));
    }
    for(std::size_t i=0;i<plan->impl_->nodes.size();++i){auto& n=plan->impl_->nodes[i];
        if(n.scalar_view())for(auto& input:n.endpoint_plan->inputs)if(input.parent==absent){
            input.clock=absent;input.clock_known=false;const auto& declared=std::get<NumericContract>(input.contract).clock;
            for(std::size_t source=0;source<plan->impl_->nodes.size();++source){const auto& candidate=plan->impl_->nodes[source];if(candidate.operation!=ProcessingOperation::Source || candidate.numeric_outputs.empty())continue;
                const auto* contract=std::get_if<NumericContract>(&candidate.numeric_outputs[0]);
                if(contract && !declared.domain.empty() && declared.tick_seconds.numerator>0 && declared.domain==contract->clock.domain && declared.tick_seconds==contract->clock.tick_seconds && declared.sample_rate==contract->clock.sample_rate &&
                    (input.clock==absent || candidate.node.id<plan->impl_->nodes[input.clock].node.id)){input.clock=source;input.clock_known=true;}}
            if(input.clock==absent)for(std::size_t source=0;source<plan->impl_->nodes.size();++source)if(plan->impl_->nodes[source].operation==ProcessingOperation::Source &&
                (input.clock==absent || plan->impl_->nodes[source].node.id<plan->impl_->nodes[input.clock].node.id))input.clock=source;
        }
        if(n.operation==ProcessingOperation::InputSelector && n.parent==absent){
            const auto& declared=std::get<NumericContract>(n.numeric_input).clock;
            auto compatible_clock=[&](std::size_t candidate,std::uint16_t port){
                if(candidate==absent || candidate==i || port>=plan->impl_->nodes[candidate].numeric_outputs.size())return false;
                const auto* value=std::get_if<NumericContract>(&plan->impl_->nodes[candidate].numeric_outputs[port]);
                return value && !declared.domain.empty() && declared.sample_rate &&
                    value->clock.domain==declared.domain && value->clock.sample_rate==declared.sample_rate && value->clock.tick_seconds==declared.tick_seconds;
            };
            // Retain the selected producer's event clock when a wire is removed.
            // Otherwise pick one compatible source deterministically; unrelated
            // ingress and the second source must not create additional ticks.
            if(previous && !n.reset){const auto old=index(previous->impl_->nodes,n.node.id);
                if(old!=absent){const auto& prior=previous->impl_->nodes[old];
                    const auto old_anchor=prior.parent!=absent?prior.parent:prior.numeric_clock_parent;
                    if(prior.selected_input==n.selected_input && old_anchor!=absent){
                        const auto candidate=index(plan->impl_->nodes,previous->impl_->nodes[old_anchor].node.id);
                        if(compatible_clock(candidate,prior.parent_port)){n.numeric_clock_parent=candidate;n.parent_port=prior.parent_port;}
                    }
                }
            }
            if(n.numeric_clock_parent==absent)for(std::size_t candidate=0;candidate<plan->impl_->nodes.size();++candidate){
                if(plan->impl_->nodes[candidate].operation!=ProcessingOperation::Source || !compatible_clock(candidate,0))continue;
                if(n.numeric_clock_parent==absent || plan->impl_->nodes[candidate].node.id<plan->impl_->nodes[n.numeric_clock_parent].node.id){n.numeric_clock_parent=candidate;n.parent_port=0;}
            }
            if(n.numeric_clock_parent==absent)n.clock=absent;
        }
        if(n.parent!=absent && n.operation!=ProcessingOperation::InputSelector)n.clock=plan->impl_->nodes[n.parent].clock;
        if(n.clock==absent && n.operation!=ProcessingOperation::InputSelector && !n.reset && previous){const auto old=index(previous->impl_->nodes,n.node.id);const auto old_clock=previous->impl_->nodes[old].clock;
            if(old_clock!=absent)n.clock=index(plan->impl_->nodes,previous->impl_->nodes[old_clock].node.id);}
        if(n.parent==absent && n.operation!=ProcessingOperation::Source && n.operation!=ProcessingOperation::InputSelector && !n.reset && previous){
            const auto old=index(previous->impl_->nodes,n.node.id);const auto& old_node=previous->impl_->nodes[old];
            const auto old_anchor=old_node.parent!=absent?old_node.parent:old_node.numeric_clock_parent;
            if(old_anchor!=absent){const auto anchor=index(plan->impl_->nodes,previous->impl_->nodes[old_anchor].node.id);
                if(anchor!=absent && old_node.parent_port<plan->impl_->nodes[anchor].numeric_outputs.size() &&
                    same_physical_contract(n.numeric_input,plan->impl_->nodes[anchor].numeric_outputs[old_node.parent_port])){
                    n.numeric_clock_parent=anchor;n.parent_port=old_node.parent_port;
                }else n.clock=absent;
            }
        }
        // A new or reconfigured processor does not invalidate a descendant's
        // compatible storage. Reset that descendant only when its own contract
        // or its logical source clock changes, not because an ancestor rebuilt.
        if(previous){const auto old=index(previous->impl_->nodes,n.node.id);
            if(old!=absent){const auto old_clock=previous->impl_->nodes[old].clock;
            n.clock_reset=(n.clock==absent)!=(old_clock==absent);
            if(!n.clock_reset && n.clock!=absent){const auto& root=plan->impl_->nodes[n.clock];const auto& prior=previous->impl_->nodes[old_clock];
                const auto* current=std::get_if<NumericContract>(&root.numeric_outputs[0]);
                const auto* before=std::get_if<NumericContract>(&prior.numeric_outputs[0]);
                if(root.node.id!=prior.node.id || root.node.kind!=prior.node.kind || bool(current)!=bool(before) ||
                   (current && (current->clock.domain!=before->clock.domain || current->clock.tick_seconds!=before->clock.tick_seconds ||
                    current->clock.sample_rate!=before->clock.sample_rate)))n.clock_reset=true;
            }
            if(n.clock_reset)n.reset=true;
            }
        }
    }
    // Retained clock edges participate in the same event schedule and cannot form a cycle.
    for(std::size_t i=0;i<plan->impl_->nodes.size();++i){auto cursor=i;std::size_t count=0;
        while(cursor!=absent){if(++count>plan->impl_->nodes.size())return Status::invalid_argument;
            const auto& node=plan->impl_->nodes[cursor];cursor=node.parent!=absent?node.parent:node.numeric_clock_parent;}}
    for(std::size_t i=0;i<plan->impl_->nodes.size();++i){const auto& n=plan->impl_->nodes[i];const auto parent=n.parent!=absent?n.parent:n.numeric_clock_parent;
        if(parent!=absent)plan->impl_->nodes[parent].children.push_back(i);
        if(n.operation==ProcessingOperation::History)clear_node_state(n,c->impl_->states[i]);
        c->impl_->states[i].pending_discontinuity=n.reset;
    }
    c->impl_->plan=plan.get();if(previous && old_context)plan->refresh_state(*c,*previous,*old_context);
    result=std::move(plan);context=std::move(c);return Status::ok;
#if defined(__cpp_exceptions)
    }catch(const std::bad_alloc&){return Status::exhausted;}
    catch(const std::length_error&){return Status::exhausted;}
#endif
}
Status PreparedGraph::process_numeric_ports(NodeId ingress,const DataFrameView& input,RuntimeContext& c,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks) const noexcept {
    return process_numeric_impl(ingress,input,c,delivery,callbacks,nullptr,{},{},{});
}
Status PreparedGraph::process_numeric_impl(NodeId ingress,const DataFrameView& input,RuntimeContext& c,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks,
    const OwnedDataFrame* owned,std::optional<std::uint64_t> history_ordinal,std::shared_ptr<const void> retention_owner,std::optional<std::uint64_t> history_filled_count) const noexcept {
    if(!impl_->numeric_graph || c.impl_->plan!=this)return Status::invalid_argument;
    const auto source_index=index(impl_->nodes,ingress);
    if(source_index==absent || impl_->nodes[source_index].operation!=ProcessingOperation::Source)return Status::invalid_argument;
    const auto& source_node=impl_->nodes[source_index];auto& source_state=c.impl_->states[source_index];
    const auto& limits=impl_->numeric_profile.admission.frame;
    const auto actual=std::visit([](const auto& value)->std::optional<DataContract>{return value.contract?std::optional<DataContract>{*value.contract}:std::nullopt;},input.payload);
    if(!actual || !same_physical_contract(*actual,source_node.numeric_outputs[0]))return Status::incompatible_contract;
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
    if(source_state.numeric_marker){const auto& old=source_state.numeric_stamp;
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
            (input.stamp.generation==old.generation && (input.stamp.available || input.stamp.sequence) && input.stamp.sequence<=source_state.last_sequence)))return Status::would_block;
        for(std::size_t i=0;i<source_state.retired_count;++i)if(source_state.retired[i]==input.stamp.producer_epoch)return Status::would_block;
        if(input.stamp.producer_epoch!=old.producer_epoch || input.stamp.generation!=old.generation || input.stamp.discontinuity){
            const auto old_epoch=old.producer_epoch;
            for(std::size_t i=0;i<impl_->nodes.size();++i)if(impl_->nodes[i].clock==source_index)clear_node_state(impl_->nodes[i],c.impl_->states[i]);
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
    auto mutable_output=[](const PlanNode& n,State& state,PortId p){auto& b=state.numeric_outputs[p];
        MutableNumericView out{&std::get<NumericContract>(n.numeric_outputs[p]),b.storage(),b.planes,b.actual,{},{}};
        for(std::size_t axis=0;axis<2;++axis)if(!b.coordinates[axis].empty())out.coordinates.axes[axis]={b.coordinates[axis].data(),b.actual[axis]*8};
        return out;};
    auto zero_value=[](const DataContract& contract,const NumericBuffer& zero)->std::variant<NumericView,StructuredView>{
        if(const auto* numeric=std::get_if<NumericContract>(&contract)){
            auto value=NumericView{numeric,zero.storage(),zero.planes,zero.actual,{},{},true};
            for(std::size_t axis=0;axis<2;++axis)if(!zero.coordinates[axis].empty())value.coordinates.axes[axis]={zero.coordinates[axis].data(),zero.actual[axis]*8};
            return value;
        }
        return StructuredView{&std::get<StructuredContract>(contract),zero.storage()};
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
    auto run=[&](auto&& self,std::size_t i,const DataFrameView& incoming,Status incoming_status=Status::ok)->Status {
        const auto& n=impl_->nodes[i];auto& state=c.impl_->states[i];
        const bool first=!state.numeric_marker;
        auto current=incoming;
        if(auto* numeric=std::get_if<NumericView>(&current.payload)){
            const auto& declaration=n.operation==ProcessingOperation::Source?n.numeric_outputs[0]:n.numeric_input;
            if(const auto* physical=std::get_if<NumericContract>(&declaration))numeric->contract=physical;
            if(n.operation==ProcessingOperation::Source && external_history && !current.stamp.available){
                numeric->actual[0]=history_input->shape.extents[0].maximum;
                numeric->ring={numeric->actual[0],state.oldest,{}};
            }
        }
        auto downstream_status=incoming_status;
        if(n.operation==ProcessingOperation::InputSelector){
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
        if(state.pending_discontinuity && n.node.kind!=NodeKind::Window && n.node.kind!=NodeKind::PcmResampler){current.stamp.discontinuity=true;state.pending_discontinuity=false;}
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
            if(callbacks.sink_input)callbacks.sink_input(callbacks.user,ep);
            if(n.gather && callbacks.history && std::get<NumericView>(current.payload).ring.capacity){const auto& source=std::get<NumericView>(current.payload);const auto producer=history_producer(n.parent!=absent?n.parent:n.numeric_clock_parent);
                const auto ordinal=producer==absent?std::uint64_t{0}:c.impl_->states[producer].row_ordinal;
                const auto filled=producer==absent?std::uint64_t{0}:std::uint64_t(c.impl_->states[producer].progress);
                const auto capacity=std::uint64_t(source.ring.capacity?source.ring.capacity:source.contract->shape.extents[0].maximum);
                auto event=!current.stamp.available?NumericHistoryEventKind::Unavailable:filled?NumericHistoryEventKind::Row:NumericHistoryEventKind::Empty;
                NumericGatherOutput compact;
                if(event==NumericHistoryEventKind::Row){const auto gathered=gather(n,state,current,compact);if(gathered!=Status::ok)return gathered;}
                callbacks.history(callbacks.user,{event,ep,*source.contract,compact,filled,capacity,ordinal});
            }
            return Status::ok;
        }
        auto emit=[&](PortId p){auto& frame=state.numeric_frames[p];
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
            if(callbacks.output && !n.internal_conversion)callbacks.output(callbacks.user,endpoint(n,PortDirection::Output,p,frame,frame.stamp.available?Status::ok:downstream_status==Status::ok?Status::unavailable:downstream_status));
            for(const auto& sink:impl_->nodes)if(sink.scalar_view())for(const auto& input:sink.endpoint_plan->inputs)
                if((input.parent==i && input.parent_port==p) || (input.parent==absent && input.clock==i && p==0)){
                    auto delivered=frame;const auto& contract=std::get<NumericContract>(input.contract);
                    const bool silence=!sink.node.enabled || input.parent==absent;
                    alignas(std::max_align_t) std::array<std::byte,8> bytes{};const std::array<PlaneLayout,1> planes{{{0,{0,0}}}};
                    if(silence){MutableNumericView zero{&contract,bytes,planes,{1,1},{},{}};
                        if(input.parent==absent && !input.clock_known)delivered.stamp.available=false;
                        const auto filled=fill_silence(zero,limits);if(filled!=Status::ok){result=filled;continue;}auto value=zero.view();if(!delivered.stamp.available)value.storage={};delivered.payload=value;}
                    else if(auto* value=std::get_if<NumericView>(&delivered.payload))value->contract=&contract;
                    if(callbacks.sink_input)callbacks.sink_input(callbacks.user,{impl_->numeric_graph->graph_id,{sink.node.id,input.port},PortDirection::Input,
                        impl_->numeric_graph->nodes[sink.configured].kind,input.contract,delivered,delivered.stamp.available?Status::ok:downstream_status==Status::ok?Status::unavailable:downstream_status,delivery});
                }
            for(auto child:n.children)if(!impl_->nodes[child].scalar_view() && impl_->nodes[child].parent_port==p){const auto sent=self(self,child,frame,downstream_status);if(sent!=Status::ok)result=sent;}
        };
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
        auto reject=[&](Status cause){result=cause;downstream_status=cause;
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
                callbacks.history(callbacks.user,{NumericHistoryEventKind::Unavailable,endpoint(n,PortDirection::Output,0,state.numeric_frames[0],downstream_status==Status::ok?Status::unavailable:downstream_status),std::get<NumericContract>(n.numeric_outputs[0]),{},std::uint64_t(state.progress),std::uint64_t(dst.ring.capacity),state.row_ordinal});}
        }else if(n.identity_bypass){
            state.numeric_frames[0]=current;
            if(n.operation==ProcessingOperation::Converter && n.gather && std::get<NumericView>(current.payload).ring.capacity){const auto producer=history_producer(n.parent!=absent?n.parent:n.numeric_clock_parent);
                if(producer!=absent){state.row_ordinal=c.impl_->states[producer].row_ordinal;state.progress=c.impl_->states[producer].progress;}
                const auto& view=std::get<NumericView>(current.payload);state.frames=view.actual[0];state.oldest=view.ring.oldest;}
        }else if(n.operation==ProcessingOperation::Extract){
            const auto& source=std::get<NumericView>(current.payload);auto output=mutable_output(n,state,0);std::uint64_t bits=0;
            if(n.node.enabled){const auto& index=n.endpoint_plan->indices;status=load_bits(source,{index[0],index[1],index[2]},source.contract->dtype,bits);if(status!=Status::ok)return reject(status);
                status=store_bits(output,{0,0,0},output.contract->dtype,bits);
            }else status=fill_silence(output,limits);
            if(status!=Status::ok)return reject(status);state.numeric_frames[0]={current.stamp,output.view()};
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
        }else if(n.operation==ProcessingOperation::Transform){const auto& src=std::get<NumericView>(current.payload);auto dst=mutable_output(n,state,0);
            const auto columns=src.actual[0],count=dst.actual[0];
            if(n.node.kind==NodeKind::PcmResampler){
                auto& resampler=std::get<ResamplerKernel>(state.kernel).operation();
                const auto input_stride=std::size_t(src.contract->shape.extents[0].maximum);
                for(std::size_t p=0;p<src.planes.size();++p)for(std::size_t j=0;j<columns;++j){
                    float value=0;
                    if(n.node.enabled){status=load_element(src,{p,j,0},value);if(status!=Status::ok)return reject(status);}
                    state.left[p*input_stride+j]=value;
                }
                if(!resampler.anchored()){
                    std::int64_t converted=0;
                    if(!convert_rate_ticks(current.stamp.timestamp_ticks,src.contract->clock.sample_rate,dst.contract->clock.sample_rate,converted))return reject(Status::overflow);
                    resampler.anchor(converted);
                }
                const auto ordinal=resampler.produced();std::size_t written=0;
                if(!resampler.process(state.left,columns,input_stride,state.values,count,written)){state.pending_discontinuity=true;return reject(Status::overflow);}
                if(!resampler.advance_input(current.stamp.timestamp_ticks,columns)){resampler.reset();state.pending_discontinuity=true;return reject(Status::overflow);}
                if(!written)return Status::ok;
                if(ordinal>std::uint64_t(INT64_MAX) || resampler.anchor_ticks()>INT64_MAX-std::int64_t(ordinal) || state.output_sequence==UINT64_MAX)return reject(Status::overflow);
                dst.actual[0]=written;
                for(std::size_t p=0;p<dst.planes.size();++p)for(std::size_t j=0;j<written;++j){
                    status=store_element(dst,{p,j,0},n.node.enabled?state.values[p*count+j]:0.f);if(status!=Status::ok)return reject(status);
                }
                const auto first_tick=resampler.anchor_ticks()+std::int64_t(ordinal);
                if(first_tick>INT64_MAX-std::int64_t(written-1))return reject(Status::overflow);
                auto stamp=current.stamp;stamp.timestamp_ticks=first_tick;stamp.sequence=++state.output_sequence;
                stamp.discontinuity=stamp.discontinuity || state.pending_discontinuity;state.pending_discontinuity=false;
                if(!stamp.source_sequence)stamp.source_sequence=current.stamp.sequence;
                state.numeric_frames[0]={stamp,dst.view()};emit(0);return Status::ok;
            }
            auto explicit_coordinate=[](std::span<const std::byte> bytes,std::size_t at){std::uint64_t bits=0;
                for(std::size_t byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[at*8+byte]))<<(byte*8);
                double value=0;std::memcpy(&value,&bits,8);return value;};
            auto write_coordinate=[](std::span<std::byte> bytes,std::size_t at,double value){std::uint64_t bits=0;std::memcpy(&bits,&value,8);
                for(std::size_t byte=0;byte<8;++byte)bytes[at*8+byte]=std::byte((bits>>(byte*8))&255);};
            if(src.contract->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE &&
               (n.node.kind==NodeKind::BarkWeighting || n.node.kind==NodeKind::Mel)){
                double previous_hz=-1;
                for(std::size_t j=0;j<columns;++j){const auto hz=explicit_coordinate(src.coordinates.axes[0],j);
                    if(hz<0 || hz<=previous_hz || hz>double(src.contract->clock.sample_rate)*.5)return reject(Status::incompatible_contract);
                    previous_hz=hz;}}
            if(dst.contract->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE && n.node.kind!=NodeKind::Window){
                auto& coords=state.numeric_outputs[0].coordinates[0];
                if(n.node.kind==NodeKind::StereoFFT){for(std::size_t j=0;j<count;++j)write_coordinate(coords,j,double(j)*src.contract->clock.sample_rate/double(columns));}
                else if(n.node.kind==NodeKind::Mel){for(std::size_t j=0;j<count;++j)write_coordinate(coords,j,dst.contract->coordinates[0].unit==NumericUnit::Hertz?state.mel_boundaries[j+1]:double(j));}
                else if(src.coordinates.axes[0].size()==count*8)std::copy(src.coordinates.axes[0].begin(),src.coordinates.axes[0].end(),coords.begin());
                else return reject(Status::incompatible_contract);
                dst.coordinates.axes[0]={coords.data(),count*8};}
            if(n.node.kind==NodeKind::Window){std::size_t offset=0;
                const bool direct=direct_window(*dst.contract);
                if(direct)dst.storage=std::as_writable_bytes(std::span(state.pcm));
                while(offset<columns){if(!state.hop){state.hop=n.hop_base;
                        if(state.remainder>=n.hop_denominator-n.hop_fraction){++state.hop;state.remainder-=n.hop_denominator-n.hop_fraction;}
                        else state.remainder+=n.hop_fraction;}
                    const auto taken=std::min(columns-offset,state.hop-state.progress),kept=std::min(count,taken);
                    if(dst.contract->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE){auto& coords=state.numeric_outputs[0].coordinates[0];
                        std::move(coords.begin()+kept*8,coords.end(),coords.begin());
                        const auto source=src.coordinates.axes[0].subspan((offset+taken-kept)*8,kept*8);
                        std::copy(source.begin(),source.end(),coords.begin()+(count-kept)*8);}
                    if(n.node.enabled)for(std::size_t plane=0;plane<src.planes.size();++plane){auto window=std::span(state.pcm).subspan(plane*count,count);
                        std::move(window.begin()+kept,window.end(),window.begin());
                        for(std::size_t j=0;j<kept;++j){status=load_element(src,{plane,offset+taken-kept+j,0},window[count-kept+j]);if(status!=Status::ok)return reject(status);}
                    }
                    offset+=taken;state.progress+=taken;
                    if(state.progress==state.hop){
                        if(state.output_sequence==UINT64_MAX)return reject(Status::overflow);
                        const auto& clock=src.contract->clock;
                        std::uint64_t ticks=0;
                        if(clock.tick_seconds.numerator<=0 || !clock.sample_rate || !divide_products(std::uint64_t(offset),clock.tick_seconds.denominator,
                            clock.sample_rate,std::uint64_t(clock.tick_seconds.numerator),ticks))return reject(Status::overflow);
                        if(ticks>std::uint64_t(INT64_MAX) || current.stamp.timestamp_ticks>INT64_MAX-std::int64_t(ticks))return reject(Status::overflow);
                        if(!direct)for(std::size_t plane=0;plane<dst.planes.size();++plane)for(std::size_t j=0;j<count;++j){
                            status=store_element(dst,{plane,j,0},n.node.enabled?state.pcm[plane*count+j]:0.f);if(status!=Status::ok)return reject(status);}
                        auto stamp=current.stamp;stamp.sequence=++state.output_sequence;
                        if(!stamp.source_sequence)stamp.source_sequence=current.stamp.sequence;
                        stamp.timestamp_ticks+=std::int64_t(ticks);stamp.discontinuity=current.stamp.discontinuity || state.pending_discontinuity;
                        auto value=dst.view();
                        // The rolling payload stays stable until synchronous descendants
                        // and observers finish this emission. Disabled windows borrow the
                        // immutable silence pool without modifying the rolling samples.
                        if(direct && !n.node.enabled){value.storage=std::as_const(state.numeric_outputs[0]).storage();value.semantic_silence=true;}
                        state.pending_discontinuity=false;state.numeric_frames[0]={stamp,value};emit(0);state.progress=state.hop=0;
                    }
                }
                return Status::ok;
            }
            status=Status::ok;
            if(!n.node.enabled)status=fill_silence(dst,limits);
            else status=validate_values(src,ValueDomain::FiniteOnly,limits);
            if(status==Status::ok && n.node.enabled){
                if(n.node.kind==NodeKind::StereoFFT){
                    for(std::size_t p=0;p<dst.planes.size() && status==Status::ok;++p)status=std::get<dsp_kernels::FFT>(state.kernel).process_plane(src,dst,p);
                }else if(n.node.kind==NodeKind::Mel){
                    for(std::size_t p=0;p<dst.planes.size() && status==Status::ok;++p){
                        for(std::size_t j=0;j<columns;++j){status=load_element(src,{p,j,0},state.left[j]);if(status!=Status::ok)break;}
                        if(status!=Status::ok)break;
                        bool processed=true;
                        if(src.contract->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE)processed=std::get<dsp_kernels::MelFilterBank>(state.kernel).process(state.left,state.values);
                        else for(std::size_t band=0;band<count;++band){double energy=0;
                            const auto left=state.mel_boundaries[band],center=state.mel_boundaries[band+1],right=state.mel_boundaries[band+2];
                            for(std::size_t j=0;j<columns;++j){const auto hz=explicit_coordinate(src.coordinates.axes[0],j);
                                const auto weight=hz<left || hz>right?0.:hz<center?(hz-left)/(center-left):(right-hz)/(right-center);
                                energy+=double(state.left[j])*weight;}
                            state.values[band]=float(energy);}
                        if(!processed){status=Status::unavailable;break;}
                        for(std::size_t j=0;j<count;++j){if(!std::isfinite(state.values[j])){status=Status::overflow;break;}
                            status=store_element(dst,{p,j,0},state.values[j]);if(status!=Status::ok)break;}
                    }
                }else for(std::size_t p=0;p<dst.planes.size() && status==Status::ok;++p)for(std::size_t j=0;j<count;++j){float value=0;
                    status=load_element(src,{p,j,0},value);if(status!=Status::ok)break;
                    if(n.node.kind==NodeKind::WindowFunction)value*=state.coefficients[j];
                    else if(n.node.kind==NodeKind::BarkWeighting){const auto hz=src.contract->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE?explicit_coordinate(src.coordinates.axes[0],j):double(j)*src.contract->clock.sample_rate/double(columns*2);
                        value*=float(7.*std::asinh(hz/650.));}
                    else if(n.node.kind==NodeKind::SpectrumGain)value*=n.gain;
                    else if(n.node.kind==NodeKind::SpectrumMerge){float right=0;status=load_element(src,{1,j,0},right);if(status!=Status::ok)break;
                        if(n.merge_mode==0)value=std::midpoint(value,right);
                        else if(n.merge_mode==1)value=std::max(value,right);
                        else {const float scale=std::max(std::abs(value),std::abs(right));value=scale?float(double(scale)*std::hypot(double(value)/scale,double(right)/scale)/std::sqrt(2.)):0.f;};}
                    if(!std::isfinite(value)){status=Status::overflow;break;}
                    status=store_element(dst,{p,j,0},n.node.enabled?value:0.f);if(status!=Status::ok)break;
                }
            }
            if(status==Status::ok)state.numeric_frames[0]={current.stamp,dst.view()};else {result=status;downstream_status=status;}
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
                    callbacks.history(callbacks.user,{NumericHistoryEventKind::Row,endpoint(n,PortDirection::Output,0,state.numeric_frames[0],Status::ok),std::get<NumericContract>(n.numeric_outputs[0]),compact,std::uint64_t(state.progress),std::uint64_t(capacity),state.row_ordinal});}
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
            callbacks.history(callbacks.user,{event,endpoint(n,PortDirection::Output,0,frame,frame.stamp.available?Status::ok:Status::unavailable),*value.contract,compact,std::uint64_t(state.progress),std::uint64_t(value.ring.capacity),state.row_ordinal});
        }
        for(std::size_t p=0;p<state.numeric_frames.size();++p)emit(PortId(p));
        return Status::ok;
    };
    status=run(run,source_index,input);if(status!=Status::ok)return status;
    for(std::size_t i=0;i<impl_->nodes.size();++i){const auto& n=impl_->nodes[i];if(n.scalar_view() || n.operation==ProcessingOperation::Source || n.parent!=absent || n.numeric_clock_parent!=absent ||
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
GraphExecutor::GraphExecutor(NumericRuntimeProfile profile):numeric_profile_(std::move(profile)) {}
Status GraphExecutor::prepare_numeric(const NumericProcessingGraph& graph,const application::NumericRuntimeLimits& limits,application::PreparedExecution& result) {
    PreparationBase base;base.owner=this;base.plan=plan_;base.borrowed_context=context_.get();
    const auto status=prepare_numeric_detached(graph,limits,base,result);
    return status==Status::ok?register_prepared(result,base):status;
}
GraphExecutor::PreparationBase GraphExecutor::snapshot_preparation_base() const {
    PreparationBase base;base.owner=this;base.plan=plan_;if(context_)base.context=std::make_unique<RuntimeContext>(*context_);return base;
}
Status GraphExecutor::register_prepared(const application::PreparedExecution& result,const PreparationBase& base) noexcept {
    if(base.owner!=this)return Status::invalid_argument;
    const auto candidate=base.prepared.lock();
    if(!result || !candidate || candidate.get()!=result.plan.get() || result.context.get()!=base.prepared_context)return Status::invalid_argument;
    if(plan_.get()!=base.plan.get())return Status::would_block;
    prepared_=candidate;
    prepared_context_=result.context.get();prepared_base_plan_=base.plan.get();return Status::ok;
}
Status GraphExecutor::prepare_numeric_detached(const NumericProcessingGraph& graph,const application::NumericRuntimeLimits& limits,
    const PreparationBase& base,application::PreparedExecution& result) const {
    if(base.owner!=this){result={};return Status::invalid_argument;}
    const auto& maximum=numeric_profile_.admission;const auto& admitted=limits.admission;
    const auto& reserved=numeric_profile_.resources;const auto& requested=limits.resources;
    for(const auto& bound:{std::pair{admitted.max_nodes,maximum.max_nodes},std::pair{admitted.max_edges,maximum.max_edges},
        std::pair{admitted.max_ports_per_node,maximum.max_ports_per_node},std::pair{admitted.max_parameters_per_node,maximum.max_parameters_per_node},
        std::pair{admitted.max_text_bytes,maximum.max_text_bytes},std::pair{admitted.max_owned_metadata_bytes,maximum.max_owned_metadata_bytes},
        std::pair{admitted.frame.max_planes,maximum.frame.max_planes},std::pair{admitted.frame.max_elements,maximum.frame.max_elements},
        std::pair{admitted.frame.max_payload_bytes,maximum.frame.max_payload_bytes},std::pair{admitted.frame.max_metadata_bytes,maximum.frame.max_metadata_bytes},
        std::pair{admitted.frame.max_string_bytes,maximum.frame.max_string_bytes},std::pair{admitted.frame.max_coordinate_bytes,maximum.frame.max_coordinate_bytes},
        std::pair{requested.maximum.plan_bytes,reserved.maximum.plan_bytes},std::pair{requested.maximum.payload_bytes,reserved.maximum.payload_bytes},
        std::pair{requested.maximum.scratch_bytes,reserved.maximum.scratch_bytes},std::pair{requested.maximum.stack_bytes,reserved.maximum.stack_bytes},
        std::pair{requested.max_peak_bytes,reserved.max_peak_bytes}})
        if(bound.first>bound.second){result={};return Status::exhausted;}
    auto profile=numeric_profile_;profile.admission=limits.admission;profile.resources=limits.resources;
    // Detached preparation temporarily retains both the live context and its
    // owning snapshot. Charge a conservative full working allocation for the
    // snapshot in addition to prepare_numeric's existing old-plan reservation.
    if(base.plan && base.context){const auto memory=base.plan->memory_requirements();
        if(!numeric_add(profile.resources.retained_working_bytes,memory.state_bytes) ||
           !numeric_add(profile.resources.retained_working_bytes,memory.buffer_bytes) ||
           !numeric_add(profile.resources.retained_working_bytes,memory.scratch_bytes)){result={};return Status::overflow;}}
    result={};std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    const auto status=PreparedGraph::prepare_numeric(graph,profile,plan,context,base.plan.get(),base.context?base.context.get():base.borrowed_context);
    if(status!=Status::ok)return status;
    base.prepared=plan;base.prepared_context=context.get();
    result.plan=std::move(plan);result.context=std::move(context);return Status::ok;
}
Status GraphExecutor::process_numeric_ports(NodeId ingress,const DataFrameView& frame,DeliveryStamp stamp,NumericRuntimeCallbacks cb) noexcept {
    if(!running_ || !plan_)return Status::would_block;
    return plan_->process_numeric_ports(ingress,frame,*context_,stamp,cb);
}
Status GraphExecutor::process_numeric_owned_ports(NodeId ingress,const OwnedDataFrame& frame,DeliveryStamp stamp,NumericRuntimeCallbacks cb,
    std::optional<std::uint64_t> history_ordinal,std::shared_ptr<const void> retention_owner,std::optional<std::uint64_t> history_filled_count) noexcept {
    if(!running_ || !plan_)return Status::would_block;
    return plan_->process_numeric_owned_ports(ingress,frame,*context_,stamp,cb,history_ordinal,std::move(retention_owner),history_filled_count);
}
Status GraphExecutor::apply(application::PreparedExecution &&execution) {
    const auto expected = prepared_.lock();
    if (!execution || !expected || execution.plan.get() != expected.get() ||
        execution.context.get() != prepared_context_ || plan_.get() != prepared_base_plan_)
        return Status::invalid_argument;
    if(plan_){const auto& destination=*static_cast<const RuntimeContext*>(execution.context.get());
        for(std::size_t i=0;i<expected->impl_->nodes.size();++i)if(!expected->impl_->nodes[i].reset){
            const auto old=index(plan_->impl_->nodes,expected->impl_->nodes[i].node.id);
            if(old!=absent && context_->impl_->states[old].numeric_stamp.stream_id.size()>destination.impl_->states[i].stream_storage.size())return Status::exhausted;
        }
    }
    // Identity is checked before casts, so no RTTI or foreign-plan downcast is
    // needed. The bundle came from this preparation port and remains untouched
    // on failure. Successful application consumes it at the owner's boundary.
    if (plan_)
        expected->refresh_state(*static_cast<RuntimeContext *>(execution.context.get()), *plan_, *context_);
    plan_ = std::static_pointer_cast<const PreparedGraph>(std::move(execution.plan));
    context_.reset(static_cast<RuntimeContext *>(execution.context.release()));
    prepared_.reset();
    prepared_context_ = nullptr;
    running_ = true;
    return Status::ok;
}
Status GraphExecutor::shutdown() {
    running_=false;prepared_.reset();prepared_context_=nullptr;prepared_base_plan_=nullptr;
    context_.reset();plan_.reset();return Status::ok;
}
} // namespace dataflow_runtime
