#include <domain/graph/window_parameters.h>
#include <cmath>
#include <limits>
namespace domain {
using namespace signal_contracts;
namespace {
bool window_number(const ResolvedProcessingNode& node,std::string_view key,double& output) noexcept {
    ProcessingParameterView field;
    if(resolve_processing_parameter(node,key,field)!=Status::ok)return false;
    const auto* value=std::get_if<NumericLiteral>(&field);if(!value)return false;
    output=std::visit([](auto v){return double(v);},*value);return std::isfinite(output);
}
bool window_evaluation(const ResolvedProcessingNode& node,window_generation::WindowFunctionParameters& parameters) noexcept {
    double evaluation=0;
    if(!window_number(node,"evaluation",evaluation) || (evaluation!=0 && evaluation!=1 && evaluation!=2) ||
       !window_number(node,"pi",parameters.pi) || parameters.pi<=0 || parameters.pi>std::numeric_limits<double>::max()/2)return false;
    parameters.evaluation=static_cast<WindowEvaluation>(int(evaluation));return true;
}
}
bool window_function_parameters(const ResolvedProcessingNode& node,window_generation::WindowFunctionParameters& parameters) noexcept {
    ProcessingParameterView function,periodic;
    if(resolve_processing_parameter(node,"function",function)!=Status::ok || !std::holds_alternative<std::string_view>(function) ||
       !parse_window_function(std::get<std::string_view>(function),parameters.function) ||
       resolve_processing_parameter(node,"periodic",periodic)!=Status::ok || !std::holds_alternative<bool>(periodic))return false;
    parameters.periodic=std::get<bool>(periodic);return window_evaluation(node,parameters);
}
}
