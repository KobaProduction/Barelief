#pragma once
#include <domain/graph/node_factory.h>
#include <domain/processing_contracts.h>
#include <optional>
#include <initializer_list>

namespace examples {
using domain::NodeId;
using domain::PortId;
class Builder {
public:
    explicit Builder(domain::NodeFactorySettings settings={});
    domain::ConfiguredProcessingNode& node(NodeId);
    const domain::OwnedProcessingContract& output(NodeId,PortId=0);
    void add(NodeId,std::string_view,std::optional<domain::ProcessingParameters> = {});
    void chain(NodeId,std::string_view,NodeId,std::optional<domain::ProcessingParameters> = {},PortId=0);
    void connect(NodeId,NodeId,PortId=0,PortId=0);
    void derive(NodeId);
    void number(NodeId,double,signal_contracts::NumericUnit=signal_contracts::NumericUnit::Values);
    void configuration(NodeId,NodeId);
    domain::NumericProcessingGraph finish();
    domain::NumericProcessingGraph graph;
    domain::ProcessingAdmissionLimits limits;
private:
    domain::NodeFactorySettings settings_;
};
domain::TransformProcessingParameters transform(std::initializer_list<domain::ProcessingParameter>);
void add_window(Builder&,NodeId source,NodeId buffer,NodeId coefficients,NodeId length,NodeId product,std::uint64_t size,PortId source_port=0);
void add_frequency_step(Builder&,NodeId source,NodeId length,NodeId step);
}
