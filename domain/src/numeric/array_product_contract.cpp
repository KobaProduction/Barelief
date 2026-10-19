#include <domain/numeric/contracts.h>
#include <cmath>
namespace domain {
using namespace signal_contracts;
namespace {
Status selected_driver(const ConfiguredProcessingNode& node,std::size_t& out) {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::ArrayProduct,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView parameter;
    if(resolve_processing_parameter(view,"frame_input",parameter)!=Status::ok)return Status::invalid_argument;
    const auto* literal=std::get_if<NumericLiteral>(&parameter);if(!literal)return Status::invalid_argument;
    const auto requested=std::visit([](auto v){return static_cast<double>(v);},*literal);
    if(!std::isfinite(requested) || requested<0 || requested>65535 || std::floor(requested)!=requested)return Status::invalid_argument;
    for(std::size_t i=0;i<node.inputs.size();++i)if(node.inputs[i].id==PortId(requested)){out=i;return Status::ok;}
    return Status::invalid_argument;
}
bool vector_valid(const NumericContract& value) {
    return value.dtype==DType::Float32 && value.channel_order.size()==1 && value.shape.rank==Rank::Vector &&
        value.shape.extents[0].minimum && value.shape.extents[0].minimum==value.shape.extents[0].maximum &&
        value.representation.encoding==StorageEncoding::Native && value.representation.layout==ChannelLayout::Planar &&
        (value.representation.normalization==Normalization::None || value.representation.normalization==Normalization::FloatUnit);
}
Status validate_inputs(const ConfiguredProcessingNode& node) {
    std::uint64_t count=0;
    for(const auto& port:node.inputs) {
        const auto data=port.contract.view();const auto* value=std::get_if<NumericContract>(&data);
        if(!value || !vector_valid(*value))return Status::incompatible_contract;
        if(count && count!=value->shape.extents[0].maximum)return Status::incompatible_contract;
        count=value->shape.extents[0].maximum;
    }
    return Status::ok;
}
bool operation_valid(const ConfiguredProcessingNode& node) {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::ArrayProduct,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView field;
    if(resolve_processing_parameter(view,"operation",field)!=Status::ok)return false;
    const auto* text=std::get_if<std::string_view>(&field);
    return text && (*text=="multiply" || *text=="add");
}
}
Status array_product_output_contract(const ConfiguredProcessingNode& node,OwnedProcessingContract& result,std::size_t* frame_input) noexcept {
    if(node.kind!="ArrayMultiply" || node.inputs.size()<2 || node.inputs.size()>max_variadic_inputs || !operation_valid(node))return Status::invalid_argument;
    std::size_t driver=0;
    if(selected_driver(node,driver)!=Status::ok)return Status::invalid_argument;
    if(validate_inputs(node)!=Status::ok)return Status::incompatible_contract;
    const auto contract=node.inputs[driver].contract.view();auto output=std::get<NumericContract>(contract);
    if(node.enabled)output.representation.normalization=Normalization::None;
    OwnedProcessingContract candidate;
    const auto status=own_processing_contract(output,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,candidate);
    if(status!=Status::ok)return status;
    result=std::move(candidate);
    if(frame_input)*frame_input=driver;
    return Status::ok;
}
}
