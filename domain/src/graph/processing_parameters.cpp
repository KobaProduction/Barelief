#include <domain/graph/processing_model.h>
#include <signal_contracts/numeric_policy.h>
#include <algorithm>
#include <bit>
namespace domain {
using namespace signal_contracts;
namespace {
bool content_arrays_equal(std::span<const float> a,std::span<const float> b) noexcept {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)
        if(std::bit_cast<std::uint32_t>(a[i])!=std::bit_cast<std::uint32_t>(b[i]))return false;
    return true;
}
bool content_values_equal(const ProcessingParameterView& a,const ProcessingParameterView& b) noexcept {
    if(a.index()!=b.index())return false;
    if(const auto* number=std::get_if<NumericLiteral>(&a))return literal_equal(*number,std::get<NumericLiteral>(b));
    if(const auto* flag=std::get_if<bool>(&a))return *flag==std::get<bool>(b);
    if(const auto* text=std::get_if<std::string_view>(&a))return *text==std::get<std::string_view>(b);
    return content_arrays_equal(std::get<std::span<const float>>(a),std::get<std::span<const float>>(b));
}
bool content_parameter_equal(const ResolvedProcessingNode& a,const ResolvedProcessingNode& b,std::string_view key) noexcept {
    ProcessingParameterView before,after;
    return resolve_processing_parameter(a,key,before)==Status::ok
        && resolve_processing_parameter(b,key,after)==Status::ok && content_values_equal(before,after);
}
bool content_parameters_equal(const ResolvedProcessingNode& a,const ResolvedProcessingNode& b) noexcept {
    const auto kind=node_kind(a.kind);
    if(!kind)return false;
    for(const auto& descriptor:parameter_descriptors(*kind)) {
        if(descriptor.classification!=ParameterClassification::Processing || descriptor.key=="enabled")continue;
        if(!content_parameter_equal(a,b,descriptor.key))return false;
    }
    return true;
}
const ProcessingPort* content_input(const ResolvedProcessingNode& node) noexcept {
    for(const auto& input:node.inputs)if(input.id==processing_input)return &input;
    return nullptr;
}
bool content_input_semantics_equal(const ResolvedProcessingNode& a,const ResolvedProcessingNode& b) noexcept {
    const auto* before=content_input(a);const auto* after=content_input(b);
    if(!before || !after)return before==after;
    if(!before->contract || !after->contract)return false;
    const auto before_contract=before->contract.view(),after_contract=after->contract.view();
    const auto* x=std::get_if<NumericContract>(&before_contract);const auto* y=std::get_if<NumericContract>(&after_contract);
    return x && y && x->quantity==y->quantity && x->unit==y->unit
        && x->representation.normalization==y->representation.normalization;
}
std::optional<NumericParameterBounds> descriptor_numeric_bounds(const ConfiguredProcessingNode& node,const ParameterDescriptor& descriptor) noexcept {
    auto bounds=descriptor.numeric_bounds?descriptor.numeric_bounds(node):std::optional<NumericParameterBounds>{NumericParameterBounds{descriptor.minimum,descriptor.maximum}};
    if(!bounds)return {};
    bounds->minimum=std::max(bounds->minimum,descriptor.minimum);bounds->maximum=std::min(bounds->maximum,descriptor.maximum);
    return bounds->minimum<=bounds->maximum?bounds:std::nullopt;
}
const std::vector<ProcessingParameter>* parameter_fields(const ProcessingParameters& parameters) noexcept {
    if(const auto* endpoint=std::get_if<EndpointProcessingParameters>(&parameters))return &endpoint->values;
    if(const auto* transform=std::get_if<TransformProcessingParameters>(&parameters))return &transform->values;
    return nullptr;
}
ProcessingParameterView parameter_view(const ProcessingParameterValue& value) noexcept {
    if(const auto* number=std::get_if<NumericLiteral>(&value))return *number;
    if(const auto* flag=std::get_if<bool>(&value))return *flag;
    if(const auto* array=std::get_if<NumericArray>(&value))return std::span<const float>(array->values);
    return std::string_view(std::get<std::string>(value));
}
Status default_parameter_view(const ParameterDescriptor& descriptor,ProcessingParameterView& out) noexcept {
    switch(descriptor.value_kind) {
    case ParameterValueKind::NumericArray:out=descriptor.default_array;break;
    case ParameterValueKind::Boolean:out=descriptor.default_number!=0;break;
    case ParameterValueKind::Integer:out=NumericLiteral{std::int64_t(descriptor.default_number)};break;
    case ParameterValueKind::Number:out=NumericLiteral{descriptor.default_number};break;
    case ParameterValueKind::WindowFunction:case ParameterValueKind::String:out=descriptor.default_text;break;
    default:return Status::invalid_argument;
    }
    return Status::ok;
}
}
bool same_processing_output_content(const ResolvedProcessingNode& current,PortId current_output,
    const ResolvedProcessingNode& prior,PortId prior_output) noexcept {
    if(current.kind!=prior.kind || current_output!=prior_output)return false;
    const auto policy=processing_output_continuity(current,current_output);
    if(policy!=processing_output_continuity(prior,prior_output))return false;
    if(policy==ProcessingContentContinuity::PreserveInput)return true;
    return content_parameters_equal(current,prior) && content_input_semantics_equal(current,prior);
}
std::optional<NumericParameterBounds> processing_parameter_numeric_bounds(const ConfiguredProcessingNode& node,std::string_view key) noexcept {
    const auto kind=node_kind(node.kind);if(!kind)return {};
    const auto* descriptor=parameter_descriptor(*kind,key);
    if(!descriptor || (descriptor->value_kind!=ParameterValueKind::Integer && descriptor->value_kind!=ParameterValueKind::Number))return {};
    return descriptor_numeric_bounds(node,*descriptor);
}
Status resolve_processing_parameter(const ResolvedProcessingNode& n,std::string_view key,ProcessingParameterView& out) noexcept {
    const auto kind=node_kind(n.kind);if(!kind || !n.parameters)return Status::invalid_argument;
    const auto* descriptor=parameter_descriptor(*kind,key);
    if(!descriptor || descriptor->classification!=ParameterClassification::Processing || key=="enabled" || descriptor->value_kind==ParameterValueKind::FrameType)return Status::invalid_argument;
    const auto* fields=parameter_fields(*n.parameters);
    if(!fields)return Status::invalid_argument;
    for(const auto& p:*fields)if(p.key==key){
        out=parameter_view(p.value);
        return Status::ok;
    }
    return default_parameter_view(*descriptor,out);
}



std::optional<ClockContract> signal_generator_clock(const ConfiguredProcessingNode& node) noexcept {
    if(node.kind!="SignalGenerator" || node.outputs.empty())return {};
    const auto output=node.outputs.front().contract.view();
    const auto* numeric=std::get_if<NumericContract>(&output);
    if(!numeric || !numeric->clock.sample_rate || numeric->clock.domain.empty())return {};
    const auto& clock=numeric->clock;
    if(clock.tick_seconds.numerator!=1 || clock.tick_seconds.denominator!=clock.sample_rate)return {};
    return clock;
}
}
