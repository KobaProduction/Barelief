#include <application/frequency_profile_application.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

namespace application {
namespace {
using namespace domain;
using namespace signal_contracts;

const ConfiguredProcessingNode* find_node(const NumericProcessingGraph& graph,NodeId id) {
    const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[id](const auto& node){return node.id==id;});
    return found==graph.nodes.end()?nullptr:&*found;
}
bool parameter(const ConfiguredProcessingNode& node,std::string_view key,ProcessingParameterView& value) {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Transform,
        node.inputs,node.outputs,&node.parameters,node.enabled};
    return resolve_processing_parameter(resolved,key,value)==Status::ok;
}
bool number(const ConfiguredProcessingNode& node,std::string_view key,double& result) {
    ProcessingParameterView value;
    if(!parameter(node,key,value))return false;
    const auto* literal=std::get_if<NumericLiteral>(&value);
    if(!literal)return false;
    result=std::visit([](auto n){return double(n);},*literal);
    return std::isfinite(result);
}
bool integer(const ConfiguredProcessingNode& node,std::string_view key,double minimum,double maximum,
    std::uint64_t& result) {
    double value=0;
    if(!number(node,key,value) || value<minimum || value>maximum || std::trunc(value)!=value)return false;
    result=std::uint64_t(value);
    return true;
}
std::string_view string(const ConfiguredProcessingNode& node,std::string_view key) {
    ProcessingParameterView value;
    if(!parameter(node,key,value))return {};
    if(const auto* text=std::get_if<std::string_view>(&value))return *text;
    return {};
}
bool flag(const ConfiguredProcessingNode& node,std::string_view key,bool fallback=false) {
    ProcessingParameterView value;
    if(!parameter(node,key,value))return fallback;
    if(const auto* boolean=std::get_if<bool>(&value))return *boolean;
    return fallback;
}
bool declared_frontend(const ConfiguredProcessingNode& fill,ProfileRoute& route) {
    if(!flag(fill,"frontend_declared"))return false;
    auto& frontend=route.frontend;
    std::uint64_t fft=0,channel=0;
    if(!number(fill,"sample_rate",frontend.sample_rate) ||
        !integer(fill,"fft_size",2,4294967295.,fft) ||
        !number(fill,"coherent_gain",frontend.coherent_gain) ||
        !number(fill,"power_gain",frontend.power_gain) ||
        !integer(fill,"channel_index",0,65535,channel))return false;
    frontend.fft_size=std::size_t(fft);
    frontend.channel_index=std::uint32_t(channel);
    frontend.include_dc=flag(fill,"include_dc",true);
    frontend.include_nyquist=flag(fill,"include_nyquist",true);
    frontend.normalize=flag(fill,"normalize",true);
    frontend.window_function=string(fill,"window_function");
    frontend.representation=string(fill,"representation");
    const auto channels=string(fill,"channel_policy");
    if(channels!="mean" && channels!="select")return false;
    frontend.channel_policy=channels;
    return true;
}
bool declared_profile(const ConfiguredProcessingNode& fill,ProfileRoute& route) {
    if(!declared_frontend(fill,route))return false;
    std::uint64_t first=0,count=0,numerator=0,denominator=0,quality=0;
    if(!integer(fill,"first_bin",0,4294967295.,first) ||
        !integer(fill,"size",1,4294967295.,count) ||
        !integer(fill,"analysis_interval_num",1,4294967295.,numerator) ||
        !integer(fill,"analysis_interval_den",1,4294967295.,denominator) ||
        !integer(fill,"resampler_quality",0,10,quality))return false;
    route.gain_first_bin=std::size_t(first);
    route.gain_count=std::size_t(count);
    route.training.interval_numerator=numerator;
    route.training.interval_denominator=denominator;
    route.frontend.resampler_mode=string(fill,"resampler_mode");
    route.frontend.resampler_quality=unsigned(quality);
    return true;
}
bool has_port(std::span<const ProcessingPort> ports,PortId id) {
    return std::ranges::any_of(ports,[id](const auto& port){return port.id==id;});
}
const Edge* frame_edge(const NumericProcessingGraph& graph,const ConfiguredProcessingNode& multiply,
    PortId coefficient_input) {
    std::uint64_t frame=0;
    if(!integer(multiply,"frame_input",0,double(std::numeric_limits<PortId>::max()),frame))return nullptr;
    const auto frame_input=PortId(frame);
    if(frame_input==coefficient_input ||
        !has_port(multiply.inputs,frame_input))return nullptr;
    const Edge* found=nullptr;
    for(const auto& edge:graph.edges)if(edge.to==multiply.id && edge.input_port==frame_input) {
        if(found)return nullptr;
        found=&edge;
    }
    return found;
}
void append_routes(const NumericProcessingGraph& graph,const ProfileRoute& declared,
    std::vector<ProfileRoute>& routes) {
    const auto* fill=find_node(graph,declared.coefficients);
    if(!fill)return;
    for(const auto& coefficient_edge:graph.edges)if(coefficient_edge.from==fill->id &&
        has_port(fill->outputs,coefficient_edge.output_port)) {
        const auto* multiply=find_node(graph,coefficient_edge.to);
        if(!multiply || multiply->kind!="ArrayMultiply" || !has_port(multiply->inputs,coefficient_edge.input_port))continue;
        const auto* frame=frame_edge(graph,*multiply,coefficient_edge.input_port);
        const auto* source=frame?find_node(graph,frame->from):nullptr;
        if(!source || !has_port(source->outputs,frame->output_port))continue;
        auto route=declared;
        route.arithmetic=multiply->id;
        route.coefficient_output=coefficient_edge.output_port;
        route.coefficient_input=coefficient_edge.input_port;
        route.frame_source=frame->from;
        route.frame_output=frame->output_port;
        route.frame_input=frame->input_port;
        routes.push_back(std::move(route));
    }
}
bool same_endpoints(const ProfileRoute& left,const ProfileRoute& right) {
    return left.graph_id==right.graph_id && left.coefficients==right.coefficients &&
        left.arithmetic==right.arithmetic &&
        left.coefficient_output==right.coefficient_output && left.coefficient_input==right.coefficient_input &&
        left.frame_source==right.frame_source && left.frame_output==right.frame_output &&
        left.frame_input==right.frame_input && left.gain_first_bin==right.gain_first_bin &&
        left.gain_count==right.gain_count;
}
bool shared_coefficients(const NumericProcessingGraph& graph,const ProfileRoute& route) {
    return std::ranges::count_if(graph.edges,[&](const auto& edge){
        return edge.from==route.coefficients;
    })>1;
}
void put(ConfiguredProcessingNode& node,const char* key,ProcessingParameterValue value) {
    auto& fields=std::get<TransformProcessingParameters>(node.parameters).values;
    for(auto& field:fields)if(field.key==key){field.value=std::move(value);return;}
    fields.push_back({key,std::move(value)});
}
}

std::vector<ProfileRoute> frequency_profile_routes(const NumericProcessingGraph& graph,
    const ProfileMatcher& matcher) {
    std::vector<ProfileRoute> routes;
    for(const auto& fill:graph.nodes)if(fill.kind=="ArrayFill") {
        ProfileRoute declared;
        declared.graph_id=graph.graph_id;
        declared.coefficients=fill.id;
        if(declared_profile(fill,declared) && matcher.valid_declaration(declared))
            append_routes(graph,declared,routes);
    }
    return routes;
}

ProfileApplyResult apply_frequency_profile(const NumericProcessingGraph& graph,const ProfileRoute& selected,
    std::span<const float> gains,const ProfileMatcher& matcher) {
    ProfileApplyResult result;
    const auto routes=frequency_profile_routes(graph,matcher);
    const auto found=std::ranges::find_if(routes,[&](const auto& route){
        return same_endpoints(route,selected) && route.frontend==selected.frontend &&
            route.training==selected.training;
    });
    if(found==routes.end()){result.refusal=ProfileApplyRefusal::RouteUnavailable;return result;}
    result.expected_frontend=found->frontend;
    if(shared_coefficients(graph,*found)){result.refusal=ProfileApplyRefusal::SharedCoefficients;return result;}
    result.mismatches=matcher.mismatches(*found,gains);
    if(!result.mismatches.empty()){result.refusal=ProfileApplyRefusal::ProfileMismatch;return result;}
    if(found->gain_first_bin>gains.size() ||
        found->gain_count>gains.size()-found->gain_first_bin){
        result.refusal=ProfileApplyRefusal::CoefficientRange;
        return result;
    }
    result.coefficients=*find_node(graph,found->coefficients);
    result.arithmetic=*find_node(graph,found->arithmetic);
    std::vector<float> values(gains.begin()+found->gain_first_bin,
        gains.begin()+found->gain_first_bin+found->gain_count);
    put(result.coefficients,"values",NumericArray{std::move(values)});
    put(result.arithmetic,"operation",std::string{"multiply"});
    put(result.arithmetic,"frame_input",NumericLiteral{std::int64_t(found->frame_input)});
    return result;
}
}
