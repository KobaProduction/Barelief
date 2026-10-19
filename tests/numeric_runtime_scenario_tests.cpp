#include <dataflow_runtime/runtime.h>
#include <application/processing_control.h>
#include <dsp_kernels/numeric_convert.h>
#include "numeric_test_builders.h"
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <limits>
using namespace signal_contracts;
using namespace domain;
using namespace dataflow_runtime;
namespace {std::size_t allocations=0;int failures=0;}
void* operator new(std::size_t n){++allocations;auto* p=std::malloc(n?n:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
#define CHECK(x) do{if(!(x)){std::printf("FAIL %d: %s\n",__LINE__,#x);++failures;}}while(false)
void fixture_check(bool yes,const char* reason){if(!yes){std::printf("FAIL fixture: %s\n",reason);++failures;}}
constexpr std::array<std::uint32_t,1> channels{42};
const FrameLimits limits{8,32768,65536,8192,256,8192};
NumericRuntimeProfile profile(){return {{32,64,8,16,256,65536,limits},{{1048576,1048576,1048576,65536},0,4194304},{4096,8,65536,65536,65536},{true,true,true}};}
NumericContract vector(DType dt){NumericContract c;c.dtype=dt;c.semantic=Semantic::MelEnergy;c.quantity=Quantity::MelEnergy;
 c.shape.rank=Rank::Vector;c.shape.axes={Axis::Band,Axis::None};c.shape.extents={ExtentBounds{2,2},ExtentBounds{1,1}};
 c.channel_order=channels;c.clock={"numeric.source",{1,1},48000};return c;}
OwnedProcessingContract own(const NumericContract& c){
 return numeric_test_builders::contract(c,limits,65536,fixture_check,"own scenario contract");}
ProcessingPort port(PortId id,PortDirection d,const NumericContract& c){
 return numeric_test_builders::port(id,d,own(c));}
ConfiguredProcessingNode node(NodeId id,const char* kind,const NumericContract* in,const NumericContract* out){
 std::vector<ProcessingPort> inputs,outputs;
 if(in)inputs.push_back(port(0,PortDirection::Input,*in));
 if(out)outputs.push_back(port(0,PortDirection::Output,*out));
 return numeric_test_builders::node(id,kind,EndpointProcessingParameters{},std::move(inputs),std::move(outputs));}
NumericProcessingGraph graph(bool open=true,DType input=DType::UInt64,DType output=DType::Int64){auto u=vector(input),s=vector(output),h=s;
 h.semantic=Semantic::History;h.history_element=s.semantic;h.shape.rank=Rank::Matrix;h.shape.axes={Axis::Time,Axis::Band};h.shape.extents={ExtentBounds{0,3},s.shape.extents[0]};
 NumericContract count;count.dtype=DType::UInt64;count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;count.channel_order=std::span(channels).first(1);count.clock=s.clock;
 auto scalar=count;scalar.dtype=output;scalar.quantity=s.quantity;scalar.unit=s.unit;
 NumericProcessingGraph g;g.graph_id="numeric.vertical";g.nodes.push_back(node(1,"NetworkInput",nullptr,&u));
 auto conv=node(2,"Converter",&u,&s);conv.parameters=ConverterProcessingParameters{};g.nodes.push_back(std::move(conv));
 auto split=node(3,"Splitter",&s,&s);split.parameters=RouteProcessingParameters{};split.outputs.push_back(port(1,PortDirection::Output,s));g.nodes.push_back(std::move(split));
 auto gate=node(4,"Gate",&s,&s);gate.parameters=RouteProcessingParameters{1,0,open};g.nodes.push_back(std::move(gate));
 auto hist=node(5,"Spectrogram",&s,&h);hist.parameters=HistoryProcessingParameters{3};hist.outputs.push_back(port(1,PortDirection::Output,count));g.nodes.push_back(std::move(hist));
 auto reduce=node(6,"ScalarReduce",&h,&scalar);ReduceProcessingParameters rp;rp.channels=ReductionChannels::SelectedChannels;rp.selected_channels={42};rp.rows=ReductionRows::LatestHistoryRow;reduce.parameters=rp;g.nodes.push_back(std::move(reduce));
 g.nodes.push_back(node(7,"NetworkOutput",&scalar,nullptr));g.nodes.push_back(node(8,"NetworkOutput",&count,nullptr));
 g.nodes.push_back(node(9,"NetworkOutput",&s,nullptr));
 g.nodes.push_back(node(10,"SpectrogramView",&h,nullptr));
 g.edges={{1,2,0,0},{2,3,0,0},{3,4,0,0},{4,5,0,0},{5,6,0,0},{6,7,0,0},{5,8,1,0},{3,9,1,0},{5,10,0,0}};return g;}
struct Capture{std::int64_t maximum=-1;std::uint64_t count=0,ordinal=0;std::size_t rows=0,input_rows=0,history_bytes=0,unavailable=0;std::int64_t raw=0;FrameStamp stamp;};
void sink(void* p,const NumericPortFrameView& f){auto& c=*static_cast<Capture*>(p);
 CHECK(validate_view(f.frame,limits)==(f.frame.stamp.available?Status::ok:Status::unavailable));
 if(f.status!=Status::ok){++c.unavailable;CHECK(!f.frame.stamp.available);return;}
 CHECK(f.status==Status::ok);const auto& n=std::get<NumericView>(f.frame.payload);
 if(f.identity.node==7){CHECK(load_element(n,{0,0,0},c.maximum)==Status::ok);c.stamp=f.frame.stamp;}
 if(f.identity.node==8)CHECK(load_element(n,{0,0,0},c.count)==Status::ok);
 if(f.identity.node==9)CHECK(load_element(n,{0,1,0},c.raw)==Status::ok);}
void history(void* p,const NumericHistoryRowView& f){auto& c=*static_cast<Capture*>(p);if(f.event!=NumericHistoryEventKind::Row)return;
 if(f.endpoint.direction==PortDirection::Input){CHECK(f.endpoint.identity.node==10&&f.endpoint.identity.port==0);++c.input_rows;
 CHECK(f.accepted_row_ordinal==c.ordinal&&f.filled_count==std::min(c.ordinal,std::uint64_t{3}));return;}
 ++c.rows;c.ordinal=f.accepted_row_ordinal;c.history_bytes=f.compact.payload_bytes;CHECK(f.compact.frame.stamp.sequence==f.endpoint.frame.stamp.sequence);
 CHECK(std::get<NumericView>(f.compact.frame.payload).contract->semantic==Semantic::MelEnergy);}
DataFrameView frame(const NumericContract& c,std::array<std::byte,32>& b,std::uint64_t sequence){static constexpr std::array<PlaneLayout,2> pl{{{0,{8,0}},{16,{8,0}}}};
 MutableNumericView m{&c,b,std::span(pl).first(1),{2,1},{},{}};const auto base=(std::uint64_t{1}<<54)+sequence*10;
 CHECK(store_element(m,{0,0,0},base)==Status::ok);CHECK(store_element(m,{0,1,0},base+1)==Status::ok);

 FrameStamp s;s.sequence=sequence;s.source_sequence=sequence;s.producer_epoch=17;s.generation=4;s.timestamp_ticks=-123;s.stream_id="original";
 return numeric_test_builders::frame(s,c,b,std::span(pl).first(1),{2,1});}
void scenario(){auto p=profile();GraphExecutor executor(p);application::PreparedExecution pending;auto g=graph();
 CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);if(!pending)return;
 CHECK(executor.apply(std::move(pending))==Status::ok);CHECK(executor.plan()->numeric_definition()!=nullptr);
 auto u=vector(DType::UInt64);std::array<std::byte,32>b{};Capture c;NumericRuntimeCallbacks cb{nullptr,sink,history,&c};
 for(std::uint64_t seq=1;seq<=5;++seq){auto f=frame(u,b,seq);auto before=allocations;CHECK(executor.process_numeric_ports(1,f,{11,12},cb)==Status::ok);CHECK(allocations==before);
 CHECK(c.maximum==std::int64_t((std::uint64_t{1}<<54)+seq*10+1));CHECK(c.raw==c.maximum);CHECK(c.count==std::min(seq,std::uint64_t{3}));CHECK(c.ordinal==seq);CHECK(c.history_bytes==16);CHECK(c.input_rows==seq);}
 CHECK(c.stamp.timestamp_ticks==-123&&c.stamp.producer_epoch==17&&c.stamp.stream_id=="original");
 auto duplicate=frame(u,b,5);auto rows=c.rows;CHECK(executor.process_numeric_ports(1,duplicate,{},cb)==Status::would_block);CHECK(c.rows==rows);
 auto closed=graph(false);CHECK(executor.prepare_numeric(closed,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);CHECK(executor.apply(std::move(pending))==Status::ok);
 auto f=frame(u,b,6);CHECK(executor.process_numeric_ports(1,f,{},cb)==Status::ok);CHECK(c.maximum==0);CHECK(c.raw==std::int64_t((std::uint64_t{1}<<54)+61));CHECK(c.count==3);CHECK(c.ordinal==6);
 // Disconnect a known clocked Gate input: typed silence, preserved compatible History.
 closed.edges.erase(closed.edges.begin()+2);CHECK(executor.prepare_numeric(closed,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);CHECK(executor.apply(std::move(pending))==Status::ok);
 f=frame(u,b,7);CHECK(executor.process_numeric_ports(1,f,{},cb)==Status::ok);CHECK(c.maximum==0&&c.count==3&&c.ordinal==7);
 CHECK(executor.shutdown()==Status::ok);CHECK(executor.process_numeric_ports(1,f,{},cb)==Status::would_block);
 // Teardown removes remembered causal clocks. Re-admit the explicit source route.
 closed.edges.push_back({3,4,0,0});CHECK(executor.prepare_numeric(closed,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);CHECK(executor.apply(std::move(pending))==Status::ok);
 f=frame(u,b,8);CHECK(executor.process_numeric_ports(1,f,{},cb)==Status::ok);CHECK(c.count==1&&c.ordinal==1);
 // A rejected preparation retains the working graph and last compatible history.
 auto bad=closed;bad.nodes[1].kind="Unknown";CHECK(executor.prepare_numeric(bad,application::NumericRuntimeLimits{p.admission,p.resources},pending)!=Status::ok);CHECK(!pending);
 f=frame(u,b,9);CHECK(executor.process_numeric_ports(1,f,{},cb)==Status::ok);CHECK(c.count==2);
 auto resource=p;resource.resources.max_peak_bytes=1;CHECK(executor.prepare_numeric(closed,application::NumericRuntimeLimits{resource.admission,resource.resources},pending)==Status::exhausted);CHECK(!pending);
 resource=p;resource.capabilities.converter=false;GraphExecutor no_converter(resource);CHECK(no_converter.prepare_numeric(closed,application::NumericRuntimeLimits{resource.admission,resource.resources},pending)!=Status::ok);CHECK(!pending);
 f=frame(u,b,10);CHECK(executor.process_numeric_ports(1,f,{},cb)==Status::ok);CHECK(c.count==3);
 // No previous clock and no edge: the disconnected sink gets unavailable, never stale/transit data.
 auto s=vector(DType::Int64);closed.nodes.push_back(node(11,"NetworkOutput",&s,nullptr));
 CHECK(executor.prepare_numeric(closed,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);CHECK(executor.apply(std::move(pending))==Status::ok);
 f=frame(u,b,11);CHECK(executor.process_numeric_ports(1,f,{},cb)==Status::ok);CHECK(c.unavailable==1&&c.ordinal==4);
 std::rotate(closed.nodes.begin(),closed.nodes.begin()+3,closed.nodes.end());
 CHECK(executor.prepare_numeric(closed,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);CHECK(executor.apply(std::move(pending))==Status::ok);
 f=frame(u,b,12);CHECK(executor.process_numeric_ports(1,f,{},cb)==Status::ok);CHECK(c.count==3&&c.ordinal==5);
 // Rejected runtime values do not append a row or deliver a previous value.
 auto reconnected=graph(false);reconnected.nodes.push_back(node(11,"NetworkOutput",&s,nullptr));
 CHECK(executor.prepare_numeric(reconnected,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);CHECK(executor.apply(std::move(pending))==Status::ok);
 auto too_large=frame(u,b,13);auto view=std::get<NumericView>(too_large.payload);MutableNumericView m{view.contract,b,view.planes,view.actual,{},{}};
 CHECK(store_element(m,{0,1,0},UINT64_MAX)==Status::ok);rows=c.rows;
 CHECK(executor.process_numeric_ports(1,too_large,{},cb)!=Status::ok);CHECK(c.rows==rows);
}
struct BitsCapture{DType type;std::uint64_t expected;bool seen=false;};
void bits_sink(void* p,const NumericPortFrameView& f){if(f.identity.node!=7)return;auto& c=*static_cast<BitsCapture*>(p);
 CHECK(f.status==Status::ok);const auto& v=std::get<NumericView>(f.frame.payload);std::uint64_t bits=0;
 CHECK(v.contract->dtype==c.type);CHECK(load_bits(v,{0,0,0},c.type,bits)==Status::ok);CHECK(bits==c.expected);c.seen=true;}
void dtype_routes(){for(const auto& dt:dtype_descriptors()){
 auto g=graph(true,dt.type,dt.type);auto p=profile();GraphExecutor executor(p);application::PreparedExecution pending;
 CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);if(!pending)continue;CHECK(executor.apply(std::move(pending))==Status::ok);
 auto contract=vector(dt.type);std::array<std::byte,32> bytes{};constexpr std::array<PlaneLayout,2> pl{{{0,{8,0}},{16,{8,0}}}};
 MutableNumericView v{&contract,bytes,std::span(pl).first(1),{2,1},{},{}};std::uint64_t expected=0;
 for(std::size_t channel=0;channel<1;++channel)for(std::size_t element=0;element<2;++element){const auto value=1+channel*2+element;
 std::uint64_t bits=value;if(dt.type==DType::Float32)bits=std::bit_cast<std::uint32_t>(float(value));
 if(dt.type==DType::Float64)bits=std::bit_cast<std::uint64_t>(double(value));
 CHECK(store_bits(v,{channel,element,0},dt.type,bits)==Status::ok);expected=bits;}
 BitsCapture capture{dt.type,expected};FrameStamp stamp;stamp.sequence=1;stamp.producer_epoch=1;
 auto before=allocations;CHECK(executor.process_numeric_ports(1,{stamp,v.view()},{},{nullptr,bits_sink,nullptr,&capture})==Status::ok);CHECK(allocations==before);CHECK(capture.seen);
 g.nodes[6].enabled=false;CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);
 CHECK(executor.apply(std::move(pending))==Status::ok);capture.expected=0;capture.seen=false;stamp.sequence=2;
 before=allocations;CHECK(executor.process_numeric_ports(1,{stamp,v.view()},{},{nullptr,bits_sink,nullptr,&capture})==Status::ok);CHECK(allocations==before);CHECK(capture.seen);
}}
NumericProcessingGraph dsp_graph(){NumericContract pcm;pcm.dtype=DType::Float32;pcm.semantic=Semantic::PCM;pcm.quantity=Quantity::Amplitude;pcm.unit=NumericUnit::NormalizedAmplitude;
 pcm.shape.rank=Rank::Vector;pcm.shape.axes={Axis::Sample,Axis::None};pcm.shape.extents={ExtentBounds{1,4096},ExtentBounds{1,1}};pcm.channel_order=channels;
 pcm.clock={"audio.test",{1,48000},48000};auto win=pcm;win.semantic=Semantic::Window;win.shape.extents[0]={512,512};auto windowed=win;windowed.semantic=Semantic::Windowed;
 auto spectrum=win;spectrum.semantic=Semantic::SpectrumMagnitude;spectrum.quantity=Quantity::SpectrumMagnitude;spectrum.unit=NumericUnit::Values;spectrum.shape.axes[0]=Axis::Bin;spectrum.shape.extents[0]={256,256};
 spectrum.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{48000,512}};auto mono=spectrum;mono.channel_order=std::span(channels).first(1);
 auto mel=mono;mel.semantic=Semantic::MelEnergy;mel.quantity=Quantity::MelEnergy;mel.shape.axes[0]=Axis::Band;mel.shape.extents[0]={8,8};mel.coordinates[0]={};
 auto hist=mel;hist.semantic=Semantic::History;hist.history_element=Semantic::MelEnergy;hist.shape.rank=Rank::Matrix;hist.shape.axes={Axis::Time,Axis::Band};hist.shape.extents={ExtentBounds{0,3},mel.shape.extents[0]};hist.coordinates={AxisCoordinates{},mel.coordinates[0]};
 NumericContract count;count.dtype=DType::UInt64;count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;count.channel_order=mono.channel_order;count.clock=pcm.clock;
 NumericProcessingGraph g;g.graph_id="typed.dsp";g.execution_interval={1,60};g.nodes.push_back(node(1,"TestSignal",nullptr,&pcm));
 auto transform=[&](NodeId id,const char* kind,const NumericContract& from,const NumericContract& to){auto n=node(id,kind,&from,&to);n.parameters=TransformProcessingParameters{};return n;};
 auto window=transform(2,"Window",pcm,win);std::get<TransformProcessingParameters>(window.parameters).values.push_back({"size",NumericLiteral{std::uint64_t{512}}});g.nodes.push_back(std::move(window));
 ConfiguredProcessingNode length;length.id=20;length.kind="ArrayLength";length.parameters=TransformProcessingParameters{};length.inputs={port(0,PortDirection::Input,win)};OwnedProcessingContract size;CHECK(numeric_metadata_output_contract(win,false,size)==Status::ok);length.outputs={{0,PortDirection::Output,size}};
 auto function=transform(3,"WindowFunction",win,windowed);function.inputs={{0,PortDirection::Input,size}};std::get<TransformProcessingParameters>(function.parameters).values.push_back({"size",NumericLiteral{std::uint64_t{512}}});OwnedProcessingContract coefficients;CHECK(array_generator_output_contract(function,std::get<NumericContract>(size.view()),coefficients)==Status::ok);function.outputs={{0,PortDirection::Output,coefficients}};g.nodes.push_back(function);auto product=transform(21,"ArrayMultiply",win,win);product.inputs.push_back({1,PortDirection::Input,coefficients});
 windowed=win;g.nodes.push_back(transform(4,"StereoFFT",windowed,spectrum));g.nodes.push_back(transform(5,"BarkWeighting",spectrum,spectrum));
 g.nodes.push_back(transform(6,"SpectrumGain",spectrum,mono));g.nodes.push_back(transform(7,"SpectrumGain",mono,mono));
 auto bands=transform(8,"Mel",mono,mel);std::get<TransformProcessingParameters>(bands.parameters).values.push_back({"bands",NumericLiteral{std::uint64_t{8}}});g.nodes.push_back(std::move(bands));
 auto acc=node(9,"Spectrogram",&mel,&hist);acc.parameters=HistoryProcessingParameters{3};acc.outputs.push_back(port(1,PortDirection::Output,count));g.nodes.push_back(std::move(acc));
 g.nodes.push_back(node(10,"SpectrogramView",&hist,nullptr));g.nodes.push_back(node(11,"NetworkOutput",&count,nullptr));
 for(NodeId id=1;id<10;++id)g.edges.push_back({id,id+1,0,0});
 g.edges.push_back({9,11,1,0});std::erase_if(g.edges,[](const auto& e){return e.to==3 || e.from==3;});g.edges.insert(g.edges.end(),{{2,20},{20,3},{2,21,0,0},{3,21,0,1},{21,4}});g.nodes.push_back(std::move(length));g.nodes.push_back(std::move(product));return g;
}
struct DspCapture{std::size_t windows=0,rows=0;std::uint64_t count=0,ordinal=0;std::int64_t timestamp=0;float first=0;bool changed=false;};
void dsp_output(void* p,const NumericPortFrameView& f){auto& c=*static_cast<DspCapture*>(p);CHECK(f.status==Status::ok);
 if(f.identity.node==2){++c.windows;CHECK(f.frame.stamp.sequence==c.windows);c.timestamp=f.frame.stamp.timestamp_ticks;}}
void dsp_history(void* p,const NumericHistoryRowView& f){if(f.event!=NumericHistoryEventKind::Row || f.endpoint.direction!=PortDirection::Output)return;auto& c=*static_cast<DspCapture*>(p);++c.rows;
 c.ordinal=f.accepted_row_ordinal;c.count=f.filled_count;float energy=0;const auto& row=std::get<NumericView>(f.compact.frame.payload);
 CHECK(load_element(row,{0,0,0},energy)==Status::ok);CHECK(std::isfinite(energy)&&energy>=0);
 if(c.rows==1)c.first=energy;else if(energy!=c.first)c.changed=true;
 CHECK(f.compact.payload_bytes==8*sizeof(float));}
void float_dsp_scenario(bool retag=false){auto g=dsp_graph();
 if(retag)for(auto& n:g.nodes)for(auto* ports:{&n.inputs,&n.outputs})for(auto& port:*ports){
  auto c=std::get<NumericContract>(port.contract.view());c.semantic=Semantic::Scalar;c.history_element=Semantic::Scalar;port.contract=own(c);}
 auto p=profile();p.admission.frame.max_elements=32768;GraphExecutor executor(p);application::PreparedExecution pending;
 CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);if(!pending)return;CHECK(executor.apply(std::move(pending))==Status::ok);
 const auto contract=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());std::array<float,8192> samples{};std::array<PlaneLayout,2> layout{{{0,{4,0}},{4096*sizeof(float),{4,0}}}};
 DspCapture capture;for(std::size_t block=0;block<2;++block){for(std::size_t index=0;index<4096;++index){float value=0;
 for(std::size_t tone=0;tone<6;++tone)value+=.05f*std::sin(float(6.283185307179586*(double(index+block*4096)/48000)*(400+tone*701+block*31)));
 samples[index]=value;samples[4096+index]=value*.75f;}
 FrameStamp stamp;stamp.sequence=block+1;stamp.source_sequence=block+1;stamp.producer_epoch=5;stamp.timestamp_ticks=std::int64_t(block*4096);
 NumericView view{&contract,std::as_bytes(std::span(samples)),std::span(layout).first(1),{4096,1},{},{},false};auto before=allocations;
 CHECK(executor.process_numeric_ports(1,{stamp,view},{},{dsp_output,nullptr,dsp_history,&capture})==Status::ok);CHECK(allocations==before);}
 CHECK(capture.windows==10&&capture.rows==10&&capture.ordinal==10&&capture.count==3&&capture.timestamp==8000&&capture.changed);
 auto disconnected=g;disconnected.edges.erase(disconnected.edges.begin()+6);
 CHECK(executor.prepare_numeric(disconnected,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);CHECK(executor.apply(std::move(pending))==Status::ok);
 FrameStamp silence_stamp;silence_stamp.sequence=3;silence_stamp.producer_epoch=5;silence_stamp.timestamp_ticks=8192;
 NumericView silence_view{&contract,std::as_bytes(std::span(samples)),std::span(layout).first(1),{4096,1},{},{},false};
 auto before=allocations;CHECK(executor.process_numeric_ports(1,{silence_stamp,silence_view},{},{dsp_output,nullptr,dsp_history,&capture})==Status::ok);CHECK(allocations==before);
 CHECK(capture.windows==15&&capture.rows==15&&capture.ordinal==15&&capture.count==3&&capture.timestamp==12000);
 // Unsupported native DSP dtype is refused at Prepare, even though routing accepts it.
 for(auto& n:g.nodes)for(auto* ports:{&n.inputs,&n.outputs})for(auto& port:*ports){auto c=std::get<NumericContract>(port.contract.view());if(c.dtype==DType::Float32){c.dtype=DType::Float64;port.contract=own(c);}}
 CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)!=Status::ok);CHECK(!pending);
}
struct Clock final:application::ControlClock {std::uint64_t now=0;std::uint64_t now_ms() const noexcept override{return now;}};
struct Persistence final:application::ProcessingPersistencePort {
 std::optional<NumericProcessingGraph> saved;std::uint64_t revision=0;
 Status persist_applied(const application::ProcessingDTO& dto,std::uint64_t rev) override {
  CHECK(dto.numeric.has_value());saved=dto.numeric;revision=rev;return Status::ok;
 }
};
void control_scenario(){using namespace application;auto p=profile();GraphExecutor executor(p);
 GraphLifecycle lifecycle(executor,executor,PreparedGraph::numeric_capabilities(p),NumericRuntimeLimits{p.admission,p.resources});
 GraphLifecycleControlPort runtime(lifecycle);Clock clock;Persistence persistence;ProcessingCapabilities caps;
 caps.numeric=NumericProcessingCapabilities{PreparedGraph::numeric_capabilities(p),{p.admission,p.resources}};
 caps.control.max_sessions=2;caps.control.max_replay_bytes_per_session=2097152;caps.control.max_replay_bytes_total=4194304;
 caps.control.max_control_scratch_bytes=2097152;caps.explicit_persist=true;
 std::array<ControlSessionSlot,2> slots;ProcessingControlService service(runtime,clock,caps,slots,77,&persistence);
 auto a=service.open_session(1),b=service.open_session(2);CHECK(a&&b);if(!a||!b)return;
 ProcessingDTO dto;dto.numeric=graph();
 auto prepared=service.submit(a.identity,{1,0},PrepareProcessing{&dto});CHECK(prepared.accepted&&prepared.prepared_token);
 CHECK(prepared.numeric_resources&&prepared.numeric_resources->resources.stack_bytes>0&&prepared.numeric_resources->peak_bytes>0);
 auto wider=NumericRuntimeLimits{p.admission,p.resources};++wider.admission.max_nodes;PreparedExecution refused;
 CHECK(executor.prepare_numeric(*dto.numeric,wider,refused)==Status::exhausted&&!refused);
 auto device=p;device.history_enabled=false;GraphExecutor restricted(device);
 CHECK(restricted.prepare_numeric(*dto.numeric,NumericRuntimeLimits{device.admission,device.resources},refused)!=Status::ok&&!refused);
 auto bad=dto;bad.numeric->nodes[1].kind="Unknown";
 CHECK(service.submit(b.identity,{1,0},PrepareProcessing{&bad}).error==ControlError::busy);
 CHECK(!service.submit(a.identity,{2,0},PrepareProcessing{&bad}).accepted);
 CHECK(service.submit(a.identity,{3,0},ApplyProcessing{prepared.prepared_token,"numeric.vertical"}).accepted);
 auto replay=service.submit(a.identity,{3,0},ApplyProcessing{prepared.prepared_token,"numeric.vertical"});
 CHECK(replay.accepted&&replay.state.revision==1&&replay.state.applied_numeric);
 CHECK(service.submit(a.identity,{3,0},ApplyProcessing{prepared.prepared_token,"changed"}).error==ControlError::conflict);
 auto u=vector(DType::UInt64);std::array<std::byte,32> bytes{};Capture capture;auto f=frame(u,bytes,1);
 CHECK(executor.process_numeric_ports(1,f,{},{nullptr,sink,history,&capture})==Status::ok);CHECK(capture.count==1);
 auto update=service.submit(b.identity,{2,1},SetNumericProcessingParameters{4,RouteProcessingParameters{1,0,false},true});
 CHECK(update.accepted&&update.state.revision==2&&update.state.graph_revision==2);
 f=frame(u,bytes,2);CHECK(executor.process_numeric_ports(1,f,{},{nullptr,sink,history,&capture})==Status::ok);
 CHECK(capture.count==2&&capture.maximum==0);
 CHECK(replay.state.applied_numeric&&std::get<RouteProcessingParameters>(replay.state.applied_numeric->nodes[3].parameters).open);
 auto persisted=service.submit(b.identity,{3,2},ExplicitPersist{2});CHECK(persisted.accepted&&persistence.saved&&persistence.revision==2);
 CHECK(!std::get<RouteProcessingParameters>(persistence.saved->nodes[3].parameters).open);
 CHECK(service.submit(a.identity,{4,1},StopProcessing{}).error==ControlError::conflict);
 CHECK(service.submit(a.identity,{5,2},StopProcessing{}).error==ControlError::unsupported);
 CHECK(executor.running());CHECK(service.submit(a.identity,{6,2},StartProcessing{}).error==ControlError::unsupported);
 auto ttl=service.submit(a.identity,{7,2},PrepareProcessing{&dto});CHECK(ttl.accepted&&ttl.prepared_token);
 CHECK(service.submit(b.identity,{4,2},ApplyProcessing{ttl.prepared_token,"numeric.vertical"}).error==ControlError::foreign_preparation);
 clock.now=ttl.expires_at_ms;
 CHECK(service.submit(a.identity,{8,2},ApplyProcessing{ttl.prepared_token,"numeric.vertical"}).error==ControlError::preparation_expired);
 CHECK(runtime.pending_memory_requirements().fits(0));
 auto refreshed=service.submit(b.identity,{5,2},PrepareProcessing{&dto});CHECK(refreshed.accepted&&refreshed.prepared_token!=ttl.prepared_token);
 CHECK(service.close_session(b.identity)==ControlError::none);CHECK(runtime.pending_memory_requirements().fits(0));
 auto state=service.submit(a.identity,{9,2},GetState{});CHECK(state.accepted&&state.state.revision==2&&state.state.graph_revision==2&&executor.running());
}
void merge_scenario(){
 // Canonical DSP admits one stream per port; implicit two-plane merging is refused.
 auto p=profile();auto c=vector(DType::Float32);const std::array<std::uint32_t,2> lanes{7,42};c.channel_order=lanes;
 NumericProcessingGraph g;g.graph_id="legacy.multi-plane.refusal";g.nodes={node(1,"NetworkInput",nullptr,&c)};
 GraphExecutor executor(p);application::PreparedExecution pending;CHECK(executor.prepare_numeric(g,{p.admission,p.resources},pending)!=Status::ok);CHECK(!pending);
}
void local_history_conversion(){auto p=profile();auto g=graph(true,DType::UInt64,DType::UInt64);
 auto h=std::get<NumericContract>(g.nodes[4].outputs[0].contract.view());
 auto converter=node(11,"Converter",&h,&h);converter.parameters=ConverterProcessingParameters{};g.nodes.push_back(std::move(converter));
 g.edges.back()={5,11,0,0};g.edges.push_back({11,10,0,0});
 GraphExecutor executor(p);application::PreparedExecution pending;
 CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);if(!pending)return;
 CHECK(executor.apply(std::move(pending))==Status::ok);auto contract=vector(DType::UInt64);std::array<std::byte,32> bytes{};
 for(std::uint64_t seq=1;seq<=6;++seq){auto f=frame(contract,bytes,seq);auto before=allocations;
  CHECK(executor.process_numeric_ports(1,f,{})==Status::ok);CHECK(allocations==before);
  DataFrameView snapshot;CHECK(executor.plan()->history(11,0,*executor.context(),snapshot)==Status::ok);
  const auto& view=std::get<NumericView>(snapshot.payload);CHECK(validate_view(snapshot,limits)==Status::ok);
  CHECK(view.actual[0]==3);
  const auto filled=std::min(seq,std::uint64_t{3});
  for(std::size_t row=0;row<view.actual[0]-filled;++row){std::uint64_t zero=1;CHECK(load_element(view,{0,row,1},zero)==Status::ok&&zero==0);}
  for(std::size_t row=view.actual[0]-filled;row<view.actual[0];++row){std::uint64_t actual=0;CHECK(load_element(view,{0,row,1},actual)==Status::ok);
   CHECK(actual==(std::uint64_t{1}<<54)+(seq-view.actual[0]+row+1)*10+1);}}
 g.nodes.back().enabled=false;CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);
 CHECK(executor.apply(std::move(pending))==Status::ok);auto f=frame(contract,bytes,7);CHECK(executor.process_numeric_ports(1,f,{})==Status::ok);
 DataFrameView snapshot;CHECK(executor.plan()->history(11,0,*executor.context(),snapshot)==Status::ok);const auto& view=std::get<NumericView>(snapshot.payload);
 std::uint64_t value=1;CHECK(load_element(view,{0,2,1},value)==Status::ok&&value==(std::uint64_t{1}<<54)+71);
 CHECK(load_element(view,{0,1,1},value)==Status::ok&&value==(std::uint64_t{1}<<54)+61);
 // The converter's retained ordinal must rebase when its upstream history is reset.
 g.nodes.back().enabled=true;CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);CHECK(executor.apply(std::move(pending))==Status::ok);
 executor.plan()->reset_history(5,*executor.context());f=frame(contract,bytes,8);CHECK(executor.process_numeric_ports(1,f,{})==Status::ok);
 CHECK(executor.plan()->history(11,0,*executor.context(),snapshot)==Status::ok);const auto& rebased=std::get<NumericView>(snapshot.payload);
 CHECK(rebased.actual[0]==3&&load_element(rebased,{0,2,1},value)==Status::ok&&value==(std::uint64_t{1}<<54)+81);
}
struct IngressArena {
 alignas(std::max_align_t) std::array<std::byte,1024> payload{};
 alignas(std::max_align_t) std::array<std::byte,32768> metadata{};
 std::array<BufferSlot,4> payload_slots,metadata_slots;
 BufferPool payload_pool{payload,payload_slots,256},metadata_pool{metadata,metadata_slots,8192};
};
void external_history_ownership(){auto p=profile();auto row=vector(DType::UInt64),h=row;
 h.semantic=Semantic::History;h.history_element=row.semantic;h.shape.rank=Rank::Matrix;h.shape.axes={Axis::Time,Axis::Band};h.shape.extents={ExtentBounds{0,4},row.shape.extents[0]};
 h.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{},{}};h.coordinates[1]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Values,{},{}};
 auto converted=h;converted.dtype=DType::Int64;
 NumericProcessingGraph g;g.graph_id="external.history";g.nodes.push_back(node(1,"NetworkInput",nullptr,&h));
 auto converter=node(2,"Converter",&h,&converted);converter.parameters=ConverterProcessingParameters{};g.nodes.push_back(std::move(converter));
 g.nodes.push_back(node(3,"SpectrogramView",&converted,nullptr));g.edges={{1,2,0,0},{2,3,0,0}};
 GraphExecutor executor(p);application::PreparedExecution pending;CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);if(!pending)return;
 CHECK(executor.apply(std::move(pending))==Status::ok);auto arena=std::make_shared<IngressArena>();std::weak_ptr<IngressArena> weak=arena;
 std::array<std::byte,128> bytes{};std::array<PlaneLayout,2> planes{{{0,{16,8}},{64,{16,8}}}};
 std::array<double,4> times{1,2,3,4};std::array<double,2> bands{10,20};
 const auto time_bytes=std::as_bytes(std::span(times));const auto band_bytes=std::as_bytes(std::span(bands));
 // Explicit LE encoding; this fixture runs on the project's little-endian Windows target.
 MutableNumericView src{&h,bytes,std::span(planes).first(1),{2,2},{4,3,{}},{{time_bytes.first(16),band_bytes}}};
 for(std::size_t plane=0;plane<1;++plane)for(std::size_t logical=0;logical<2;++logical)for(std::size_t col=0;col<2;++col)
  CHECK(store_element(src,{plane,logical,col},std::uint64_t{100+plane*10+logical*2+col})==Status::ok);
 FrameStamp stamp;stamp.sequence=1;stamp.producer_epoch=88;DataFrameView input{stamp,src.view()};
 CHECK(executor.process_numeric_ports(1,input,{})==Status::invalid_argument);
 OwnedDataFrame owned;CHECK(retain_frame(input,limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok);
 auto before=allocations;CHECK(executor.process_numeric_owned_ports(1,owned,{}, {},100,arena)==Status::ok);CHECK(allocations==before);
 owned={};arena.reset();CHECK(!weak.expired());DataFrameView snapshot;
 CHECK(executor.plan()->history(1,0,*executor.context(),snapshot)==Status::ok);CHECK(validate_view(snapshot,limits)==Status::ok);
 CHECK(executor.plan()->history(2,0,*executor.context(),snapshot)==Status::ok);CHECK(validate_view(snapshot,limits)==Status::ok);
 std::int64_t value=0;CHECK(load_element(std::get<NumericView>(snapshot.payload),{0,1,1},value)==Status::ok&&value==103);
 CHECK(std::equal(std::get<NumericView>(snapshot.payload).coordinates.axes[0].begin(),std::get<NumericView>(snapshot.payload).coordinates.axes[0].end(),time_bytes.begin()));
 arena=weak.lock();src.actual={4,2};src.coordinates.axes[0]=time_bytes;src.ring.oldest=3;
 for(std::size_t plane=0;plane<1;++plane)for(std::size_t logical=0;logical<4;++logical)for(std::size_t col=0;col<2;++col)
  CHECK(store_element(src,{plane,logical,col},std::uint64_t{200+plane*10+logical*2+col})==Status::ok);
 stamp.sequence=2;input={stamp,src.view()};CHECK(retain_frame(input,limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok);
 before=allocations;CHECK(executor.process_numeric_owned_ports(1,owned,{}, {},106,arena)==Status::ok);CHECK(allocations==before);
 CHECK(executor.plan()->history(2,0,*executor.context(),snapshot)==Status::ok);CHECK(validate_view(snapshot,limits)==Status::ok);
 CHECK(load_element(std::get<NumericView>(snapshot.payload),{0,3,1},value)==Status::ok&&value==207);
 owned={};arena.reset();CHECK(executor.shutdown()==Status::ok);CHECK(weak.expired());
 // A disabled source mutes every newly accepted row of a history snapshot,
 // including a full-ring batch; masking just the newest row exposes stale data.
 g.nodes.front().enabled=false;CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);
 CHECK(executor.apply(std::move(pending))==Status::ok);arena=std::make_shared<IngressArena>();stamp.sequence=3;input={stamp,src.view()};
 CHECK(retain_frame(input,limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok);
 before=allocations;CHECK(executor.process_numeric_owned_ports(1,owned,{}, {},107,arena)==Status::ok);CHECK(allocations==before);
 CHECK(executor.plan()->history(2,0,*executor.context(),snapshot)==Status::ok);const auto& muted=std::get<NumericView>(snapshot.payload);
 CHECK(muted.actual[0]==4&&validate_view(snapshot,limits)==Status::ok);
 for(std::size_t plane=0;plane<1;++plane)for(std::size_t row=0;row<4;++row)for(std::size_t col=0;col<2;++col)
  CHECK(load_element(muted,{plane,row,col},value)==Status::ok&&value==0);
 owned={};src.actual={1,2};src.ring.oldest=0;src.coordinates.axes[0]=time_bytes.first(8);stamp.sequence=1;stamp.producer_epoch=89;stamp.discontinuity=true;input={stamp,src.view()};
 CHECK(retain_frame(input,limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok);CHECK(executor.process_numeric_owned_ports(1,owned,{}, {},1,arena)==Status::ok);
 owned={};src.actual={3,2};src.coordinates.axes[0]=time_bytes.first(24);stamp.sequence=2;stamp.discontinuity=false;input={stamp,src.view()};
 CHECK(retain_frame(input,limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok);CHECK(executor.process_numeric_owned_ports(1,owned,{}, {},3,arena)==Status::ok);
 CHECK(executor.plan()->history(2,0,*executor.context(),snapshot)==Status::ok);const auto& batch=std::get<NumericView>(snapshot.payload);CHECK(batch.actual[0]==3);
 for(std::size_t plane=0;plane<1;++plane)for(std::size_t row=0;row<3;++row)for(std::size_t col=0;col<2;++col)CHECK(load_element(batch,{plane,row,col},value)==Status::ok&&value==0);
 // Producer restart may reduce the upstream ordinal without freezing conversion.
 g.nodes.front().enabled=true;CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);
 CHECK(executor.apply(std::move(pending))==Status::ok);owned={};src.actual={2,2};src.ring.oldest=0;src.coordinates.axes[0]=time_bytes.first(16);stamp.sequence=1;stamp.producer_epoch=90;stamp.discontinuity=true;
 input={stamp,src.view()};CHECK(retain_frame(input,limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok);
 CHECK(executor.process_numeric_owned_ports(1,owned,{}, {},2,arena)==Status::ok);
 CHECK(executor.plan()->history(2,0,*executor.context(),snapshot)==Status::ok);CHECK(std::get<NumericView>(snapshot.payload).actual[0]==2);
 CHECK(load_element(std::get<NumericView>(snapshot.payload),{0,1,1},value)==Status::ok&&value!=0);
 // Explicit ring marker identity survives missing numeric storage/ring descriptor.
 owned={};stamp.sequence=2;stamp.discontinuity=false;stamp.available=false;
 CHECK(retain_frame({stamp,NumericView{&h}},limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok);
 CHECK(executor.process_numeric_owned_ports(1,owned,{}, {},2,arena,2)==Status::ok);
 std::uint64_t real=0,ordinal=0;
 CHECK(executor.plan()->history(1,0,*executor.context(),snapshot,&real,&ordinal)==Status::unavailable);
 CHECK(real==2&&ordinal==2&&std::get<NumericView>(snapshot.payload).ring.capacity==4);
 // A dense Matrix is an ordinary current value even with a History label.
 owned={};stamp.available=true;stamp.sequence=3;src.ring={};input={stamp,src.view()};
 CHECK(retain_frame(input,limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok);
 CHECK(executor.process_numeric_owned_ports(1,owned,{}, {},3,arena)==Status::invalid_argument);
 CHECK(executor.process_numeric_owned_ports(1,owned,{}, {},{},arena)==Status::ok);
 CHECK(executor.plan()->history(1,0,*executor.context(),snapshot)==Status::incompatible_contract);
 CHECK(executor.plan()->history(2,0,*executor.context(),snapshot)==Status::incompatible_contract);
}
void whole_history_kernel(){for(const auto& dt:dtype_descriptors()){
 auto h=vector(dt.type);h.semantic=Semantic::History;h.history_element=Semantic::MelEnergy;h.shape.rank=Rank::Matrix;h.shape.axes={Axis::Time,Axis::Band};h.shape.extents={ExtentBounds{0,3},ExtentBounds{2,2}};
 std::array<std::byte,96> source{},output{},scratch{};std::array<PlaneLayout,2> planes{{{0,{16,8}},{48,{16,8}}}};
 std::array<std::byte,3> mask{std::byte{0},std::byte{1},std::byte{0}};
 MutableNumericView src{&h,source,std::span(planes).first(1),{2,2},{3,2,{}},{}};
 std::uint64_t expected=dt.type==DType::UInt64?(std::uint64_t{1}<<54)+7:7;
 if(dt.type==DType::Float32)expected=std::bit_cast<std::uint32_t>(7.f);
 if(dt.type==DType::Float64)expected=std::bit_cast<std::uint64_t>(7.);
 for(std::size_t plane=0;plane<1;++plane)for(std::size_t row=0;row<2;++row)for(std::size_t col=0;col<2;++col)CHECK(store_bits(src,{plane,row,col},dt.type,expected)==Status::ok);
 auto input=src.view();input.ring.silence_mask=mask;MutableNumericView dst{&h,output,std::span(planes).first(1),{2,2},{3,1,{}},{}};
 dsp_kernels::PreparedConversion plan;CHECK(dsp_kernels::prepare_conversion({h,h,{},profile().kernels,limits},{true,true,true},plan)==dsp_kernels::NumericStatus::Ok);
 const auto before=allocations;CHECK(dsp_kernels::convert(plan,input,dst,scratch).status==dsp_kernels::NumericStatus::Ok);CHECK(allocations==before);
 for(std::size_t plane=0;plane<1;++plane)for(std::size_t col=0;col<2;++col){std::uint64_t actual=0;
  CHECK(load_bits(dst.view(),{plane,0,col},dt.type,actual)==Status::ok&&actual==expected);
  CHECK(load_bits(dst.view(),{plane,1,col},dt.type,actual)==Status::ok&&actual==0);}
}}
void local_history_coordinates(){auto p=profile();auto g=graph();
 const AxisCoordinates feature{CoordinateMode::ExplicitFloat64LE,NumericUnit::Values,{},{}};
 const AxisCoordinates time{CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{},{}};
 for(auto& configured:g.nodes)for(auto* ports:{&configured.inputs,&configured.outputs})for(auto& port:*ports){auto contract=std::get<NumericContract>(port.contract.view());
  if(contract.semantic==Semantic::History)contract.coordinates={time,feature};
  else if(contract.shape.rank==Rank::Vector)contract.coordinates[0]=feature;
  port.contract=own(contract);}
 GraphExecutor executor(p);application::PreparedExecution pending;
 CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);if(!pending)return;
 CHECK(executor.apply(std::move(pending))==Status::ok);const auto contract=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
 std::array<std::byte,32> bytes{};std::array<double,2> features{10,20};
 for(std::uint64_t seq=1;seq<=4;++seq){auto f=frame(contract,bytes,seq);f.stamp.timestamp_ticks=std::int64_t(seq)-100;
  std::get<NumericView>(f.payload).coordinates.axes[0]=std::as_bytes(std::span(features));const auto before=allocations;
  CHECK(executor.process_numeric_ports(1,f,{})==Status::ok);CHECK(allocations==before);}
 DataFrameView snapshot;CHECK(executor.plan()->history(5,0,*executor.context(),snapshot)==Status::ok);CHECK(validate_view(snapshot,limits)==Status::ok);
 auto view=std::get<NumericView>(snapshot.payload);CHECK(view.actual[0]==3);
 std::array<double,3> expected_times{-98,-97,-96};const auto expected=std::as_bytes(std::span(expected_times));
 CHECK(view.coordinates.axes[0].size()==expected.size()&&std::equal(view.coordinates.axes[0].begin(),view.coordinates.axes[0].end(),expected.begin()));
 const auto expected_features=std::as_bytes(std::span(features));CHECK(std::equal(view.coordinates.axes[1].begin(),view.coordinates.axes[1].end(),expected_features.begin()));
 g.nodes[4].enabled=false;CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);CHECK(executor.apply(std::move(pending))==Status::ok);
 auto f=frame(contract,bytes,5);f.stamp.timestamp_ticks=-95;std::get<NumericView>(f.payload).coordinates.axes[0]=expected_features;
 CHECK(executor.process_numeric_ports(1,f,{})==Status::ok);CHECK(executor.plan()->history(5,0,*executor.context(),snapshot)==Status::ok);
 CHECK(validate_view(snapshot,limits)==Status::ok);view=std::get<NumericView>(snapshot.payload);std::int64_t value=1;
 CHECK(load_element(view,{0,2,1},value)==Status::ok&&value==0);
 CHECK(load_element(view,{0,1,1},value)==Status::ok&&value==std::int64_t((std::uint64_t{1}<<54)+41));
}
void fixed_pcm_history_resize(){
 auto p=profile();auto pcm=vector(DType::UInt8);pcm.semantic=Semantic::PCM;pcm.quantity=Quantity::Amplitude;pcm.shape.axes[0]=Axis::Sample;pcm.representation.normalization=Normalization::UnsignedUnit;
 auto history=pcm;history.semantic=Semantic::History;history.history_element=Semantic::PCM;history.shape.rank=Rank::Matrix;
 history.shape.axes={Axis::Time,Axis::Sample};history.shape.extents={ExtentBounds{0,4},pcm.shape.extents[0]};
 auto count_graph=graph();auto count=std::get<NumericContract>(count_graph.nodes[4].outputs[1].contract.view());
 NumericProcessingGraph g;g.graph_id="fixed.pcm.history";g.nodes.push_back(node(1,"NetworkInput",nullptr,&pcm));
 auto h=node(5,"Spectrogram",&pcm,&history);h.parameters=HistoryProcessingParameters{4};h.outputs.push_back(port(1,PortDirection::Output,count));g.nodes.push_back(std::move(h));
 g.nodes.push_back(node(10,"SpectrogramView",&history,nullptr));g.edges={{1,5,0,0},{5,10,0,0}};
 GraphExecutor executor(p);application::PreparedExecution prepared;
 auto apply=[&](){CHECK(executor.prepare_numeric(g,{p.admission,p.resources},prepared)==Status::ok);CHECK(executor.apply(std::move(prepared))==Status::ok);};apply();
 auto empty=[&](std::size_t capacity){DataFrameView snapshot;std::uint64_t filled=99,ordinal=99;const auto before=allocations;
  CHECK(executor.plan()->history(10,0,*executor.context(),snapshot,&filled,&ordinal)==Status::ok);CHECK(allocations==before);
  const auto& view=std::get<NumericView>(snapshot.payload);CHECK(validate_view(snapshot,limits)==Status::ok && view.actual[0]==capacity && view.ring.capacity==capacity && filled==0 && ordinal==0 && snapshot.stamp.discontinuity);
  for(std::size_t ch=0;ch<1;++ch)for(std::size_t row=0;row<capacity;++row)for(std::size_t col=0;col<2;++col){std::uint8_t value=0;CHECK(load_element(view,{ch,row,col},value)==Status::ok && value==128);}
 };empty(4);
 std::array<std::byte,4> bytes{};const std::array<PlaneLayout,2> planes{{{0,{1,0}},{2,{1,0}}}};
 MutableNumericView input{&pcm,bytes,std::span(planes).first(1),{2,1},{},{}};
 for(std::size_t ch=0;ch<1;++ch)for(std::size_t col=0;col<2;++col)CHECK(store_element(input,{ch,col,0},std::uint8_t(17+ch*10+col))==Status::ok);
 FrameStamp stamp;stamp.producer_epoch=71;stamp.generation=9;stamp.sequence=1;
 auto send=[&](){const auto before=allocations;CHECK(executor.process_numeric_ports(1,{stamp,input.view()},{})==Status::ok);CHECK(allocations==before);};send();
 auto inspect=[&](std::size_t capacity){DataFrameView snapshot;std::uint64_t filled=0,ordinal=0;
  CHECK(executor.plan()->history(10,0,*executor.context(),snapshot,&filled,&ordinal)==Status::ok);
  const auto& view=std::get<NumericView>(snapshot.payload);CHECK(view.actual[0]==capacity&&filled==1&&ordinal==1);
  CHECK(snapshot.stamp.producer_epoch==71&&snapshot.stamp.generation==9);
  CHECK(validate_view(snapshot,limits)==Status::ok);
  for(std::size_t ch=0;ch<1;++ch)for(std::size_t row=0;row<capacity;++row)for(std::size_t col=0;col<2;++col){std::uint8_t value=0;
   CHECK(load_element(view,{ch,row,col},value)==Status::ok);CHECK(value==(row+1==capacity?17+ch*10+col:128));}
 };inspect(4);
 auto resize=[&](std::size_t capacity){std::get<HistoryProcessingParameters>(g.nodes[1].parameters).capacity=capacity;
  history.shape.extents[0]={0,capacity};g.nodes[1].outputs[0].contract=own(history);g.nodes[2].inputs[0].contract=own(history);apply();inspect(capacity);};
 resize(8);resize(2);
 executor.plan()->reset_history(5,*executor.context());empty(2);++stamp.sequence;send();inspect(2);
 auto unavailable=input.view();unavailable.storage={};++stamp.sequence;stamp.available=false;
 CHECK(executor.process_numeric_ports(1,{stamp,unavailable},{})==Status::ok);
 DataFrameView missing;std::uint64_t retained=0,ordinal=0;
 CHECK(executor.plan()->history(10,0,*executor.context(),missing,&retained,&ordinal)==Status::unavailable && retained==1 && ordinal==1 && !missing.stamp.available);
 CHECK(std::get<NumericView>(missing.payload).storage.empty());
}
void resource_dimensions(){auto p=profile();auto g=graph();std::array<std::size_t,64> workspace{};ProcessingGraphAdmission admitted;
 CHECK(admit_processing_graph(g,p.admission,PreparedGraph::numeric_capabilities(p),workspace,admitted));
 p.resources.maximum.plan_bytes=admitted.resources.plan_bytes;GraphExecutor executor(p);application::PreparedExecution pending;
 CHECK(executor.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::ok);
 CHECK(pending&&pending.plan->memory_requirements().state_bytes>p.resources.maximum.plan_bytes);
 if(pending)CHECK(pending.plan->numeric_admission()->resources.plan_bytes==admitted.resources.plan_bytes);
 --p.resources.maximum.plan_bytes;GraphExecutor insufficient(p);pending={};
 CHECK(insufficient.prepare_numeric(g,application::NumericRuntimeLimits{p.admission,p.resources},pending)==Status::exhausted&&!pending);
}
}
void route_fanout_identity(){auto p=profile();const auto contract=vector(DType::UInt64);NumericProcessingGraph g;g.graph_id="fanout.identity";
 g.nodes.push_back(node(1,"NetworkInput",nullptr,&contract));auto route=node(2,"Splitter",&contract,nullptr);
 route.parameters=RouteProcessingParameters{8,0,true};
 for(PortId port_id=0;port_id<8;++port_id){route.outputs.push_back(port(port_id,PortDirection::Output,contract));
  g.nodes.push_back(node(10+port_id,"NetworkOutput",&contract,nullptr));g.edges.push_back({2,10+port_id,port_id,0});}
 g.nodes.push_back(std::move(route));g.edges.push_back({1,2,0,0});
 GraphExecutor executor(p);application::PreparedExecution pending;
 struct Observations {std::array<unsigned,8> deliveries{};std::uint32_t active=255;std::uint64_t sequence=0,expected=0;const std::byte* payload=nullptr;} observed;
 auto collect=[](void* user,const NumericPortFrameView& f){auto& out=*static_cast<Observations*>(user);CHECK(f.status==Status::ok&&f.frame.stamp.available);
  const auto branch=std::size_t(f.identity.node-10);CHECK(branch<8);if(branch>=8 || f.status!=Status::ok)return;
  ++out.deliveries[branch];const auto& view=std::get<NumericView>(f.frame.payload);std::uint64_t value=0;
  CHECK(load_element(view,{0,1,0},value)==Status::ok);const bool active=(out.active&(1u<<branch))!=0;
  CHECK(value==(active?out.expected:0));if(active)CHECK(view.storage.data()==out.payload);
  CHECK(view.actual==(std::array<std::size_t,2>{2,1}));CHECK(f.delivery.graph_revision==101&&f.delivery.epoch==202);
  CHECK(f.frame.stamp.sequence==out.sequence&&f.frame.stamp.source_sequence==out.sequence&&f.frame.stamp.producer_epoch==17&&
   f.frame.stamp.generation==4&&f.frame.stamp.timestamp_ticks==-123&&f.frame.stamp.stream_id=="original");};
 auto apply=[&]{CHECK(executor.prepare_numeric(g,{p.admission,p.resources},pending)==Status::ok);CHECK(bool(pending));
  if(pending)CHECK(executor.apply(std::move(pending))==Status::ok);};
 auto feed=[&](std::uint32_t active){observed.active=active;observed.deliveries={};std::array<std::byte,32> storage{};
  const auto incoming=frame(contract,storage,++observed.sequence);observed.expected=(std::uint64_t{1}<<54)+observed.sequence*10+1;
  observed.payload=std::get<NumericView>(incoming.payload).storage.data();const auto before=allocations;
  CHECK(executor.process_numeric_ports(1,incoming,{101,202},{nullptr,collect,nullptr,&observed})==Status::ok);CHECK(allocations==before);
  for(auto count:observed.deliveries)CHECK(count==1);};
 apply();feed(255);
 auto& configured=g.nodes.back();configured.kind="Switch";
 for(std::uint16_t selected=0;selected<8;++selected){configured.parameters=RouteProcessingParameters{8,selected,true};apply();feed(1u<<selected);}
 configured.enabled=false;apply();feed(0);configured.enabled=true;configured.parameters=RouteProcessingParameters{8,7,true};apply();feed(128);
 std::printf("ROUTES Splitter1->8 and Switch8 selections/disabled: each sink exactly1 frame, exact64/stamp/delivery preserved, active borrowed payload unchanged, noalloc\n");
}

void dtype_edge_policy_scenario(){
 auto p=profile();auto input=vector(DType::Float64),output=vector(DType::Int16);
 NumericProcessingGraph g;g.graph_id="dtype.edge";g.nodes={node(1,"NetworkInput",nullptr,&input),node(2,"NetworkOutput",&output,nullptr)};g.edges={{1,2,0,0}};
 struct Seen{std::array<std::int16_t,4> values{};bool available=false;unsigned deliveries=0;} seen;
 auto collect=[](void* user,const NumericPortFrameView& f){auto& seen=*static_cast<Seen*>(user);CHECK(f.identity.node==2);++seen.deliveries;seen.available=f.frame.stamp.available;
  if(!seen.available)return;const auto& view=std::get<NumericView>(f.frame.payload);for(std::size_t plane=0;plane<1;++plane)for(std::size_t col=0;col<2;++col)CHECK(load_element(view,{plane,col,0},seen.values[plane*2+col])==Status::ok);
 };
 auto run=[&](std::uint8_t mode,std::array<double,4> values,bool available,std::array<std::int16_t,4> expected){
  g.edges[0].conversion=mode;GraphExecutor executor(p);application::PreparedExecution prepared;
  CHECK(executor.prepare_numeric(g,{p.admission,p.resources},prepared)==Status::ok);if(!prepared)return;
  CHECK(executor.apply(std::move(prepared))==Status::ok);CHECK(executor.plan()->numeric_definition()->nodes.size()==2);CHECK(executor.plan()->numeric_definition()->edges[0].conversion==mode);
  std::array<std::byte,32> payload{};std::array<PlaneLayout,2> planes{{{0,{8,0}},{16,{8,0}}}};
  MutableNumericView source{&input,payload,std::span(planes).first(1),{2,1},{},{}};for(std::size_t plane=0;plane<1;++plane)for(std::size_t col=0;col<2;++col)CHECK(store_element(source,{plane,col,0},values[plane*2+col])==Status::ok);
  FrameStamp stamp;stamp.sequence=1;stamp.stream_id="edge";seen={};const auto before=allocations;
  const auto status=executor.process_numeric_ports(1,{stamp,source.view()},{},{nullptr,collect,nullptr,&seen});CHECK(allocations==before);
  CHECK((status==Status::ok)==available);CHECK(seen.deliveries==1);CHECK(seen.available==available);if(available)CHECK(std::equal(seen.values.begin(),seen.values.begin()+2,expected.begin()));
 };
 run(0,{10,-20,32767,-32768},true,{10,-20,32767,-32768});
 run(0,{1.5,2.5,-1.5,-2.5},false,{});
 run(1,{1.5,2.5,-1.5,-2.5},true,{2,2,-2,-2});
 run(2,{1.5,2.5,-1.5,-2.5},true,{1,2,-1,-2});
 run(1,{40000,0,0,0},false,{});
 run(3,{40000,-40000,1.5,-1.5},true,{32767,-32768,2,-2});
 auto bad=output;bad.semantic=Semantic::Numeric;bad.quantity=Quantity::Unspecified;bad.shape.axes[0]=Axis::Declared;g.nodes[1].inputs[0].contract=own(bad);application::PreparedExecution prepared;GraphExecutor executor(p);CHECK(executor.prepare_numeric(g,{p.admission,p.resources},prepared)!=Status::ok);
 std::printf("DTYPE EDGES exact/nearest/toward-zero/saturation, visible policy snapshot, unavailable on rejection, semantic refusal, no hot-path allocations\n");
}

void dtype_edge_geometries(){
 auto p=profile();
 for(auto rank:{Rank::Scalar,Rank::Matrix}){
  auto input=vector(DType::Float64);input.semantic=Semantic::Numeric;input.quantity=Quantity::Unspecified;input.shape.axes={Axis::Declared,Axis::Declared};input.shape.rank=rank;input.shape.extents={ExtentBounds{2,2},ExtentBounds{2,2}};
  if(rank==Rank::Scalar){input.shape={};input.semantic=Semantic::Scalar;input.channel_order=std::span(channels).first(1);}
  auto output=input;output.dtype=DType::Int16;NumericProcessingGraph g;g.graph_id="edge.geometry";g.nodes={node(1,"NetworkInput",nullptr,&input),node(2,"NetworkOutput",&output,nullptr)};g.edges={{1,2,0,0}};
  GraphExecutor executor(p);application::PreparedExecution prepared;CHECK(executor.prepare_numeric(g,{p.admission,p.resources},prepared)==Status::ok);if(!prepared)continue;CHECK(executor.apply(std::move(prepared))==Status::ok);
  std::array<std::byte,64> payload{};std::array<PlaneLayout,2> planes{{{0,{16,8}},{32,{16,8}}}};
  const std::array<std::size_t,2> actual=rank==Rank::Scalar?std::array<std::size_t,2>{1,1}:std::array<std::size_t,2>{2,2};
  MutableNumericView value{&input,payload,std::span(planes).first(input.channel_order.size()),actual,{},{}};
  for(std::size_t plane=0;plane<value.planes.size();++plane)for(std::size_t row=0;row<actual[0];++row)for(std::size_t col=0;col<actual[1];++col)CHECK(store_element(value,{plane,row,col},double(plane*4+row*2+col))==Status::ok);
  unsigned seen=0;auto collect=[](void* user,const NumericPortFrameView& frame){++*static_cast<unsigned*>(user);CHECK(frame.status==Status::ok);const auto& view=std::get<NumericView>(frame.frame.payload);for(std::size_t plane=0;plane<view.planes.size();++plane)for(std::size_t row=0;row<view.actual[0];++row)for(std::size_t col=0;col<view.actual[1];++col){std::int16_t value=-1;CHECK(load_element(view,{plane,row,col},value)==Status::ok);CHECK(value==std::int16_t(plane*4+row*2+col));}};
  FrameStamp stamp;stamp.sequence=1;auto before=allocations;CHECK(executor.process_numeric_ports(1,{stamp,value.view()},{},{nullptr,collect,nullptr,&seen})==Status::ok);CHECK(allocations==before&&seen==1);
 }
 auto g=graph(true,DType::UInt64,DType::UInt64);auto history=std::get<NumericContract>(g.nodes[4].outputs[0].contract.view());history.dtype=DType::Int64;g.nodes[9].inputs[0].contract=own(history);
 GraphExecutor executor(p);application::PreparedExecution prepared;CHECK(executor.prepare_numeric(g,{p.admission,p.resources},prepared)==Status::ok);if(!prepared)return;CHECK(executor.apply(std::move(prepared))==Status::ok);
 struct Seen{unsigned outputs=0,rows=0;std::uint64_t sequence=0;} seen;
 auto outputs=[](void* user,const NumericPortFrameView& f){++static_cast<Seen*>(user)->outputs;CHECK(f.identity.node<=10);};
 auto sink=[](void* user,const NumericPortFrameView& f){if(f.identity.node!=10)return;auto& seen=*static_cast<Seen*>(user);++seen.rows;CHECK(f.status==Status::ok);const auto& view=std::get<NumericView>(f.frame.payload);CHECK(view.actual[0]==3);std::int64_t value=0;CHECK(load_element(view,{0,view.actual[0]-1,1},value)==Status::ok);CHECK(value==std::int64_t((std::uint64_t{1}<<54)+seen.sequence*10+1));};
 auto contract=vector(DType::UInt64);std::array<std::byte,32> payload{};
 for(seen.sequence=1;seen.sequence<=6;++seen.sequence){auto incoming=frame(contract,payload,seen.sequence);auto before=allocations;CHECK(executor.process_numeric_ports(1,incoming,{},{outputs,sink,nullptr,&seen})==Status::ok);CHECK(allocations==before);}
 CHECK(seen.rows==6);std::printf("DTYPE GEOMETRY scalar/matrix/History ring conversion preserves exact data and public IDs, noalloc\n");
}
void pcm_resampler_full_chain(){
 auto g=dsp_graph();g.nodes[0].kind="NetworkInput";
 auto input=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
 auto resampled=input;resampled.clock.sample_rate=16000;resampled.clock.tick_seconds={1,16000};resampled.shape.extents[0]={1,1367};
 for(std::size_t i=1;i<g.nodes.size();++i)for(auto* ports:{&g.nodes[i].inputs,&g.nodes[i].outputs})for(auto& port:*ports){
   auto c=std::get<NumericContract>(port.contract.view());c.clock=resampled.clock;
   if(c.coordinates[0].mode==CoordinateMode::Regular && c.coordinates[0].unit==NumericUnit::Hertz)c.coordinates[0].step={16000,512};
   if(g.nodes[i].id==2 && port.direction==PortDirection::Input)c=resampled;
   port.contract=own(c);
 }
 auto convert=node(12,"PcmResampler",&input,&resampled);TransformProcessingParameters parameters;
 parameters.values.push_back({"input_rate",NumericLiteral{std::uint64_t{input.clock.sample_rate}}});
 parameters.values.push_back({"output_rate",NumericLiteral{std::uint64_t{16000}}});convert.parameters=parameters;g.nodes.push_back(convert);
 g.edges[0]={1,12,0,0};g.edges.push_back({12,2,0,0});
 auto p=profile();GraphExecutor executor(p);application::PreparedExecution prepared;
 CHECK(executor.prepare_numeric(g,{p.admission,p.resources},prepared)==Status::ok);if(!prepared)return;
 CHECK(executor.apply(std::move(prepared))==Status::ok);
 std::array<float,8192> samples{};constexpr std::array<PlaneLayout,2> planes{{{0,{4,0}},{4096*4,{4,0}}}};
 DspCapture capture;
 for(std::size_t block=0;block<2;++block){
   for(std::size_t i=0;i<4096;++i){float x=0;for(unsigned tone=0;tone<6;++tone)x+=float(.05*std::sin(6.283185307179586*(400+tone*701+block*31)*double(block*4096+i)/48000.));samples[i]=x;samples[4096+i]=x*.75f;}
   FrameStamp stamp;stamp.sequence=block+1;stamp.source_sequence=block+1;stamp.producer_epoch=77;stamp.timestamp_ticks=std::int64_t(block*4096);
   NumericView view{&input,std::as_bytes(std::span(samples)),std::span(planes).first(1),{4096,1},{},{}};
   auto before=allocations;CHECK(executor.process_numeric_ports(1,{stamp,view},{},{dsp_output,nullptr,dsp_history,&capture})==Status::ok);CHECK(before==allocations);
 }
 // 8192 samples at48k produce2731 samples on the16k grid. At60Hz,
 // ten analysis apertures end at floor(10*16000/60)=2666 exclusive.
 // Resampler supplies an explicit interval, so timestamp is its last sample.
 if(capture.windows!=10 || capture.rows!=10 || capture.ordinal!=10 || capture.timestamp!=2665 || !capture.changed)std::fprintf(stderr,"resampled chain windows=%zu rows=%zu ordinal=%llu timestamp=%lld changed=%d\n",capture.windows,capture.rows,(unsigned long long)capture.ordinal,(long long)capture.timestamp,int(capture.changed));
 CHECK(capture.windows==10 && capture.rows==10 && capture.ordinal==10 && capture.timestamp==std::int64_t(10*16000/60-1) && capture.changed);
}
void pcm_resampler_scenario(){
 auto in=vector(DType::Float32);in.semantic=Semantic::PCM;in.quantity=Quantity::Amplitude;in.unit=NumericUnit::Values;
 in.shape.axes={Axis::Sample,Axis::None};in.shape.extents={ExtentBounds{1,16},ExtentBounds{1,1}};
 in.clock.sample_rate=48000;in.clock.tick_seconds={1,48000};
 auto out=in;out.clock.sample_rate=16000;out.clock.tick_seconds={1,16000};out.shape.extents[0]={1,7};
 NumericProcessingGraph g;g.graph_id="resampler.oracle";g.nodes.push_back(node(1,"NetworkInput",nullptr,&in));
 auto resampler=node(2,"PcmResampler",&in,&out);TransformProcessingParameters parameters;
 parameters.values.push_back({"input_rate",NumericLiteral{std::uint64_t{in.clock.sample_rate}}});
 parameters.values.push_back({"output_rate",NumericLiteral{std::uint64_t{16000}}});resampler.parameters=parameters;g.nodes.push_back(resampler);
 g.nodes.push_back(node(3,"NetworkOutput",&out,nullptr));g.edges={{1,2,0,0},{2,3,0,0}};
 auto p=profile();GraphExecutor executor(p);application::PreparedExecution prepared;
 CHECK(executor.prepare_numeric(g,{p.admission,p.resources},prepared)==Status::ok);if(!prepared)return;
 CHECK(executor.apply(std::move(prepared))==Status::ok);
 struct Seen{std::size_t frames=0,markers=0,samples=0;std::uint64_t sequence=0;std::int64_t ticks=0;bool discontinuity=false,require_zero=false;std::uint32_t rate=16000;} seen;
 auto observe=[](void* user,const NumericPortFrameView& event){auto& seen=*static_cast<Seen*>(user);
   CHECK(event.frame.stamp.sequence>seen.sequence);seen.sequence=event.frame.stamp.sequence;
   CHECK(event.frame.stamp.source_sequence>0);seen.ticks=event.frame.stamp.timestamp_ticks;seen.discontinuity=event.frame.stamp.discontinuity;
   CHECK(validate_view(event.frame,limits)==(event.frame.stamp.available?Status::ok:Status::unavailable));
   const auto& value=std::get<NumericView>(event.frame.payload);CHECK(value.contract->clock.sample_rate==seen.rate);
   if(event.frame.stamp.available){++seen.frames;seen.samples+=value.actual[0];
     if(seen.require_zero)for(std::size_t plane=0;plane<value.planes.size();++plane)for(std::size_t i=0;i<value.actual[0];++i){float x=1;CHECK(load_element(value,{plane,i,0},x)==Status::ok && x==0);}
   }else ++seen.markers;
 };
 std::array<float,32> values{};values.fill(.25f);constexpr std::array<PlaneLayout,2> planes{{{0,{4,0}},{64,{4,0}}}};
 FrameStamp stamp;stamp.producer_epoch=1;stamp.generation=1;stamp.source_sequence=1;stamp.sequence=1;stamp.timestamp_ticks=0;
 // Keep plane metadata owned by the closure; the published view borrows it synchronously.
 auto send=[&,input_planes=planes](std::size_t count,bool available=true,Status expected=Status::ok){NumericView view{&in,std::as_bytes(std::span(values)),std::span(input_planes).first(1),{count,1},{},{}};
   auto current=stamp;current.available=available;if(!available)view.storage={};
   const auto before=allocations;const auto status=executor.process_numeric_ports(1,{current,view},{},{nullptr,observe,nullptr,&seen});
   CHECK(status==expected);CHECK(before==allocations);
   ++stamp.sequence;++stamp.source_sequence;stamp.timestamp_ticks+=std::int64_t(count);
 };
 send(1);CHECK(seen.frames==1 && seen.samples==1 && seen.ticks==0 && seen.discontinuity);
 auto snapshot=executor.snapshot_preparation_base();Seen copied=seen;
 send(1);send(1);CHECK(seen.frames==1);send(1);CHECK(seen.frames==2 && seen.ticks==1);
 // The snapshot must own an independent FIR phase/history, even after the live
 // operation advances. Shared mutable optional-kernel storage would emit early.
 for(std::uint64_t sequence=2;sequence<=4;++sequence){
   auto snapshot_stamp=stamp;snapshot_stamp.sequence=snapshot_stamp.source_sequence=sequence;snapshot_stamp.timestamp_ticks=std::int64_t(sequence-1);
   NumericView snapshot_view{&in,std::as_bytes(std::span(values)),std::span(planes).first(1),{1,1},{},{}};
   const auto before=allocations;CHECK(snapshot.plan->process_numeric_ports(1,{snapshot_stamp,snapshot_view},*snapshot.context,{},{nullptr,observe,nullptr,&copied})==Status::ok);CHECK(before==allocations);
   CHECK(copied.frames==(sequence<4?1u:2u));
 }
 CHECK(copied.ticks==seen.ticks && copied.samples==seen.samples);
 send(1,false);CHECK(seen.markers==1 && seen.ticks==1);
 // Resume at input tick5: ceil(5*16000/48000)=2. Tick2 requires
 // input tick6; emitting tick1 here would invent a sample before recovery.
 send(1);CHECK(seen.frames==2 && seen.ticks==1);
 send(1);CHECK(seen.frames==3 && seen.ticks==2 && seen.discontinuity);
 stamp.timestamp_ticks=480;send(1);CHECK(seen.frames==4 && seen.ticks==160 && seen.discontinuity);
 // A new epoch begins at481: its first valid output is ceil(481/3)=161,
 // requiring input483. Two single-sample packets must neither emit early
 // nor lose that first sample once the exact output-grid point arrives.
 ++stamp.producer_epoch;send(1);CHECK(seen.frames==4 && seen.ticks==160);
 send(1);CHECK(seen.frames==4 && seen.ticks==160);
 send(1);CHECK(seen.frames==5 && seen.ticks==161 && seen.discontinuity);
 // An unchanged Apply must retain fractional phase and FIR state, without allocations on the next ingress.
 application::PreparedExecution retained;CHECK(executor.prepare_numeric(g,{p.admission,p.resources},retained)==Status::ok);
 CHECK(executor.apply(std::move(retained))==Status::ok);const auto frames=seen.frames;send(1);CHECK(seen.frames==frames);
 const auto markers=seen.markers;values[0]=std::numeric_limits<float>::quiet_NaN();send(1,true,Status::overflow);
 CHECK(seen.markers==markers+1);values[0]=.25f;send(1);CHECK(seen.discontinuity);
 // Bypass changes the prepared downstream grid to the native input contract.
 g.nodes[1].enabled=false;application::PreparedExecution bypass;
 NumericProcessingGraph evaluated,executable;CHECK(bool(resolve_numeric_parameter_bindings(g,p.admission,evaluated,executable,2)));g=std::move(evaluated);
 CHECK(executor.prepare_numeric(g,{p.admission,p.resources},bypass)==Status::ok);CHECK(executor.apply(std::move(bypass))==Status::ok);
 seen.sequence=0;seen.rate=48000;send(16);CHECK(seen.frames>frames);
}

// The same primitive frame reaches every presentation kind. In particular an
// ordinary Matrix with a descriptive History tag is not an accumulating ring.
void primitive_views() {
 for(auto rank:{Rank::Scalar,Rank::Vector,Rank::Matrix})for(const auto& dtype:dtype_descriptors()) {
  auto c=vector(dtype.type);c.semantic=Semantic::History;c.history_element=Semantic::PCM;c.coordinates={};c.clock={};
  if(rank==Rank::Scalar){c.shape={};c.channel_order=std::span(channels).first(1);}
  if(rank==Rank::Matrix)c.shape={rank,{Axis::None,Axis::None},{ExtentBounds{2,2},ExtentBounds{3,3}}};
  NumericProcessingGraph g;g.graph_id="primitive.views";g.nodes.push_back(node(1,"NetworkInput",nullptr,&c));
  int id=2;for(auto kind:{"AudioView","WindowView","WindowedView","FFTView","MelView","SpectrogramView","ScalarView"}) {
   const auto expectedRank=std::string_view(kind)=="ScalarView"?Rank::Scalar:std::string_view(kind)=="SpectrogramView"?Rank::Matrix:Rank::Vector;
   if(rank!=expectedRank)continue;
   auto input=c;input.semantic=Semantic::Scalar;input.history_element=Semantic::Numeric;
   g.nodes.push_back(node(id,kind,&input,nullptr));g.edges.push_back({1,id++,0,0});
  }
  auto p=profile();GraphExecutor executor(p);application::PreparedExecution prepared;
  CHECK(executor.prepare_numeric(g,{p.admission,p.resources},prepared)==Status::ok);if(!prepared)continue;
  CHECK(executor.apply(std::move(prepared))==Status::ok);
  std::size_t width=0;CHECK(storage_width(c,width)==Status::ok);
  const auto rows=rank==Rank::Scalar?1u:2u,cols=rank==Rank::Matrix?3u:1u;
  std::array<PlaneLayout,2> planes{};for(std::size_t ch=0;ch<c.channel_order.size();++ch)
   planes[ch]={ch*rows*cols*width,{rank==Rank::Scalar?0:cols*width,rank==Rank::Matrix?width:0}};
  alignas(16) std::array<std::byte,96> bytes{};
  MutableNumericView input{&c,std::span(bytes).first(c.channel_order.size()*rows*cols*width),std::span(planes).first(c.channel_order.size()),{rows,cols},{},{}};
  const std::uint64_t value=dtype.type==DType::Float32?std::bit_cast<std::uint32_t>(7.0f):dtype.type==DType::Float64?std::bit_cast<std::uint64_t>(7.0):7;
  for(std::size_t ch=0;ch<c.channel_order.size();++ch)for(std::size_t r=0;r<rows;++r)for(std::size_t col=0;col<cols;++col)
   CHECK(store_bits(input,{ch,r,col},dtype.type,value)==Status::ok);
  struct Seen{std::size_t frames=0,histories=0;};Seen seen;
  NumericRuntimeCallbacks callbacks;callbacks.user=&seen;
  callbacks.sink_input=[](void* opaque,const NumericPortFrameView& f){auto& seen=*static_cast<Seen*>(opaque);++seen.frames;
   const auto& v=std::get<NumericView>(f.frame.payload);CHECK(!v.ring.capacity);CHECK(validate_view(f.frame,limits)==Status::ok);
   std::uint64_t value=0;CHECK(load_bits(v,{},v.contract->dtype,value)==Status::ok);
   const auto expected=v.contract->dtype==DType::Float32?std::bit_cast<std::uint32_t>(7.0f):v.contract->dtype==DType::Float64?std::bit_cast<std::uint64_t>(7.0):7;CHECK(value==expected);};
  callbacks.history=[](void* opaque,const NumericHistoryRowView&){++static_cast<Seen*>(opaque)->histories;};
  FrameStamp stamp;stamp.sequence=1;const auto before=allocations;
  CHECK(executor.process_numeric_ports(1,{stamp,input.view()},{},callbacks)==Status::ok);
  CHECK(allocations==before&&seen.frames==(rank==Rank::Vector?5:1)&&seen.histories==0);
 }
}
void index_extractors() {
 for(const auto rank:{Rank::Vector,Rank::Matrix})for(const auto& type:dtype_descriptors()){
  auto input=vector(type.type);if(rank==Rank::Matrix)input.shape={rank,{Axis::None,Axis::None},{ExtentBounds{1,2},ExtentBounds{3,3}}};
  else input.shape.extents[0]={1,2};
  auto scalar=input;scalar.shape={};scalar.coordinates={};scalar.channel_order=input.channel_order.first(1);
  auto first=scalar;first.channel_order=input.channel_order.first(1);auto unknown=scalar;unknown.clock={};
  auto extract=node(2,"IndexExtract",&input,&scalar);extract.parameters=TransformProcessingParameters{{
   {"channel_index",NumericLiteral{std::uint64_t{0}}},{"index",NumericLiteral{std::uint64_t{1}}},{"column",NumericLiteral{std::uint64_t(rank==Rank::Matrix?2:0)}}}};
  auto firstExtract=node(3,"IndexExtract",&input,&first);firstExtract.parameters=TransformProcessingParameters{};
  auto view=node(4,"ScalarView",nullptr,nullptr);view.inputs={port(3,PortDirection::Input,scalar),port(7,PortDirection::Input,first),port(9,PortDirection::Input,scalar),port(11,PortDirection::Input,unknown)};
  NumericProcessingGraph graph;graph.graph_id="exact.extract";graph.nodes={node(1,"NetworkInput",nullptr,&input),extract,firstExtract,view};graph.edges={{1,2,0,0},{1,3,0,0},{2,4,0,3},{3,4,0,7}};
  auto p=profile();GraphExecutor executor(p);application::PreparedExecution prepared;CHECK(executor.prepare_numeric(graph,{p.admission,p.resources},prepared)==Status::ok);if(!prepared)continue;CHECK(executor.apply(std::move(prepared))==Status::ok);
  std::size_t width=0;CHECK(storage_width(input,width)==Status::ok);const std::size_t columns=rank==Rank::Matrix?3:1;
  std::array<PlaneLayout,2> planes{{{0,{columns*width,rank==Rank::Matrix?width:0}},{2*columns*width,{columns*width,rank==Rank::Matrix?width:0}}}};
  alignas(16) std::array<std::byte,96> bytes{};MutableNumericView data{&input,std::span(bytes).first(4*columns*width),std::span(planes).first(1),{2,columns},{},{}};
  const std::uint64_t bits=type.type==DType::Float32?std::bit_cast<std::uint32_t>(7.25f):type.type==DType::Float64?std::bit_cast<std::uint64_t>(7.25):type.type==DType::UInt64?UINT64_MAX:71;
  CHECK(store_bits(data,{0,1,columns-1},type.type,bits)==Status::ok);CHECK(store_bits(data,{0,0,0},type.type,1)==Status::ok);
  struct Seen{std::uint64_t bits;std::array<unsigned,12> counts{};unsigned producer_refusals=0,unavailable_sinks=0;} seen{bits};NumericRuntimeCallbacks callbacks;callbacks.user=&seen;
  callbacks.sink_input=[](void* opaque,const NumericPortFrameView& frame){auto& seen=*static_cast<Seen*>(opaque);const auto id=frame.identity.port;CHECK(frame.identity.node==4 && id<seen.counts.size());if(id>=seen.counts.size())return;++seen.counts[id];
   if(id==11){CHECK(frame.status==Status::unavailable&&!frame.frame.stamp.available);return;}
   CHECK(frame.status==Status::ok&&validate_view(frame.frame,limits)==Status::ok);const auto& value=std::get<NumericView>(frame.frame.payload);std::uint64_t bits=0;CHECK(load_bits(value,{},value.contract->dtype,bits)==Status::ok);CHECK(bits==(id==3?seen.bits:id==7?1:0));};
  FrameStamp stamp;stamp.sequence=1;const auto before=allocations;CHECK(executor.process_numeric_ports(1,{stamp,data.view()},{},callbacks)==Status::ok);CHECK(allocations==before);
  CHECK(seen.counts[3]==1&&seen.counts[7]==1&&seen.counts[9]==1&&seen.counts[11]==1);
  if(rank==Rank::Matrix){
   // Rotate the backing storage. Configured indices remain logical, including
   // through an externally retained full ring rather than a dense matrix.
   data.ring={2,1,{}};CHECK(store_bits(data,{0,1,2},type.type,bits)==Status::ok);CHECK(store_bits(data,{0,0,0},type.type,1)==Status::ok);
   auto arena=std::make_shared<IngressArena>();OwnedDataFrame owned;stamp.sequence=2;
   CHECK(retain_frame({stamp,data.view()},limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok);
   const auto beforeRing=allocations;CHECK(executor.process_numeric_owned_ports(1,owned,{},callbacks,2,arena,2)==Status::ok);CHECK(allocations==beforeRing);
   CHECK(seen.counts[3]==2&&seen.counts[7]==2&&seen.counts[9]==2&&seen.counts[11]==2);
   // Once seeded, the next accepted row must advance the physical cursor.
   // A refused cursor must leave the previous lineage and deliveries intact.
   stamp.sequence=3;CHECK(retain_frame({stamp,data.view()},limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok);
   CHECK(executor.process_numeric_owned_ports(1,owned,{},callbacks,3,arena,2)==Status::incompatible_contract);
   CHECK(seen.counts[3]==2&&seen.counts[7]==2);
   data.ring.oldest=0;CHECK(store_bits(data,{0,1,2},type.type,bits)==Status::ok);CHECK(store_bits(data,{0,0,0},type.type,1)==Status::ok);
   CHECK(retain_frame({stamp,data.view()},limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok);
   const auto beforeAdvance=allocations;CHECK(executor.process_numeric_owned_ports(1,owned,{},callbacks,3,arena,2)==Status::ok);CHECK(allocations==beforeAdvance);
   CHECK(seen.counts[3]==3&&seen.counts[7]==3);data.ring={};
  }
  // An admitted variable extent can shrink below the chosen index. The
  // extractor must publish refusal, never read the previous buffer contents.
  data.actual[0]=1;stamp.sequence=4;
  callbacks.output=[](void* opaque,const NumericPortFrameView& frame){if(frame.identity.node==2 && frame.identity.port==0){CHECK(frame.status==Status::invalid_argument&&!frame.frame.stamp.available);++static_cast<Seen*>(opaque)->producer_refusals;}};
  callbacks.sink_input=[](void* opaque,const NumericPortFrameView& frame){if(frame.identity.port==3){CHECK(frame.status==Status::unavailable&&!frame.frame.stamp.available);++static_cast<Seen*>(opaque)->unavailable_sinks;}};
  const auto beforeShort=allocations;CHECK(executor.process_numeric_ports(1,{stamp,data.view()},{},callbacks)==Status::invalid_argument);CHECK(allocations==beforeShort);
  CHECK(seen.producer_refusals==1 && seen.unavailable_sinks==1);
 }
}
#include "numeric_dsp_stage_oracle.h"
void pcm_resampler_identity_bypass(){
 auto input=vector(DType::Float32);input.semantic=Semantic::PCM;input.quantity=Quantity::Amplitude;input.unit=NumericUnit::Values;input.shape.axes={Axis::Sample,Axis::None};input.shape.extents={ExtentBounds{1,16},ExtentBounds{1,1}};input.clock={"native",{1,48000},48000};
 auto resampler=node(2,"PcmResampler",&input,&input);resampler.enabled=false;resampler.parameters=TransformProcessingParameters{{{"input_rate",NumericLiteral{std::uint64_t{input.clock.sample_rate}}},{"output_rate",NumericLiteral{std::uint64_t{36000}}},{"mode",std::string{"existing_fir"}}}};
 NumericProcessingGraph graph;graph.graph_id="resampler.identity-bypass";graph.nodes={node(1,"NetworkInput",nullptr,&input),resampler};graph.edges={{1,2,0,0}};
 std::shared_ptr<const PreparedGraph> prepared;std::unique_ptr<RuntimeContext> state;CHECK(PreparedGraph::prepare_numeric(graph,profile(),prepared,state)==Status::ok);if(!prepared)return;
 struct Seen{unsigned frames=0;std::array<float,16> expected{};static void output(void* user,const NumericPortFrameView& event){if(event.identity.node!=2 || !event.frame.stamp.available)return;auto& seen=*static_cast<Seen*>(user);++seen.frames;const auto& view=std::get<NumericView>(event.frame.payload);CHECK(view.contract->clock.sample_rate==48000 && view.actual[0]==16 && event.frame.stamp.timestamp_ticks==64);for(unsigned i=0;i<16;++i){float x=0;CHECK(load_element(view,{0,i,0},x)==Status::ok && x==seen.expected[i]);}}} seen;
 for(unsigned i=0;i<16;++i)for(unsigned tone=1;tone<=5;++tone)seen.expected[i]+=.02f*std::cos(float(i*tone)*.2f);
 const PlaneLayout plane{0,{4,0}};NumericView values{&input,std::as_bytes(std::span(seen.expected)),std::span(&plane,1),{16,1},{},{}};FrameStamp stamp;stamp.available=true;stamp.sequence=stamp.source_sequence=1;stamp.producer_epoch=1;stamp.timestamp_ticks=64;stamp.has_sample_interval=true;stamp.sample_begin=64;stamp.sample_count=16;
 const auto before=allocations;CHECK(prepared->process_numeric_ports(1,{stamp,values},*state,{},NumericRuntimeCallbacks{Seen::output,nullptr,nullptr,&seen})==Status::ok);CHECK(seen.frames==1 && allocations==before);
}
int main(){pcm_resampler_identity_bypass();index_extractors();primitive_views();fixed_pcm_history_resize();pcm_resampler_scenario();pcm_resampler_full_chain();dtype_edge_policy_scenario();dtype_edge_geometries();scenario();dtype_routes();float_dsp_scenario();float_dsp_scenario(true);numeric_full_chain_oracle();numeric_full_chain_oracle(true);control_scenario();merge_scenario();local_history_conversion();external_history_ownership();whole_history_kernel();local_history_coordinates();resource_dimensions();route_fanout_identity();std::printf("failures=%d\n",failures);return failures?1:0;}

