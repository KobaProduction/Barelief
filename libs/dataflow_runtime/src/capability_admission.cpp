#include "preparation_support.h"
#include "signal_time.h"
#include <wled_protocol/wled_packet.h>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace dataflow_runtime::detail {
bool numeric_float_dsp(const ResolvedProcessingNode&,ProcessingResources&) noexcept;
bool numeric_parameter(const ResolvedProcessingNode&,std::string_view,double&) noexcept;
namespace {
bool constant_conversion_resources(const ResolvedProcessingNode& node,const NumericRuntimeProfile& profile,ProcessingResources& resources) noexcept {
    ProcessingParameterView parameter;
    if(resolve_processing_parameter(node,"value",parameter)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&parameter);if(!literal)return false;
    const auto output=std::get<NumericContract>(node.outputs.front().contract.view());
    const auto dtype=std::visit([](auto value){return dtype_of<decltype(value)>();},*literal);
    if(dtype==output.dtype)return true;
    const auto input=constant_literal_contract(output,dtype);
    const auto support=dsp_kernels::conversion_support({input,output,{},profile.kernels,profile.admission.frame},profile.capabilities);
    return support.status==dsp_kernels::NumericStatus::Ok &&
        numeric_add(resources.scratch_bytes,support.requirements.scratch_bytes) &&
        numeric_add(resources.stack_bytes,support.requirements.stack_bytes);
}
}
namespace {
class ResourceAdmission {
    const ResolvedProcessingNode& n;
    const NumericRuntimeProfile& p;
    ProcessingSupport out;
    const ProcessingPort* primary(std::span<const ProcessingPort> ports) const noexcept {
        const auto* zero=numeric_port(ports,0);
        return zero?zero:(ports.empty()?nullptr:&ports.front());
    }
    NumericContract get(std::span<const ProcessingPort> ports) const noexcept {
        return std::get<NumericContract>(primary(ports)->contract.view());
    }
    bool history(std::span<const ProcessingPort> ports) const noexcept {
        const auto* port=primary(ports);if(!port)return false;
        const auto contract=port->contract.view();const auto* value=std::get_if<NumericContract>(&contract);
        return value && (value->shape.rank==Rank::Matrix || value->semantic==Semantic::History);
    }
    bool content_storage() noexcept {
        // Reserve every output, even when preparation reuses a token. The
        // control-block allowance covers both native and 32-bit shared owners.
        std::size_t inputs=0,output=sizeof(ContentToken)+sizeof(ContentLineage)+2*sizeof(ContentToken),bytes=0;
        return numeric_mul(std::max(std::size_t{1},n.inputs.size()),sizeof(ContentContribution),inputs) &&
            numeric_add(output,inputs) && numeric_mul(n.outputs.size(),output,bytes) && numeric_add(out.resources.plan_bytes,bytes);
    }
    bool endpoints() noexcept {
        out.resources.plan_bytes=sizeof(PlanNode)+sizeof(State)+2*sizeof(std::size_t);
        if(!content_storage())return false;
        const bool multi_switch=n.operation==ProcessingOperation::Switch && std::get<RouteProcessingParameters>(*n.parameters).channels_per_stream>1;
        const auto kind=node_kind(n.kind);
        const bool scalar=(kind && is_scalar_view(*kind)) || multi_switch;
        if((scalar || n.operation==ProcessingOperation::Mixer || n.operation==ProcessingOperation::ChannelComparison || n.operation==ProcessingOperation::Extract) && !numeric_add(out.resources.plan_bytes,sizeof(PlanNode::EndpointPlan)))return false;
        return !scalar || numeric_add(out.resources.plan_bytes,n.inputs.size()*sizeof(PlanNode::ScalarInput));
    }
    bool music() noexcept {
        const bool musical=n.kind=="MusicalSpectrogram" || n.kind=="NoteSpectrum" || n.kind=="NoteSalience" || n.kind=="RhythmAnalyzer" || n.kind=="RhythmFeatures";
        if(musical && !p.music_enabled)return false;
        if((n.operation==ProcessingOperation::MusicTransform || paired_processing(n.operation)) && !music_support(n,out.resources,p))return false;
        return !paired_processing(n.operation) || numeric_add(out.resources.plan_bytes,sizeof(PlanNode::EndpointPlan)+n.inputs.size()*sizeof(PlanNode::ScalarInput));
    }
    bool resampler() noexcept {
        dsp_kernels::ResamplerConfig config;dsp_kernels::ResamplerResources requirements;
        if(!resampler_config(n,config) || (!p.experimental_resampling_enabled && n.enabled && config.mode!=dsp_kernels::ResamplerMode::ExistingFir && config.mode!=dsp_kernels::ResamplerMode::Bypass) ||
           !dsp_kernels::PreparedResampler::resource_requirements(config,p.resampler_backend,requirements) ||
           !numeric_add(out.resources.payload_bytes,requirements.owned_bytes))return false;

        return true;
    }
    bool mixer_port(const ProcessingPort& port,const MixerProcessingParameters& params,bool aligned) noexcept {
        const auto contract=std::get<NumericContract>(port.contract.view());
        const auto count=!spectral_vector(contract)?
            std::max<std::uint64_t>(contract.shape.extents[0].maximum,
                                    (std::uint64_t(contract.clock.sample_rate)*params.fifo_milliseconds+999)/1000):
            contract.shape.extents[0].maximum*2;
        if(n.operation==ProcessingOperation::ChannelComparison) {
            double window=512;
            if(!numeric_parameter(n,"window_samples",window)||window>count)return false;
        }
        return numeric_add(out.resources.payload_bytes,
            count*(sizeof(float)+(!spectral_vector(contract)?sizeof(MixerSampleProvenance):0)+
                   (aligned?sizeof(AlignedSample):0))+sizeof(MixerQueue)+
            (contract.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE?
             contract.shape.extents[0].maximum*16:0));
    }
    bool mixer() noexcept {
        const auto params=n.operation==ProcessingOperation::Mixer?
            std::get<MixerProcessingParameters>(*n.parameters):MixerProcessingParameters{};
        const bool aligned=n.operation==ProcessingOperation::ChannelComparison ||
            params.consumption==MixerConsumption::SourceAligned;
        for(const auto& port:n.inputs)if(!mixer_port(port,params,aligned))return false;
        if(!numeric_add(out.resources.plan_bytes,n.inputs.size()*(sizeof(PlanNode::ScalarInput)+sizeof(PlanNode::MixerDestination)+sizeof(double))))return false;

        return true;
    }
    bool common_storage() noexcept {
        if(n.kind=="SignalGenerator" && !numeric_add(out.resources.payload_bytes,sizeof(dsp_kernels::OscillatorBank)))return false;
        if((n.operation==ProcessingOperation::Mixer || n.operation==ProcessingOperation::ChannelComparison) && !mixer())return false;
        if(!numeric_add(out.resources.payload_bytes,p.admission.frame.max_string_bytes+n.outputs.size()*(sizeof(FrameStamp)+sizeof(bool))))return false;
        // Conservative callback/dispatch reservation, not a native ABI measurement.
        if(!numeric_mul(p.admission.max_nodes,1536,out.resources.stack_bytes) || !numeric_add(out.resources.stack_bytes,4096))return false;
        return !numeric_constant_kind(n.kind) || constant_conversion_resources(n,p,out.resources);
    }
    bool output_objects(const ProcessingPort& port) noexcept {
        return port.direction!=PortDirection::Output || numeric_add(out.resources.plan_bytes,sizeof(DataContract)+sizeof(DataFrameView)+sizeof(NumericBuffer));
    }
    bool structured_port(const ProcessingPort& port,const StructuredContract& contract) noexcept {
        if(n.operation!=ProcessingOperation::Source && n.operation!=ProcessingOperation::Sink && n.operation!=ProcessingOperation::Gate && n.operation!=ProcessingOperation::Switch && n.operation!=ProcessingOperation::Splitter && n.operation!=ProcessingOperation::WLEDEncoder)return false;
        return contract.payload_bytes.maximum<=p.admission.frame.max_payload_bytes && numeric_add(out.resources.payload_bytes,std::size_t(contract.payload_bytes.maximum)) && output_objects(port);
    }
    bool coordinate_storage(const NumericContract& contract,std::size_t& metadata) noexcept {
        std::size_t coordinates=0;
        for(std::size_t axis=0;axis<2;++axis)if(contract.coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE){
            std::size_t bytes=0;
            if(!numeric_mul(std::size_t(contract.shape.extents[axis].maximum),8,bytes) || !numeric_add(coordinates,bytes))return false;
        }
        if(coordinates>p.admission.frame.max_coordinate_bytes || !numeric_add(metadata,coordinates) || metadata>p.admission.frame.max_metadata_bytes || !numeric_add(out.resources.payload_bytes,coordinates))return false;
        return contract.semantic!=Semantic::History || contract.coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE || numeric_add(out.resources.payload_bytes,std::size_t(contract.shape.extents[0].maximum)*8);
    }
    bool numeric_metadata(const NumericContract& contract) noexcept {
        if(!p.history_enabled && n.operation==ProcessingOperation::History)return false;
        std::size_t metadata=contract.channel_order.size()*(sizeof(PlaneLayout)+sizeof(std::uint32_t));
        if(!numeric_add(metadata,contract.clock.domain.size()) || !numeric_add(metadata,p.admission.frame.max_string_bytes))return false;
        if(contract.shape.rank==Rank::Matrix && !n.inputs.empty() && n.operation!=ProcessingOperation::History && !numeric_add(metadata,std::size_t(contract.shape.extents[0].maximum)))return false;
        return coordinate_storage(contract,metadata);
    }
    bool numeric_port_storage(const ProcessingPort& port,const NumericContract& contract) noexcept {
        if(!numeric_metadata(contract))return false;
        std::size_t bytes=0,stride=0,plane=0;std::array<std::size_t,2> shape;
        if(!numeric_geometry(contract,bytes,stride,plane,shape) || bytes>p.admission.frame.max_payload_bytes)return false;
        const bool shared_zero=zero_bytes(contract) && (port.direction==PortDirection::Input || (n.operation==ProcessingOperation::Source && n.kind!="SignalGenerator" && !numeric_constant_kind(n.kind)) || (n.operation==ProcessingOperation::Transform && n.kind=="Window" && direct_window(contract)));
        // Inline input objects are charged by endpoints(); output vectors own objects.
        return (shared_zero || numeric_add(out.resources.payload_bytes,bytes)) && numeric_add(out.resources.plan_bytes,contract.channel_order.size()*sizeof(PlaneLayout)) && output_objects(port);
    }
    bool ports() noexcept {
        for(auto values:{n.inputs,n.outputs})for(const auto& port:values){
            const auto contract=port.contract.view();const auto* numeric=std::get_if<NumericContract>(&contract);
            if(numeric ? !numeric_port_storage(port,*numeric) : !structured_port(port,std::get<StructuredContract>(contract)))return false;
        }
        return true;
    }
    bool wled() noexcept {
        const auto* port=numeric_port(n.inputs,0);if(!port || n.inputs.size()!=3)return false;
        const auto input=port->contract.view();const auto* c=std::get_if<NumericContract>(&input);double gain=0;
        if(!c || c->dtype!=DType::Float32 || c->representation.encoding!=StorageEncoding::Native ||
           c->representation.normalization!=Normalization::None || c->channel_order.size()!=1 ||
           c->shape.rank!=Rank::Vector || c->shape.extents[0].maximum>wled_protocol::formatted_mel_count ||
           !numeric_parameter(n,"gain",gain))return false;
        return numeric_add(out.resources.payload_bytes,std::size_t(c->shape.extents[0].maximum)*sizeof(float)) &&
            numeric_add(out.resources.payload_bytes,sizeof(WledInputState)+p.admission.frame.max_string_bytes) &&
            numeric_add(out.resources.plan_bytes,sizeof(PlanNode::EndpointPlan)+3*(sizeof(PlanNode::ScalarInput)+sizeof(PlanNode::MixerDestination)));
    }
    bool matrix_center() noexcept {
        const auto input=std::get<NumericContract>(numeric_port(n.inputs,0)->contract.view());
        if(input.representation.encoding!=StorageEncoding::Native || input.representation.layout!=ChannelLayout::Planar || input.representation.valid_bits ||
           (input.representation.normalization!=Normalization::None && input.representation.normalization!=Normalization::FloatUnit))return false;

        return true;
    }
    bool matrix_window() noexcept {
        const auto input=std::get<NumericContract>(numeric_port(n.inputs,0)->contract.view());
        std::size_t bank_bytes=0;
        if(input.representation.encoding!=StorageEncoding::Native || input.representation.layout!=ChannelLayout::Planar ||
           input.representation.valid_bits || (input.representation.normalization!=Normalization::None && input.representation.normalization!=Normalization::FloatUnit) ||
           !dsp_kernels::PreparedWindows::storage_bytes(std::size_t(input.shape.extents[1].maximum),bank_bytes) ||
           !numeric_add(out.resources.plan_bytes,sizeof(dsp_kernels::PreparedWindows)) ||
           !numeric_add(out.resources.payload_bytes,bank_bytes) ||
           !numeric_add(out.resources.payload_bytes,std::size_t(input.shape.extents[1].maximum)*sizeof(float)))return false;

        return true;
    }
    bool matrix_fft() noexcept {
        const auto input=std::get<NumericContract>(numeric_port(n.inputs,0)->contract.view());
        const auto output=std::get<NumericContract>(numeric_port(n.outputs,0)->contract.view());
        const auto samples=std::size_t(output.shape.extents[1].maximum)*2;
        if(input.representation.encoding!=StorageEncoding::Native || input.representation.layout!=ChannelLayout::Planar ||
           input.representation.valid_bits || (input.representation.normalization!=Normalization::None && input.representation.normalization!=Normalization::FloatUnit) ||
           samples>dsp_kernels::max_fft_samples)return false;
        // One reusable FFT and one row staging vector.
        // estimate_memory has a legacy minimum of 512; the backend supports 256.
        const auto memory=dsp_kernels::estimate_memory(std::max(samples,std::size_t{512}),0);if(!memory)return false;
        if(!numeric_add(out.resources.payload_bytes,(samples+samples/2)*sizeof(float)) ||
           !numeric_add(out.resources.scratch_bytes,samples*2*sizeof(float)+memory->shared_radix2_tables_bytes))return false;

        return true;
    }
    bool operations() noexcept {
        if(n.operation==ProcessingOperation::WLEDEncoder && !wled())return false;
        if(n.operation==ProcessingOperation::MatrixCenter && !matrix_center())return false;
        if(n.operation==ProcessingOperation::MatrixWindowFunction && !matrix_window())return false;
        if(n.operation==ProcessingOperation::MatrixFFT && !matrix_fft())return false;
        return n.operation!=ProcessingOperation::Transform || numeric_float_dsp(n,out.resources);
    }
    bool history_masks() noexcept {
        const auto rows=get(n.operation==ProcessingOperation::Source?n.outputs:n.inputs).shape.extents[0].maximum;std::size_t mask_bytes=0;
        if(!numeric_mul(std::size_t(rows),2*std::max(std::size_t{1},n.outputs.size()),mask_bytes) ||
           !numeric_add(out.resources.payload_bytes,mask_bytes) || !numeric_add(out.resources.plan_bytes,std::max(std::size_t{1},n.outputs.size())*(sizeof(std::vector<std::byte>)+sizeof(HistoryRouteState))))return false;

        return true;
    }
    bool history_source() noexcept {
        std::size_t bytes=0,stride=0,plane=0;std::array<std::size_t,2> shape;
        if(!numeric_geometry(get(n.outputs),bytes,stride,plane,shape) || !numeric_add(out.resources.payload_bytes,bytes) ||
           !numeric_add(out.resources.plan_bytes,p.admission.frame.max_metadata_bytes+sizeof(OwnedDataFrame)))return false;
        return true;
    }
    bool history_contract() noexcept {const auto c=get(n.inputs);
        const auto h=get(n.outputs);
        if(c.shape.extents[0].minimum!=c.shape.extents[0].maximum)return false;
        if(h.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE && h.coordinates[0].unit!=NumericUnit::Seconds && h.coordinates[0].unit!=NumericUnit::Frames)return false;
        return true;
    }
    bool converter() noexcept {if(get(n.inputs).semantic==Semantic::History && get(n.inputs).shape.rank==Rank::Vector)return false;
        const auto& policy=std::get<ConverterProcessingParameters>(*n.parameters).policy;
        const auto s=dsp_kernels::conversion_support({get(n.inputs),get(n.outputs),policy,p.kernels,p.admission.frame},p.capabilities);
        if(s.status!=dsp_kernels::NumericStatus::Ok)return false;
        out.resources.scratch_bytes=s.requirements.scratch_bytes;if(!numeric_add(out.resources.stack_bytes,s.requirements.stack_bytes))return false;
        if(!numeric_add(out.resources.plan_bytes,s.requirements.plan_bytes))return false;
        return !history(n.inputs) || converter_rows();
    }
    bool converter_rows() noexcept {NumericGatherRequirements req;const NumericGatherRequest request{get(n.inputs),{},
                {p.admission.frame,p.admission.frame.max_payload_bytes,p.admission.frame.max_metadata_bytes,p.kernels.max_scratch_bytes},true};
            if(numeric_gather_requirements(request,req)!=Status::ok || !numeric_add(out.resources.plan_bytes,req.plan_metadata_bytes+sizeof(GatherOwner)+sizeof(dsp_kernels::PreparedConversion)) ||
               !numeric_add(out.resources.payload_bytes,req.payload_bytes+req.output_metadata_bytes+get(n.outputs).channel_order.size()*sizeof(PlaneLayout)) ||
               !numeric_add(out.resources.scratch_bytes,req.scratch_bytes))return false;
        return true;
    }
    bool reduction() noexcept {const auto request=numeric_gather_request(n,p);const auto& reduction=std::get<ReduceProcessingParameters>(*n.parameters);
            const auto input=numeric_projection(request.input,request.selected_channels,request.matrix_row);
            const auto s=dsp_kernels::reduction_support({input,get(n.outputs),numeric_reduction(reduction.operation),reduction.precision_loss,reduction.rounding,p.kernels,p.admission.frame},p.capabilities);
            if(s.status!=dsp_kernels::NumericStatus::Ok)return false;
            if(!numeric_add(out.resources.scratch_bytes,s.requirements.scratch_bytes) || !numeric_add(out.resources.plan_bytes,s.requirements.plan_bytes))return false;
            if(!numeric_add(out.resources.stack_bytes,s.requirements.stack_bytes))return false;

        return true;
    }
    bool gather() noexcept {
        const auto request=numeric_gather_request(n,p);NumericGatherRequirements r;
        if(numeric_gather_requirements(request,r)!=Status::ok)return false;
        if(!numeric_add(out.resources.plan_bytes,r.plan_metadata_bytes+sizeof(GatherOwner)) ||
           !numeric_add(out.resources.payload_bytes,r.payload_bytes+r.output_metadata_bytes) || !numeric_add(out.resources.scratch_bytes,r.scratch_bytes))return false;
        return n.operation!=ProcessingOperation::Reduce || reduction();
    }
    bool needs_input_history() const noexcept {
        return !n.inputs.empty() && history(n.inputs) &&
            n.operation!=ProcessingOperation::History && n.operation!=ProcessingOperation::MusicTransform;
    }
    bool needs_gather() const noexcept {
        if(n.operation==ProcessingOperation::History)return get(n.outputs).shape.rank==Rank::Matrix;
        if(n.operation==ProcessingOperation::Reduce)return true;
        if(n.operation==ProcessingOperation::Source || n.operation==ProcessingOperation::Converter)
            return history(n.outputs) && get(n.outputs).shape.rank==Rank::Matrix;
        return n.operation==ProcessingOperation::Sink && history(n.inputs) && get(n.inputs).shape.rank==Rank::Matrix;
    }
    bool histories() noexcept {
        const bool input_history=needs_input_history();
        const bool source_history=n.operation==ProcessingOperation::Source && history(n.outputs);
        if((input_history || source_history) && !history_masks())return false;
        if(source_history && !history_source())return false;
        if(n.operation==ProcessingOperation::History && !history_contract())return false;
        if(n.operation==ProcessingOperation::Converter && !converter())return false;
        return !needs_gather() || gather();
    }
public:
    ResourceAdmission(const ResolvedProcessingNode& node,const NumericRuntimeProfile& profile) noexcept:n(node),p(profile){}
    ProcessingSupport assess() noexcept {
        if(!endpoints() || !music() || (n.kind=="PcmResampler" && !resampler()))return out;
        if(n.operation==ProcessingOperation::Clock){out.supported=true;return out;}
        out.supported=common_storage() && ports() && operations() && histories();
        return out;
    }
};
}
ProcessingSupport numeric_support(const void* context,const ResolvedProcessingNode& node,const ProcessingResourceProfile&) noexcept {
    if(!context)return {};
    return ResourceAdmission(node,*static_cast<const NumericRuntimeProfile*>(context)).assess();
}
Status numeric_kernel_status(dsp_kernels::NumericStatus s) noexcept {
    if(s==dsp_kernels::NumericStatus::Ok)return Status::ok;
    if(s==dsp_kernels::NumericStatus::EmptyPopulation)return Status::unavailable;
    if(s==dsp_kernels::NumericStatus::BufferTooSmall)return Status::buffer_too_small;
    if(s==dsp_kernels::NumericStatus::Overflow)return Status::overflow;
    return Status::invalid_argument;
}
namespace {
bool comparison_parameters_match(const ProcessingParameters& a,const ProcessingParameters& b) noexcept {
    const auto& current=std::get<TransformProcessingParameters>(a).values;
    const auto& prior=std::get<TransformProcessingParameters>(b).values;
    if(current.size()!=prior.size())return false;
    for(std::size_t i=0;i<current.size();++i)if(current[i].key!=prior[i].key || current[i].value!=prior[i].value)return false;
    return true;
}
bool selector_outputs_match(const ConfiguredProcessingNode& a,const ConfiguredProcessingNode& b) noexcept {
    if(a.outputs.size()!=b.outputs.size())return false;
    for(const auto& port:a.outputs){
        const auto* prior=numeric_port(b.outputs,port.id);
        if(!prior || !same_physical_contract(port.contract.view(),prior->contract.view()))return false;
    }
    return true;
}
bool retained_port_matches(const ProcessingPort& port,const ProcessingPort& other,bool fixed_source) noexcept {
    if(port.direction!=other.direction)return false;
    auto current=port.contract.view(),prior=other.contract.view();
    if(!fixed_source){
        // Capacity is retention policy; signal identity remains stable.
        auto* value=std::get_if<NumericContract>(&current);const auto* before=std::get_if<NumericContract>(&prior);
        if(value && before && value->shape.rank==Rank::Matrix && before->shape.rank==Rank::Matrix)value->shape.extents[0]=before->shape.extents[0];
    }
    return same_physical_contract(current,prior);
}
bool retained_direction_matches(const ConfiguredProcessingNode& a,const ConfiguredProcessingNode& b,PortDirection direction) noexcept {
    const auto& current=direction==PortDirection::Input?a.inputs:a.outputs;
    const auto& prior=direction==PortDirection::Input?b.inputs:b.outputs;
    const bool fixed_source=a.kind=="NetworkInput" || a.kind=="AudioSource" || a.kind=="TestSignal";
    for(const auto& port:current){
        const auto* other=numeric_port(prior,port.id);
        if(!other){if(direction==PortDirection::Input)return false;continue;}
        if(!retained_port_matches(port,*other,fixed_source))return false;
    }
    return true;
}
}
bool numeric_compatible_node(const ConfiguredProcessingNode& a,const ConfiguredProcessingNode& b) noexcept {
    if(a.kind!=b.kind)return false;
    if(a.kind=="PcmResampler" && !same_processing_parameters(a.parameters,b.parameters))return false;
    if(a.kind=="Mixer" && std::get<MixerProcessingParameters>(a.parameters).consumption!=std::get<MixerProcessingParameters>(b.parameters).consumption)return false;
    if(a.kind=="ChannelComparison" && !comparison_parameters_match(a.parameters,b.parameters))return false;
    if(a.kind=="InputSelector")return selector_outputs_match(a,b);
    if(a.inputs.size()!=b.inputs.size())return false;
    // Cold preparation replaces immutable coefficients while preserving compatible history.
    return retained_direction_matches(a,b,PortDirection::Input) && retained_direction_matches(a,b,PortDirection::Output);
}
bool numeric_parameter(const ResolvedProcessingNode& n,std::string_view key,double& out) noexcept {
    ProcessingParameterView value;if(resolve_processing_parameter(n,key,value)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&value);if(!literal)return false;
    // Registered DSP sizes/gain are bounded small fields, never payload integers.
    out=std::visit([](auto x){return static_cast<double>(x);},*literal);return std::isfinite(out);
}
bool frequency_axis(const AxisCoordinates& axis,std::uint32_t rate,std::size_t samples,bool nonzero=false) noexcept {
    if(axis.mode==CoordinateMode::ExplicitFloat64LE)return axis.unit==NumericUnit::Hertz;
    if(axis.mode!=CoordinateMode::Regular || axis.unit!=NumericUnit::Hertz || (axis.origin.numerator && (!nonzero || !axis.origin.denominator || double(axis.origin.numerator)/axis.origin.denominator!=double(axis.step.numerator)/axis.step.denominator)) ||
       axis.step.numerator<=0 || !axis.step.denominator || !samples)return false;
    auto numerator=std::uint64_t(axis.step.numerator),denominator=axis.step.denominator;
    const auto divisor=std::gcd(numerator,denominator);numerator/=divisor;denominator/=divisor;
    const auto expected=std::gcd(std::uint64_t(rate),std::uint64_t(samples));
    return numerator==std::uint64_t(rate)/expected && denominator==std::uint64_t(samples)/expected;
}
namespace {
class FloatingAdmission {
    const ResolvedProcessingNode& n;
    ProcessingResources& resources;
    const NumericContract input,output;
    const std::size_t columns,result,planes;
    bool window_coefficients() noexcept {
        std::size_t bytes=0;
        return input.dtype==DType::UInt32 && input.shape.rank==Rank::Scalar && output.dtype==DType::Float32 && dsp_kernels::PreparedWindows::storage_bytes(result,bytes) && numeric_add(resources.plan_bytes,sizeof(dsp_kernels::PreparedWindows)) && numeric_add(resources.payload_bytes,bytes);
    }
    bool representation() const noexcept {
        return input.dtype==DType::Float32 && output.dtype==DType::Float32 && input.representation.encoding==StorageEncoding::Native && input.representation.layout==ChannelLayout::Planar && !input.representation.valid_bits && (input.representation.normalization==Normalization::None || input.representation.normalization==Normalization::FloatUnit);
    }
    bool SpectrumAmplitude() noexcept {
        return columns==result && planes==1;
    }
    bool PcmResampler() noexcept {
        if(!n.enabled)return same_physical_contract(input,output) && numeric_add(resources.payload_bytes,(columns+result)*planes*sizeof(float));
        if(input.shape.rank!=Rank::Vector || !planes || planes>2 || !columns || columns>8192 || result>65536 ||
            output.shape.extents[0].minimum!=1 || !dsp_kernels::PcmResampler::supported(input.clock.sample_rate,output.clock.sample_rate) ||
            result<dsp_kernels::PcmResampler::maximum_output(columns,input.clock.sample_rate,output.clock.sample_rate))return false;
        return numeric_add(resources.payload_bytes,(columns+result)*planes*sizeof(float));
    }
    bool Window() noexcept {
        double parameter=0;
        if(output.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE && input.coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE)return false;
        if(input.shape.rank!=Rank::Vector || !planes || !input.clock.sample_rate || !numeric_parameter(n,"size",parameter) || parameter!=double(result))return false;
        if(result<512 || result>dsp_kernels::max_fft_samples || (result&(result-1)))return false;
        return numeric_add(resources.payload_bytes,result*planes*sizeof(float));
    }
    bool WindowFunction() noexcept {

        ProcessingParameterView value;if(resolve_processing_parameter(n,"function",value)!=Status::ok || !std::holds_alternative<std::string_view>(value))return false;
        const auto function=std::get<std::string_view>(value);
        dsp_kernels::WindowFunction parsed;if(!dsp_kernels::parse_window_function(function,parsed))return false;
        std::size_t bank_bytes=0;
        return columns==result && planes && dsp_kernels::PreparedWindows::storage_bytes(columns,bank_bytes) &&
            numeric_add(resources.plan_bytes,sizeof(dsp_kernels::PreparedWindows)) && numeric_add(resources.payload_bytes,bank_bytes);
    }
    bool StereoFFT() noexcept {

        if(!planes || columns!=result*2 || columns<512 || columns>dsp_kernels::max_fft_samples || (columns&(columns-1)))return false;
        if(output.coordinates[0].mode!=CoordinateMode::None && !frequency_axis(output.coordinates[0],input.clock.sample_rate,columns))return false;
        auto memory=dsp_kernels::estimate_memory(columns,0);if(!memory)return false;
        if(!numeric_add(resources.scratch_bytes,memory->fft_scratch_bytes+memory->shared_radix2_tables_bytes))return false;
        return true;
    }
    bool BarkWeighting() noexcept {
        if(!input.clock.sample_rate || columns!=result || !planes)return false;
        if(input.coordinates[0].mode!=CoordinateMode::None &&
           input.coordinates[0].unit!=NumericUnit::Hertz)return false;
        if(!numeric_add(resources.payload_bytes,columns*(sizeof(float)+sizeof(double))))return false;
        return true;
    }
    bool SpectrumMerge() noexcept {
        double parameter=0;
        if(planes!=2 || columns!=result || !numeric_parameter(n,"merge",parameter))return false;
        return true;
    }
    bool SpectrumGain() noexcept {
        double parameter=0;
        if(planes!=1 || columns!=result || !numeric_parameter(n,"gain",parameter))return false;
        return true;
    }
    bool Mel() noexcept {
        double parameter=0;
        if(output.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE && output.coordinates[0].unit!=NumericUnit::Hertz && output.coordinates[0].unit!=NumericUnit::Values)return false;
        if(planes!=1 || !input.clock.sample_rate || columns>dsp_kernels::max_fft_samples/2 || result>dsp_kernels::max_mel_bands ||
           !numeric_parameter(n,"bands",parameter) || parameter!=double(result))return false;
        if(input.coordinates[0].mode!=CoordinateMode::None &&
           input.coordinates[0].unit!=NumericUnit::Hertz)return false;
        std::size_t weight_cells=0,weight_bytes=0;
        if(!numeric_mul(columns,result,weight_cells) || !numeric_mul(weight_cells,sizeof(double),weight_bytes))return false;
        if(!numeric_add(resources.scratch_bytes,(result+2)*sizeof(double)) || !numeric_add(resources.payload_bytes,(result+2)*sizeof(double)))return false;
        return numeric_add(resources.payload_bytes,(columns+result)*sizeof(float)+columns*sizeof(double)) &&
               numeric_add(resources.payload_bytes,weight_bytes);
    }
    bool signal(NodeKind kind) noexcept {
        switch(kind){
        case NodeKind::PcmResampler:return PcmResampler();
        case NodeKind::Window:return Window();
        case NodeKind::WindowFunction:return WindowFunction();
        case NodeKind::StereoFFT:return StereoFFT();
        default:return false;
        }
    }
    bool spectrum(NodeKind kind) noexcept {
        switch(kind){
        case NodeKind::SpectrumAmplitude:return SpectrumAmplitude();
        case NodeKind::BarkWeighting:return BarkWeighting();
        case NodeKind::SpectrumMerge:return SpectrumMerge();
        case NodeKind::SpectrumGain:return SpectrumGain();
        case NodeKind::Mel:return Mel();
        default:return signal(kind);
        }
    }
public:
    FloatingAdmission(const ResolvedProcessingNode& node,ProcessingResources& reservation) noexcept
        :n(node),resources(reservation),input(std::get<NumericContract>(numeric_port(n.inputs,0)->contract.view())),output(std::get<NumericContract>(numeric_port(n.outputs,0)->contract.view())),columns(std::size_t(input.shape.extents[0].maximum)),result(std::size_t(output.shape.extents[0].maximum)),planes(input.channel_order.size()){}
    bool assess() noexcept {
        if(input.channel_order.size()!=1 || output.channel_order.size()!=1)return false;
        if(n.kind=="WindowFunction")return window_coefficients();
        if(!representation())return false;
        if(n.kind!="PcmResampler" && output.shape.extents[0].minimum!=output.shape.extents[0].maximum)return false;
        const auto kind=node_kind(n.kind);
        return kind && spectrum(*kind);
    }
};
}
bool numeric_float_dsp(const ResolvedProcessingNode& node,ProcessingResources& resources) noexcept {
    return FloatingAdmission(node,resources).assess();
}

}
namespace dataflow_runtime {
using namespace detail;
using namespace domain;
using namespace signal_contracts;
namespace {
ProcessingGeometryLimits runtime_geometry(const void* context,std::string_view kind) noexcept {
    const auto& profile=*static_cast<const NumericRuntimeProfile*>(context);
    const auto capacity=std::min<std::size_t>(dsp_kernels::max_fft_samples,profile.kernels.max_elements);
    const bool raw=kind=="Fourier" || kind=="RealFourier" || kind=="MatrixFFT";
    return {raw?256u:512u,std::uint32_t(capacity),1,768000};
}
}
NumericDeviceCapabilities PreparedGraph::numeric_capabilities(const NumericRuntimeProfile& p) noexcept {
    auto resources=p.resources;
    if(!numeric_add(resources.retained_working_bytes,fixed_plan_reservation) || !numeric_add(resources.retained_working_bytes,p.admission.frame.max_payload_bytes))resources.retained_working_bytes=SIZE_MAX;
    return {&p,numeric_support,resources,runtime_geometry};
}

} // namespace dataflow_runtime
