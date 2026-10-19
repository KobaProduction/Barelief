#include "numeric_graph_draft.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <limits>
namespace numeric_editor {
using namespace domain;
using namespace signal_contracts;
ProcessingAdmissionLimits limits() {return {128,256,64,128,1024*1024,16*1024*1024,{16,800000,16*1024*1024,65536,4096,65536}};}

namespace {
bool scalar_view_kind(std::string_view kind){const auto id=node_kind(kind);return id && is_scalar_view(*id);}
bool scale_frequency(Ratio& ratio,std::uint32_t before,std::uint32_t after){
    if(!before || !ratio.denominator || ratio.numerator<0)return false;
    if(before==after)return true;
    auto numerator=std::uint64_t(ratio.numerator),denominator=ratio.denominator;
    auto multiplier=std::uint64_t(after),divisor=std::uint64_t(before);
    auto common=std::gcd(numerator,divisor);numerator/=common;divisor/=common;
    common=std::gcd(multiplier,denominator);multiplier/=common;denominator/=common;
    if(numerator>std::uint64_t(INT64_MAX)/multiplier || denominator>UINT64_MAX/divisor)return false;
    ratio={std::int64_t(numerator*multiplier),denominator*divisor};return true;
}
std::uint32_t declared_rate(const ConfiguredProcessingNode& node,const char* key,std::uint32_t fallback){
    const auto* p=std::get_if<TransformProcessingParameters>(&node.parameters);if(!p)return fallback;
    for(const auto& value:p->values)if(value.key==key)if(const auto* literal=std::get_if<NumericLiteral>(&value.value))
        return std::visit([&](auto n){return n>=8000 && n<=192000 && std::floor(double(n))==double(n)?std::uint32_t(n):0;},*literal);
    return fallback;
}
}
namespace {
bool scale_clock_coordinates(NumericContract& contract,const ClockContract& next_clock) {
    for(std::size_t axis=0;axis<contract.coordinates.size();++axis){
        auto& coordinate=contract.coordinates[axis];
        if(coordinate.mode==CoordinateMode::Regular && coordinate.unit==NumericUnit::Hertz)
            if(!scale_frequency(coordinate.origin,contract.clock.sample_rate,next_clock.sample_rate) ||
               !scale_frequency(coordinate.step,contract.clock.sample_rate,next_clock.sample_rate))return false;
        if(coordinate.mode==CoordinateMode::Regular && coordinate.unit==NumericUnit::Seconds && contract.shape.axes[axis]==Axis::Sample)
            if(!scale_frequency(coordinate.step,next_clock.sample_rate,contract.clock.sample_rate))return false;
    }
    contract.clock=next_clock;
    return true;
}
bool resampler_extent(const ConfiguredProcessingNode& node,NumericContract& contract,
                      const ClockContract& clock,std::uint32_t output_rate,bool bypass) {
    if(node.inputs.empty())return false;
    const auto input_data=node.inputs[0].contract.view();
    const auto* input=std::get_if<NumericContract>(&input_data);
    if(!input)return false;
    const auto maximum=input->shape.extents[0].maximum;
    if(!maximum || maximum>8192)return false;
    contract.shape.extents[0]=bypass?input->shape.extents[0]:
        ExtentBounds{1,(maximum*output_rate+clock.sample_rate-1)/clock.sample_rate+1};
    return contract.shape.extents[0].maximum<=65536;
}
bool update_clock_port(ConfiguredProcessingNode& node,ProcessingPort& port,const ClockContract& clock,
                       std::uint32_t output_rate,const ExtentBounds* pcm_extent,bool output) {
    if(!output && !numeric_parameter_key(node.kind,port.id).empty())return true;
    if(output && has_sample_rate_output(node.kind) && port.id==sample_rate_output)return true;
    auto data=port.contract.view();
    auto* contract=std::get_if<NumericContract>(&data);
    if(!contract)return true;
    const bool resampler=node.kind=="PcmResampler";
    auto next_clock=clock;
    if(resampler && output){next_clock.sample_rate=output_rate;next_clock.tick_seconds={1,output_rate};}
    if(!scale_clock_coordinates(*contract,next_clock))return false;
    // Window owns its accumulated output size; fixed-window algorithms retain theirs.
    if(pcm_extent && contract->shape.rank==Rank::Vector && contract->shape.axes[0]==Axis::Sample &&
       (node.kind!="Window" || !output) && node.kind!="WindowFunction" && node.kind!="StereoFFT")contract->shape.extents[0]=*pcm_extent;
    if(resampler && output && !resampler_extent(node,*contract,clock,output_rate,!node.enabled))return false;
    return own_processing_contract(data,limits().frame,limits().max_owned_metadata_bytes,port.contract)==Status::ok;
}
bool valid_resampler_clock(const ConfiguredProcessingNode& node,const ClockContract& clock,std::uint32_t output_rate) {
    if(node.kind!="PcmResampler")return true;
    return !node.inputs.empty() && data_outputs(node).size()==1 &&
        std::holds_alternative<TransformProcessingParameters>(node.parameters) && clock.sample_rate>=8000 && output_rate>=8000 &&
        std::uint64_t(output_rate)<=std::uint64_t(clock.sample_rate)*8 &&
        std::uint64_t(clock.sample_rate)<=std::uint64_t(output_rate)*8;
}
}
bool clock_node(ConfiguredProcessingNode& node,const ClockContract& clock,const ExtentBounds* pcm_extent){
    const std::string domain(clock.domain);auto owned_clock=clock;owned_clock.domain=domain;
    const auto output_rate=node.kind=="PcmResampler" && node.enabled?declared_rate(node,"output_rate",48000):clock.sample_rate;
    if(clock.sample_rate<1000 || clock.sample_rate>192000 || !output_rate || !valid_resampler_clock(node,clock,output_rate))return false;
    for(auto& port:node.inputs)if(!update_clock_port(node,port,owned_clock,output_rate,pcm_extent,false))return false;
    for(auto& port:node.outputs)if(!update_clock_port(node,port,owned_clock,output_rate,pcm_extent,true))return false;
    return attach_numeric_parameter_ports(node,limits())==Status::ok;
}
namespace {
void set_rate_parameter(std::vector<ProcessingParameter>& values,const char* key,std::uint32_t rate) {
    auto parameter=std::find_if(values.begin(),values.end(),[&](const auto& value){return value.key==key;});
    if(parameter==values.end())values.push_back({key,NumericLiteral{std::uint64_t(rate)}});
    else parameter->value=NumericLiteral{std::uint64_t(rate)};
}
bool set_resampler_rate(ConfiguredProcessingNode& candidate,std::uint32_t rate) {
    if(candidate.inputs.empty() || !std::holds_alternative<TransformProcessingParameters>(candidate.parameters))return false;
    auto& values=std::get<TransformProcessingParameters>(candidate.parameters).values;
    set_rate_parameter(values,"output_rate",rate);
    const auto input=candidate.inputs[0].contract.view();
    const auto* input_numeric=std::get_if<NumericContract>(&input);
    return input_numeric && clock_node(candidate,input_numeric->clock);
}
bool set_source_rate(ConfiguredProcessingNode& candidate,const NumericContract& output,std::uint32_t rate) {
    if(!std::holds_alternative<EndpointProcessingParameters>(candidate.parameters))return false;
    auto clock=output.clock;
    clock.sample_rate=rate;
    clock.tick_seconds={1,rate};
    if(!clock_node(candidate,clock))return false;
    auto& values=std::get<EndpointProcessingParameters>(candidate.parameters).values;
    set_rate_parameter(values,"sample_rate",rate);
    return true;
}
}
bool set_sample_rate(ConfiguredProcessingNode& node,std::uint32_t rate){
    const auto minimum=node.kind=="AudioSource"?1000u:8000u;
    if(rate<minimum || rate>192000 || (node.kind!="AudioSource" && node.kind!="TestSignal" && node.kind!="PcmResampler"))return false;
    auto candidate=node;
    if(candidate.outputs.empty())return false;
    const auto output=candidate.outputs[0].contract.view();const auto* c=std::get_if<NumericContract>(&output);if(!c)return false;
    if(candidate.kind=="PcmResampler"?!set_resampler_rate(candidate,rate):!set_source_rate(candidate,*c,rate))return false;
    node=std::move(candidate);return true;
}
bool bind_target_clock(ConfiguredProcessingNode& node,const ClockContract& clock) {
    if(node.kind!="PcmResampler")return clock_node(node,clock);
    if(!set_sample_rate(node,clock.sample_rate))return false;
    for(auto& p:node.outputs){auto data=p.contract.view();auto* c=std::get_if<NumericContract>(&data);if(!c)return false;c->clock=clock;
        if(own_processing_contract(data,limits().frame,limits().max_owned_metadata_bytes,p.contract)!=Status::ok)return false;}
    return true;
}
bool set_dtype(ConfiguredProcessingNode& n,PortDirection direction,PortId id,DType type) {
    auto& ports=direction==PortDirection::Input?n.inputs:n.outputs;
    auto it=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==id;});if(it==ports.end() || !it->contract)return false;
    auto c=it->contract.view();auto* numeric=std::get_if<NumericContract>(&c);if(!numeric)return false;numeric->dtype=type;
    return set_contract(n,direction,id,c);
}
namespace {
void propagate_route_contract(ConfiguredProcessingNode& candidate,PortId id,const OwnedProcessingContract& owner) {
    if(candidate.kind=="InputSelector"){
        const auto width=candidate.outputs.size();const auto lane=id%width;
        for(auto& port:candidate.inputs)if(port.id%width==lane)port.contract=owner;
        candidate.outputs[lane].contract=owner;
    }else if(std::holds_alternative<RouteProcessingParameters>(candidate.parameters))
        for(auto* collection:{&candidate.inputs,&candidate.outputs})
            for(auto& port:*collection)if(port.id!=sample_rate_output)port.contract=owner;
}
}
bool set_contract(ConfiguredProcessingNode& n,PortDirection direction,PortId id,const DataContract& c) {
    auto candidate=n;auto& ports=direction==PortDirection::Input?candidate.inputs:candidate.outputs;
    auto it=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==id;});if(it==ports.end())return false;
    OwnedProcessingContract owner;if(own_processing_contract(c,limits().frame,limits().max_owned_metadata_bytes,owner)!=Status::ok)return false;it->contract=owner;
    // Routing preserves a single exact contract at every endpoint.
    propagate_route_contract(candidate,id,owner);
    n=std::move(candidate);return true;
}
namespace {
bool prepare_array_multiply_input(OwnedProcessingContract& input) {
    const auto contract=input.view();
    auto value=std::get<NumericContract>(contract);
    value.quantity=Quantity::Unspecified;
    value.unit=NumericUnit::Values;
    value.coordinates={};
    value.representation.normalization=Normalization::None;
    return own_processing_contract(value,limits().frame,limits().max_owned_metadata_bytes,input)==Status::ok;
}
}
bool add_input(ConfiguredProcessingNode& n) {
    if((n.kind!="InputSelector" && !scalar_view_kind(n.kind) && n.kind!="ArrayMultiply") || n.inputs.empty() || n.inputs.size()>=limits().max_ports_per_node)return false;
    if(n.kind=="InputSelector"&&data_outputs(n).size()>1) {
        const auto channels=data_outputs(n).size();if(n.inputs.size()+channels>limits().max_ports_per_node)return false;
        for(std::size_t lane=0;lane<channels;++lane)n.inputs.push_back({PortId(n.inputs.size()),PortDirection::Input,n.outputs[lane].contract});
        return true;
    }
    const auto maximum=std::max_element(n.inputs.begin(),n.inputs.end(),[](const auto& a,const auto& b){return a.id<b.id;})->id;
    if(maximum==std::numeric_limits<PortId>::max())return false;
    auto input=n.inputs.front().contract;
    if(n.kind=="ArrayMultiply" && !prepare_array_multiply_input(input))return false;
    n.inputs.push_back({PortId(maximum+1),PortDirection::Input,std::move(input)});return true;
}
namespace {
bool remove_selector_group(ConfiguredProcessingNode& node,PortId id) {
    // Only the final group may be removed; stable port IDs stay unchanged.
    const auto channels=data_outputs(node).size();
    if(node.inputs.size()<=channels || id/channels!=(node.inputs.size()/channels)-1)return false;
    node.inputs.resize(node.inputs.size()-channels);
    for(auto& parameter:std::get<TransformProcessingParameters>(node.parameters).values)
        if(parameter.key=="selected_input") {
            const auto selected=std::visit([](auto value){return std::uint64_t(value);},std::get<NumericLiteral>(parameter.value));
            parameter.value=NumericLiteral{std::int64_t(std::min(selected,std::uint64_t(node.inputs.size()/channels-1)))};
        }
    return true;
}
void clear_removed_array_frame(ConfiguredProcessingNode& node,PortId id) {
    if(node.kind!="ArrayMultiply")return;
    for(auto& field:std::get<TransformProcessingParameters>(node.parameters).values)if(field.key=="frame_input"){
        const auto selected=std::visit([](auto value){return double(value);},std::get<NumericLiteral>(field.value));
        if(selected==id)field.value=NumericLiteral{std::int64_t{-1}};
    }
}
void reselect_removed_selector_input(ConfiguredProcessingNode& node,PortId id,std::size_t position) {
    if(node.kind!="InputSelector")return;
    for(auto& parameter:std::get<TransformProcessingParameters>(node.parameters).values)if(parameter.key=="selected_input") {
        const auto selected=std::visit([](auto value){return std::uint64_t(value);},std::get<NumericLiteral>(parameter.value));
        if(selected==id)parameter.value=NumericLiteral{std::int64_t(node.inputs[std::min(position,node.inputs.size()-1)].id)};
    }
}
}
bool remove_input(ConfiguredProcessingNode& n,PortId id) {
    if((n.kind!="InputSelector" && !scalar_view_kind(n.kind) && n.kind!="ArrayMultiply") || n.inputs.size()<=(n.kind=="ArrayMultiply"?2u:1u))return false;
    const auto it=std::find_if(n.inputs.begin(),n.inputs.end(),[&](const auto& p){return p.id==id;});
    if(it==n.inputs.end())return false;
    const auto position=std::size_t(it-n.inputs.begin());
    if(n.kind=="InputSelector"&&data_outputs(n).size()>1) {
        return remove_selector_group(n,id);
    }
    n.inputs.erase(it);
    clear_removed_array_frame(n,id);
    reselect_removed_selector_input(n,id,position);
    return true;
}
bool set_input_count(ConfiguredProcessingNode& n,std::uint16_t count) {
    const auto channels=n.kind=="ArrayMultiply"?1:data_outputs(n).size();
    if((n.kind!="InputSelector" && n.kind!="ArrayMultiply") || count<(n.kind=="ArrayMultiply"?2:1) || std::size_t(count)*channels>limits().max_ports_per_node || n.inputs.empty())return false;
    auto candidate=n;
    const auto ports=std::size_t(count)*channels;
    while(candidate.inputs.size()<ports)if(!add_input(candidate))return false;
    while(candidate.inputs.size()>ports)if(!remove_input(candidate,candidate.inputs.back().id))return false;
    n=std::move(candidate);return true;
}
namespace {
void rebuild_selector_ports(ConfiguredProcessingNode& candidate,const ConfiguredProcessingNode& previous_node,
                            std::size_t streams,std::uint16_t channels) {
    const auto& previous=previous_node.outputs;
    const auto old_width=previous.size();
    candidate.inputs.clear();candidate.outputs.clear();
    for(std::size_t port=0;port<streams*channels;++port)
        candidate.inputs.push_back({PortId(port),PortDirection::Input,previous[port%channels<old_width?port%channels:0].contract});
    for(std::size_t port=0;port<channels;++port)
        candidate.outputs.push_back({PortId(port),PortDirection::Output,previous[port<old_width?port:0].contract});
}
void set_selector_width_parameter(ConfiguredProcessingNode& candidate,std::uint16_t channels) {
    auto& fields=std::get<TransformProcessingParameters>(candidate.parameters).values;
    fields.erase(std::remove_if(fields.begin(),fields.end(),[](const auto& parameter){return parameter.key=="channels_per_stream";}),fields.end());
    fields.push_back({"channels_per_stream",NumericLiteral{std::uint64_t(channels)}});
}
}
bool set_selector_channels(ConfiguredProcessingNode& n,std::uint16_t channels) {
    if(n.kind!="InputSelector"||!channels||channels>32||n.outputs.empty()||n.inputs.empty())return false;
    const bool rate=data_outputs(n).size()!=n.outputs.size();
    const auto streams=n.inputs.size()/data_outputs(n).size();if(streams*channels>limits().max_ports_per_node)return false;
    auto candidate=n;
    rebuild_selector_ports(candidate,n,streams,channels);
    set_selector_width_parameter(candidate,channels);
    if(rate)candidate.outputs.push_back(n.outputs.back());
    if(attach_numeric_parameter_ports(candidate,limits())!=Status::ok)return false;
    n=std::move(candidate);return true;
}
namespace {
void rebuild_route_ports(ConfiguredProcessingNode& candidate,const OwnedProcessingContract& contract,
                         std::uint16_t channels,std::uint16_t outputs) {
    candidate.inputs.clear();candidate.outputs.clear();
    for(std::size_t index=0;index<channels;++index)
        candidate.inputs.push_back({PortId(index),PortDirection::Input,contract});
    for(std::size_t index=0;index<std::size_t(outputs)*channels;++index)
        candidate.outputs.push_back({PortId(index),PortDirection::Output,contract});
}
}
bool set_route_channels(ConfiguredProcessingNode& n,std::uint16_t channels) {
    auto* p=std::get_if<RouteProcessingParameters>(&n.parameters);
    if(n.kind!="Switch" || !p || !channels || channels>32 || n.inputs.empty() || std::size_t(p->outputs)*channels>limits().max_ports_per_node)return false;
    auto candidate=n;auto& route=std::get<RouteProcessingParameters>(candidate.parameters);route.channels_per_stream=channels;
    const auto contract=n.inputs.front().contract;
    rebuild_route_ports(candidate,contract,channels,route.outputs);
    n=std::move(candidate);return true;
}
bool set_output_count(ConfiguredProcessingNode& n,std::uint16_t count) {
    auto* p=std::get_if<RouteProcessingParameters>(&n.parameters);
    if(!p || (n.kind!="Splitter" && n.kind!="Switch") || count<2 || count>8 || n.inputs.empty())return false;
    p->outputs=count;p->selected=std::min<std::uint16_t>(p->selected,count-1);
    const auto channels=p->channels_per_stream;
    n.outputs.resize(std::size_t(count)*channels);for(std::size_t i=0;i<n.outputs.size();++i)n.outputs[i]={PortId(i),PortDirection::Output,n.inputs[i%channels].contract};return true;
}
}
