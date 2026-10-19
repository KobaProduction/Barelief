#include <core_app/numeric_default_graph.h>
#include <dataflow_runtime/runtime.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
using namespace domain;
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {std::size_t allocations=0;int failures=0;}
void* operator new(std::size_t size){++allocations;auto* value=std::malloc(size?size:1);if(!value)std::abort();return value;}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* value)noexcept{std::free(value);}void operator delete[](void* value)noexcept{std::free(value);}
void operator delete(void* value,std::size_t)noexcept{std::free(value);}void operator delete[](void* value,std::size_t)noexcept{std::free(value);}
namespace {
#define CHECK(x) do{if(!(x)){std::printf("FAIL line%d %s\n",__LINE__,#x);++failures;}}while(false)
constexpr FrameLimits frame_limits{8,32768,262144,16384,256,8192};
NumericRuntimeProfile profile(){return {{32,64,8,16,256,65536,frame_limits},{{1048576,2*1048576,1048576,65536},0,8388608},{32768,8,262144,262144,262144},{true,true,true}};}
struct Snapshot {std::size_t rows=0,bands=0,capacity=0;std::uint64_t ordinal=0;std::vector<std::uint64_t> bits;};
void appended_preserves(const Snapshot& before,const Snapshot& after){
 CHECK(after.capacity==before.capacity&&after.bands==before.bands&&after.ordinal>before.ordinal);
 if(after.ordinal<=before.ordinal || after.bands!=before.bands)return;
 const auto added=after.ordinal-before.ordinal;
 CHECK(after.rows==std::min<std::uint64_t>(after.capacity,before.rows+added));
 const auto retained=std::min(before.rows,after.rows-std::min<std::uint64_t>(added,after.rows));
 CHECK(std::equal(before.bits.end()-retained*before.bands,before.bits.end(),after.bits.begin()));
}
void positive_latest(const Snapshot& snapshot){
 CHECK(snapshot.rows>0&&snapshot.bands>0);
 bool positive=false;
 for(std::size_t band=0;band<snapshot.bands;++band){
  const auto value=std::bit_cast<float>(std::uint32_t(snapshot.bits[(snapshot.rows-1)*snapshot.bands+band]));
  CHECK(std::isfinite(value));positive|=value>0;
 }
 CHECK(positive);
}
struct Fixture {
 NumericRuntimeProfile p=profile();GraphExecutor executor{p};application::GraphLifecycle lifecycle{executor,executor,PreparedGraph::numeric_capabilities(p),{p.admission,p.resources}};
 NumericProcessingGraph graph=numeric_test_graph();std::uint64_t sequence=0,ordinal=0,filled=0,history_sequence=0;std::size_t events=0;
 NumericContract source;std::array<float,256> samples{};std::array<PlaneLayout,1> planes{{{0,{4,0}}}};
 Fixture(){graph.execution_interval={1,60};source=std::get<NumericContract>(graph.nodes[0].outputs[0].contract.view());}
 ConfiguredProcessingNode& node(NodeId id){auto it=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==id;});CHECK(it!=graph.nodes.end());return *it;}
 bool apply(){auto prepared=lifecycle.prepare_transactional(graph);if(!prepared)std::printf("prepare status=%d error=%u node=%lld port=%u\n",int(prepared.status),unsigned(prepared.numeric_validation.error),static_cast<long long>(prepared.numeric_validation.node),unsigned(prepared.numeric_validation.port));CHECK(bool(prepared));if(!prepared)return false;auto applied=lifecycle.apply_transactional();CHECK(bool(applied));return bool(applied);}
 void check_padding(const NumericView& numeric,const Snapshot& result) {
  for(std::size_t row=0;row<result.capacity-result.rows;++row)
   for(std::size_t band=0;band<result.bands;++band){std::uint64_t zero=1;CHECK(load_bits(numeric,{0,row,band},numeric.contract->dtype,zero)==Status::ok&&zero==0);}
 }
 void read_rows(const NumericView& numeric,Snapshot& result) {
  result.bits.resize(result.rows*result.bands);
  for(std::size_t row=0;row<result.rows;++row)for(std::size_t band=0;band<result.bands;++band)
   CHECK(load_bits(numeric,{0,result.capacity-result.rows+row,band},numeric.contract->dtype,result.bits[row*result.bands+band])==Status::ok);
 }
 Snapshot snapshot(){
  DataFrameView view;std::uint64_t real_filled=0,accepted=0;
  const auto status=executor.plan()->history(7,0,*executor.context(),view,&real_filled,&accepted);
  if(status!=Status::ok){std::printf("History query status=%d\n",int(status));return {};}
  const auto& numeric=std::get<NumericView>(view.payload);
  Snapshot result;result.rows=std::size_t(real_filled);result.capacity=numeric.actual[0];result.ordinal=accepted;result.bands=numeric.actual[1];
  CHECK(result.capacity==numeric.contract->shape.extents[0].maximum&&result.rows<=result.capacity);
  check_padding(numeric,result);read_rows(numeric,result);return result;
 }
 void preserved(const Snapshot& before){auto after=snapshot();CHECK(after.rows==before.rows&&after.capacity==before.capacity&&after.ordinal==before.ordinal&&after.bands==before.bands&&after.bits==before.bits);}
 float sample(std::size_t at) const {
  double value=0;
  for(std::size_t tone=0;tone<8;++tone)value+=.025*std::sin(6.283185307179586*double(sequence*256+at)*(400+tone*701+sequence*.03)/48000.);
  return float(value);
 }
 FrameStamp next_stamp() {
  FrameStamp stamp;stamp.sequence=++sequence;stamp.source_sequence=sequence;stamp.producer_epoch=17;stamp.generation=4;
  stamp.timestamp_ticks=std::int64_t((sequence-1)*256);stamp.stream_id="edit.source";return stamp;
 }
 static void collect(void* user,const NumericHistoryRowView& row) {
  if(row.event!=NumericHistoryEventKind::Row || row.endpoint.direction!=PortDirection::Output || row.endpoint.identity.node!=7)return;
  auto& f=*static_cast<Fixture*>(user);++f.events;
  if(row.accepted_row_ordinal>1){CHECK(!row.endpoint.frame.stamp.discontinuity);CHECK(row.endpoint.frame.stamp.sequence>f.history_sequence);}
  f.history_sequence=row.endpoint.frame.stamp.sequence;f.ordinal=row.accepted_row_ordinal;f.filled=row.filled_count;
 }
 void feed_block() {
  for(std::size_t i=0;i<samples.size();++i)samples[i]=sample(i);
  const auto stamp=next_stamp();const auto before=allocations;
  CHECK(executor.process_numeric_ports(1,{stamp,NumericView{&source,std::as_bytes(std::span(samples)),planes,{256,1},{},{}}},{1,1},{nullptr,nullptr,collect,this})==Status::ok);
  CHECK(allocations==before);
 }
 void feed(std::size_t blocks){for(std::size_t block=0;block<blocks;++block)feed_block();}
 void edit_preserves(const char* label){const auto before=snapshot();const auto prior=ordinal;if(!apply())return;preserved(before);feed(8);
  const auto after=snapshot();CHECK(ordinal>prior&&after.ordinal==ordinal);appended_preserves(before,after);
  std::printf("EDIT %s before%zu after%zu ordinal%llu\n",label,before.rows,after.rows,(unsigned long long)ordinal);}
};
void simple_parameter_edits(Fixture& f){
 std::get<EndpointProcessingParameters>(f.node(1).parameters).values[0].value=NumericLiteral{std::uint64_t{7}};
 f.edit_preserves("source parameter");
 f.node(9).enabled=false;f.edit_preserves("view disabled");f.node(9).enabled=true;f.edit_preserves("view enabled");
 auto& window_fields=std::get<TransformProcessingParameters>(f.node(3).parameters).values;
 const auto function=std::find_if(window_fields.begin(),window_fields.end(),[](const auto& field){return field.key=="function";});
 CHECK(function!=window_fields.end());if(function!=window_fields.end())function->value=std::string("hamming");f.edit_preserves("Hann to Hamming");
}
void insert_gate(Fixture& f){
 const auto contract=f.node(6).inputs[0].contract;
 ConfiguredProcessingNode gate;gate.id=20;gate.kind="Gate";gate.parameters=RouteProcessingParameters{1,0,true};gate.inputs={{0,PortDirection::Input,contract}};gate.outputs={{0,PortDirection::Output,contract}};
 f.graph.nodes.push_back(gate);std::erase_if(f.graph.edges,[](const auto& e){return e.to==6;});f.graph.edges.push_back({15,20,0,0});f.graph.edges.push_back({20,6,0,0});
 f.edit_preserves("insert compatible Gate");
}
void toggle_gate(Fixture& f){
 f.node(20).parameters=RouteProcessingParameters{1,0,false};f.edit_preserves("close Gate");auto zero=f.snapshot();
 CHECK(std::all_of(zero.bits.end()-zero.bands,zero.bits.end(),[](auto value){return value==0;}));
 f.node(20).parameters=RouteProcessingParameters{1,0,true};f.edit_preserves("open Gate");
 positive_latest(f.snapshot());
}
void remove_gate(Fixture& f){
 std::erase_if(f.graph.edges,[](const auto& e){return e.from==20 || e.to==20;});f.graph.edges.push_back({15,6,0,0});std::erase_if(f.graph.nodes,[](const auto& n){return n.id==20;});
 f.edit_preserves("bypass/remove Gate");
}
void resize_fft_contract(ProcessingPort& port){
 auto value=std::get<NumericContract>(port.contract.view());
 if(value.semantic==Semantic::Window || value.semantic==Semantic::Windowed)value.shape.extents[0]={2048,2048};
 else if(value.semantic==Semantic::SpectrumMagnitude){value.shape.extents[0]={1024,1024};value.coordinates[0].step={48000,2048};}
 else return;
 CHECK(own_processing_contract(value,frame_limits,65536,port.contract)==Status::ok);
}
void fft_geometry_edit(Fixture& f){
 // The window geometry changes, but the final Mel50/History contract and clock do not.
 for(auto& n:f.graph.nodes)for(auto* ports:{&n.inputs,&n.outputs})for(auto& port:*ports)resize_fft_contract(port);
 std::get<TransformProcessingParameters>(f.node(2).parameters).values[0].value=NumericLiteral{std::uint64_t{2048}};
 for(const NodeId id:{15,6})for(auto& field:std::get<TransformProcessingParameters>(f.node(id).parameters).values)
  if(field.key=="bin_hz")field.value=NumericLiteral{48000./2048.};
 f.edit_preserves("FFT window4096 to2048 compatible Mel50");
}
Snapshot disconnect_history(Fixture& f){
 const auto before=f.snapshot();CHECK(f.lifecycle.disconnect(6));f.graph=*f.lifecycle.working_numeric_graph();f.preserved(before);f.feed(8);auto zero=f.snapshot();
 appended_preserves(before,zero);
 CHECK(std::all_of(zero.bits.end()-zero.bands,zero.bits.end(),[](auto value){return value==0;}));
 return zero;
}
void reconnect_history(Fixture& f,const Snapshot& zero){
 CHECK(f.lifecycle.connect({15,6,0,0}));f.graph=*f.lifecycle.working_numeric_graph();f.preserved(zero);f.feed(8);auto resumed=f.snapshot();appended_preserves(zero,resumed);positive_latest(resumed);
 const auto newest=std::vector<std::uint64_t>(resumed.bits.end()-resumed.bands,resumed.bits.end());
 f.feed(8);resumed=f.snapshot();positive_latest(resumed);CHECK(!std::equal(newest.begin(),newest.end(),resumed.bits.end()-resumed.bands));
}
void reordered_full_ring(Fixture& f){
 std::reverse(f.graph.nodes.begin(),f.graph.nodes.end());std::reverse(f.graph.edges.begin(),f.graph.edges.end());f.edit_preserves("topology serialization reorder");
 f.feed(1280);CHECK(f.snapshot().rows==f.snapshot().capacity);f.edit_preserves("full ring continuation");
}
void lifecycle_boundaries(Fixture& f){
 auto bad=f.graph;bad.edges.push_back({6,15,0,0});const auto* working=f.executor.plan();CHECK(!f.lifecycle.prepare_transactional(bad));CHECK(f.executor.plan()==working);f.feed(4);
 CHECK(f.lifecycle.shutdown_transactional()==Status::ok);CHECK(!f.executor.running());CHECK(f.apply()&&f.executor.running());f.feed(4);CHECK(f.filled==1&&f.ordinal==1);
}
void compatible_edits(){
 Fixture f;if(!f.apply())return;f.feed(32);CHECK(f.filled>=10);
 simple_parameter_edits(f);insert_gate(f);toggle_gate(f);remove_gate(f);fft_geometry_edit(f);
 const auto zero=disconnect_history(f);reconnect_history(f,zero);reordered_full_ring(f);lifecycle_boundaries(f);
}
}
int main(){compatible_edits();std::printf("graph continuity failures=%d\n",failures);return failures?1:0;}
