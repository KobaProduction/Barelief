#pragma once
#include "synthetic_capture.h"
#include <pipeline/numeric_engine.h>
#include <desktop/numeric_project_document.h>
#include <domain/graph/preparation_values.h>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <map>
#include <tuple>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <type_traits>
#include <optional>
#include <array>

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
template<class Predicate> bool wait(const Predicate& predicate,int timeout=5000){
 QElapsedTimer timer;timer.start();
 while(timer.elapsed()<timeout){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(2);}
 return predicate();
}
inline AudioSourceKind source_kind(const domain::ConfiguredProcessingNode& node){
 auto kind=node.kind=="TestSignal"?AudioSourceKind::TestSignal:AudioSourceKind::SystemOutput;
 if(const auto* params=std::get_if<domain::EndpointProcessingParameters>(&node.parameters))
  for(const auto& p:params->values)if(p.key=="source")if(const auto* literal=std::get_if<NumericLiteral>(&p.value))
   if(const auto* value=std::get_if<std::int64_t>(literal))kind=AudioSourceKind(*value);
 return kind;
}
inline void report_engine_snapshot(const char* phase,const EngineSnapshot& snapshot) {
 QJsonObject statuses;
 for(auto at=snapshot.statuses.cbegin();at!=snapshot.statuses.cend();++at)
  statuses.insert(at.key(),QJsonObject{{"state",int(at->state)},{"error",at->error},{"revision",QString::number(at->revision)}});
 const QJsonObject report{{"epoch",QString::number(snapshot.epoch)},{"revision",QString::number(snapshot.revision)},
  {"graph_revision",QString::number(snapshot.graph_revision)},{"statuses",statuses},{"diagnostics",snapshot.diagnostics}};
 const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Compact);
 std::fprintf(stderr,"%s snapshot=%s\n",phase,bytes.constData());std::fflush(stderr);
}
inline void report_numeric_frame(const char* label,const DeliveredNumericFrame& frame) {
 std::fprintf(stderr,"%s status=%u present=%d delivery=%llu/%llu\n",label,unsigned(frame.status),int(bool(frame.frame)),
  (unsigned long long)frame.delivery.epoch,(unsigned long long)frame.delivery.graph_revision);
 if(!frame.frame)return;
 const auto view=frame.frame.view();
 std::fprintf(stderr," stamp available=%d producer=%llu generation=%llu sequence=%llu\n",int(view.stamp.available),
  (unsigned long long)view.stamp.producer_epoch,(unsigned long long)view.stamp.generation,(unsigned long long)view.stamp.sequence);
 if(const auto* numeric=std::get_if<NumericView>(&view.payload);numeric&&numeric->contract)
  std::fprintf(stderr," numeric dtype=%u rank=%u actual=%zu/%zu planes=%zu semantic=%u quantity=%u\n",unsigned(numeric->contract->dtype),
   unsigned(numeric->contract->shape.rank),numeric->actual[0],numeric->actual[1],numeric->planes.size(),unsigned(numeric->contract->semantic),unsigned(numeric->contract->quantity));
 std::fflush(stderr);
}

struct EvidenceObservation {
 domain::NodeId node=0;
 std::array<float,56> values{};
 std::uint32_t count=0;
 float maturity=0;
 std::uint64_t matrix_sequence=0,count_sequence=0;
 bool table_seen=false;
 bool matrix(const NumericView& view,std::uint64_t sequence) {
  if(view.actual[0]!=8||view.actual[1]!=7)return false;
  for(std::size_t row=0;row<8;++row)for(std::size_t column=0;column<7;++column)
   if(load_element(view,{0,row,column},values[row*7+column])!=Status::ok||!std::isfinite(values[row*7+column]))return false;
  matrix_sequence=sequence;return true;
 }
 bool record(domain::PortId port,const NumericView& view,std::uint64_t sequence) {
  if(port==0)return matrix(view,sequence);
  if(port==1){count_sequence=sequence;return load_element(view,{},count)==Status::ok&&count<=8;}
  if(port==2)return load_element(view,{},maturity)==Status::ok&&std::isfinite(maturity)&&maturity>=0&&maturity<=1;
  std::uint8_t ambiguous=2;
  return port==3&&load_element(view,{},ambiguous)==Status::ok&&ambiguous<=1;
 }
 bool row(std::size_t index)const {
  const auto* value=values.data()+index*7;
  if(index>=count)return std::all_of(value,value+7,[](float x){return x==0;});
  for(std::size_t column=2;column<6;++column)if(value[column]<0)return false;
  return value[0]>=40&&value[0]<=240&&value[1]>=0&&
   value[6]>=1&&value[6]<=15&&std::floor(value[6])==value[6];
 }
 bool observe(domain::PortId port,const NumericView& view,std::uint64_t sequence) {
  if(!record(port,view,sequence))return false;
  if(matrix_sequence!=count_sequence||!matrix_sequence)return true;
  for(std::size_t index=0;index<8;++index)if(!row(index))return false;
  table_seen=true;return true;
 }
 bool ready()const {return !node||(table_seen&&maturity>=.5f);}
};
inline int delivery_timeout(const domain::NumericProcessingGraph& graph) {
 return std::any_of(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.kind=="TempoEvidence";})?10000:5000;
}

inline const domain::ConfiguredProcessingNode* evidence_graph_node(const domain::NumericProcessingGraph& graph,domain::NodeId id) {
    const auto at=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& node){return node.id==id;});
    return at==graph.nodes.end()?nullptr:&*at;
}

inline bool evidence_table_endpoint(const domain::NumericProcessingGraph& graph,domain::NodeId id,
                                    domain::PortDirection direction,domain::PortId port) {
    if(port!=0)return false;
    const auto* node=evidence_graph_node(graph,id);
    if(!node)return false;
    if(direction==domain::PortDirection::Output)return node->kind=="TempoEvidence";
    if(node->kind!="MatrixImageView")return false;
    const auto edge=std::find_if(graph.edges.begin(),graph.edges.end(),[&](const auto& value){return value.to==id&&value.input_port==port;});
    if(edge==graph.edges.end()||edge->output_port!=0)return false;
    const auto* producer=evidence_graph_node(graph,edge->from);
    return producer&&producer->kind=="TempoEvidence";
}

inline bool inactive_route_output(const domain::NumericProcessingGraph& graph,domain::NodeId id,domain::PortId port) {
    const auto* node=evidence_graph_node(graph,id);
    if(!node||node->kind!="Switch")return false;
    const auto* route=std::get_if<domain::RouteProcessingParameters>(&node->parameters);
    return route&&route->channels_per_stream&&port<route->outputs*route->channels_per_stream&&
        (!route->open||port/route->channels_per_stream!=route->selected);
}
inline bool inactive_route_endpoint(const domain::NumericProcessingGraph& graph,domain::NodeId id,
                                    domain::PortDirection direction,domain::PortId port) {
    if(direction==domain::PortDirection::Output)return inactive_route_output(graph,id,port);
    const auto* node=evidence_graph_node(graph,id);
    if(!node)return false;
    const auto kind=domain::node_kind(node->kind);
    if(!kind||domain::node_role(*kind)!=domain::NodeRole::Sink)return false;
    const auto edge=std::find_if(graph.edges.begin(),graph.edges.end(),[&](const auto& e){return e.to==id&&e.input_port==port;});
    return edge!=graph.edges.end()&&inactive_route_output(graph,edge->from,edge->output_port);
}
template<class Value> bool zero_element(const NumericView& view,ElementIndex index) {
    Value value{};
    return load_element(view,index,value)==Status::ok&&value==Value{};
}
inline bool zero_element(const NumericView& view,ElementIndex index) {
    switch(view.contract->dtype){
    case DType::Float32:return zero_element<float>(view,index);
    case DType::Float64:return zero_element<double>(view,index);
    case DType::UInt8:return zero_element<std::uint8_t>(view,index);
    case DType::UInt16:return zero_element<std::uint16_t>(view,index);
    case DType::UInt32:return zero_element<std::uint32_t>(view,index);
    case DType::UInt64:return zero_element<std::uint64_t>(view,index);
    default:return false;
    }
}
inline bool zero_values(const NumericView& view) {
    const auto columns=view.contract->shape.rank==Rank::Matrix?view.actual[1]:1;
    for(std::size_t plane=0;plane<view.planes.size();++plane)
        for(std::size_t row=0;row<view.actual[0];++row)
            for(std::size_t column=0;column<columns;++column)
                if(!zero_element(view,{plane,row,column}))return false;
    return true;
}

struct Observation {
 const domain::NumericProcessingGraph& graph;
 std::mutex mutex;std::map<Key,Stage> stages;std::map<domain::NodeId,History> histories;std::map<Key,NumericLiteral> prepared;
 EvidenceObservation evidence;
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
  if(status!=Status::ok){mark_bad(key,"prepared status");return true;}
  if(!std::visit([](auto scalar){return std::isfinite(double(scalar));},value))mark_bad(key,"prepared nonfinite value");
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
  if(configured.kind=="TempoEvidence")evidence.node=configured.id;
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
 template<class Value> static bool element_energy(const NumericView& numeric,std::size_t plane,std::size_t i,std::size_t column,double& energy){
  Value x=0;if(load_element(numeric,{plane,i,column},x)!=Status::ok)return false;
  if constexpr(std::is_floating_point_v<Value>){if(!std::isfinite(x))return false;energy+=double(x)*x;}
  else energy+=double(x);
  return true;
 }
 static bool element(const NumericView& numeric,std::size_t plane,std::size_t i,std::size_t column,double& energy){
  switch(numeric.contract->dtype){
   case DType::Float32:return element_energy<float>(numeric,plane,i,column,energy);
   case DType::Float64:return element_energy<double>(numeric,plane,i,column,energy);
   case DType::UInt8:return element_energy<std::uint8_t>(numeric,plane,i,column,energy);
   case DType::UInt16:return element_energy<std::uint16_t>(numeric,plane,i,column,energy);
   case DType::UInt32:return element_energy<std::uint32_t>(numeric,plane,i,column,energy);
   case DType::UInt64:return element_energy<std::uint64_t>(numeric,plane,i,column,energy);
   default:return false;
  }
 }
 static bool energy(const NumericView& numeric,double& sum){
  for(std::size_t plane=0;plane<numeric.planes.size();++plane)for(std::size_t i=0;i<numeric.actual[0];++i)
   for(std::size_t column=0;column<(numeric.contract->shape.rank==Rank::Matrix?numeric.actual[1]:1);++column)
    if(!element(numeric,plane,i,column,sum))return false;
  return true;
 }
 bool read_stage_energy(const EndpointIdentity& endpoint,const NumericView& numeric,double& sum){
  if(!energy(numeric,sum)){mark_bad({endpoint.node,endpoint.direction,endpoint.port},"element read or finiteness");return false;}
  if(inactive_route_endpoint(graph,endpoint.node,endpoint.direction,endpoint.port)&&!zero_values(numeric)){mark_bad({endpoint.node,endpoint.direction,endpoint.port},"inactive Switch is not exact typed zero");return false;}
  return true;
 }

 void observe_stage(const DeliveredNumericFrame& delivered,const EndpointIdentity& endpoint,Stage& state){
  const auto view=delivered.frame.view();const auto* numeric=std::get_if<NumericView>(&view.payload);
  if(!numeric||!matches_port(endpoint,*numeric)){mark_bad({endpoint.node,endpoint.direction,endpoint.port},"payload or declared contract mismatch");return;}
  double sum=0;if(!read_stage_energy(endpoint,*numeric,sum))return;
  if(endpoint.node==evidence.node&&!evidence.observe(endpoint.port,*numeric,view.stamp.sequence))mark_bad({endpoint.node,endpoint.direction,endpoint.port},"TempoEvidence count/table/maturity");
  if(state.frames&&view.stamp.sequence<state.last)mark_bad({endpoint.node,endpoint.direction,endpoint.port},"sequence regressed");
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
 void observe_history_row(const DeliveredNumericHistoryRow& row,const EndpointIdentity& endpoint,History& history,std::uint64_t capacity) {
  if(row.capacity!=capacity||row.filled_count!=std::min(row.accepted_row_ordinal,capacity)||row.accepted_row_ordinal<=history.ordinal)
   mark_bad({endpoint.node,endpoint.direction,endpoint.port},"History capacity/filled/ordinal");
  history.ordinal=row.accepted_row_ordinal;++history.rows;
  if(!history_payload(row.compact,history))mark_bad({endpoint.node,endpoint.direction,endpoint.port},"History payload read or finiteness");
 }

 void observe_history(const DeliveredNumericHistoryRow& row){
  if(!row.compact.context)return;
  const auto& endpoint=row.compact.context->endpoint;std::lock_guard lock(mutex);
  const auto at=histories.find(endpoint.node);if(at==histories.end())return;
  const auto capacity=at->second.capacity;
  if(row.event!=NumericHistoryEvent::Row){history_lifecycle(row,at->second);return;}
  observe_history_row(row,endpoint,at->second,capacity);
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
  return s.frames>=5&&s.last>s.first&&((numeric&&numeric->shape.rank==Rank::Scalar)||inactive_route_endpoint(graph,std::get<0>(key),std::get<1>(key),std::get<2>(key))||(evidence.count==0&&evidence_table_endpoint(graph,std::get<0>(key),std::get<1>(key),std::get<2>(key)))||signed_metric(key,*configured)||s.positive);
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
  return evidence.ready();
 }
 void mark_bad(const Key& key,const char* reason) {
  if(!bad)std::fprintf(stderr,"shipped invalid node=%lld direction=%u port=%u reason=%s\n",(long long)std::get<0>(key),unsigned(std::get<1>(key)),unsigned(std::get<2>(key)),reason);
  bad=true;
 }
 void report_contract(const Key& key)const {
  const auto configured=node(std::get<0>(key));if(configured==graph.nodes.end())return;
  const auto& ports=std::get<1>(key)==domain::PortDirection::Output?configured->outputs:configured->inputs;
  const auto port=std::find_if(ports.begin(),ports.end(),[&](const auto& value){return value.id==std::get<2>(key);});if(port==ports.end())return;
  const auto contract=port->contract.view();const auto* numeric=std::get_if<NumericContract>(&contract);if(!numeric)return;
  std::printf("  contract kind=%s dtype=%u rank=%u extents=%llu..%llu/%llu..%llu axes=%u/%u semantic=%u quantity=%u unit=%u\n",configured->kind.c_str(),unsigned(numeric->dtype),unsigned(numeric->shape.rank),
   (unsigned long long)numeric->shape.extents[0].minimum,(unsigned long long)numeric->shape.extents[0].maximum,(unsigned long long)numeric->shape.extents[1].minimum,(unsigned long long)numeric->shape.extents[1].maximum,
   unsigned(numeric->shape.axes[0]),unsigned(numeric->shape.axes[1]),unsigned(numeric->semantic),unsigned(numeric->quantity),unsigned(numeric->unit));
 }
 void report_stage(const Key& key,const Stage& state)const {
  const bool ready=stage_ready(key,state);
  std::printf(" stage%lld/%u/%u frames=%u positive=%d seq=%llu..%llu unavailable=%u ready=%d\n",(long long)std::get<0>(key),unsigned(std::get<1>(key)),unsigned(std::get<2>(key)),state.frames,int(state.positive),(unsigned long long)state.first,(unsigned long long)state.last,state.unavailable,int(ready));
  if(!ready)report_contract(key);
 }

 void report(const char* name,bool good)const {
  std::printf("shipped %s: opens=%u stages=%zu history=%zu prepared=%zu bad=%d %s\n",name,opens,stages.size(),histories.size(),prepared.size(),int(bad),good?"PASS":"FAIL");
  if(!good)for(const auto& [key,state]:stages)report_stage(key,state);
  if(!good)for(const auto& [id,history]:histories)std::printf(" history%lld rows=%u ordinal=%llu capacity=%llu missing=%d ready=%d\n",(long long)id,history.rows,(unsigned long long)history.ordinal,(unsigned long long)history.capacity,int(history.unavailable),int(history_ready(id,history)));
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
inline bool finish_observation(pipeline::NumericEngine& engine,std::uint64_t tap,shipped_probe::Observation& observation,const char* name,int timeout){
 using namespace shipped_probe;
 const bool delivered=wait([&]{std::lock_guard lock(observation.mutex);return observation.ready();},timeout);
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
 return finish_observation(engine,tap,observation,name,delivery_timeout(graph));
}
