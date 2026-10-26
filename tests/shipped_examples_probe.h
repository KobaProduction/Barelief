#pragma once
#include "synthetic_capture.h"
#include <pipeline/numeric_engine.h>
#include <desktop/numeric_project_document.h>
#include <domain/graph/preparation_values.h>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QThread>
#include <map>
#include <tuple>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <type_traits>
#include <optional>

// Test the actual canonical source/executor/owned-delivery path, including
// every DSP branch and graph-view input. Capture remains in-memory.
namespace shipped_probe {
using namespace pipeline;using namespace signal_contracts;
using Key=std::tuple<domain::NodeId,domain::PortDirection,domain::PortId>;
struct Stage {unsigned frames=0;bool positive=false;std::uint64_t first=0,last=0;unsigned unavailable=0;};
struct History {unsigned rows=0;std::uint64_t ordinal=0,capacity=0;bool positive=false,unavailable=false;};

inline bool allows_missing(const domain::NumericProcessingGraph& graph,domain::NodeId id,
                          domain::PortDirection direction,domain::PortId port,std::size_t remaining) {
 if(!remaining)return false;
 const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& item){return item.id==id;});
 if(node==graph.nodes.end())return false;
 if(direction==domain::PortDirection::Output && node->kind=="RhythmAnalyzer"){
  constexpr std::array<domain::PortId,7> optional{0,1,4,5,6,8,10};
  return std::find(optional.begin(),optional.end(),port)!=optional.end();
 }
 if(direction==domain::PortDirection::Output && (node->kind!="NumberAccumulator" || port!=0))return false;
 const auto input=direction==domain::PortDirection::Input?port:0;
 const auto edge=std::find_if(graph.edges.begin(),graph.edges.end(),[&](const auto& value){return value.to==id&&value.input_port==input;});
 return edge!=graph.edges.end()&&allows_missing(graph,edge->from,domain::PortDirection::Output,edge->output_port,remaining-1);
}
template<class Predicate> bool wait(const Predicate& predicate){
 QElapsedTimer timer;timer.start();
 while(timer.elapsed()<5000){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(2);}
 return predicate();
}
inline AudioSourceKind source_kind(const domain::ConfiguredProcessingNode& node){
 auto kind=node.kind=="TestSignal"?AudioSourceKind::TestSignal:AudioSourceKind::SystemOutput;
 if(const auto* params=std::get_if<domain::EndpointProcessingParameters>(&node.parameters))
  for(const auto& p:params->values)if(p.key=="source")if(const auto* literal=std::get_if<NumericLiteral>(&p.value))
   if(const auto* value=std::get_if<std::int64_t>(literal))kind=AudioSourceKind(*value);
 return kind;
}
struct Observation {
 const domain::NumericProcessingGraph& graph;
 std::mutex mutex;std::map<Key,Stage> stages;std::map<domain::NodeId,History> histories;std::map<Key,NumericLiteral> prepared;
 bool bad=false;unsigned opens=0;std::map<AudioSourceKind,unsigned> opened,expected_opens;
 explicit Observation(const domain::NumericProcessingGraph& g):graph(g){for(const auto& node:graph.nodes)register_node(node);}
 auto node(domain::NodeId id)const{return std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==id;});}
 std::uint64_t history_capacity(domain::NodeId id)const {
  const auto producer=node(id);if(producer==graph.nodes.end())return 0;
  const auto ports=producer->outputs.empty()?std::span<const domain::ProcessingPort>(producer->inputs):std::span<const domain::ProcessingPort>(producer->outputs);
  for(const auto& port:ports){const auto c=port.contract.view();
   if(const auto* numeric=std::get_if<NumericContract>(&c);numeric&&numeric->semantic==Semantic::History)return numeric->shape.extents[0].maximum;}
  return 0;
 }
 void register_history(const domain::ConfiguredProcessingNode& configured){
  if(const auto capacity=history_capacity(configured.id)){histories[configured.id].capacity=capacity;return;}
  const auto kind=domain::node_kind(configured.kind);
  if(!kind||domain::node_role(*kind)!=domain::NodeRole::Sink)return;
  for(const auto& edge:graph.edges){
   if(edge.to!=configured.id||edge.output_port!=0)continue;
   if(const auto capacity=history_capacity(edge.from))histories[configured.id].capacity=capacity;
  }
 }
 bool compact_history_port(const domain::ConfiguredProcessingNode& configured,const domain::ProcessingPort& port)const {
  if(!histories.contains(configured.id))return false;
  const auto contract=port.contract.view();const auto* numeric=std::get_if<NumericContract>(&contract);
  return numeric&&numeric->semantic==Semantic::History;
 }
 bool record_prepared_value(const Key& key,const NumericLiteral& value,Status status,domain::PreparationValueKind kind){
  if(status==Status::unavailable&&kind==domain::PreparationValueKind::Routed)return false;
  if(status!=Status::ok){bad=true;return true;}
  if(!std::visit([](auto scalar){return std::isfinite(double(scalar));},value))bad=true;
  prepared.emplace(key,value);return true;
 }
 bool prepared_output(domain::NodeId id,domain::PortId port,const Key& key){
  const auto producer=node(id);if(producer==graph.nodes.end())return false;
  const auto capability=domain::preparation_value_capability(*producer,port);if(!capability)return false;
  NumericLiteral value;const auto status=domain::resolve_preparation_value(graph,id,port,value);
  return record_prepared_value(key,value,status,capability.kind);
 }
 std::optional<NumericLiteral> configured_parameter(const domain::ConfiguredProcessingNode& configured,std::string_view key)const {
  const domain::ResolvedProcessingNode view{configured.id,configured.kind,domain::NodeRole::Sink,domain::ProcessingOperation::Sink,
   configured.inputs,configured.outputs,&configured.parameters,configured.enabled};
  domain::ProcessingParameterView value;
  if(domain::resolve_processing_parameter(view,key,value)!=Status::ok)return {};
  if(const auto* literal=std::get_if<NumericLiteral>(&value))return *literal;
  return {};
 }
 bool prepared_default(const domain::ConfiguredProcessingNode& configured,const domain::ProcessingPort& port,const Key& key){
  const auto field=domain::numeric_parameter_key(configured.kind,port.id);if(field.empty())return false;
  const auto value=configured_parameter(configured,field);if(!value){bad=true;return true;}
  return record_prepared_value(key,*value,Status::ok,domain::PreparationValueKind::FiniteScalar);
 }
 bool prepared_port(const domain::ConfiguredProcessingNode& configured,const domain::ProcessingPort& port,domain::PortDirection direction){
  const Key key{configured.id,direction,port.id};
  if(direction==domain::PortDirection::Output)return prepared_output(configured.id,port.id,key);
  const auto edge=std::find_if(graph.edges.begin(),graph.edges.end(),[&](const auto& e){return e.to==configured.id&&e.input_port==port.id;});
  return edge==graph.edges.end()?prepared_default(configured,port,key):prepared_output(edge->from,edge->output_port,key);
 }
 void register_ports(const domain::ConfiguredProcessingNode& configured){
  for(const auto& port:configured.outputs)if(!compact_history_port(configured,port)&&!prepared_port(configured,port,domain::PortDirection::Output))stages[{configured.id,domain::PortDirection::Output,port.id}]={};
  if(configured.outputs.empty())for(const auto& port:configured.inputs)
   if(!compact_history_port(configured,port)&&!prepared_port(configured,port,domain::PortDirection::Input))stages[{configured.id,domain::PortDirection::Input,port.id}]={};
 }
 void register_node(const domain::ConfiguredProcessingNode& configured){
  if(configured.kind=="AudioSource"||configured.kind=="TestSignal")++expected_opens[source_kind(configured)];
  if(configured.kind=="AudioSource"||configured.kind=="TestSignal"||configured.kind=="SampleClock"||configured.kind=="PcmResampler")return;
  register_history(configured);
  register_ports(configured);
 }
 void opened_source(AudioSourceKind kind){std::lock_guard lock(mutex);++opens;++opened[kind];}
 bool matches_port(const EndpointIdentity& endpoint,const NumericView& numeric)const {
  const auto configured=node(endpoint.node);
  const auto& ports=endpoint.direction==domain::PortDirection::Output?configured->outputs:configured->inputs;
  const auto port=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==endpoint.port;});
  return port!=ports.end()&&same_contract(port->contract.view(),*numeric.contract);
 }
 template<class Value> static bool element_energy(const NumericView& numeric,std::size_t plane,std::size_t i,double& energy){
  Value x=0;if(load_element(numeric,{plane,i,0},x)!=Status::ok)return false;
  if constexpr(std::is_floating_point_v<Value>){if(!std::isfinite(x))return false;energy+=double(x)*x;}
  else energy+=double(x);
  return true;
 }
 static bool element(const NumericView& numeric,std::size_t plane,std::size_t i,double& energy){
  switch(numeric.contract->dtype){
   case DType::Float32:return element_energy<float>(numeric,plane,i,energy);
   case DType::Float64:return element_energy<double>(numeric,plane,i,energy);
   case DType::UInt8:return element_energy<std::uint8_t>(numeric,plane,i,energy);
   case DType::UInt16:return element_energy<std::uint16_t>(numeric,plane,i,energy);
   case DType::UInt32:return element_energy<std::uint32_t>(numeric,plane,i,energy);
   case DType::UInt64:return element_energy<std::uint64_t>(numeric,plane,i,energy);
   default:return false;
  }
 }
 static bool energy(const NumericView& numeric,double& sum){
  for(std::size_t plane=0;plane<numeric.planes.size();++plane)for(std::size_t i=0;i<numeric.actual[0];++i)
   if(!element(numeric,plane,i,sum))return false;
  return true;
 }
 void observe_stage(const DeliveredNumericFrame& delivered,const EndpointIdentity& endpoint,Stage& state){
  const auto view=delivered.frame.view();const auto* numeric=std::get_if<NumericView>(&view.payload);
  if(!numeric||!matches_port(endpoint,*numeric)){bad=true;return;}
  double sum=0;if(!energy(*numeric,sum)){bad=true;return;}
  update_stage(state,view.stamp.sequence,sum);
 }
 void observe(const DeliveredNumericFrame& delivered){
  if(!delivered.context)return;
  const auto& endpoint=delivered.context->endpoint;std::lock_guard lock(mutex);
  const auto at=stages.find({endpoint.node,endpoint.direction,endpoint.port});if(at==stages.end())return;
  if(delivered.status==Status::unavailable || (delivered.status==Status::ok&&delivered.frame&&!delivered.frame.view().stamp.available)){++at->second.unavailable;return;}
  if(!delivered.frame||!delivered.frame.view().stamp.available)return;
  observe_stage(delivered,endpoint,at->second);
 }
 void update_stage(Stage& state,std::uint64_t sequence,double energy){
  if(!state.frames)state.first=sequence;else if(sequence<state.last)bad=true;
  state.last=sequence;++state.frames;state.positive|=energy>1e-12;
 }
 bool history_payload(const DeliveredNumericFrame& frame,History& history){
  if(!frame.frame)return false;
  const auto view=frame.frame.view();const auto* values=std::get_if<NumericView>(&view.payload);
  if(!values)return false;
  double sum=0;if(!energy(*values,sum))return false;
  history.positive|=sum>1e-12;
  return true;
 }
 void history_lifecycle(const DeliveredNumericHistoryRow& row,History& history){
  if((row.event==NumericHistoryEvent::Empty||row.event==NumericHistoryEvent::Unavailable)&&row.filled_count==0&&row.capacity==history.capacity)history.unavailable=true;
 }
 void observe_history(const DeliveredNumericHistoryRow& row){
  if(!row.compact.context)return;
  const auto& endpoint=row.compact.context->endpoint;std::lock_guard lock(mutex);
  const auto at=histories.find(endpoint.node);if(at==histories.end())return;
  const auto capacity=at->second.capacity;
  if(row.event!=NumericHistoryEvent::Row){history_lifecycle(row,at->second);return;}
  bad|=row.capacity!=capacity||row.filled_count!=std::min(row.accepted_row_ordinal,capacity)||row.accepted_row_ordinal<=at->second.ordinal;
  at->second.ordinal=row.accepted_row_ordinal;++at->second.rows;
  if(!history_payload(row.compact,at->second))bad=true;
 }
 bool scalar_agreement(domain::NodeId id,const domain::ConfiguredProcessingNode& configured)const {
  if(configured.kind!="ScalarView")return false;
  return std::any_of(graph.edges.begin(),graph.edges.end(),[&](const auto& e){
   if(e.to!=id)return false;const auto parent=node(e.from);return parent!=graph.nodes.end()&&parent->kind=="MusicalSpectrogram";
  });
 }
 bool signed_metric(const Key& key,const domain::ConfiguredProcessingNode& configured)const {
  const auto id=std::get<0>(key);
  const bool agreement=configured.kind=="MusicalSpectrogram"&&(std::get<2>(key)==1||std::get<2>(key)==2);
  return agreement||scalar_agreement(id,configured)||configured.kind=="ChannelComparison"||id==221||
   ((configured.kind=="Fourier"||configured.kind=="RealFourier")&&std::get<2>(key)>=2);
 }
 bool stage_ready(const Key& key,const Stage& s)const {
  const auto configured=node(std::get<0>(key));
  if(s.unavailable&&allows_missing(graph,std::get<0>(key),std::get<1>(key),std::get<2>(key),graph.nodes.size()))return true;
  if(domain::numeric_constant_kind(configured->kind))return s.frames&&s.positive;
  const auto& ports=std::get<1>(key)==domain::PortDirection::Output?configured->outputs:configured->inputs;
  const auto port=std::find_if(ports.begin(),ports.end(),[&](const auto& value){return value.id==std::get<2>(key);});
  const auto c=port->contract.view();const auto* numeric=std::get_if<NumericContract>(&c);
  return s.frames>=5&&s.last>s.first&&((numeric&&numeric->shape.rank==Rank::Scalar)||signed_metric(key,*configured)||s.positive);
 }
 bool history_ready(domain::NodeId id,const History& history)const {
  const auto configured=node(id);const auto side=configured->outputs.empty()?domain::PortDirection::Input:domain::PortDirection::Output;
  if(history.unavailable&&allows_missing(graph,id,side,0,graph.nodes.size()))return true;
  return history.rows>=5&&history.positive;
 }
 bool ready()const {
  if(bad)return true;
  for(const auto& [key,s]:stages)if(!stage_ready(key,s))return false;
  for(const auto& [id,h]:histories)if(!history_ready(id,h))return false;
  return true;
 }
 void report(const char* name,bool good)const {
  std::printf("shipped %s: opens=%u stages=%zu history=%zu prepared=%zu %s\n",name,opens,stages.size(),histories.size(),prepared.size(),good?"PASS":"FAIL");
  if(!good)for(const auto& [key,s]:stages)std::printf(" stage%lld/%u/%u frames=%u positive=%d seq=%llu..%llu\n",(long long)std::get<0>(key),unsigned(std::get<1>(key)),unsigned(std::get<2>(key)),s.frames,int(s.positive),(unsigned long long)s.first,(unsigned long long)s.last);
  if(!good)for(const auto& [id,h]:histories)std::printf(" history%lld rows=%u capacity=%llu missing=%d ready=%d\n",(long long)id,h.rows,(unsigned long long)h.capacity,int(h.unavailable),int(history_ready(id,h)));
 }
};
inline bool apply(NumericEngine& engine,const domain::NumericProcessingGraph& graph,const char* name){
 bool replied=false,accepted=false;
 engine.set_reply_handler([&](const auto& reply){replied=true;accepted=reply.accepted;if(!accepted)std::fprintf(stderr,"%s command: %s\n",name,qPrintable(reply.error));});
 QByteArray bytes;if(!desktop::encode_numeric_graph(graph,bytes)){engine.set_reply_handler({});return false;}
 EngineCommand command;command.kind=EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(bytes).object();engine.submit(std::move(command));
 const bool result=wait([&]{return replied;})&&accepted;
 engine.set_reply_handler({});return result;
}
}
inline bool finish_observation(pipeline::NumericEngine& engine,std::uint64_t tap,shipped_probe::Observation& observation,const char* name){
 using namespace shipped_probe;
 const bool delivered=wait([&]{std::lock_guard lock(observation.mutex);return observation.ready();});
 engine.unsubscribe_numeric_tap(tap);
 std::lock_guard lock(observation.mutex);
 const bool good=delivered&&!observation.bad&&observation.opened==observation.expected_opens&&observation.ready();
 observation.report(name,good);return good;
}
inline bool measure_shipped_example(const domain::NumericProcessingGraph& graph,const char* name,pipeline::AudioSourceKind){
 using namespace shipped_probe;
 Observation observation(graph);const auto factory=test_fixture::synthetic_capture_factory();
 NumericEngine engine([&](auto kind){observation.opened_source(kind);return factory(kind);});
 if(!apply(engine,graph,name))return false;
 const auto tap=engine.subscribe_numeric_tap({},[&](const auto& frame){observation.observe(frame);},[&](const auto& row){observation.observe_history(row);});
 if(!tap)return false;
 return finish_observation(engine,tap,observation,name);
}
