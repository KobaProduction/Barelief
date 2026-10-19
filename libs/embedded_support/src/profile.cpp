#include <embedded_support/profile.h>
#include <bdsp_protocol/numeric_data_codec.h>
#include <bdsp_protocol/stream_codec.h>
#include <dsp_kernels/dsp.h>
#include <limits>
#include <algorithm>
#include <array>
namespace embedded_support {
using signal_contracts::Status;
namespace {
bool profile_valid(const Profile& p,std::size_t id) noexcept {
    return id && id<=64 && p.sample_rate>=1000 && p.sample_rate<=192000 && p.block_samples &&
       p.block_samples<=4096 && p.fft_size>=512 && p.fft_size<=4096 && !(p.fft_size&(p.fft_size-1)) &&
       p.mel_bands && p.mel_bands<=p.fft_size/2 && p.queue_frames && p.queue_frames<=8 && p.channels==1 &&
       p.packet_payload_capacity>8 && p.packet_payload_capacity<=65535 &&
       domain::valid_execution_interval(p.execution_interval) && p.utilization_percent &&
       p.utilization_percent<=100 && (!p.spectral || p.fft_size<=dsp_kernels::max_fft_samples);
}
signal_contracts::NumericContract reference_contract(const Profile& p,
                                                     std::span<const std::uint32_t> channels) noexcept {
    signal_contracts::NumericContract contract;contract.dtype=signal_contracts::DType::Float32;
    contract.channel_order=channels.first(p.spectral?1:p.channels);
    contract.semantic=p.spectral?signal_contracts::Semantic::MelEnergy:signal_contracts::Semantic::PCM;
    contract.quantity=p.spectral?signal_contracts::Quantity::MelEnergy:signal_contracts::Quantity::Amplitude;
    contract.unit=p.spectral?signal_contracts::NumericUnit::Values:signal_contracts::NumericUnit::NormalizedAmplitude;
    contract.shape.rank=signal_contracts::Rank::Vector;
    contract.shape.axes={p.spectral?signal_contracts::Axis::Band:signal_contracts::Axis::Sample,signal_contracts::Axis::None};
    const std::size_t elements=p.spectral?p.mel_bands:p.block_samples;
    contract.shape.extents={signal_contracts::ExtentBounds{p.spectral?elements:1,elements},signal_contracts::ExtentBounds{1,1}};
    contract.clock={"embedded-source",{1,p.sample_rate},p.sample_rate};
    contract.representation.encoding=signal_contracts::StorageEncoding::FixedLittleEndian;
    return contract;
}
void configure_reference_message(bdsp_protocol::numeric_data::Message& message,
    const signal_contracts::NumericContract& contract,std::string_view stream,
    std::span<const std::byte> zeros,std::span<const signal_contracts::PlaneLayout> planes,
    std::size_t elements) noexcept {
    namespace nd=bdsp_protocol::numeric_data;
    message.endpoint={"embedded-synthetic",1,nd::Direction::Input,0};
    message.envelope={1,1,1,1,1,1,0};
    message.contract=contract;
    message.stamp.sequence=1;message.stamp.stream_id=stream;
    message.numeric={zeros,planes,{elements,1},{},{},false};
    message.reference={1,1};
}

bdsp_protocol::numeric_data::Result measure_reference_frame(const Profile& p,std::size_t id,
                                                              std::size_t ceiling) noexcept {
    namespace nd=bdsp_protocol::numeric_data;
    static constexpr std::array<std::byte,32768> zeros{};
    static constexpr std::array<char,256> stream=[] {std::array<char,256> s{};s.fill('s');return s;}();
    static constexpr std::array<std::uint32_t,2> channels{0,1};
    const auto contract=reference_contract(p,channels);
    const std::size_t elements=p.spectral?p.mel_bands:p.block_samples;
    const std::array<signal_contracts::PlaneLayout,2> planes{{{0,{4,0}},{elements*4,{4,0}}}};
    nd::Message message;
    configure_reference_message(message,contract,{stream.data(),id},
        std::span(zeros).first(elements*contract.channel_order.size()*4),
        std::span(planes).first(contract.channel_order.size()),elements);
    nd::Limits limits{{2,8192,32768,4096,256,256},ceiling,4096,0,0,256};
    const nd::Binding binding{message.reference,message.endpoint,message.stamp.stream_id,
                              message.contract,{},message.values};
    return nd::measure(message,limits,{1,1,1,std::span(&binding,1)});
}
Status charge_link(const Profile& p,Admission& admission) noexcept {
    const auto chunk=std::min<std::size_t>(60000,p.packet_payload_capacity-8);
    const auto fragments=(admission.frame_bytes+chunk-1)/chunk;
    admission.wire_bytes=2*(admission.frame_bytes+8*fragments)+16*fragments;
    const std::uint64_t denominator=p.spectral?std::uint64_t(p.execution_interval.numerator):p.block_samples;
    const std::uint64_t frequency=p.spectral?p.execution_interval.denominator:p.sample_rate;
    if(admission.wire_bytes>UINT64_MAX/frequency ||
       admission.wire_bytes*frequency>UINT64_MAX-(denominator-1))return Status::exhausted;
    admission.required_bytes_per_second=(admission.wire_bytes*frequency+denominator-1)/denominator;
    const std::uint64_t capacity=p.link==Link::uart_8n1?p.uart_baud/10:p.tcp_bytes_per_second;
    admission.allowed_bytes_per_second=capacity*p.utilization_percent/100;
    return admission.required_bytes_per_second>admission.allowed_bytes_per_second?Status::exhausted:Status::ok;
}
Status charge_memory(const Profile& p,application::MemoryRequirements runtime,Admission& admission) noexcept {
    const auto limit=std::numeric_limits<std::size_t>::max();
    admission.reserved_bytes=p.adapter_bytes;
    // Pool capacity is retained even when a frame is smaller than its slot.
    for(auto bytes:{p.max_frame_bytes*p.queue_frames,p.control_bytes,runtime.state_bytes,
                    runtime.buffer_bytes,runtime.scratch_bytes}) {
        if(bytes>limit-admission.reserved_bytes)return Status::exhausted;
        admission.reserved_bytes+=bytes;
    }
    return admission.reserved_bytes<=p.memory_bytes?Status::ok:Status::exhausted;
}
}
signal_contracts::Status admit_reference_frame(const Profile& profile,std::size_t stream_id,
                                                Admission& admission) noexcept {
    const auto ceiling=bdsp_protocol::numeric_data::desktop_limits().message_bytes;
    const auto measured=measure_reference_frame(profile,stream_id,ceiling);
    if(!measured){admission.status=measured.status;return admission.status;}
    admission.frame_bytes=measured.required.wire;
    if(admission.frame_bytes>profile.max_frame_bytes || profile.max_frame_bytes>ceiling) {
        admission.status=signal_contracts::Status::buffer_too_small;
        return admission.status;
    }
    return signal_contracts::Status::ok;
}

Admission admit(const Profile &p, std::size_t id, application::MemoryRequirements runtime) noexcept {
    Admission a;
    if(!profile_valid(p,id))return a;
    if(admit_reference_frame(p,id,a)!=Status::ok)return a;
    a.status=charge_link(p,a);if(a.status!=Status::ok)return a;
    a.status=charge_memory(p,runtime,a);
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
    const auto control=bdsp_protocol::numeric_control::device_limits();
    return {{control.nodes,control.edges,4,16,64,16384,frame},{{budget,budget,budget,p.task_stack_bytes},0,budget},
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
Status port_layout(const signal_contracts::NumericContract& contract,
                   std::span<signal_contracts::PlaneLayout> planes,
                   std::size_t& rows,std::size_t& columns,std::size_t& bytes) noexcept {
    std::size_t width=0;
    const auto sized=signal_contracts::storage_width(contract,width);if(sized!=Status::ok)return sized;
    const auto rank=contract.shape.rank;
    rows=rank==signal_contracts::Rank::Scalar?1:std::size_t(contract.shape.extents[0].maximum);
    columns=rank==signal_contracts::Rank::Matrix?std::size_t(contract.shape.extents[1].maximum):1;
    const auto channels=contract.channel_order.size();
    const auto lane=contract.representation.layout==signal_contracts::ChannelLayout::Interleaved?channels*width:width;
    for(std::size_t index=0;index<channels;++index)
        planes[index]={contract.representation.layout==signal_contracts::ChannelLayout::Interleaved?
            index*width:index*rows*columns*width,
            {rank==signal_contracts::Rank::Scalar?0:columns*lane,
             rank==signal_contracts::Rank::Matrix?lane:0}};
    return signal_contracts::required_storage(contract,planes.first(channels),bytes);
}
Status port_coordinates(const signal_contracts::NumericContract& contract,
                        bdsp_protocol::numeric_data::Message& message,
                        std::size_t rows,std::size_t columns,
                        std::span<const std::byte> zeros) noexcept {
    for(std::size_t axis=0;axis<2;++axis)
        if(contract.coordinates[axis].mode==signal_contracts::CoordinateMode::ExplicitFloat64LE) {
            const auto elements=axis?columns:rows;
            if(elements>zeros.size()/8)return Status::exhausted;
            message.numeric.coordinates.axes[axis]=zeros.first(elements*8);
        }
    return Status::ok;
}
signal_contracts::Status port_measurement_layout(const signal_contracts::DataContract& contract,
    const bdsp_protocol::numeric_data::Limits& limits,
    std::span<signal_contracts::PlaneLayout> planes,std::size_t& rows,
    std::size_t& columns,std::size_t& bytes) noexcept {
    namespace nd=bdsp_protocol::numeric_data;
    const auto* numeric=std::get_if<signal_contracts::NumericContract>(&contract);
    std::uint8_t symbol=0;
    if(!numeric || !nd::symbol_id(numeric->representation.encoding,symbol))
        return signal_contracts::Status::incompatible_contract;
    const auto valid=signal_contracts::validate_contract(contract,limits.frame);
    if(valid!=signal_contracts::Status::ok)return valid;
    if(numeric->channel_order.size()>planes.size())return signal_contracts::Status::exhausted;
    return port_layout(*numeric,planes,rows,columns,bytes);
}

void initialize_port_message(bdsp_protocol::numeric_data::Message& message,
    const signal_contracts::DataContract& contract,std::span<const std::byte> zeros,
    std::span<const signal_contracts::PlaneLayout> planes,std::size_t rows,
    std::size_t columns) noexcept {
    message.endpoint={"device",0,bdsp_protocol::numeric_data::Direction::Input,0};
    message.contract=contract;
    message.stamp.stream_id="device";message.stamp.sequence=1;
    message.envelope={1,1,1,1,1,1,0};message.reference={1,1};
    message.numeric={zeros,planes,{rows,columns},{},{},false};
}

bdsp_protocol::numeric_data::Result measure_port(const signal_contracts::DataContract& contract,
    const bdsp_protocol::numeric_data::Limits& limits) noexcept {
    namespace nd=bdsp_protocol::numeric_data;
    static constexpr std::array<std::byte,32768> zeros{};
    std::array<signal_contracts::PlaneLayout,2> planes{};
    std::size_t rows=0,columns=0,bytes=0;
    const auto storage=port_measurement_layout(contract,limits,planes,rows,columns,bytes);
    if(storage!=Status::ok)return {storage,{}};
    if(bytes>zeros.size())return {Status::exhausted,{}};
    const auto& numeric=std::get<signal_contracts::NumericContract>(contract);
    nd::Message message;
    initialize_port_message(message,contract,std::span(zeros).first(bytes),
        std::span(planes).first(numeric.channel_order.size()),rows,columns);
    const auto coordinates=port_coordinates(numeric,message,rows,columns,zeros);
    if(coordinates!=Status::ok)return {coordinates,{}};
    const nd::Binding binding{message.reference,message.endpoint,message.stamp.stream_id,
                              contract,{},message.values};
    return nd::measure(message,limits,{1,1,1,std::span(&binding,1)});
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
class LinkAdmission {
    const Profile& profile;
    const domain::NumericProcessingGraph& graph;
    const bdsp_protocol::numeric_data::Limits limits;
    Admission& admission;
    const std::size_t chunk;
    std::size_t sources=0;
    std::size_t node_index(std::int32_t id) const noexcept {
        for(std::size_t index=0;index<graph.nodes.size();++index)
            if(graph.nodes[index].id==id)return index;
        return graph.nodes.size();
    }
    bool expand_upstream(std::array<bool,512>& upstream) const noexcept {
        bool changed=false;
        for(const auto& edge:graph.edges) {
            const auto from=node_index(edge.from),to=node_index(edge.to);
            if(from<graph.nodes.size() && to<graph.nodes.size() && upstream[to] && !upstream[from]) {
                upstream[from]=true;changed=true;
            }
        }
        return changed;
    }
void collect_upstream(std::int32_t output,std::array<bool,512>& upstream) const noexcept {
        for(std::size_t index=0;index<graph.nodes.size();++index)
            upstream[index]=graph.nodes[index].id==output;
        for(std::size_t pass=0;pass<graph.nodes.size();++pass)
            if(!expand_upstream(upstream))break;
    }

    bool scheduled_output(std::int32_t output) const noexcept {
        std::array<bool,512> upstream{};
        if(graph.nodes.size()>upstream.size())return true;
        collect_upstream(output,upstream);
        for(std::size_t index=0;index<graph.nodes.size();++index)
            if(upstream[index] && graph.nodes[index].kind=="Window")return true;
        return false;
    }
    Status check_source(const domain::ConfiguredProcessingNode& node) noexcept {
        ++sources;
        if(node.outputs.empty() || (node.kind!="NetworkInput" && node.outputs.size()!=1))
            return Status::incompatible_contract;
        const auto contract=node.outputs.front().contract.view();
        const auto* numeric=std::get_if<signal_contracts::NumericContract>(&contract);
        if(!numeric || numeric->clock.sample_rate!=profile.sample_rate ||
           numeric->shape.rank!=signal_contracts::Rank::Vector ||
           numeric->shape.extents[0].maximum>profile.block_samples)return Status::incompatible_contract;
        return Status::ok;
    }
    Status required_rate(std::int32_t node_id,std::size_t wire,std::uint64_t& required) const noexcept {
        required=(std::uint64_t(wire)*profile.sample_rate+profile.block_samples-1)/profile.block_samples;
        if(!scheduled_output(node_id))return Status::ok;
        if(!domain::valid_execution_interval(graph.execution_interval))return Status::invalid_argument;
        const auto numerator=std::uint64_t(graph.execution_interval.numerator);
        const auto denominator=graph.execution_interval.denominator;
        if(wire>UINT64_MAX/denominator || wire*denominator>UINT64_MAX-(numerator-1))return Status::exhausted;
        required=std::max(required,(wire*denominator+numerator-1)/numerator);
        return Status::ok;
    }
    Status charge_port(std::int32_t node_id,const domain::ProcessingPort& port) noexcept {
        const auto contract=port.contract.view();
        const auto measured=measure_port(contract,limits);
        if(!measured)return measured.status;
        const auto bytes=measured.required.wire,fragments=(bytes+chunk-1)/chunk;
        const auto wire=2*(bytes+8*fragments)+16*fragments;
        std::uint64_t required=0;
        const auto rate=required_rate(node_id,wire,required);if(rate!=Status::ok)return rate;
        if(bytes>SIZE_MAX-admission.frame_bytes || wire>SIZE_MAX-admission.wire_bytes ||
           required>UINT64_MAX-admission.required_bytes_per_second)return Status::exhausted;
        admission.frame_bytes+=bytes;admission.wire_bytes+=wire;
        admission.required_bytes_per_second+=required;
        return Status::ok;
    }
public:
    LinkAdmission(const Profile& p,const domain::NumericProcessingGraph& g,
                  bdsp_protocol::numeric_data::Limits data,Admission& result) noexcept
        :profile(p),graph(g),limits(data),admission(result),chunk(std::min<std::size_t>(60000,p.packet_payload_capacity-8)){}
Status scan_node(const decltype(graph.nodes)::value_type& node) noexcept {
        if(node.kind=="TestSignal")return Status::incompatible_contract;
        if(node.kind=="AudioSource" || node.kind=="SignalGenerator" ||
           node.kind=="NetworkInput") {
            const auto source=check_source(node);
            if(source!=Status::ok)return source;
        }
        if(node.kind!="NetworkOutput")return Status::ok;
        for(const auto& port:node.inputs) {
            const auto status=charge_port(node.id,port);
            if(status!=Status::ok)return status;
        }
        return Status::ok;
    }

    Status scan() noexcept {
        for(const auto& node:graph.nodes) {
            const auto status=scan_node(node);
            if(status!=Status::ok)return status;
        }
        if(!sources)return Status::incompatible_contract;
        return admission.required_bytes_per_second<=admission.allowed_bytes_per_second?
            Status::ok:Status::exhausted;
    }
};
}
Admission graph_link_admission(const Profile& configured,const domain::NumericProcessingGraph& graph,
    std::size_t packet_payload_capacity) noexcept {
    auto p=configured;p.packet_payload_capacity=packet_payload_capacity;
    auto admission=admit(p,1);if(admission.status!=Status::ok)return admission;
    admission.frame_bytes=admission.wire_bytes=0;admission.required_bytes_per_second=0;
    const auto limits=numeric_data_limits(p);
    // Capture emits one chunk per block. Only an upstream Window owns the
    // execution-interval scheduler; a descriptive PCM/Spectrum label cannot
    // establish the delivery cadence of a NetworkOutput.
    admission.status=LinkAdmission(p,graph,limits,admission).scan();
    return admission;
}
domain::NumericDeviceCapabilities numeric_capabilities(const DeviceProfile& p) noexcept {
    const auto runtime=dataflow_runtime::PreparedGraph::numeric_capabilities(p.runtime);
    return {&p,device_support,runtime.profile};
}
}
