#include <bdsp_protocol/version_admission.h>
#include <control/numeric_processing_message_mapper.h>
#include <project_storage/numeric_array_codec.h>
#include <bdsp_protocol/numeric_control_codec.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>

namespace control::numeric {
namespace nc=bdsp_protocol::numeric_control;
namespace {
nc::Result failure(nc::Status s) { nc::Result r; r.status=s; return r; }
bool array_parameter(std::string_view key) {
    return key=="values" || key=="gains" || key=="weights" || key=="boundaries_hz" ||
        key=="frequencies_hz";
}
bool own_parameter_value(const nc::Parameter& source,domain::ProcessingParameterValue& out) {
    if(const auto* text=std::get_if<std::string_view>(&source.value)) {
        if(!array_parameter(source.key)){out=std::string(*text);return true;}
        domain::NumericArray array;
        if(project_storage::parse_numeric_array(*text,array)!=signal_contracts::Status::ok)return false;
        out=std::move(array);return true;
    }
    if(const auto* number=std::get_if<nc::Literal>(&source.value))out=*number;
    else out=std::get<bool>(source.value);
    return !array_parameter(source.key);
}
template<class To> bool copy_values(std::span<const nc::Parameter> from, To& to) {
    for(const auto& p:from) {
        domain::ProcessingParameter value;value.key=p.key;
        if(!own_parameter_value(p,value.value))return false;
        to.values.push_back(std::move(value));
    }
    return true;
}
}
namespace {
struct ParameterOwner {
 domain::ProcessingParameters& destination;
 template<class T> bool operator()(const T& source) const {destination=source;return true;}
 bool operator()(const nc::EndpointParameters& source) const {
  domain::EndpointProcessingParameters value;
  if(!copy_values(source.values,value))return false;
  destination=std::move(value);return true;
 }
 bool operator()(const nc::TransformParameters& source) const {
  domain::TransformProcessingParameters value;
  if(!copy_values(source.values,value))return false;
  destination=std::move(value);return true;
 }
 bool operator()(const nc::ReductionParameters& source) const {
  domain::ReduceProcessingParameters value;
  value.operation=source.operation;value.channels=source.channels;value.rows=source.rows;
  value.selected_channels.assign(source.selected_channels.begin(),source.selected_channels.end());
  value.precision_loss=source.precision_loss;value.rounding=source.rounding;
  destination=std::move(value);return true;
 }
 bool operator()(const nc::GeneratorParameters& source) const {
  domain::SignalGeneratorProcessingParameters value;
  value.oscillators.assign(source.oscillators.begin(),source.oscillators.end());
  destination=std::move(value);return true;
 }
 bool operator()(const nc::ClockParameters& source) const {
  destination=domain::SampleClockProcessingParameters{source.sample_rate,std::string(source.domain)};return true;
 }
 bool operator()(const nc::MixerParameters& source) const {
  domain::MixerProcessingParameters value;value.mode=source.mode;
  value.weights.assign(source.weights.begin(),source.weights.end());
  value.fifo_milliseconds=source.fifo_milliseconds;value.consumption=source.consumption;
  destination=std::move(value);return true;
 }
};
bool graph_versions_supported(const domain::ProcessingVersions& versions) {
 return domain::processing_versions_supported(versions)==signal_contracts::Status::ok;
}
nc::Result validate_wire_graph(const nc::Graph& graph,const nc::Limits& limits) {
 nc::Message message;nc::Command command;command.id=nc::CommandId::Prepare;command.graph=graph;message.body=command;
 return nc::validate(message,limits);
}
domain::NumericProcessingGraph graph_envelope(const nc::Graph& source) {
 domain::NumericProcessingGraph graph;graph.graph_id=source.graph_id;graph.versions={source.versions.graph,source.versions.processing};
 graph.execution_interval=source.execution_interval;graph.nodes.reserve(source.nodes.size());
 graph.edges.assign(source.edges.begin(),source.edges.end());
 graph.configuration_bindings.assign(source.configuration_bindings.begin(),source.configuration_bindings.end());
 return graph;
}
struct GraphOwner {
 const nc::Limits& limits;
 std::size_t owned=0;
 nc::Result ports(std::span<const nc::Port> source,std::vector<domain::ProcessingPort>& destination) {
  destination.reserve(source.size());
  for(const auto& port:source) {
   domain::ProcessingPort value;value.id=port.id;value.direction=port.direction;
   const auto status=domain::own_processing_contract(port.contract,limits.frame,limits.arena_bytes-owned,value.contract);
   if(status!=signal_contracts::Status::ok || value.contract.owned_bytes()>limits.arena_bytes-owned)return failure(nc::Status::Limit);
   owned+=value.contract.owned_bytes();destination.push_back(std::move(value));
  }
  return {};
 }
 nc::Result node(const nc::Node& source,domain::ConfiguredProcessingNode& destination) {
  destination.id=source.id;destination.kind=source.kind;destination.enabled=source.enabled;
  const auto parameters=own_parameters(source.parameters,limits,destination.parameters);
  if(!parameters)return parameters;
  auto result=ports(source.inputs,destination.inputs);if(!result)return result;
  result=ports(source.outputs,destination.outputs);return result?parameters:result;
 }
};
}
nc::Result own_parameters(const nc::Parameters& from,const nc::Limits& limits,domain::ProcessingParameters& out) {
 nc::Message message;nc::Command command;
 command.id=nc::CommandId::SetParameters;command.node=0;command.parameters=from;message.body=command;
 const auto result=nc::validate(message,limits);if(!result)return result;
 domain::ProcessingParameters next;
 if(!std::visit(ParameterOwner{next},from))return failure(nc::Status::InvalidArgument);
 out=std::move(next);return result;
}
nc::Result own_graph(const nc::Graph& from,const nc::Limits& limits,domain::NumericProcessingGraph& out) {
 auto result=validate_wire_graph(from,limits);if(!result)return result;
 if(nc::graph_versions_supported(from.versions)!=signal_contracts::Status::ok)return failure(nc::Status::UnsupportedVersion);
 auto next=graph_envelope(from);
 GraphOwner owner{limits};
 for(const auto& node:from.nodes) {
  domain::ConfiguredProcessingNode value;result=owner.node(node,value);if(!result)return result;
  next.nodes.push_back(std::move(value));
 }
 out=std::move(next);return result;
}
namespace {
struct ProjectionBounds {
 const nc::Limits& limits;
 std::size_t text=0,ports=0,parameters=0,metadata=0;
 static bool charge(std::size_t& total,std::size_t count,std::size_t maximum) {
  if(total>maximum || count>maximum-total)return false;
  total+=count;return true;
 }
 bool field(const domain::ProcessingParameter& parameter) {
  if(parameter.key.size()>limits.string_bytes || !charge(text,parameter.key.size(),limits.total_text_bytes))return false;
  if(const auto* value=std::get_if<std::string>(&parameter.value))
   return value->size()<=limits.string_bytes && charge(text,value->size(),limits.total_text_bytes);
  if(const auto* array=std::get_if<domain::NumericArray>(&parameter.value))return array_field(*array);
  return true;
 }
 bool array_field(const domain::NumericArray& array) {
  if(array.values.size()>domain::max_numeric_array_values ||
     !std::all_of(array.values.begin(),array.values.end(),[](float value){return std::isfinite(value);}))return false;
  const auto bytes=16+4*((array.values.size()*sizeof(float)+2)/3);
  return bytes<=limits.string_bytes && charge(text,bytes,limits.total_text_bytes);
 }
 bool fields(std::span<const domain::ProcessingParameter> values) {
  if(!charge(parameters,values.size(),limits.parameters))return false;
  return std::all_of(values.begin(),values.end(),[&](const auto& value){return field(value);});
 }
 template<class T> bool operator()(const T&){return true;}
 bool operator()(const domain::EndpointProcessingParameters& value){return fields(value.values);}
 bool operator()(const domain::TransformProcessingParameters& value){return fields(value.values);}
 bool operator()(const domain::ReduceProcessingParameters& value){return value.selected_channels.size()<=limits.channels;}
 bool operator()(const domain::SignalGeneratorProcessingParameters& value) {
  return value.oscillators.size()<=signal_contracts::max_oscillators && charge(parameters,value.oscillators.size(),limits.parameters);
 }
 bool operator()(const domain::SampleClockProcessingParameters& value) {
  return value.domain.size()<=limits.string_bytes && charge(text,value.domain.size(),limits.total_text_bytes);
 }
 bool operator()(const domain::MixerProcessingParameters& value) {
  return value.weights.size()<=limits.ports && charge(parameters,value.weights.size(),limits.parameters);
 }
 bool contracts(std::span<const domain::ProcessingPort> values) {
  return std::all_of(values.begin(),values.end(),[&](const auto& value) {
   return value.contract && charge(metadata,value.contract.owned_bytes(),limits.arena_bytes);
  });
 }
 bool node(const domain::ConfiguredProcessingNode& value) {
  if(value.kind.size()>limits.string_bytes || !charge(text,value.kind.size(),limits.total_text_bytes) ||
     !charge(ports,value.inputs.size(),limits.ports) || !charge(ports,value.outputs.size(),limits.ports))return false;
  if(!contracts(value.inputs) || !contracts(value.outputs))return false;
  return std::visit([&](const auto& parameters){return (*this)(parameters);},value.parameters);
 }
 bool graph(const domain::NumericProcessingGraph& graph) {
  if(graph.nodes.size()>limits.nodes || graph.edges.size()>limits.edges ||
     graph.configuration_bindings.size()>limits.edges || graph.graph_id.size()>limits.string_bytes)return false;
  text=graph.graph_id.size();
  return std::all_of(graph.nodes.begin(),graph.nodes.end(),[&](const auto& value){return node(value);});
 }
};
struct ValueProjection {
 std::vector<std::string>& encoded;
 template<class T> nc::ParameterValue operator()(const T& value) const {return value;}
 nc::ParameterValue operator()(const std::string& value) const {return std::string_view(value);}
 nc::ParameterValue operator()(const domain::NumericArray& value) const {
  encoded.push_back(project_storage::encode_array_generator_values(value.values));
  return std::string_view(encoded.back());
 }
};
struct ParameterProjection {
 std::vector<nc::Parameter>& values;
 std::vector<std::string>& encoded;
 template<class T> nc::Parameters operator()(const T& value) const {return value;}
 void fields(std::span<const domain::ProcessingParameter> source) const {
  encoded.reserve(source.size());
  for(const auto& field:source)values.push_back({field.key,std::visit(ValueProjection{encoded},field.value)});
 }
 nc::Parameters operator()(const domain::EndpointProcessingParameters& value) const {
  fields(value.values);return nc::EndpointParameters{values};
 }
 nc::Parameters operator()(const domain::TransformProcessingParameters& value) const {
  fields(value.values);return nc::TransformParameters{values};
 }
 nc::Parameters operator()(const domain::ReduceProcessingParameters& value) const {
  return nc::ReductionParameters{value.operation,value.channels,value.rows,value.selected_channels,value.precision_loss,value.rounding};
 }
 nc::Parameters operator()(const domain::SignalGeneratorProcessingParameters& value) const{return nc::GeneratorParameters{value.oscillators};}
 nc::Parameters operator()(const domain::SampleClockProcessingParameters& value) const{return nc::ClockParameters{value.sample_rate,value.domain};}
 nc::Parameters operator()(const domain::MixerProcessingParameters& value) const {
  return nc::MixerParameters{value.mode,value.weights,value.fifo_milliseconds,value.consumption};
 }
};
void project_ports(std::span<const domain::ProcessingPort> source,std::vector<nc::Port>& destination) {
 for(const auto& port:source)destination.push_back({port.id,port.direction,port.contract.view()});
}
}
struct GraphProjection::Storage {
 domain::NumericProcessingGraph graph;
 std::vector<nc::Node> nodes;
 std::vector<std::vector<nc::Port>> inputs,outputs;
 std::vector<std::vector<nc::Parameter>> parameters;
 std::vector<std::vector<std::string>> encoded_arrays;
 nc::Graph wire;
 void prepare(const domain::NumericProcessingGraph& source) {
  graph=source;const auto count=graph.nodes.size();nodes.reserve(count);
  inputs.resize(count);outputs.resize(count);parameters.resize(count);encoded_arrays.resize(count);
 }
 void project_node(std::size_t index) {
  const auto& source=graph.nodes[index];nc::Node node;
  node.id=source.id;node.kind=source.kind;node.enabled=source.enabled;
  project_ports(source.inputs,inputs[index]);project_ports(source.outputs,outputs[index]);
  node.inputs=inputs[index];node.outputs=outputs[index];
  node.parameters=std::visit(ParameterProjection{parameters[index],encoded_arrays[index]},source.parameters);
  nodes.push_back(node);
 }
 void project() {
  for(std::size_t index=0;index<graph.nodes.size();++index)project_node(index);
  const nc::GraphVersions versions{bdsp_protocol::document_version,graph.versions.graph,graph.versions.processing,
      bdsp_protocol::control_version,bdsp_protocol::processing_data_version};
  wire={graph.graph_id,versions,nodes,graph.edges,graph.execution_interval,graph.configuration_bindings};
 }
};
GraphProjection::GraphProjection()=default;
GraphProjection::~GraphProjection()=default;
GraphProjection::GraphProjection(GraphProjection&&) noexcept=default;
GraphProjection& GraphProjection::operator=(GraphProjection&&) noexcept=default;
const nc::Graph& GraphProjection::wire() const noexcept{return storage_->wire;}
nc::Result GraphProjection::assign(const domain::NumericProcessingGraph& from,const nc::Limits& limits) {
 if(!graph_versions_supported(from.versions))return failure(nc::Status::UnsupportedVersion);
 // Bound retained copies before allocation; complete boundary validation precedes publication.
 if(!ProjectionBounds{limits}.graph(from))return failure(nc::Status::Limit);
 auto next=std::make_unique<Storage>();next->prepare(from);next->project();
 const auto result=validate_wire_graph(next->wire,limits);if(!result)return result;
 storage_=std::move(next);return result;
}
namespace {
nc::Reply maximum_reply(const nc::Graph& graph,const nc::Capabilities& capabilities,std::string_view parameter) {
 constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
 nc::Reply reply;reply.command=nc::CommandId::Capabilities;reply.error=nc::Error::Validation;
 reply.diagnostic.node=std::numeric_limits<std::int32_t>::min();
 reply.diagnostic.port=std::numeric_limits<std::uint16_t>::max();reply.diagnostic.edge=maximum;
 reply.diagnostic.parameter=parameter;reply.diagnostic.reason=domain::ProcessingReason::ProfileEncoding;reply.state.lifecycle=nc::Lifecycle::Running;reply.state.run_intent=true;
 reply.state.revision=reply.state.token=maximum;reply.state.applied=graph;
 reply.state.resources={maximum,maximum,maximum,maximum,maximum};reply.capabilities=capabilities;return reply;
}
nc::Result measure_reply_envelope(const nc::Message& message,const nc::Limits& limits) {
 nc::CborCodec cbor;nc::ProtobufCodec protobuf;nc::Result maximum;
 // Enum symbols share the maximum single-byte value class; omitted fields shrink this envelope.
 for(const nc::Codec* codec:{static_cast<const nc::Codec*>(&cbor),static_cast<const nc::Codec*>(&protobuf)}) {
  const auto result=codec->measure(message,limits);if(!result)return result;
  maximum.required_wire=std::max(maximum.required_wire,result.required_wire);
  maximum.required_scratch=std::max(maximum.required_scratch,result.required_scratch);
  maximum.required_arena=std::max(maximum.required_arena,result.required_arena);
  maximum.required_text=std::max(maximum.required_text,result.required_text);
 }
 return maximum;
}
}
nc::Result reply_envelope_fits(const domain::NumericProcessingGraph& graph,const nc::Limits& limits,
                             const nc::Capabilities& capabilities) {
 GraphProjection projection;const auto result=projection.assign(graph,limits);if(!result)return result;
 std::string parameter(limits.string_bytes,'x');
 nc::Message message;
 message.client=message.session=message.epoch=message.request=message.expected_revision=std::numeric_limits<std::uint64_t>::max();
 message.body=maximum_reply(projection.wire(),capabilities,parameter);
 std::array<std::uint8_t,16> revisions{};nc::Extension extension{1024,revisions};message.extensions=std::span(&extension,1);
 return measure_reply_envelope(message,limits);
}
}
