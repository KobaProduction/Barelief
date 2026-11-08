#include <domain/signal/contracts.h>
#include <dsp_kernels/window_math.h>
namespace domain {
using namespace signal_contracts;
namespace {
bool window_output_size(const ConfiguredProcessingNode& node,std::size_t count) noexcept {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Transform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView field;
    if(!count || resolve_processing_parameter(resolved,"size",field)!=Status::ok || !std::holds_alternative<NumericLiteral>(field))return false;
    return std::visit([count](auto size){return double(count)==double(size);},std::get<NumericLiteral>(field));
}
Status fill_default_window(std::span<float> output,const window_generation::WindowFunctionParameters& parameters) noexcept {
    for(std::size_t i=0;i<output.size();++i)output[i]=dsp_kernels::window_coefficient(i,output.size(),parameters.function,parameters.periodic);
    return Status::ok;
}
Status fill_default_window(std::span<double> output,const window_generation::WindowFunctionParameters& parameters) noexcept {
    if(parameters.function==WindowFunction::Hann && output.size()>1){
        constexpr std::array coefficients{.5,.5};const double period=double(output.size())-(parameters.periodic?0:1);
        return dsp_kernels::cosine_sum_window(output,coefficients,period,false,-period/2.)?Status::ok:Status::invalid_argument;
    }
    for(std::size_t i=0;i<output.size();++i)output[i]=dsp_kernels::window_coefficient_value<double>(i,output.size(),parameters.function,parameters.periodic);
    return Status::ok;
}
template<class Scalar> Status fill_window(const ConfiguredProcessingNode& node,std::span<Scalar> output) noexcept {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Transform,node.inputs,node.outputs,&node.parameters,node.enabled};
    window_generation::WindowFunctionParameters parameters;
    if(!window_output_size(node,output.size()) || !window_function_parameters(resolved,parameters))return Status::invalid_argument;
    if(!node.enabled)parameters.function=WindowFunction::Rectangular;
    if(parameters.evaluation==WindowEvaluation::Default)return fill_default_window(output,parameters);
    if(parameters.evaluation==WindowEvaluation::SymmetricCosineSeries)
        return dsp_kernels::symmetric_cosine_window(output,parameters.function,parameters.periodic)?Status::ok:Status::invalid_argument;
    for(std::size_t i=0;i<output.size();++i)
        output[i]=dsp_kernels::direct_window_coefficient<Scalar>(i,output.size(),parameters.function,parameters.periodic,parameters.pi);
    return Status::ok;
}
}
Status window_function_coefficients(const ConfiguredProcessingNode& node,std::span<float> output) noexcept {return fill_window(node,output);}
Status window_function_coefficients(const ConfiguredProcessingNode& node,std::span<double> output) noexcept {return fill_window(node,output);}
}
