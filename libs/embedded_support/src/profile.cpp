#include <embedded_support/profile.h>
#include <bdsp_protocol/numeric_data_codec.h>
#include <bdsp_protocol/stream_codec.h>
#include <dsp_kernels/dsp.h>
#include <limits>
#include <algorithm>
#include <array>
namespace embedded_support {
using signal_contracts::Status;
Admission admit(const Profile &p, std::size_t id, application::MemoryRequirements runtime) noexcept {
    Admission a;
    if(!id || id>64 || p.sample_rate<1000 || p.sample_rate>192000 || !p.block_samples ||
       p.block_samples>4096 || p.fft_size<512 || p.fft_size>4096 || (p.fft_size&(p.fft_size-1)) ||
       !p.mel_bands || p.mel_bands>p.fft_size/2 || !p.queue_frames || p.queue_frames>8 || !p.channels || p.channels>2 ||
       p.packet_payload_capacity<=8 || p.packet_payload_capacity>65535 ||
       (p.spectral && p.channels!=2) || !domain::valid_execution_interval(p.execution_interval) ||
       !p.utilization_percent || p.utilization_percent>100 ||
       (p.spectral && p.fft_size>dsp_kernels::max_fft_samples)) return a;
    // Measure the canonical reference DATA4 frame through the existing
    // codec. Immutable zero samples avoid heap and large control-task stacks.
    static constexpr std::array<std::byte,32768> zeros{};
    static constexpr std::array<char,256> stream=[] {std::array<char,256> s{};s.fill('s');return s;}();
    static constexpr std::array<std::uint32_t,2> channels{0,1};
    signal_contracts::NumericContract c;c.dtype=signal_contracts::DType::Float32;
    c.channel_order=std::span(channels).first(p.spectral?1:p.channels);
    c.semantic=p.spectral?signal_contracts::Semantic::MelEnergy:signal_contracts::Semantic::PCM;
    c.quantity=p.spectral?signal_contracts::Quantity::MelEnergy:signal_contracts::Quantity::Amplitude;
    c.unit=p.spectral?signal_contracts::NumericUnit::Values:signal_contracts::NumericUnit::NormalizedAmplitude;
    c.shape.rank=signal_contracts::Rank::Vector;c.shape.axes={p.spectral?signal_contracts::Axis::Band:signal_contracts::Axis::Sample,signal_contracts::Axis::None};
    const std::size_t elements=p.spectral?p.mel_bands:p.block_samples;
    c.shape.extents={signal_contracts::ExtentBounds{p.spectral?elements:1,elements},signal_contracts::ExtentBounds{1,1}};
    c.clock={"embedded-source",{1,p.sample_rate},p.sample_rate};c.representation.encoding=signal_contracts::StorageEncoding::FixedLittleEndian;
    const std::array<signal_contracts::PlaneLayout,2> planes{{{0,{4,0}},{elements*4,{4,0}}}};
    bdsp_protocol::numeric_data::Message message;message.endpoint={"embedded-synthetic",1,bdsp_protocol::numeric_data::Direction::Input,0};
    message.envelope={1,1,1,1,1,1,0};message.contract=c;message.stamp.sequence=1;message.stamp.stream_id={stream.data(),id};
    message.numeric={std::span(zeros).first(elements*c.channel_order.size()*4),std::span(planes).first(c.channel_order.size()),{elements,1},{},{},false};
    // DATA4 has its own complete-message ceiling, below the generic framing cap.
    const auto data_ceiling=bdsp_protocol::numeric_data::desktop_limits().message_bytes;
    bdsp_protocol::numeric_data::Limits wire_limits{{2,8192,32768,4096,256,256},data_ceiling,4096,0,0,256};
    message.reference={1,1};const bdsp_protocol::numeric_data::Binding binding{message.reference,message.endpoint,message.stamp.stream_id,message.contract,{},message.values};
    const auto measured=bdsp_protocol::numeric_data::measure(message,wire_limits,{1,1,1,std::span(&binding,1)});
    if(!measured){a.status=measured.status;return a;}a.frame_bytes=measured.required.wire;
    if(a.frame_bytes>p.max_frame_bytes || p.max_frame_bytes>data_ceiling) {
        a.status=Status::buffer_too_small;return a;
    }
    // Conservative bound for upstream COBS-ZPE: at most two encoded bytes per
    // source byte plus header/checksum/delimiters, for every existing fragment.
    const auto chunk=std::min<std::size_t>(60000,p.packet_payload_capacity-8);
    const auto fragments=(a.frame_bytes+chunk-1)/chunk;
    a.wire_bytes=2*(a.frame_bytes+8*fragments)+16*fragments;
    const std::uint64_t denominator=p.spectral?std::uint64_t(p.execution_interval.numerator):p.block_samples;
    const std::uint64_t frequency=p.spectral?p.execution_interval.denominator:p.sample_rate;
    if(a.wire_bytes>UINT64_MAX/frequency || a.wire_bytes*frequency>UINT64_MAX-(denominator-1)){a.status=Status::exhausted;return a;}
    a.required_bytes_per_second=(a.wire_bytes*frequency+denominator-1)/denominator;
    const std::uint64_t capacity=p.link==Link::uart_8n1?p.uart_baud/10:p.tcp_bytes_per_second;
    a.allowed_bytes_per_second=capacity*p.utilization_percent/100;
    if(a.required_bytes_per_second>a.allowed_bytes_per_second){a.status=Status::exhausted;return a;}
    const auto limit=std::numeric_limits<std::size_t>::max();
    a.reserved_bytes=p.adapter_bytes;
    // Include fixed pool capacity even when frames are smaller than their slots.
    for(auto bytes : {p.max_frame_bytes*p.queue_frames,p.control_bytes,runtime.state_bytes,runtime.buffer_bytes,runtime.scratch_bytes}) {
        if(bytes>limit-a.reserved_bytes){a.status=Status::exhausted;return a;}
        a.reserved_bytes+=bytes;
    }
    a.status=a.reserved_bytes<=p.memory_bytes?Status::ok:Status::exhausted;
    return a;
}
namespace {
std::size_t processing_budget(const Profile& p) noexcept {
    const auto limit=std::numeric_limits<std::size_t>::max();
    const auto reserve=(p.max_frame_bytes>limit/(p.queue_frames?p.queue_frames:1) ||
                        p.max_frame_bytes*p.queue_frames>limit-p.adapter_bytes ||
                        p.control_bytes>limit-p.adapter_bytes-p.max_frame_bytes*p.queue_frames)
        ?limit:p.adapter_bytes+p.max_frame_bytes*p.queue_frames+p.control_bytes;
    return reserve<p.memory_bytes?p.memory_bytes-reserve:0;
}
}
dataflow_runtime::NumericRuntimeProfile numeric_runtime_profile(const Profile& p) noexcept {
    if(admit(p,1).status!=Status::ok)return {};
    const auto budget=processing_budget(p);
    signal_contracts::FrameLimits frame{p.channels,4096*std::min<std::uint32_t>(p.channels,2),32768,2048,64,256};
    return {{8,16,4,16,64,16384,frame},{{budget,budget,budget,p.task_stack_bytes},0,budget},
        {frame.max_elements,p.channels,32768,32768,budget},{true,true,false},false};
}
bdsp_protocol::numeric_control::Limits numeric_control_limits(const Profile& p) noexcept {
    auto limits=bdsp_protocol::numeric_control::device_limits();limits.frame=numeric_runtime_profile(p).admission.frame;return limits;
}
bdsp_protocol::numeric_data::Limits numeric_data_limits(const Profile& p) noexcept {
    return {numeric_runtime_profile(p).admission.frame,p.max_frame_bytes,2048,0,0,64};
}
DeviceProfile device_profile(const Profile& p) noexcept {return {p,numeric_runtime_profile(p),numeric_data_limits(p)};}
namespace {
bdsp_protocol::numeric_data::Result measure_port(const signal_contracts::DataContract& contract,
    const bdsp_protocol::numeric_data::Limits& limits) noexcept {
    namespace nd=bdsp_protocol::numeric_data;
    const auto* c=std::get_if<signal_contracts::NumericContract>(&contract);
    std::uint8_t symbol=0;
    if(!c || !nd::symbol_id(c->representation.encoding,symbol))return {Status::incompatible_contract,{}};
    const auto valid=signal_contracts::validate_contract(contract,limits.frame);
    if(valid!=Status::ok)return {valid,{}};
    static constexpr std::array<std::byte,32768> zeros{};
    std::array<signal_contracts::PlaneLayout,2> planes{};
    if(c->channel_order.size()>planes.size())return {Status::exhausted,{}};
    std::size_t width=0;const auto sized=signal_contracts::storage_width(*c,width);
    if(sized!=Status::ok)return {sized,{}};
    const auto rank=c->shape.rank;
    const auto rows=rank==signal_contracts::Rank::Scalar?1:std::size_t(c->shape.extents[0].maximum);
    const auto columns=rank==signal_contracts::Rank::Matrix?std::size_t(c->shape.extents[1].maximum):1;
    const auto channels=c->channel_order.size(),lane=c->representation.layout==signal_contracts::ChannelLayout::Interleaved?channels*width:width;
    for(std::size_t i=0;i<channels;++i)planes[i]={c->representation.layout==signal_contracts::ChannelLayout::Interleaved?i*width:i*rows*columns*width,
        {rank==signal_contracts::Rank::Scalar?0:columns*lane,rank==signal_contracts::Rank::Matrix?lane:0}};
    std::size_t bytes=0;const auto storage=signal_contracts::required_storage(*c,std::span(planes).first(channels),bytes);
    if(storage!=Status::ok)return {storage,{}};
    if(bytes>zeros.size())return {Status::exhausted,{}};
    nd::Message m;m.endpoint={"device",0,nd::Direction::Input,0};
    m.contract=contract;m.stamp.stream_id="device";m.stamp.sequence=1;m.envelope={1,1,1,1,1,1,0};m.reference={1,1};
    m.numeric={std::span(zeros).first(bytes),std::span(planes).first(channels),{rows,columns},{},{},false};
    for(std::size_t axis=0;axis<2;++axis)if(c->coordinates[axis].mode==signal_contracts::CoordinateMode::ExplicitFloat64LE) {
        const auto elements=axis?columns:rows;
        if(elements>zeros.size()/8)return {Status::exhausted,{}};
        m.numeric.coordinates.axes[axis]=std::span(zeros).first(elements*8);
    }
    const nd::Binding binding{m.reference,m.endpoint,m.stamp.stream_id,contract,{},m.values};
    return nd::measure(m,limits,{1,1,1,std::span(&binding,1)});
}
domain::ProcessingSupport device_support(const void* ctx,const domain::ResolvedProcessingNode& node,const domain::ProcessingResourceProfile& resources) noexcept {
    const auto& device=*static_cast<const DeviceProfile*>(ctx);
    const auto actual=dataflow_runtime::PreparedGraph::numeric_capabilities(device.runtime);
    auto support=actual.query(actual.context,node,resources);if(!support.supported)return support;
    if(node.kind!="NetworkInput" && node.kind!="NetworkOutput")return support;
    auto ports=node.kind=="NetworkInput"?node.outputs:node.inputs;
    for(const auto& port:ports) {
        if(!measure_port(port.contract.view(),device.data))return {};
    }
    return support;
}
}
Admission graph_link_admission(const Profile& configured,const domain::NumericProcessingGraph& graph,
    std::size_t packet_payload_capacity) noexcept {
    auto p=configured;p.packet_payload_capacity=packet_payload_capacity;
    auto admission=admit(p,1);if(admission.status!=Status::ok)return admission;
    admission.frame_bytes=admission.wire_bytes=0;admission.required_bytes_per_second=0;
    const auto limits=numeric_data_limits(p);
    const auto chunk=std::min<std::size_t>(60000,p.packet_payload_capacity-8);
    // Capture emits one chunk per block. Only an upstream Window owns the
    // execution-interval scheduler; a descriptive PCM/Spectrum label cannot
    // establish the delivery cadence of a NetworkOutput.
    auto scheduled_output=[&](std::int32_t output){
        std::array<bool,512> upstream{};
        if(graph.nodes.size()>upstream.size())return true; // conservative bounded fallback
        for(std::size_t i=0;i<graph.nodes.size();++i)upstream[i]=graph.nodes[i].id==output;
        for(std::size_t pass=0;pass<graph.nodes.size();++pass){bool changed=false;
            for(const auto& edge:graph.edges){std::size_t from=graph.nodes.size(),to=from;
                for(std::size_t i=0;i<graph.nodes.size();++i){if(graph.nodes[i].id==edge.from)from=i;if(graph.nodes[i].id==edge.to)to=i;}
                if(from<graph.nodes.size() && to<graph.nodes.size() && upstream[to] && !upstream[from]){upstream[from]=true;changed=true;}}
            if(!changed)break;
        }
        for(std::size_t i=0;i<graph.nodes.size();++i)if(upstream[i] && graph.nodes[i].kind=="Window")return true;
        return false;
    };
    std::size_t sources=0;
    for(const auto& node:graph.nodes) {
        if(node.kind=="NetworkInput" || node.kind=="TestSignal") {admission.status=Status::incompatible_contract;return admission;}
        if(node.kind=="AudioSource") {
            ++sources;
            if(node.id!=0 || node.outputs.size()!=1) {admission.status=Status::incompatible_contract;return admission;}
            const auto contract=node.outputs.front().contract.view();const auto* c=std::get_if<signal_contracts::NumericContract>(&contract);
            if(!c || c->clock.sample_rate!=p.sample_rate ||
               c->shape.rank!=signal_contracts::Rank::Vector || c->shape.extents[0].maximum!=p.block_samples) {
                admission.status=Status::incompatible_contract;return admission;
            }
        }
        if(node.kind!="NetworkOutput")continue;
        for(const auto& port:node.inputs) {
            const auto contract=port.contract.view();const auto measured=measure_port(contract,limits);
            if(!measured){admission.status=measured.status;return admission;}
            const auto& c=std::get<signal_contracts::NumericContract>(contract);
            const auto bytes=measured.required.wire,fragments=(bytes+chunk-1)/chunk;
            const auto wire=2*(bytes+8*fragments)+16*fragments;
            auto required=(std::uint64_t(wire)*p.sample_rate+p.block_samples-1)/p.block_samples;
            if(scheduled_output(node.id)) {
                if(!domain::valid_execution_interval(graph.execution_interval)){admission.status=Status::invalid_argument;return admission;}
                const auto numerator=std::uint64_t(graph.execution_interval.numerator),denominator=graph.execution_interval.denominator;
                if(wire>UINT64_MAX/denominator || wire*denominator>UINT64_MAX-(numerator-1)){admission.status=Status::exhausted;return admission;}
                required=std::max(required,(wire*denominator+numerator-1)/numerator);
            }
            if(bytes>SIZE_MAX-admission.frame_bytes || wire>SIZE_MAX-admission.wire_bytes ||
               required>UINT64_MAX-admission.required_bytes_per_second) {admission.status=Status::exhausted;return admission;}
            admission.frame_bytes+=bytes;admission.wire_bytes+=wire;admission.required_bytes_per_second+=required;
        }
    }
    if(sources!=1){admission.status=Status::incompatible_contract;return admission;}
    admission.status=admission.required_bytes_per_second<=admission.allowed_bytes_per_second?Status::ok:Status::exhausted;
    return admission;
}
domain::NumericDeviceCapabilities numeric_capabilities(const DeviceProfile& p) noexcept {
    const auto runtime=dataflow_runtime::PreparedGraph::numeric_capabilities(p.runtime);
    return {&p,device_support,runtime.profile};
}
}
