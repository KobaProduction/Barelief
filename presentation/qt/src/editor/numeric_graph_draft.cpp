#include "numeric_graph_draft.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <type_traits>
#include <string_view>
namespace numeric_editor {
bool draft_view(std::string_view kind) {
    const auto k=domain::node_kind(kind);
    return k && domain::node_role(*k)==domain::NodeRole::Sink && domain::is_view(*k);
}
bool draft_view_accepts(std::string_view kind,const signal_contracts::NumericContract& c) {
    const auto rank=kind=="ScalarView"?signal_contracts::Rank::Scalar:kind=="SpectrogramView"?signal_contracts::Rank::Matrix:signal_contracts::Rank::Vector;
    return draft_view(kind) && c.shape.rank==rank;
}
bool adopt_extract_input(domain::ConfiguredProcessingNode& node,const signal_contracts::NumericContract& input){
    if(node.kind!="IndexExtract" || node.inputs.size()!=1 || node.outputs.size()!=1 || input.shape.rank==signal_contracts::Rank::Scalar)return false;
    std::uint64_t channel=0;
    const auto* parameters=std::get_if<domain::TransformProcessingParameters>(&node.parameters);if(!parameters)return false;
    for(const auto& parameter:parameters->values)if(parameter.key=="channel_index"){
        const auto* literal=std::get_if<signal_contracts::NumericLiteral>(&parameter.value);if(!literal)return false;
        const bool valid=std::visit([&](auto value){
            using Value=decltype(value);
            if constexpr(std::is_floating_point_v<Value>)if(!std::isfinite(value) || std::trunc(value)!=value)return false;
            if constexpr(std::is_signed_v<Value>)if(value<0)return false;
            if(static_cast<long double>(value)>=static_cast<long double>(input.channel_order.size()))return false;
            channel=static_cast<std::uint64_t>(value);return true;
        },*literal);
        if(!valid)return false;
    }
    if(channel>=input.channel_order.size())return false;
    auto scalar=input;scalar.shape={};scalar.coordinates={};scalar.channel_order=input.channel_order.subspan(std::size_t(channel),1);
    // Own both views before replacing either endpoint: input may borrow node's
    // current metadata, including the selected channel span.
    domain::OwnedProcessingContract input_owner,scalar_owner;
    const auto limits=numeric_editor::limits();
    if(domain::own_processing_contract(input,limits.frame,limits.max_owned_metadata_bytes,input_owner)!=signal_contracts::Status::ok ||
       domain::own_processing_contract(scalar,limits.frame,limits.max_owned_metadata_bytes,scalar_owner)!=signal_contracts::Status::ok)return false;
    node.inputs[0].contract=std::move(input_owner);node.outputs[0].contract=std::move(scalar_owner);return true;
}
bool adapt_connection_clock(const domain::Edge& stable,domain::NumericProcessingGraph& graph,const NodeCheck& valid_node) {
    domain::ConfiguredProcessingNode *from=nullptr,*to=nullptr;
    for(auto& node:graph.nodes){if(node.id==stable.from)from=&node;if(node.id==stable.to)to=&node;}
    if(!from || !to)return false;
    const domain::ProcessingPort* output=nullptr;for(const auto& p:from->outputs)if(p.id==stable.output_port)output=&p;
    if(!output)return false;const auto data=output->contract.view();const auto* numeric=std::get_if<signal_contracts::NumericContract>(&data);
    if(!numeric)return false;
    if(to->kind=="IndexExtract"){if(!adopt_extract_input(*to,*numeric))return false;graph.edges.push_back(stable);return propagate_clock(graph,to->id,valid_node);}
    if(to->kind=="InputSelector"){
        // Each source group may supply a different bounded PCM chunk size.
        // Adopt only this lane's real input contract; the selector's existing
        // output envelope and other source groups remain unchanged.
        auto input=std::find_if(to->inputs.begin(),to->inputs.end(),[&](const auto& p){return p.id==stable.input_port;});
        if(input==to->inputs.end())return false;
        input->contract=output->contract;
        domain::ResolvedProcessingNode resolved;
        if(!domain::resolve_processing_node(*to,numeric_editor::limits(),resolved))return false;
        graph.edges.push_back(stable);return true;
    }
    if(draft_view(to->kind)){
        if(!draft_view_accepts(to->kind,*numeric))return false;
        auto input=std::find_if(to->inputs.begin(),to->inputs.end(),[&](const auto& p){return p.id==stable.input_port;});
        if(input==to->inputs.end())return false;
        input->contract=output->contract;
        graph.edges.push_back(stable);return propagate_clock(graph,to->id,valid_node);
    }
    const auto extent=numeric->shape.extents[0];
    if(!numeric_editor::clock_node(*to,numeric->clock,numeric->shape.rank==signal_contracts::Rank::Vector && numeric->shape.axes[0]==signal_contracts::Axis::Sample?&extent:nullptr))return false;
    graph.edges.push_back(stable);return propagate_clock(graph,to->id,valid_node);
}
bool propagate_clock(domain::NumericProcessingGraph& graph,domain::NodeId root,const NodeCheck& valid_node) {
    const auto find_node=[&](domain::NodeId id)->domain::ConfiguredProcessingNode*{for(auto& n:graph.nodes)if(n.id==id)return &n;return nullptr;};
    const auto find_port=[](auto& ports,domain::PortId id)->domain::ProcessingPort*{for(auto& p:ports)if(p.id==id)return &p;return nullptr;};
    std::vector<domain::NodeId> queue{root};std::set<domain::NodeId> visited;
    for(std::size_t cursor=0;cursor<queue.size();++cursor){
        const auto id=queue[cursor];if(!visited.insert(id).second)continue;
        auto* from=find_node(id);if(!from)return false;
        for(const auto& edge:graph.edges)if(edge.from==id){
            auto* to=find_node(edge.to);auto* output=find_port(from->outputs,edge.output_port);if(!to || !output)return false;
            auto* input=find_port(to->inputs,edge.input_port);if(!input)return false;
            const auto source=output->contract.view(),target=input->contract.view();
            const auto* clock=std::get_if<signal_contracts::NumericContract>(&source);const auto* old=std::get_if<signal_contracts::NumericContract>(&target);
            if(!clock || !old)continue;
            const auto extent=clock->shape.extents[0];
            if(to->kind=="IndexExtract"){if(!adopt_extract_input(*to,*clock))return false;}
            else if(draft_view(to->kind)){if(!draft_view_accepts(to->kind,*clock))return false;input->contract=output->contract;}
            else if(!numeric_editor::clock_node(*to,clock->clock,clock->shape.rank==signal_contracts::Rank::Vector && clock->shape.axes[0]==signal_contracts::Axis::Sample?&extent:nullptr))return false;
            if(!visited.contains(to->id))queue.push_back(to->id);
        }
    }
    for(const auto id:visited){const auto* node=find_node(id);if(!node || !valid_node(*node))return false;}
    // A second source feeding a multi-input processor must agree explicitly.
    // Never silently re-clock that independent upstream branch.
    for(const auto& edge:graph.edges){auto* from=find_node(edge.from);auto* to=find_node(edge.to);if(!from || !to)return false;
        const auto* out=find_port(from->outputs,edge.output_port);const auto* in=find_port(to->inputs,edge.input_port);
        if(!out || !in || (!domain::processing_ports_compatible(*out,*in) && !domain::processing_ports_dtype_convertible(*out,*in)))return false;
    }
    return true;
}
}
