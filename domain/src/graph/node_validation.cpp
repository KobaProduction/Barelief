#include "node_validation.h"
#include <domain/processing_contracts.h>
#include <domain/signal/contracts.h>
#include <signal_contracts/resampling_policy.h>
#include <signal_contracts/window_policy.h>
#include <algorithm>
#include <cmath>
#include <numeric>
namespace domain::detail {
using namespace signal_contracts;
namespace {
const ProcessingPort* find_port(std::span<const ProcessingPort> ports,PortId id) noexcept {
    for(const auto& p:ports)if(p.id==id)return &p;
    return nullptr;
}
bool clock_equal(const ClockContract& a,const ClockContract& b) noexcept {
    return a.domain==b.domain && a.tick_seconds==b.tick_seconds && a.sample_rate==b.sample_rate;
}
bool policy_structural(const ConversionPolicy& p) noexcept {
    return (p.rounding==Rounding::TowardZero || p.rounding==Rounding::NearestTiesEven) &&
        (p.overflow==Overflow::Reject || p.overflow==Overflow::Saturate) &&
        (p.precision_loss==PrecisionLoss::RejectInexact || p.precision_loss==PrecisionLoss::AllowDeclared) &&
        (p.nonfinite==NonFinite::Reject || p.nonfinite==NonFinite::ReplaceFinite || p.nonfinite==NonFinite::ClampFinite) &&
        (p.scaling.mode==ScaleMode::Identity || p.scaling.mode==ScaleMode::IntegerAffine || p.scaling.mode==ScaleMode::DyadicAffine ||
         p.scaling.mode==ScaleMode::PcmNormalize || p.scaling.mode==ScaleMode::PcmDenormalize || p.scaling.mode==ScaleMode::Binary64Affine) &&
        validate_literal(p.scaling.gain.numerator)==Status::ok && validate_literal(p.scaling.offset.numerator)==Status::ok &&
        validate_literal(p.nan_replacement)==Status::ok && validate_literal(p.negative_inf_replacement)==Status::ok && validate_literal(p.positive_inf_replacement)==Status::ok;
}
bool integer_parameter_valid(const ProcessingParameterValue& value,const ParameterDescriptor& d) noexcept {
    const auto* numeric=std::get_if<NumericLiteral>(&value);if(!numeric)return false;
    if(const auto* v=std::get_if<std::uint64_t>(numeric))return *v>=std::uint64_t(d.minimum) && *v<=std::uint64_t(d.maximum);
    if(const auto* v=std::get_if<std::int64_t>(numeric))return *v>=std::int64_t(d.minimum) && *v<=std::int64_t(d.maximum);
    return false;
}
bool number_parameter_valid(const ProcessingParameterValue& value,const ParameterDescriptor& d) noexcept {
    const auto* numeric=std::get_if<NumericLiteral>(&value);if(!numeric || validate_literal(*numeric)!=Status::ok)return false;
        // Compare integer literals in their own type, never narrowing through double.
        return std::visit([&](auto v){using T=decltype(v);
            if constexpr(std::is_floating_point_v<T>)return std::isfinite(v) && v>=d.minimum && v<=d.maximum;
            else if constexpr(std::is_signed_v<T>)return static_cast<long double>(v)>=d.minimum && static_cast<long double>(v)<=d.maximum;
            else return static_cast<long double>(v)>=d.minimum && static_cast<long double>(v)<=d.maximum;
        },*numeric);
}
bool array_parameter_valid(const ProcessingParameterValue& value,const ParameterDescriptor& d) noexcept {
    const auto* array=std::get_if<NumericArray>(&value);
    return array && array->values.size()<=max_numeric_array_values && (d.maximum<=0 || array->values.size()<=std::size_t(d.maximum)) && std::all_of(array->values.begin(),array->values.end(),[](float v){return std::isfinite(v);});
}
bool text_parameter_valid(const ProcessingParameterValue& value,const ParameterDescriptor& d) noexcept {
    const auto* text=std::get_if<std::string>(&value);if(!text)return false;
    if(d.value_kind==ParameterValueKind::String)return (!text->empty() || d.default_text.empty()) && text->find('\0')==std::string::npos;
    signal_contracts::WindowFunction parsed;return signal_contracts::parse_window_function(*text,parsed);
}
bool parameter_value_valid(const ProcessingParameterValue& value,const ParameterDescriptor& d) noexcept {
    switch(d.value_kind) {
    case ParameterValueKind::NumericArray:return array_parameter_valid(value,d);
    case ParameterValueKind::Boolean:return std::holds_alternative<bool>(value);
    case ParameterValueKind::String:case ParameterValueKind::WindowFunction:return text_parameter_valid(value,d);
    case ParameterValueKind::Integer:return integer_parameter_valid(value,d);
    case ParameterValueKind::Number:return number_parameter_valid(value,d);
    default:return false;
    }
}
bool single_parameter_valid(NodeKind kind,const ProcessingParameter& parameter,const ProcessingAdmissionLimits& limits) noexcept {
    if(parameter.key.size()>limits.max_text_bytes || parameter.key=="enabled")return false;
    const auto* descriptor=parameter_descriptor(kind,parameter.key);
    if(!descriptor || descriptor->classification!=ParameterClassification::Processing || !parameter_value_valid(parameter.value,*descriptor))return false;
    const auto* text=std::get_if<std::string>(&parameter.value);
    return !text || text->size()<=limits.max_text_bytes;
}
bool parameter_fields_valid(NodeKind k,const std::vector<ProcessingParameter>& values,const ProcessingAdmissionLimits& limits) noexcept {
    if(values.size()>limits.max_parameters_per_node)return false;
    for(std::size_t i=0;i<values.size();++i){const auto& p=values[i];
        if(!single_parameter_valid(k,p,limits))return false;
        for(std::size_t j=0;j<i;++j)if(p.key==values[j].key)return false;
    }return true;
}
bool wled_contract(const DataContract& c) noexcept {
    const auto* s=std::get_if<StructuredContract>(&c);
    return s && s->schema=="wled-audio-sync" && s->version==1 && s->encoding=="audio-sync-44-le" && s->payload_bytes==ExtentBounds{44,44};
}
bool test_signal_rules(const ConfiguredProcessingNode& node) noexcept {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Source,ProcessingOperation::Source,
        node.inputs,node.outputs,&node.parameters,node.enabled};
    const auto number=[&](std::string_view key,double& out){
        ProcessingParameterView field;
        if(resolve_processing_parameter(resolved,key,field)!=Status::ok)return false;
        const auto* value=std::get_if<NumericLiteral>(&field);if(!value)return false;
        // Per-field validation already bounded these values to <=192000 and finite.
        // Integer values in that range are exactly representable; the stored tag/bits remain intact.
        out=std::visit([](auto x){return static_cast<double>(x);},*value);return true;
    };
    double minimum=0,maximum=0,rate=0,sweep=0;
    return number("minimum_hz",minimum) && number("maximum_hz",maximum) &&
        number("sample_rate",rate) && number("sweep_depth",sweep) &&
        minimum<maximum && maximum*(1+sweep)<rate/2;
}
ProcessingDiagnostic validate_arrayfill(const OperationValidation& context) noexcept {
    const auto& [configured,limits,n,k,role,op,input_count,output_count,in,result,input,output]=context;
 {
        const auto* p=std::get_if<TransformProcessingParameters>(&n.parameters);OwnedProcessingContract expected;
        if(!p || !parameter_fields_valid(*k,p->values,limits) || !input || !output || array_generator_output_contract(configured,*input,expected)!=Status::ok || !same_physical_contract(expected.view(),result))return context.error(ProcessingError::InvalidContract);
        return {};
    }
}
ProcessingDiagnostic validate_arrayproduct(const OperationValidation& context) noexcept {
    const auto& [configured,limits,n,k,role,op,input_count,output_count,in,result,input,output]=context;
 {
        const auto* p=std::get_if<TransformProcessingParameters>(&n.parameters);OwnedProcessingContract expected;
        if(!p || !parameter_fields_valid(*k,p->values,limits) || !output || array_product_output_contract(configured,expected)!=Status::ok)return context.error(ProcessingError::InvalidContract);
        if(!same_physical_contract(expected.view(),result))return context.error(ProcessingError::IncompatiblePorts);
        return {};
    }
}
ProcessingDiagnostic validate_arrayconcat(const OperationValidation& context) noexcept {
    const auto& [configured,limits,n,k,role,op,input_count,output_count,in,result,input,output]=context;
 {
        const auto* p=std::get_if<TransformProcessingParameters>(&n.parameters);OwnedProcessingContract expected;
        if(!p || !parameter_fields_valid(*k,p->values,limits) || array_concat_output_contract(configured,expected)!=Status::ok || !same_physical_contract(expected.view(),result))return context.error(ProcessingError::IncompatiblePorts);
        return {};
    }
}
bool transform_parameters_valid(const OperationValidation& context) noexcept {
    const auto* parameters=std::get_if<TransformProcessingParameters>(&context.node.parameters);
    return parameters && parameter_fields_valid(*context.kind,parameters->values,context.limits);
}
bool array_length_axis_valid(const OperationValidation& context) noexcept {
    if(context.kind!=NodeKind::ArrayLength)return true;
    ProcessingParameterView field;
    if(resolve_processing_parameter(context.resolved(),"axis",field)!=Status::ok)return false;
    const auto axis=std::visit([](auto value){return std::uint32_t(value);},std::get<NumericLiteral>(field));
    return axis==0 || context.input->shape.rank==Rank::Matrix;
}
Status metadata_contract(const OperationValidation& context,OwnedProcessingContract& expected) noexcept {
    if(context.kind==NodeKind::FrequencyStep)return frequency_step_output_contract(*context.input,expected);
    if(context.kind==NodeKind::WindowGain)return window_gain_output_contract(*context.input,expected);
    return numeric_metadata_output_contract(*context.input,context.kind==NodeKind::SampleRate,expected);
}
ProcessingDiagnostic validate_metadata(const OperationValidation& context) noexcept {
    if(!context.input || !transform_parameters_valid(context) || !array_length_axis_valid(context))return context.error(ProcessingError::InvalidParameters);
    OwnedProcessingContract expected;
    if(metadata_contract(context,expected)!=Status::ok || !same_physical_contract(expected.view(),context.node.outputs[0].contract.view()))return context.error(ProcessingError::InvalidContract);
    return {};
}
ProcessingDiagnostic validate_complex_pair(const OperationValidation& context) noexcept {
    auto second_data=context.node.inputs[1].contract.view();auto* second=std::get_if<NumericContract>(&second_data);
    if(!second)return context.error(ProcessingError::InvalidContract,1);
    second->quantity=context.input->quantity;second->semantic=context.input->semantic;
    if(!same_physical_contract(*context.input,*second))return context.error(ProcessingError::IncompatiblePorts,1);
    return {};
}
ProcessingDiagnostic expected_outputs(const OperationValidation& context,std::span<const OwnedProcessingContract> expected) noexcept {
    for(std::size_t output=0;output<context.node.outputs.size();++output)
        if(!same_physical_contract(expected[output].view(),context.node.outputs[output].contract.view()))return context.error(ProcessingError::IncompatiblePorts,PortId(output));
    return {};
}
ProcessingDiagnostic validate_complexpolar(const OperationValidation& context) noexcept {
    if(!context.input || !transform_parameters_valid(context))return context.error(ProcessingError::InvalidParameters);
    const auto pair=validate_complex_pair(context);if(!pair)return pair;
    std::array<OwnedProcessingContract,2> expected;
    const auto status=context.kind==NodeKind::AmplitudePhase?amplitude_phase_output_contracts(context.configured,*context.input,expected):complex_polar_output_contracts(*context.input,expected);
    if(status!=Status::ok)return context.error(ProcessingError::InvalidContract);
    return expected_outputs(context,expected);
}
ProcessingDiagnostic validate_fourier(const OperationValidation& context) noexcept {
    if(!context.input || !transform_parameters_valid(context))return context.error(ProcessingError::InvalidParameters);
    if(context.input_count==2 && !same_physical_contract(context.node.inputs[0].contract.view(),context.node.inputs[1].contract.view()))return context.error(ProcessingError::IncompatiblePorts,1);
    std::array<OwnedProcessingContract,4> expected;
    if(fourier_output_contracts(context.configured,*context.input,expected)!=Status::ok)return context.error(ProcessingError::InvalidContract);
    return expected_outputs(context,expected);
}
ProcessingDiagnostic validate_clock(const OperationValidation& context) noexcept {
    const auto& [configured,limits,n,k,role,op,input_count,output_count,in,result,input,output]=context;
 {
        const auto* p=std::get_if<SampleClockProcessingParameters>(&n.parameters);
        if(!p || p->sample_rate<min_sample_clock_rate || p->sample_rate>max_sample_clock_rate || p->domain.empty() || p->domain.find('\0')!=std::string::npos || n.clock_node!=-1)return context.error(ProcessingError::InvalidParameters);
        return {};
    }
}
bool mixer_parameters_valid(const MixerProcessingParameters& parameters,std::size_t inputs) noexcept {
    return (parameters.consumption==MixerConsumption::ClockedFifo || parameters.consumption==MixerConsumption::SourceAligned) && parameters.weights.size()==inputs && parameters.fifo_milliseconds>=min_mixer_fifo_ms && parameters.fifo_milliseconds<=max_mixer_fifo_ms &&
        (parameters.mode==MixerMode::WeightedSum || parameters.mode==MixerMode::WeightedMean || parameters.mode==MixerMode::WeightedRMS || parameters.mode==MixerMode::MaxAbsolute);
}
bool mixer_spectral(const NumericContract& input) noexcept {
    return input.shape.axes[0]==Axis::Bin || input.shape.axes[0]==Axis::Band || input.coordinates[0].unit==NumericUnit::Hertz;
}
bool mixer_geometry(const NumericContract& input,const NumericContract& lane) noexcept {
    return lane.dtype==input.dtype && lane.shape.rank==Rank::Vector && lane.shape.axes==input.shape.axes && lane.representation==input.representation && clock_equal(lane.clock,input.clock) && lane.coordinates==input.coordinates && (!mixer_spectral(input) || lane.shape==input.shape);
}
bool mixer_weight_valid(const MixerProcessingParameters& parameters,PortId port) noexcept {
    std::size_t found=0;
    const bool nonnegative=mixer_requires_nonnegative_weights(parameters.mode);
    for(const auto& weight:parameters.weights)if(weight.port==port) {
        ++found;if(!std::isfinite(weight.weight) || (nonnegative && weight.weight<0))return false;
    }
    return found==1;
}
ProcessingDiagnostic mixer_primary_contract(const OperationValidation& context,const MixerProcessingParameters& parameters) noexcept {
    const auto* input=context.input;const auto* output=context.output;
    if(!input || !output || input->dtype!=DType::Float32 || input->shape.rank!=Rank::Vector || input->channel_order.size()!=1 || output->channel_order.size()!=1 || input->representation.encoding!=StorageEncoding::Native || output->representation.encoding!=StorageEncoding::Native)return context.error(ProcessingError::InvalidContract);
    const bool spectral=mixer_spectral(*input);
    if(parameters.consumption==MixerConsumption::SourceAligned && (spectral || input->shape.axes[0]!=Axis::Sample || !input->clock.sample_rate || input->clock.domain.empty()))return context.error(ProcessingError::InvalidContract);
    if(spectral && input->shape.extents[0].minimum!=input->shape.extents[0].maximum)return context.error(ProcessingError::InvalidContract);
    return {};
}
ProcessingDiagnostic mixer_input_contracts(const OperationValidation& context,const MixerProcessingParameters& parameters) noexcept {
    for(const auto& port:context.node.inputs) {
        const auto contract=port.contract.view();const auto* input=std::get_if<NumericContract>(&contract);
        if(!input || input->channel_order.size()!=1 || !mixer_geometry(*context.input,*input))return context.error(ProcessingError::IncompatiblePorts,port.id);
        if(!mixer_weight_valid(parameters,port.id))return context.error(ProcessingError::InvalidParameters,port.id);
    }
    return {};
}
ProcessingDiagnostic validate_mixer(const OperationValidation& context) noexcept {
    const auto* parameters=std::get_if<MixerProcessingParameters>(&context.node.parameters);
    if(!parameters || !mixer_parameters_valid(*parameters,context.input_count))return context.error(ProcessingError::InvalidParameters);
    auto valid=mixer_primary_contract(context,*parameters);if(!valid)return valid;
    valid=mixer_input_contracts(context,*parameters);if(!valid)return valid;
    return mixer_geometry(*context.input,*context.output)?ProcessingDiagnostic{}:context.error(ProcessingError::IncompatiblePorts);
}
ProcessingDiagnostic validate_channelcomparison(const OperationValidation& context) noexcept {
    const auto& [configured,limits,n,k,role,op,input_count,output_count,in,result,input,output]=context;
 {
        const auto* p=std::get_if<TransformProcessingParameters>(&n.parameters);
        if(!p || !parameter_fields_valid(*k,p->values,limits))return context.error(ProcessingError::InvalidParameters);
        if(!input || !output || !input->clock.sample_rate || input->clock.domain.empty() || input->dtype!=DType::Float32 || input->shape.rank!=Rank::Vector || input->shape.axes[0]!=Axis::Sample || input->representation.encoding!=StorageEncoding::Native || output->dtype!=DType::Float32 || output->shape.rank!=Rank::Scalar || output->representation.encoding!=StorageEncoding::Native || !clock_equal(input->clock,output->clock))return context.error(ProcessingError::InvalidContract);
        auto second=n.inputs[1].contract.view();const auto* lane=std::get_if<NumericContract>(&second);
        if(!lane || lane->dtype!=input->dtype || lane->shape.rank!=Rank::Vector || lane->shape.axes!=input->shape.axes || lane->representation!=input->representation || !clock_equal(lane->clock,input->clock))return context.error(ProcessingError::IncompatiblePorts,1);
        return {};
    }
}
bool valid_generator_output(const NumericContract* output) {
    return output && output->dtype==DType::Float32 && output->shape.rank==Rank::Vector && output->channel_order.size()==1 &&
        output->representation.encoding==StorageEncoding::Native && output->clock.sample_rate && output->shape.axes[0]==Axis::Sample && output->coordinates[0].mode==CoordinateMode::None;
}
bool valid_oscillator(const OscillatorParameters& value,std::uint32_t rate) {
    return value.id && std::isfinite(value.frequency_hz) && value.frequency_hz>0 && value.frequency_hz<double(rate)/2 &&
        std::isfinite(value.amplitude) && std::isfinite(value.phase_radians) && std::isfinite(value.attack_seconds) && std::isfinite(value.release_seconds) && value.attack_seconds>=0 && value.release_seconds>=0;
}
ProcessingDiagnostic validate_generator(const OperationValidation& context) {
    const auto* parameters=std::get_if<SignalGeneratorProcessingParameters>(&context.node.parameters);
    if(!parameters || !valid_generator_output(context.output) || parameters->oscillators.empty() || parameters->oscillators.size()>signal_contracts::max_oscillators)return context.error(ProcessingError::InvalidParameters);
    for(std::size_t i=0;i<parameters->oscillators.size();++i) {
        const auto& oscillator=parameters->oscillators[i];
        if(!valid_oscillator(oscillator,context.output->clock.sample_rate))return context.error(ProcessingError::InvalidParameters);
        for(std::size_t j=0;j<i;++j)if(oscillator.id==parameters->oscillators[j].id)return context.error(ProcessingError::InvalidParameters);
    }
    return {};
}
ProcessingDiagnostic validate_numeric_constant(const OperationValidation& context) {
    const auto* output=context.output;
    if(!output || output->shape.rank!=Rank::Scalar || output->channel_order.size()!=1 || output->representation.encoding!=StorageEncoding::Native || output->representation.normalization!=Normalization::None)return context.error(ProcessingError::InvalidContract);
    if(context.kind==NodeKind::Number)return {};
    const auto unit=context.kind==NodeKind::Fs?NumericUnit::Hertz:NumericUnit::Values;
    return output->dtype==DType::UInt32 && output->unit==unit?ProcessingDiagnostic{}:context.error(ProcessingError::InvalidContract);
}
Rank view_rank(NodeKind kind) noexcept {
    switch(kind) {
    case NodeKind::ScalarView:case NodeKind::ScalarMetricsView:case NodeKind::ScalarSignedBarView:return Rank::Scalar;
    case NodeKind::SpectrogramView:case NodeKind::MatrixImageView:case NodeKind::MatrixSurfaceView:
    case NodeKind::AudioImageView:case NodeKind::AudioSurfaceView:return Rank::Matrix;
    default:return Rank::Vector;
    }
}
bool audio_vector_view(NodeKind kind) noexcept {
    return kind==NodeKind::AudioLineView || kind==NodeKind::AudioPointView
        || kind==NodeKind::AudioBarView || kind==NodeKind::AudioPieView;
}
ProcessingDiagnostic validate_view_port(const OperationValidation& context,const ProcessingPort& port,NodeKind kind,Rank rank) {
        const auto data=port.contract.view();const auto* numeric=std::get_if<NumericContract>(&data);
        if(!numeric)return context.error(ProcessingError::InvalidContract,port.id);
        const bool scalar_frequency=(kind==NodeKind::FFTView
            || (audio_vector_view(kind) && port.id==frequency_step_input))
            && numeric->shape.rank==Rank::Scalar && numeric->channel_order.size()==1;
        if(numeric->shape.rank!=rank && !scalar_frequency)return context.error(ProcessingError::InvalidContract,port.id);
        if(rank==Rank::Scalar && numeric->channel_order.size()!=1)return context.error(ProcessingError::InvalidContract,port.id);
    return {};
}
ProcessingDiagnostic validate_view_inputs(const OperationValidation& context) {
    if(!context.kind)return context.error(ProcessingError::UnknownKind);
    const auto kind=*context.kind;
    const auto rank=view_rank(kind);
    for(const auto& port:context.node.inputs) {
        const auto status=validate_view_port(context,port,kind,rank);if(!status)return status;
    }
    return {};
}
ProcessingDiagnostic validate_capture_lanes(const OperationValidation& context) {
    for(const auto& port:context.node.outputs) {
        const auto data=port.contract.view();const auto* lane=std::get_if<NumericContract>(&data);
        if(!lane || lane->shape.rank!=Rank::Vector || !context.output || !clock_equal(lane->clock,context.output->clock))return context.error(ProcessingError::InvalidContract,port.id);
    }
    return {};
}
ProcessingDiagnostic validate_declared_source_rate(const OperationValidation& context) {
    if(!context.output || !context.output->clock.sample_rate)return {};
    const bool auxiliary=context.configured.outputs.size()!=context.node.outputs.size();
    if(context.kind!=NodeKind::TestSignal && (!auxiliary || (context.kind!=NodeKind::AudioSource && context.kind!=NodeKind::NetworkInput)))return {};
    ProcessingParameterView value;
    if(resolve_processing_parameter(context.resolved(),"sample_rate",value)!=Status::ok)return context.error(ProcessingError::InvalidParameters);
    const auto* literal=std::get_if<NumericLiteral>(&value);
    if(!literal || std::visit([](auto v){return double(v);},*literal)!=context.output->clock.sample_rate)return context.error(ProcessingError::IncompatiblePorts);
    return {};
}
ProcessingDiagnostic validate_endpoint_contract(const OperationValidation& context) noexcept {
    if(numeric_constant_kind(context.node.kind))return validate_numeric_constant(context);
    if(context.kind==NodeKind::TestSignal && (!context.output || !test_signal_rules(context.configured)))return context.error(ProcessingError::InvalidParameters);
    if(context.kind==NodeKind::WLEDOutput && !wled_contract(context.incoming))return context.error(ProcessingError::InvalidContract);
    if(context.kind==NodeKind::AudioSource){const auto result=validate_capture_lanes(context);if(!result)return result;}
    if(is_view(*context.kind))return validate_view_inputs(context);
    return validate_declared_source_rate(context);
}
ProcessingDiagnostic validate_source(const OperationValidation& context) noexcept {
    if(context.kind==NodeKind::SignalGenerator)return validate_generator(context);
    const auto* parameters=std::get_if<EndpointProcessingParameters>(&context.node.parameters);
    if(!parameters || !context.kind || !parameter_fields_valid(*context.kind,parameters->values,context.limits))return context.error(ProcessingError::InvalidParameters);
    return validate_endpoint_contract(context);
}
struct SelectorSelection {std::uint64_t selected=0,channels=1;};
SelectorSelection selector_selection(const TransformProcessingParameters& parameters) noexcept {
    SelectorSelection result;
    for(const auto& field:parameters.values) {
        if(field.key=="selected_input")result.selected=std::visit([](auto v){return std::uint64_t(v);},std::get<NumericLiteral>(field.value));
        if(field.key=="channels_per_stream")result.channels=std::visit([](auto v){return std::uint64_t(v);},std::get<NumericLiteral>(field.value));
    }
    return result;
}
ProcessingDiagnostic validate_selector_group(const OperationValidation& context,SelectorSelection selection) noexcept {
    if(selection.channels!=context.output_count || !context.output_count)return context.error(ProcessingError::InvalidParameters);
    if(selection.channels==1)return selection.selected<=65535 && find_port(context.node.inputs,PortId(selection.selected))?ProcessingDiagnostic{}:context.error(ProcessingError::InvalidParameters);
    if(context.input_count%selection.channels || selection.selected>=context.input_count/selection.channels)return context.error(ProcessingError::InvalidParameters);
    for(std::size_t port=0;port<context.input_count;++port)if(!find_port(context.node.inputs,PortId(port)))return context.error(ProcessingError::InvalidPort);
    return {};
}
bool selector_lane_compatible(const NumericContract& lane,const NumericContract& output,bool active) noexcept {
    auto normalized=lane;
    if(!active)normalized.clock=output.clock;
    // Sample chunk sizes can differ; every other physical requirement remains explicit.
    if(output.shape.rank==Rank::Vector && output.shape.axes[0]==Axis::Sample && output.clock.sample_rate) {
        if(lane.shape.extents[0].minimum<output.shape.extents[0].minimum || lane.shape.extents[0].maximum>output.shape.extents[0].maximum)return false;
        normalized.shape.extents=output.shape.extents;
    }
    return same_physical_contract(normalized,output);
}
ProcessingDiagnostic validate_selector_lane(const OperationValidation& context,const ProcessingPort& port,SelectorSelection selection) noexcept {
    const auto* destination=find_port(context.node.outputs,PortId(selection.channels==1?0:port.id%selection.channels));
    if(!destination)return context.error(ProcessingError::InvalidPort,port.id);
    const auto target=destination->contract.view(),value=port.contract.view();
    const auto* output=std::get_if<NumericContract>(&target);const auto* lane=std::get_if<NumericContract>(&value);
    if(!output || !lane)return context.error(ProcessingError::IncompatiblePorts,port.id);
    const bool active=selection.channels==1?port.id==selection.selected:port.id/selection.channels==selection.selected;
    return selector_lane_compatible(*lane,*output,active)?ProcessingDiagnostic{}:context.error(ProcessingError::IncompatiblePorts,port.id);
}
ProcessingDiagnostic validate_inputselector(const OperationValidation& context) noexcept {
    if(!transform_parameters_valid(context))return context.error(ProcessingError::InvalidParameters);
    if(!context.input || !context.output)return context.error(ProcessingError::InvalidContract);
    const auto selection=selector_selection(std::get<TransformProcessingParameters>(context.node.parameters));
    const auto group=validate_selector_group(context,selection);if(!group)return group;
    for(const auto& port:context.node.inputs) {
        const auto valid=validate_selector_lane(context,port,selection);if(!valid)return valid;
    }
    return {};
}
ProcessingDiagnostic validate_splitter(const OperationValidation& context) noexcept {
    const auto& [configured,limits,n,k,role,op,input_count,output_count,in,result,input,output]=context;

        for(const auto& p:n.inputs)if(!same_physical_contract(in,p.contract.view()))return context.error(ProcessingError::IncompatiblePorts,p.id);
        for(const auto& p:n.outputs)if(!same_physical_contract(in,p.contract.view()))return context.error(ProcessingError::IncompatiblePorts,p.id);
        return {};

    return {};
}
ProcessingDiagnostic validate_converter(const OperationValidation& context) noexcept {
    const auto& [configured,limits,n,k,role,op,input_count,output_count,in,result,input,output]=context;
 {
        const auto* p=std::get_if<ConverterProcessingParameters>(&n.parameters);
        if(!p || !policy_structural(p->policy))return context.error(ProcessingError::InvalidParameters);
        if(!input || !output)return context.error(ProcessingError::InvalidContract);
        auto expected=*input;expected.semantic=output->semantic;expected.history_element=output->history_element;expected.dtype=output->dtype;expected.representation=output->representation;
        if(p->policy.scaling.mode==ScaleMode::PcmNormalize || p->policy.scaling.mode==ScaleMode::PcmDenormalize)expected.unit=output->unit;
        if(!same_physical_contract(expected,*output))return context.error(ProcessingError::IncompatiblePorts);
        return {};
    }
}
std::uint64_t unsigned_parameter(const OperationValidation& context,const char* key) noexcept {
    ProcessingParameterView value;resolve_processing_parameter(context.resolved(),key,value);
    return std::visit([](auto v){return std::uint64_t(v);},std::get<NumericLiteral>(value));
}
ProcessingDiagnostic validate_extract(const OperationValidation& context) noexcept {
    const auto& [configured,limits,n,k,role,op,input_count,output_count,in,result,input,output]=context;
 {
        const auto* p=std::get_if<TransformProcessingParameters>(&n.parameters);
        if(!p || !parameter_fields_valid(*k,p->values,limits) || !input || !output)return context.error(ProcessingError::InvalidParameters);
        const auto channel=unsigned_parameter(context,"channel_index"),row=unsigned_parameter(context,"index"),column=unsigned_parameter(context,"column");
        if(input->shape.rank==Rank::Scalar || channel>=input->channel_order.size() || row>=input->shape.extents[0].maximum ||
            (input->shape.rank==Rank::Vector?column!=0:column>=input->shape.extents[1].maximum))return context.error(ProcessingError::InvalidParameters);
        auto expected=*input;expected.shape={};expected.coordinates={};expected.channel_order=input->channel_order.subspan(std::size_t(channel),1);
        if(!same_physical_contract(expected,*output))return context.error(ProcessingError::IncompatiblePorts);
        return {};
    }
}
bool reduction_policy_valid(const ReduceProcessingParameters* p) noexcept {
        return p && (p->operation==NumericReduction::MaxAbs || p->operation==NumericReduction::SignedMean || p->operation==NumericReduction::RMS) &&
            (p->rows==ReductionRows::WholeValue || p->rows==ReductionRows::LatestHistoryRow) &&
            (p->channels==ReductionChannels::AllChannels || p->channels==ReductionChannels::BothChannels || p->channels==ReductionChannels::SelectedChannels) &&
            (p->rounding==Rounding::TowardZero || p->rounding==Rounding::NearestTiesEven) &&
            (p->precision_loss==PrecisionLoss::RejectInexact || p->precision_loss==PrecisionLoss::AllowDeclared);
}
bool reduction_selected_ids(const NumericContract& input,std::span<const std::uint32_t> selected) noexcept {
    for(std::size_t i=0;i<selected.size();++i) {
        const auto channel=selected[i];
        if(std::find(input.channel_order.begin(),input.channel_order.end(),channel)==input.channel_order.end())return false;
        for(std::size_t j=0;j<i;++j)if(channel==selected[j])return false;
    }
    return true;
}
ProcessingDiagnostic reduction_channel_selection(const OperationValidation& context,const ReduceProcessingParameters& parameters) noexcept {
    const auto* p=&parameters;const auto* input=context.input;
    if(p->rows==ReductionRows::LatestHistoryRow && input->shape.rank!=Rank::Matrix)return context.error(ProcessingError::InvalidParameters);
    if(p->channels==ReductionChannels::BothChannels && input->channel_order.size()!=2)return context.error(ProcessingError::InvalidParameters);
    if(p->selected_channels.size()>context.limits.frame.max_planes || (p->channels==ReductionChannels::SelectedChannels?p->selected_channels.empty():!p->selected_channels.empty()))return context.error(ProcessingError::InvalidParameters);
    return reduction_selected_ids(*input,p->selected_channels)?ProcessingDiagnostic{}:context.error(ProcessingError::InvalidParameters);
}
ProcessingDiagnostic validate_reduce(const OperationValidation& context) noexcept {
        const auto* p=std::get_if<ReduceProcessingParameters>(&context.node.parameters);
        if(!reduction_policy_valid(p))return context.error(ProcessingError::InvalidParameters);
        const auto* input=context.input;const auto* output=context.output;
        if(!input || !output || output->shape.rank!=Rank::Scalar || output->channel_order.size()!=1 ||
            output->quantity!=input->quantity || output->unit!=input->unit || !clock_equal(output->clock,input->clock))return context.error(ProcessingError::InvalidContract);
        const auto selected=reduction_channel_selection(context,*p);if(!selected)return selected;
        if(p->operation!=NumericReduction::MaxAbs && output->dtype!=DType::Float32 && output->dtype!=DType::Float64)return context.error(ProcessingError::InvalidContract);
        return {};
}
NumericContract retained_history_contract(const NumericContract& input,const NumericContract& output,std::size_t capacity) {
    auto expected=input;expected.semantic=Semantic::History;expected.history_element=input.semantic;
    expected.shape.rank=Rank::Matrix;expected.shape.axes={Axis::Time,input.shape.axes[0]};
    expected.shape.extents={ExtentBounds{0,capacity},input.shape.extents[0]};expected.coordinates={output.coordinates[0],input.coordinates[0]};
    return expected;
}

NumericContract retained_number_history_contract(const NumericContract& input,const NumericContract& output,std::size_t capacity) {
    auto expected=input;expected.semantic=Semantic::History;expected.history_element=input.semantic;
    expected.shape={Rank::Vector,{Axis::Time,Axis::None},{ExtentBounds{0,capacity},ExtentBounds{}}};
    expected.coordinates={output.coordinates[0],AxisCoordinates{}};
    return expected;
}
ProcessingDiagnostic validate_history_count(const OperationValidation& context) noexcept {
    auto count_value=find_port(context.node.outputs,filled_count_output)->contract.view();const auto* count=std::get_if<NumericContract>(&count_value);
    if(!count || count->dtype!=DType::UInt64 || count->shape.rank!=Rank::Scalar || count->quantity!=Quantity::FilledFrameCount || count->unit!=NumericUnit::Frames || count->channel_order.size()!=1 || count->representation.normalization!=Normalization::None || !clock_equal(count->clock,context.input->clock))return context.error(ProcessingError::InvalidContract,filled_count_output);
    return {};
}
ProcessingDiagnostic validate_history_geometry(const OperationValidation& context,bool scalar) noexcept {
    const auto* input=context.input;const auto* output=context.output;
    if(input->shape.rank!=(scalar?Rank::Scalar:Rank::Vector) ||
       output->shape.rank!=(scalar?Rank::Vector:Rank::Matrix))return context.error(ProcessingError::InvalidContract);
    if(!scalar && input->shape.extents[0].minimum!=input->shape.extents[0].maximum)
        return context.error(ProcessingError::InvalidContract);
    if(scalar && (output->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE ||
       (output->coordinates[0].unit!=NumericUnit::Seconds && output->coordinates[0].unit!=NumericUnit::Frames)))
        return context.error(ProcessingError::InvalidContract);
    if(!scalar && output->coordinates[0].mode==CoordinateMode::Regular &&
       output->coordinates[0].unit==NumericUnit::Seconds)
        return context.error(ProcessingError::InvalidContract);
    return {};
}
ProcessingDiagnostic validate_history(const OperationValidation& context) noexcept {
    const auto& [configured,limits,n,k,role,op,input_count,output_count,in,result,input,output]=context;
    const auto* p=std::get_if<HistoryProcessingParameters>(&n.parameters);
    if(!p || !p->capacity)return context.error(ProcessingError::InvalidParameters);
    if(!input || !output)return context.error(ProcessingError::InvalidContract);
    const bool scalar=n.kind=="NumberAccumulator";
    const auto geometry=validate_history_geometry(context,scalar);if(!geometry)return geometry;
    const auto expected=scalar?retained_number_history_contract(*input,*output,p->capacity):
                               retained_history_contract(*input,*output,p->capacity);
    if(!same_physical_contract(expected,*output))return context.error(ProcessingError::IncompatiblePorts);
    return validate_history_count(context);
}
bool wled_scalar_contract(const NumericContract& mel,const DataContract& data) noexcept {
    const auto* scalar=std::get_if<NumericContract>(&data);
    return scalar && (scalar->dtype==DType::Float32 || scalar->dtype==DType::Float64) &&
        scalar->shape.rank==Rank::Scalar && scalar->channel_order.size()==1 &&
        scalar->representation.encoding==StorageEncoding::Native &&
        scalar->representation.normalization==Normalization::None && clock_equal(mel.clock,scalar->clock);
}
ProcessingDiagnostic validate_spectrumpeak(const OperationValidation& context) noexcept {
    if(!context.input || !transform_parameters_valid(context))return context.error(ProcessingError::InvalidParameters);
    std::array<OwnedProcessingContract,2> expected;
    if(spectrum_peak_output_contracts(*context.input,expected)!=Status::ok)return context.error(ProcessingError::InvalidContract);
    return expected_outputs(context,expected);
}

ProcessingDiagnostic validate_wledencoder(const OperationValidation& context) noexcept {
    if(!transform_parameters_valid(context))return context.error(ProcessingError::InvalidParameters);
    if(!context.input || !wled_contract(context.outgoing))return context.error(ProcessingError::InvalidContract);
    for(PortId id=1;id<=2;++id){
        const auto* port=find_port(context.node.inputs,id);
        if(!port || !wled_scalar_contract(*context.input,port->contract.view()))return context.error(ProcessingError::IncompatiblePorts,id);
    }
    return {};
}
bool contextual_row_in_range(const ConfiguredProcessingNode& node,std::size_t row) {
    const auto bounds=processing_parameter_numeric_bounds(node,"row");
    return bounds && row>=bounds->minimum && row<=bounds->maximum;
}
Status expected_matrix_contract(const OperationValidation& context,std::size_t number,OwnedProcessingContract& expected) noexcept {
    switch(context.operation) {
    case ProcessingOperation::MatrixFFT:return matrix_fft_output_contract(*context.input,number,expected);
    case ProcessingOperation::MatrixTranspose:return matrix_transpose_output_contract(*context.input,expected);
    case ProcessingOperation::MatrixWindowFunction:return matrix_window_output_contract(*context.input,expected);
    case ProcessingOperation::MatrixCenter:return matrix_center_output_contract(*context.input,expected);
    default:return matrix_row_output_contract(*context.input,expected);
    }
}
ProcessingDiagnostic validate_matrixtranspose(const OperationValidation& context) noexcept {
    if(!context.input || !context.output || !transform_parameters_valid(context))return context.error(ProcessingError::InvalidParameters);
    const auto operation=context.operation;
    std::size_t number=0;
    if(operation==ProcessingOperation::MatrixFFT || operation==ProcessingOperation::MatrixRow)
        number=unsigned_parameter(context,operation==ProcessingOperation::MatrixFFT?"fft_size":"row");
    OwnedProcessingContract expected;
    if(expected_matrix_contract(context,number,expected)!=Status::ok || (operation==ProcessingOperation::MatrixRow && !contextual_row_in_range(context.configured,number)))return context.error(ProcessingError::InvalidContract);
    return same_physical_contract(expected.view(),context.outgoing)?ProcessingDiagnostic{}:context.error(ProcessingError::IncompatiblePorts);
}
ProcessingDiagnostic validate_musical_rows(const OperationValidation& context) noexcept {
    std::array<OwnedProcessingContract,4> expected;
    if(musical_spectrogram_output_contracts(context.configured,*context.input,expected)!=Status::ok)return context.error(ProcessingError::InvalidContract);
    return expected_outputs(context,expected);
}
ProcessingDiagnostic validate_rhythm(const OperationValidation& context) noexcept {
    std::array<OwnedProcessingContract,rhythm_output_count> expected;
    if(context.node.outputs.size()!=rhythm_output_count || rhythm_output_contracts(context.configured,*context.input,expected)!=Status::ok)return context.error(ProcessingError::InvalidContract);
    return expected_outputs(context,expected);
}
ProcessingDiagnostic validate_music_operation(const OperationValidation& context) noexcept {
    if(context.kind==NodeKind::FrequencyNormalizer){
        const auto diagnostic=frequency_normalizer_diagnostic(context.configured,*context.input);
        if(!diagnostic)return diagnostic;
    }
    if(!transform_parameters_valid(context))return context.error(ProcessingError::InvalidParameters);
    if(context.kind==NodeKind::MusicalSpectrogram || context.kind==NodeKind::BroadbandRow || context.kind==NodeKind::NoteSpectrum)return validate_musical_rows(context);
    OwnedProcessingContract expected;
    if(music_output_contract(context.configured,*context.input,expected)!=Status::ok)return context.error(ProcessingError::InvalidContract);
    return same_physical_contract(expected.view(),*context.output)?ProcessingDiagnostic{}:context.error(ProcessingError::IncompatiblePorts);
}
ProcessingDiagnostic validate_musictransform(const OperationValidation& context) noexcept {
    if(!context.input || !context.output)return context.error(ProcessingError::InvalidParameters);
    if(context.kind==NodeKind::RhythmAnalyzer)return validate_rhythm(context);
    return validate_music_operation(context);
}
ProcessingDiagnostic validate_transform_geometry(const OperationValidation& context) {
    const auto& input=*context.input;const auto& output=*context.output;
    auto representation=input.representation;
    if(context.kind==NodeKind::StereoFFT && representation.normalization==Normalization::FloatUnit)representation.normalization=Normalization::None;
    const bool input_shape=input.shape.rank==Rank::Vector || (context.kind==NodeKind::Window && input.shape.rank==Rank::Scalar);
    if(!input_shape || output.shape.rank!=Rank::Vector || input.dtype!=output.dtype || representation!=output.representation)return context.error(ProcessingError::IncompatiblePorts);
    if(context.kind!=NodeKind::PcmResampler && !clock_equal(input.clock,output.clock))return context.error(ProcessingError::IncompatiblePorts);
    if(context.kind!=NodeKind::SpectrumMerge && !std::equal(input.channel_order.begin(),input.channel_order.end(),output.channel_order.begin(),output.channel_order.end()))return context.error(ProcessingError::IncompatiblePorts);
    return {};
}
ProcessingDiagnostic validate_resampler_input_rate(const OperationValidation& context) {
    ProcessingParameterView parameter;
    if(resolve_processing_parameter(context.resolved(),"input_rate",parameter)!=Status::ok || !std::holds_alternative<NumericLiteral>(parameter))return context.error(ProcessingError::InvalidParameters);
    const auto rate=std::visit([](auto value){return double(value);},std::get<NumericLiteral>(parameter));
    if(rate==0)return context.error(ProcessingError::PreparationValueUnavailable);
    if(rate!=context.input->clock.sample_rate)return context.error(ProcessingError::IncompatiblePorts);
    return {};
}
ProcessingDiagnostic validate_resampler_clock(const OperationValidation& context) {
    const auto& input=context.input->clock;const auto& output=context.output->clock;
    const bool input_clock=input.tick_seconds.numerator==1 && input.tick_seconds.denominator==input.sample_rate;
    const bool output_clock=output.tick_seconds.numerator==1 && output.tick_seconds.denominator==output.sample_rate;
    return input_clock && output_clock && input.domain==output.domain?ProcessingDiagnostic{}:context.error(ProcessingError::IncompatiblePorts);
}
bool resampler_policy(const OperationValidation& context,ResamplerMode& policy) {
    ProcessingParameterView mode;
    if(resolve_processing_parameter(context.resolved(),"mode",mode)!=Status::ok || !std::holds_alternative<std::string_view>(mode))return false;
    return parse_resampler_mode(std::get<std::string_view>(mode),policy);
}
ProcessingDiagnostic validate_resampler_target(const OperationValidation& context) {
    const auto& input=*context.input;const auto& output=*context.output;
    ProcessingParameterView rate;
    if(resolve_processing_parameter(context.resolved(),"output_rate",rate)!=Status::ok || !std::holds_alternative<NumericLiteral>(rate))return context.error(ProcessingError::InvalidParameters);
    if(std::visit([](auto v){return double(v);},std::get<NumericLiteral>(rate))!=output.clock.sample_rate)return context.error(ProcessingError::IncompatiblePorts);
    if(input.quantity!=output.quantity || input.unit!=output.unit || input.shape.axes!=output.shape.axes || input.coordinates[0].mode!=CoordinateMode::None || output.coordinates[0].mode!=CoordinateMode::None)return context.error(ProcessingError::IncompatiblePorts);
    return {};
}
ProcessingDiagnostic validate_resampler(const OperationValidation& context) {
    const auto declared=validate_resampler_input_rate(context);if(!declared)return declared;
    ResamplerMode policy;
    if(!resampler_policy(context,policy))return context.error(ProcessingError::InvalidParameters);
    const auto clock=validate_resampler_clock(context);if(!clock)return clock;
    const auto& input=*context.input;const auto& output=*context.output;
    if(!context.node.enabled || policy==ResamplerMode::Bypass)return same_physical_contract(input,output)?ProcessingDiagnostic{}:context.error(ProcessingError::IncompatiblePorts);
    return validate_resampler_target(context);
}
ProcessingDiagnostic validate_transform_identity(const OperationValidation& context) {
    const bool merge=context.kind==NodeKind::SpectrumMerge;
    if(!merge && context.kind!=NodeKind::SpectrumGain && context.kind!=NodeKind::BarkWeighting)return {};
    auto expected=*context.input;
    expected.semantic=context.output->semantic;expected.history_element=context.output->history_element;
    if(merge){if(context.output->channel_order.size()!=1)return context.error(ProcessingError::InvalidContract);expected.channel_order=context.output->channel_order;}
    return same_physical_contract(expected,*context.output)?ProcessingDiagnostic{}:context.error(ProcessingError::IncompatiblePorts);
}
bool spectral_grid_number(const ResolvedProcessingNode& node,std::string_view key,double& out) {
    ProcessingParameterView value;
    if(resolve_processing_parameter(node,key,value)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&value);
    if(!literal)return false;
    out=std::visit([](auto v){return double(v);},*literal);
    return std::isfinite(out);
}
bool explicit_spectral_grid(std::span<const float> values,std::uint64_t bins,double nyquist) {
    if(values.size()!=bins)return false;
    double previous=-1;
    for(const auto value:values){
        if(!std::isfinite(value) || value<0 || value<=previous || value>nyquist)return false;
        previous=value;
    }
    return true;
}
ProcessingDiagnostic validate_spectral_coordinates(const OperationValidation& context,const ResolvedProcessingNode& node,
    const NumericContract& input,double fs,double step,double first) {
    ProcessingParameterView parameter;
    if(resolve_processing_parameter(node,"frequencies_hz",parameter)!=Status::ok)return context.error(ProcessingError::InvalidParameters);
    const auto* values=std::get_if<std::span<const float>>(&parameter);
    if(!values)return context.error(ProcessingError::InvalidParameters);
    const auto& axis=input.coordinates[0];const auto bins=input.shape.extents[0].maximum;
    if(!values->empty())return axis.mode==CoordinateMode::ExplicitFloat64LE && explicit_spectral_grid(*values,bins,fs*.5)?
        ProcessingDiagnostic{}:context.error(ProcessingError::IncompatiblePorts);
    if((first+double(bins-1))*step>fs*.5)return context.error(ProcessingError::InvalidParameters);
    if(axis.mode==CoordinateMode::None)return {};
    return declared_frequency_axis_matches(axis,step,std::uint64_t(first))?ProcessingDiagnostic{}:
        context.error(ProcessingError::IncompatiblePorts);
}
ProcessingDiagnostic validate_spectral_weight_grid(const OperationValidation& context) {
    if(context.kind!=NodeKind::BarkWeighting && context.kind!=NodeKind::Mel)return {};
    const auto node=context.resolved();const auto& input=*context.input;
    double fs=0,step=0,first=0;
    if(!spectral_grid_number(node,"sample_rate",fs) || !spectral_grid_number(node,"bin_hz",step) ||
       !spectral_grid_number(node,"first_bin",first) || fs<=0 || step<=0 || first<0 || std::floor(first)!=first)
        return context.error(ProcessingError::InvalidParameters);
    if(fs!=input.clock.sample_rate || input.shape.extents[0].minimum!=input.shape.extents[0].maximum)
        return context.error(ProcessingError::IncompatiblePorts);
    return validate_spectral_coordinates(context,node,input,fs,step,first);
}
ProcessingDiagnostic validate_window_rate(const OperationValidation& context) {
    if(context.kind!=NodeKind::Window)return {};
    double rate=0;
    if(!spectral_grid_number(context.resolved(),"input_rate",rate) || rate<=0)
        return context.error(ProcessingError::InvalidParameters);
    return rate==context.input->clock.sample_rate?ProcessingDiagnostic{}:
        context.error(ProcessingError::IncompatiblePorts);
}
ProcessingDiagnostic validate_generated_transform(const OperationValidation& context) {
    OwnedProcessingContract expected;
    const auto status=context.kind==NodeKind::SpectrumAmplitude?spectrum_amplitude_output_contract(context.configured,*context.input,expected):array_generator_output_contract(context.configured,*context.input,expected);
    return status==Status::ok && same_physical_contract(expected.view(),context.outgoing)?ProcessingDiagnostic{}:context.error(ProcessingError::IncompatiblePorts);
}
ProcessingDiagnostic validate_transform_processing(const OperationValidation& context) noexcept {
    const auto geometry=validate_transform_geometry(context);if(!geometry)return geometry;
    if(context.kind==NodeKind::PcmResampler)return validate_resampler(context);
    const auto rate=validate_window_rate(context);if(!rate)return rate;
    const auto grid=validate_spectral_weight_grid(context);if(!grid)return grid;
    return validate_transform_identity(context);
}
ProcessingDiagnostic validate_transform(const OperationValidation& context) noexcept {
    if(!context.input || !context.output || context.input->channel_order.size()!=1 || context.output->channel_order.size()!=1)return context.error(ProcessingError::InvalidContract);
    const auto* parameters=std::get_if<TransformProcessingParameters>(&context.node.parameters);
    if(!parameters || !context.kind || !parameter_fields_valid(*context.kind,parameters->values,context.limits))return context.error(ProcessingError::InvalidParameters);
    if(context.kind==NodeKind::SpectrumAmplitude || context.kind==NodeKind::WindowFunction)return validate_generated_transform(context);
    return validate_transform_processing(context);
}
using OperationValidator=ProcessingDiagnostic (*)(const OperationValidation&) noexcept;
struct OperationRule {ProcessingOperation operation;OperationValidator validate;};
constexpr OperationRule operation_rules[]{
    {ProcessingOperation::SpectrumPeak,validate_spectrumpeak},
    {ProcessingOperation::ArrayFill,validate_arrayfill},
    {ProcessingOperation::ArrayProduct,validate_arrayproduct},
    {ProcessingOperation::ArrayConcat,validate_arrayconcat},
    {ProcessingOperation::Metadata,validate_metadata},
    {ProcessingOperation::ComplexPolar,validate_complexpolar},
    {ProcessingOperation::Fourier,validate_fourier},
    {ProcessingOperation::Clock,validate_clock},
    {ProcessingOperation::Mixer,validate_mixer},
    {ProcessingOperation::ChannelComparison,validate_channelcomparison},
    {ProcessingOperation::Source,validate_source},
    {ProcessingOperation::Sink,validate_source},
    {ProcessingOperation::InputSelector,validate_inputselector},
    {ProcessingOperation::Splitter,validate_splitter},
    {ProcessingOperation::Switch,validate_splitter},
    {ProcessingOperation::Gate,validate_splitter},
    {ProcessingOperation::Converter,validate_converter},
    {ProcessingOperation::Extract,validate_extract},
    {ProcessingOperation::Reduce,validate_reduce},
    {ProcessingOperation::History,validate_history},
    {ProcessingOperation::WLEDEncoder,validate_wledencoder},
    {ProcessingOperation::MatrixTranspose,validate_matrixtranspose},
    {ProcessingOperation::MatrixWindowFunction,validate_matrixtranspose},
    {ProcessingOperation::MatrixCenter,validate_matrixtranspose},
    {ProcessingOperation::MatrixFFT,validate_matrixtranspose},
    {ProcessingOperation::MatrixRow,validate_matrixtranspose},
    {ProcessingOperation::MusicTransform,validate_musictransform},
    {ProcessingOperation::Transform,validate_transform},
};
}
ProcessingDiagnostic validate_operation(const OperationValidation& context) noexcept {
    for(const auto& rule:operation_rules)if(rule.operation==context.operation)return rule.validate(context);
    return {ProcessingError::UnsupportedCombination,context.configured.id};
}
}
