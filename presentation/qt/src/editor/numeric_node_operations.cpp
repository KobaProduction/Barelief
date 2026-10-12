#include "numeric_graph_draft.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <limits>
namespace numeric_editor {
using namespace domain;
using namespace signal_contracts;
ProcessingAdmissionLimits limits() {return {128,256,64,128,4096,16*1024*1024,{16,800000,16*1024*1024,65536,4096,65536}};}

namespace {
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
bool clock_node(ConfiguredProcessingNode& node,const ClockContract& clock,const ExtentBounds* pcm_extent){
    const std::string domain(clock.domain);auto owned_clock=clock;owned_clock.domain=domain;
    const bool resampler=node.kind=="PcmResampler";
    const auto output_rate=resampler?declared_rate(node,"output_rate",48000):clock.sample_rate;
    if(clock.sample_rate<1000 || clock.sample_rate>192000 || !output_rate)return false;
    if(resampler && (node.inputs.size()!=1 || node.outputs.size()!=1 || !std::holds_alternative<TransformProcessingParameters>(node.parameters) ||
        clock.sample_rate<8000 || output_rate<8000 ||
        std::uint64_t(output_rate)>std::uint64_t(clock.sample_rate)*8 || std::uint64_t(clock.sample_rate)>std::uint64_t(output_rate)*8))return false;
    for(auto* ports:{&node.inputs,&node.outputs})for(auto& p:*ports){
        auto data=p.contract.view();auto* c=std::get_if<NumericContract>(&data);if(!c)continue;
        const bool output=ports==&node.outputs;
        auto next_clock=owned_clock;if(resampler && output){next_clock.sample_rate=output_rate;next_clock.tick_seconds={1,output_rate};}
        for(std::size_t axis=0;axis<c->coordinates.size();++axis){auto& coordinate=c->coordinates[axis];
            if(coordinate.mode==CoordinateMode::Regular && coordinate.unit==NumericUnit::Hertz)
                if(!scale_frequency(coordinate.origin,c->clock.sample_rate,next_clock.sample_rate) || !scale_frequency(coordinate.step,c->clock.sample_rate,next_clock.sample_rate))return false;
            if(coordinate.mode==CoordinateMode::Regular && coordinate.unit==NumericUnit::Seconds && c->shape.axes[axis]==Axis::Sample)
                if(!scale_frequency(coordinate.step,next_clock.sample_rate,c->clock.sample_rate))return false;
        }
        c->clock=next_clock;
        // Window owns its accumulated output size; fixed-window algorithms
        // retain theirs. Transport/routing/conversion sample blocks adopt ingress geometry.
        if(pcm_extent && c->shape.rank==Rank::Vector && c->shape.axes[0]==Axis::Sample &&
           (node.kind!="Window" || ports==&node.inputs) && node.kind!="WindowFunction" && node.kind!="StereoFFT")c->shape.extents[0]=*pcm_extent;
        if(resampler && output){
            if(node.inputs.empty())return false;
            const auto input_data=node.inputs[0].contract.view();const auto* input=std::get_if<NumericContract>(&input_data);if(!input)return false;
            const auto maximum=input->shape.extents[0].maximum;
            if(!maximum || maximum>8192)return false;
            c->shape.extents[0]={1,(maximum*output_rate+clock.sample_rate-1)/clock.sample_rate+1};
            if(c->shape.extents[0].maximum>65536)return false;
        }
        if(own_processing_contract(data,limits().frame,limits().max_owned_metadata_bytes,p.contract)!=Status::ok)return false;
    }
    return true;
}
bool set_sample_rate(ConfiguredProcessingNode& node,std::uint32_t rate){
    const auto minimum=node.kind=="AudioSource"?1000u:8000u;
    if(rate<minimum || rate>192000 || (node.kind!="AudioSource" && node.kind!="TestSignal" && node.kind!="PcmResampler"))return false;
    auto candidate=node;
    if(candidate.outputs.empty())return false;
    const auto output=candidate.outputs[0].contract.view();const auto* c=std::get_if<NumericContract>(&output);if(!c)return false;
    if(candidate.kind=="PcmResampler"){
        if(candidate.inputs.size()!=1 || !std::holds_alternative<TransformProcessingParameters>(candidate.parameters))return false;
        auto& values=std::get<TransformProcessingParameters>(candidate.parameters).values;
        auto parameter=std::find_if(values.begin(),values.end(),[](const auto& p){return p.key=="output_rate";});
        if(parameter==values.end())values.push_back({"output_rate",NumericLiteral{std::uint64_t(rate)}});else parameter->value=NumericLiteral{std::uint64_t(rate)};
        const auto input=candidate.inputs[0].contract.view();const auto* input_numeric=std::get_if<NumericContract>(&input);
        if(!input_numeric || !clock_node(candidate,input_numeric->clock))return false;
    }else{
        if(!std::holds_alternative<EndpointProcessingParameters>(candidate.parameters))return false;
        auto clock=c->clock;clock.sample_rate=rate;clock.tick_seconds={1,rate};if(!clock_node(candidate,clock))return false;
        auto& values=std::get<EndpointProcessingParameters>(candidate.parameters).values;
        auto parameter=std::find_if(values.begin(),values.end(),[](const auto& p){return p.key=="sample_rate";});
        if(parameter==values.end())values.push_back({"sample_rate",NumericLiteral{std::uint64_t(rate)}});else parameter->value=NumericLiteral{std::uint64_t(rate)};
    }
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
bool set_contract(ConfiguredProcessingNode& n,PortDirection direction,PortId id,const DataContract& c) {
    auto candidate=n;auto& ports=direction==PortDirection::Input?candidate.inputs:candidate.outputs;
    auto it=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==id;});if(it==ports.end())return false;
    OwnedProcessingContract owner;if(own_processing_contract(c,limits().frame,limits().max_owned_metadata_bytes,owner)!=Status::ok)return false;it->contract=owner;
    // Routing preserves a single exact contract at every endpoint.
    if(candidate.kind=="InputSelector" || std::holds_alternative<RouteProcessingParameters>(candidate.parameters))for(auto* collection:{&candidate.inputs,&candidate.outputs})for(auto& p:*collection)p.contract=owner;
    n=std::move(candidate);return true;
}
bool add_input(ConfiguredProcessingNode& n) {
    if((n.kind!="InputSelector" && n.kind!="ScalarView") || n.inputs.empty() || n.inputs.size()>=limits().max_ports_per_node)return false;
    if(n.kind=="InputSelector"&&n.outputs.size()>1) {
        const auto channels=n.outputs.size();if(n.inputs.size()+channels>limits().max_ports_per_node)return false;
        for(std::size_t lane=0;lane<channels;++lane)n.inputs.push_back({PortId(n.inputs.size()),PortDirection::Input,n.outputs[lane].contract});
        return true;
    }
    const auto maximum=std::max_element(n.inputs.begin(),n.inputs.end(),[](const auto& a,const auto& b){return a.id<b.id;})->id;
    if(maximum==std::numeric_limits<PortId>::max())return false;
    n.inputs.push_back({PortId(maximum+1),PortDirection::Input,n.inputs.front().contract});return true;
}
bool remove_input(ConfiguredProcessingNode& n,PortId id) {
    if((n.kind!="InputSelector" && n.kind!="ScalarView") || n.inputs.size()<=1)return false;
    const auto it=std::find_if(n.inputs.begin(),n.inputs.end(),[&](const auto& p){return p.id==id;});
    if(it==n.inputs.end())return false;
    const auto position=std::size_t(it-n.inputs.begin());
    if(n.kind=="InputSelector"&&n.outputs.size()>1) {
        // Group deletion cannot silently rebind remaining wires to another stream.
        // Only the final group can be removed; stable port IDs stay unchanged.
        const auto channels=n.outputs.size();if(n.inputs.size()<=channels||id/channels!=(n.inputs.size()/channels)-1)return false;
        n.inputs.resize(n.inputs.size()-channels);
        for(auto& p:std::get<TransformProcessingParameters>(n.parameters).values)if(p.key=="selected_input") {
            auto selected=std::visit([](auto v){return std::uint64_t(v);},std::get<NumericLiteral>(p.value));
            p.value=NumericLiteral{std::int64_t(std::min(selected,std::uint64_t(n.inputs.size()/channels-1)))};
        }
        return true;
    }
    n.inputs.erase(it);
    if(n.kind=="InputSelector")for(auto& p:std::get<TransformProcessingParameters>(n.parameters).values)if(p.key=="selected_input") {
        const auto selected=std::visit([](auto v){return std::uint64_t(v);},std::get<NumericLiteral>(p.value));
        if(selected==id)p.value=NumericLiteral{std::int64_t(n.inputs[std::min(position,n.inputs.size()-1)].id)};
    }
    return true;
}
bool set_input_count(ConfiguredProcessingNode& n,std::uint16_t count) {
    if(n.kind!="InputSelector" || !count || std::size_t(count)*n.outputs.size()>limits().max_ports_per_node || n.inputs.empty())return false;
    auto candidate=n;
    const auto ports=std::size_t(count)*n.outputs.size();
    while(candidate.inputs.size()<ports)if(!add_input(candidate))return false;
    while(candidate.inputs.size()>ports)if(!remove_input(candidate,candidate.inputs.back().id))return false;
    n=std::move(candidate);return true;
}
bool set_selector_channels(ConfiguredProcessingNode& n,std::uint16_t channels) {
    if(n.kind!="InputSelector"||!channels||channels>32||n.outputs.empty()||n.inputs.empty())return false;
    const auto streams=n.inputs.size()/n.outputs.size();if(streams*channels>limits().max_ports_per_node)return false;
    const auto contract=n.outputs.front().contract;auto candidate=n;
    candidate.inputs.clear();candidate.outputs.clear();
    for(std::size_t port=0;port<streams*channels;++port)candidate.inputs.push_back({PortId(port),PortDirection::Input,contract});
    for(std::size_t port=0;port<channels;++port)candidate.outputs.push_back({PortId(port),PortDirection::Output,contract});
    auto& fields=std::get<TransformProcessingParameters>(candidate.parameters).values;
    fields.erase(std::remove_if(fields.begin(),fields.end(),[](const auto& p){return p.key=="channels_per_stream";}),fields.end());
    fields.push_back({"channels_per_stream",NumericLiteral{std::uint64_t(channels)}});n=std::move(candidate);return true;
}
bool set_route_channels(ConfiguredProcessingNode& n,std::uint16_t channels) {
    auto* p=std::get_if<RouteProcessingParameters>(&n.parameters);
    if(n.kind!="Switch" || !p || !channels || channels>32 || n.inputs.empty() || std::size_t(p->outputs)*channels>limits().max_ports_per_node)return false;
    auto candidate=n;auto& route=std::get<RouteProcessingParameters>(candidate.parameters);route.channels_per_stream=channels;
    const auto contract=n.inputs.front().contract;candidate.inputs.clear();candidate.outputs.clear();
    for(std::size_t i=0;i<channels;++i)candidate.inputs.push_back({PortId(i),PortDirection::Input,contract});
    for(std::size_t i=0;i<std::size_t(route.outputs)*channels;++i)candidate.outputs.push_back({PortId(i),PortDirection::Output,contract});
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
