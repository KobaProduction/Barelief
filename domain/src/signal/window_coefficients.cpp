#include <domain/signal/contracts.h>
#include <dsp_kernels/window_math.h>
namespace domain {
using namespace signal_contracts;
Status window_function_coefficients(const ConfiguredProcessingNode& node,std::span<float> output) noexcept {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Transform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView size,function,periodic;
    if(resolve_processing_parameter(resolved,"size",size)!=Status::ok || !std::holds_alternative<NumericLiteral>(size) ||
       resolve_processing_parameter(resolved,"function",function)!=Status::ok || !std::holds_alternative<std::string_view>(function) ||
       resolve_processing_parameter(resolved,"periodic",periodic)!=Status::ok || !std::holds_alternative<bool>(periodic))return Status::invalid_argument;
    const auto count=std::visit([](auto value){return double(value);},std::get<NumericLiteral>(size));
    if(output.empty() || double(output.size())!=count)return Status::invalid_argument;
    auto window=dsp_kernels::WindowFunction::Rectangular;
    if(node.enabled && !dsp_kernels::parse_window_function(std::get<std::string_view>(function),window))return Status::invalid_argument;
    for(std::size_t index=0;index<output.size();++index)
        output[index]=dsp_kernels::window_coefficient(index,output.size(),window,std::get<bool>(periodic));
    return Status::ok;
}
}
