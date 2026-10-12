#include "preparation_support.h"
#include "signal_time.h"
#include <wled_protocol/wled_packet.h>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace dataflow_runtime::detail {
bool numeric_float_dsp(const ResolvedProcessingNode&,ProcessingResources&) noexcept;
bool numeric_parameter(const ResolvedProcessingNode&,std::string_view,double&) noexcept;
ProcessingSupport numeric_support(const void* context,const ResolvedProcessingNode& n,const ProcessingResourceProfile&) noexcept {
    ProcessingSupport out;if(!context)return out;
    const auto& p=*static_cast<const NumericRuntimeProfile*>(context);
    out.resources.plan_bytes=sizeof(PlanNode)+sizeof(State)+2*sizeof(std::size_t);
    if((n.kind=="ScalarView" || n.operation==ProcessingOperation::Mixer || n.operation==ProcessingOperation::ChannelComparison || n.operation==ProcessingOperation::Extract || (n.operation==ProcessingOperation::Switch&&std::get<RouteProcessingParameters>(*n.parameters).channels_per_stream>1)) && !numeric_add(out.resources.plan_bytes,sizeof(PlanNode::EndpointPlan)))return out;
    if((n.kind=="ScalarView" || (n.operation==ProcessingOperation::Switch&&std::get<RouteProcessingParameters>(*n.parameters).channels_per_stream>1)) && !numeric_add(out.resources.plan_bytes,n.inputs.size()*sizeof(PlanNode::ScalarInput)))return out;
    if(n.operation==ProcessingOperation::Clock){out.supported=true;return out;}
    if(n.kind=="SignalGenerator" && !numeric_add(out.resources.payload_bytes,sizeof(dsp_kernels::OscillatorBank)))return out;
    if(n.operation==ProcessingOperation::Mixer || n.operation==ProcessingOperation::ChannelComparison){const auto params=n.operation==ProcessingOperation::Mixer?std::get<MixerProcessingParameters>(*n.parameters):MixerProcessingParameters{};
        const bool aligned=n.operation==ProcessingOperation::ChannelComparison || params.consumption==MixerConsumption::SourceAligned;
        for(const auto& port:n.inputs){const auto c=std::get<NumericContract>(port.contract.view());
            const auto count=!spectral_vector(c)?std::max<std::uint64_t>(c.shape.extents[0].maximum,(std::uint64_t(c.clock.sample_rate)*params.fifo_milliseconds+999)/1000):c.shape.extents[0].maximum*2;
            if(n.operation==ProcessingOperation::ChannelComparison){double window=512;if(!numeric_parameter(n,"window_samples",window)||window>count)return out;}
            if(!numeric_add(out.resources.payload_bytes,count*(sizeof(float)+(!spectral_vector(c)?sizeof(MixerSampleProvenance):0)+(aligned?sizeof(AlignedSample):0))+sizeof(MixerQueue)+(c.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE?c.shape.extents[0].maximum*16:0)))return out;}
        if(!numeric_add(out.resources.plan_bytes,n.inputs.size()*(sizeof(PlanNode::ScalarInput)+sizeof(PlanNode::MixerDestination)+sizeof(double))))return out;
    }
    if(!numeric_add(out.resources.payload_bytes,p.admission.frame.max_string_bytes+n.outputs.size()*(sizeof(FrameStamp)+sizeof(bool))))return out;
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
        const bool shared_zero=zero_bytes(*c) && (port.direction==PortDirection::Input || (n.operation==ProcessingOperation::Source && n.kind!="SignalGenerator") ||
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
    if(a.kind=="Mixer"&&std::get<MixerProcessingParameters>(a.parameters).consumption!=std::get<MixerProcessingParameters>(b.parameters).consumption)return false;
    if(a.kind=="ChannelComparison"){
        const auto& current=std::get<TransformProcessingParameters>(a.parameters).values;
        const auto& prior=std::get<TransformProcessingParameters>(b.parameters).values;
        if(current.size()!=prior.size())return false;
        for(std::size_t i=0;i<current.size();++i)if(current[i].key!=prior[i].key||current[i].value!=prior[i].value)return false;
    }
    if(a.kind=="InputSelector"){
        if(a.outputs.size()!=b.outputs.size())return false;
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
    if(input.channel_order.size()!=1 || output.channel_order.size()!=1)return false;
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
namespace dataflow_runtime {
using namespace detail;
using namespace domain;
using namespace signal_contracts;
NumericDeviceCapabilities PreparedGraph::numeric_capabilities(const NumericRuntimeProfile& p) noexcept {
    auto resources=p.resources;
    if(!numeric_add(resources.retained_working_bytes,fixed_plan_reservation) || !numeric_add(resources.retained_working_bytes,p.admission.frame.max_payload_bytes))resources.retained_working_bytes=SIZE_MAX;
    return {&p,numeric_support,resources};
}

} // namespace dataflow_runtime
