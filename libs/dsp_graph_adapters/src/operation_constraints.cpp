#include "operation_constraints.h"
#include "operation_support.h"
#include <domain/numeric/common_operations.h>
#include <cmath>
#include <limits>
#include <utility>

namespace dsp_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
using Direction=domain::PortDirection;
constexpr double coordinate_tolerance=8*std::numeric_limits<double>::epsilon();
double setting(const dataflow_runtime::NumericOperationNodeView& n,std::string_view key,float fallback) noexcept {
    float value=fallback;return number(n,key,fallback,value)?value:0;
}
double configured_feature_rate(const dataflow_runtime::NumericOperationNodeView& n) noexcept {
    float value=0;return feature_rate(n,44100.f/512,value)?double(value):0;
}
double basic_output_step(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId port) noexcept {
    const auto k=n.kind;
    if(k=="SpectralNovelty" && port==0)return 1/setting(n,"frame_rate",100);
    if(k=="DynamicBeatPath")return 1/configured_feature_rate(n);
    return 0;
}
double output_time_step(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId port,std::size_t axis) noexcept {
    if(axis)return 0;
    if(const auto step=basic_output_step(n,port);step)return step;
    return 0;
}
double tracked_feature_rate(const dataflow_runtime::NumericOperationNodeView& n) noexcept {
    if(n.kind=="PatternStateSequence")return setting(n,"frame_rate",50);
    const auto* spec=domain::sequence_operation_spec(n.kind);
    if((spec && spec->family==domain::SequenceOperationFamily::Histogram) || n.kind=="BeatStateSequence" || n.kind=="DownbeatStateSequence")return setting(n,"frame_rate",100);
    return 0;
}
double primary_input_rate(const dataflow_runtime::NumericOperationNodeView& n) noexcept {
    const auto k=n.kind;
    if(k.starts_with("Pcm"))return setting(n,"sample_rate",44100);
    return tracked_feature_rate(n);
}
double input_rate(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId port) noexcept {
    if(n.kind=="SpectralNovelty")return setting(n,"frame_rate",100);
    if(port)return 0;
    if(n.kind=="DynamicBeatPath")return port?0:configured_feature_rate(n);
    if(n.kind=="TempoEstimate")return 0;
    return primary_input_rate(n);
}
NumericOperationAxisRule index_rule(domain::PortId port,std::size_t axis,bool common) {
    NumericOperationAxisRule rule;rule.direction=Direction::Output;rule.port=port;rule.axis=axis;
    rule.allow_explicit=!common;rule.default_absent=!common;return rule;
}
NumericOperationAxisRule uniform_rule(Direction direction,domain::PortId port,std::size_t axis,double step) {
    NumericOperationAxisRule rule;rule.direction=direction;rule.port=port;rule.axis=axis;
    rule.kind=NumericOperationAxisKind::Uniform;rule.unit=NumericUnit::Seconds;rule.step=step;
    rule.tolerance=coordinate_tolerance;return rule;
}
void sequence_input_constraints(const NumericOperationNodeView& node,domain::PortId port,NumericOperationPolicy& policy) {
    const auto rate=input_rate(node,port);if(!rate)return;
    auto rule=uniform_rule(Direction::Input,port,0,1/rate);
    rule.any_origin=true;rule.ignore_other_units=true;rule.default_absent=false;rule.require_increasing=true;
    policy.axes.push_back(rule);
    if(node.kind.starts_with("Pcm"))
        policy.clocks.push_back({Direction::Input,port,0,NumericOperationClockKind::SampleRate,rate,true});
}
void sequence_output_constraints(const NumericOperationNodeView& node,domain::PortId port,NumericOperationPolicy& policy) {
    policy.clocks.push_back({Direction::Output,port,0,NumericOperationClockKind::Unclocked,0,false});
    for(std::size_t axis=0;axis<2;++axis){
        const auto step=output_time_step(node,port,axis);
        if(!std::isfinite(step) || step<=0){policy.axes.push_back(index_rule(port,axis,false));continue;}
        auto rule=uniform_rule(Direction::Output,port,axis,step);rule.allow_index=true;
        policy.axes.push_back(rule);
    }
}
bool retained_matrix_axis(const NumericOperationNodeView& node,std::size_t axis) noexcept {
    return !axis && (node.kind=="FilterMatrixProjection" || node.kind=="DirectCorrelation" || node.kind=="InverseFourier");
}
bool retained_common_axis(const NumericOperationNodeView& node,domain::PortId port,std::size_t axis) noexcept {
    if(port)return false;
    if(node.kind=="AbsoluteValue" || node.kind=="Clamp" || node.kind=="NumericMap" || node.kind=="LogCompression" || node.kind=="VectorNormalize" || node.kind=="RecursiveFilter")return true;
    const auto input=contract(node,Direction::Input,0);
    return input && input->shape.rank==Rank::Matrix && retained_matrix_axis(node,axis);
}
bool projected_common_axis(const NumericOperationNodeView& node,domain::PortId port,std::size_t axis) noexcept {
    if(port || node.kind!="FilterMatrixProjection" || array(node,"centers_hz").empty())return false;
    const auto input=contract(node,Direction::Input,0);
    return input && axis==(input->shape.rank==Rank::Matrix?1u:0u);
}
void retain_axis(domain::PortId port,std::size_t axis,NumericOperationPolicy& policy) {
    policy.axis_matches.push_back({Direction::Output,port,0,axis,axis,true,false});
    policy.coordinate_routes.push_back({0,port,axis,axis});
}
void projection_axis(domain::PortId port,std::size_t axis,NumericOperationPolicy& policy) {
    auto rule=index_rule(port,axis,true);rule.kind=NumericOperationAxisKind::Declaration;
    rule.mode=CoordinateMode::ExplicitFloat64LE;rule.unit=NumericUnit::Hertz;rule.exact_step=Ratio{1,1};
    rule.allow_absent=false;rule.allow_explicit=true;policy.axes.push_back(rule);
}
bool first_lag(const NumericOperationNodeView& node,std::int64_t& first) noexcept {
    const auto* value=parameter(node,"first_lag");if(!value)return true;
    const auto* literal=std::get_if<NumericLiteral>(value);if(!literal)return false;
    return std::visit([&](auto number){
        if constexpr(std::is_integral_v<decltype(number)>){
            if(!std::in_range<std::int64_t>(number))return false;
            first=std::int64_t(number);return true;
        }else return false;
    },*literal);
}
bool correlation_axis(const NumericOperationNodeView& node,NumericOperationAxisRule& rule) noexcept {
    std::int64_t first=0;if(!first_lag(node,first))return false;
    rule.kind=NumericOperationAxisKind::Uniform;rule.unit=NumericUnit::Frames;
    rule.exact_origin=Ratio{first,1};rule.allow_absent=false;rule.mode=CoordinateMode::Regular;
    return true;
}
bool common_axis(const NumericOperationNodeView& node,domain::PortId port,std::size_t axis,NumericOperationPolicy& policy) {
    if(retained_common_axis(node,port,axis)){retain_axis(port,axis,policy);return true;}
    if(projected_common_axis(node,port,axis)){projection_axis(port,axis,policy);return true;}
    auto rule=index_rule(port,axis,true);
    const auto input=contract(node,Direction::Input,0);
    const auto lag_axis=input && input->shape.rank==Rank::Matrix?1u:0u;
    if(node.kind=="DirectCorrelation" && axis==lag_axis && !correlation_axis(node,rule))return false;
    policy.axes.push_back(rule);return true;
}
}
void sequence_constraints(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    for(const auto& port:node.inputs)sequence_input_constraints(node,port.id,policy);
    for(const auto& port:node.outputs)sequence_output_constraints(node,port.id,policy);
}
bool common_constraints(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    const domain::PortId driver=node.kind=="ArrayGather"?1:0;
    policy.stream_input=driver;policy.trigger=driver;
    for(const auto& port:node.outputs){
        policy.clocks.push_back({Direction::Output,port.id,driver,NumericOperationClockKind::MatchInput,0,true});
        for(std::size_t axis=0;axis<2;++axis)if(!common_axis(node,port.id,axis,policy))return false;
    }
    return true;
}
}
