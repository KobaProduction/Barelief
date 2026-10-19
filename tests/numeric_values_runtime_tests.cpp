#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <dsp_kernels/dsp.h>
#include <dsp_kernels/musical_features.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <cmath>
#include <utility>

using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
namespace {
std::size_t allocations=0;
void check(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
constexpr FrameLimits frames{8,32768,262144,16384,256,8192};
constexpr ProcessingAdmissionLimits limits{32,64,16,16,256,65536,frames};
NumericRuntimeProfile profile(){NumericRuntimeProfile p{limits,{{4194304,4194304,4194304,131072},0,16777216},{32768,8,262144,262144,262144},{true,true,true}};p.music_enabled=true;return p;}
OwnedProcessingContract own(const NumericContract& c){OwnedProcessingContract result;check(own_processing_contract(c,frames,65536,result)==Status::ok,"own numeric value contract");return result;}
NumericContract base(){static constexpr std::array<std::uint32_t,1> mono{0};NumericContract c;c.channel_order=mono;c.dtype=DType::UInt32;c.semantic=Semantic::Scalar;c.unit=NumericUnit::Values;c.representation.alignment=alignof(std::uint32_t);return c;}
struct Running {
 std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
 explicit Running(const NumericProcessingGraph& g){const auto status=PreparedGraph::prepare_numeric(g,profile(),plan,state);if(status!=Status::ok)std::fprintf(stderr,"graph=%s status=%u\n",g.graph_id.c_str(),unsigned(status));check(status==Status::ok,"prepare numeric primitives");}
};
struct Observation {
 std::array<std::uint32_t,8> values{};std::array<unsigned,8> available{},unavailable{};FrameStamp stamp{};
 static void output(void* p,const NumericPortFrameView& f){auto& o=*static_cast<Observation*>(p);if(f.identity.node<0 || f.identity.node>=8 || f.direction!=PortDirection::Output)return;
  if(!f.frame.stamp.available){++o.unavailable[f.identity.node];return;}
  const auto& v=std::get<NumericView>(f.frame.payload);if(v.contract->shape.rank!=Rank::Scalar)return;
  check(v.contract->dtype==DType::UInt32,"ordinary UInt32 scalar output");
  check(load_element(v,{0,0,0},o.values[f.identity.node])==Status::ok,"load numeric scalar");++o.available[f.identity.node];o.stamp=f.frame.stamp;
 }
 NumericRuntimeCallbacks callbacks(){return {output,nullptr,nullptr,this};}
};
FrameStamp tick(std::uint64_t sequence=1){FrameStamp s;s.sequence=sequence;s.producer_epoch=1;s.has_sample_interval=true;s.sample_begin=std::int64_t(sequence-1);s.sample_count=1;return s;}
NumericProcessingGraph scalar_number_graph(NumericLiteral value){
 ConfiguredProcessingNode constant;constant.id=1;constant.kind="Number";
 constant.parameters=EndpointProcessingParameters{{{"value",std::move(value)}}};constant.outputs={{0,PortDirection::Output,own(base())}};
 NumericProcessingGraph graph;graph.graph_id="literal-capability";graph.nodes={constant};return graph;
}
void constant_capability_admission(){
 auto graph=scalar_number_graph(NumericLiteral{std::uint64_t{44100}});auto p=profile();ResolvedProcessingNode node;
 check(bool(resolve_processing_node(graph.nodes[0],limits,node)),"resolve cold literal capability");
 auto caps=PreparedGraph::numeric_capabilities(p);const auto cast=caps.query(caps.context,node,p.resources);
 check(cast.supported && cast.resources.scratch_bytes>=sizeof(std::uint32_t),"cold conversion scratch is reserved by admission");
 p.capabilities.converter=false;caps=PreparedGraph::numeric_capabilities(p);
 check(!caps.query(caps.context,node,p.resources).supported,"unavailable converter refuses literal cast at capability boundary");
 auto matching=scalar_number_graph(NumericLiteral{std::uint64_t{44100}});auto matching_contract=base();matching_contract.dtype=DType::UInt64;matching_contract.representation.alignment=alignof(std::uint64_t);
 matching.nodes[0].outputs[0].contract=own(matching_contract);ResolvedProcessingNode direct;
 check(bool(resolve_processing_node(matching.nodes[0],limits,direct)),"resolve matching tagged literal");
 const auto native=caps.query(caps.context,direct,p.resources);
 check(native.supported && native.resources.scratch_bytes==0 && cast.resources.stack_bytes>=native.resources.stack_bytes+dsp_kernels::numeric_reference_stack_bytes,"matching literal requires no converter/scratch and cold cast stack is reserved");
 std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
 check(PreparedGraph::prepare_numeric(graph,p,plan,state)!=Status::ok && !plan,"disabled conversion refuses atomic Prepare");
 check(PreparedGraph::prepare_numeric(matching,p,plan,state)==Status::ok,"matching literal remains supported without converter capability");
}
void constant_materialization(){
 auto scalar=base();ConfiguredProcessingNode constant;constant.id=1;constant.kind="Number";
 constant.parameters=EndpointProcessingParameters{{{"value",NumericLiteral{std::uint64_t{44100}}}}};constant.outputs={{0,PortDirection::Output,own(scalar)}};
 NumericProcessingGraph graph;graph.graph_id="literal-materialization";graph.nodes={constant};Running running(graph);Observation observed;
 const auto before=allocations;
 check(running.plan->process_clock(1,tick(),*running.state,{},observed.callbacks())==Status::ok && allocations==before && observed.values[1]==44100,"UInt64 literal materializes exact declared UInt32 without hot conversion/allocation");
 auto overflow=graph;std::get<EndpointProcessingParameters>(overflow.nodes[0].parameters).values[0].value=NumericLiteral{std::uint64_t{UINT32_MAX}+1};
 auto previous=running.plan;std::unique_ptr<RuntimeContext> context;
 check(PreparedGraph::prepare_numeric(overflow,profile(),previous,context)!=Status::ok && previous==running.plan,"constant overflow rejected atomically at Prepare");
 auto real=scalar;real.dtype=DType::Float32;real.representation.alignment=alignof(float);
 auto inexact=graph;inexact.nodes[0].outputs={{0,PortDirection::Output,own(real)}};std::get<EndpointProcessingParameters>(inexact.nodes[0].parameters).values[0].value=NumericLiteral{0.1};
 check(PreparedGraph::prepare_numeric(inexact,profile(),previous,context)!=Status::ok && previous==running.plan,"constant inexact narrowing rejected using declared conversion policy");
}
void constants(){for(const auto* kind:{"Fs","UInt32Constant"}){
 const auto expected=std::uint32_t(kind==std::string_view("Fs")?48000:UINT32_MAX);auto c=base();c.unit=kind==std::string_view("Fs")?NumericUnit::Hertz:NumericUnit::Values;
 ConfiguredProcessingNode n;n.id=1;n.kind=kind;n.parameters=EndpointProcessingParameters{{{"value",NumericLiteral{std::uint64_t(expected)}}}};n.outputs={{0,PortDirection::Output,own(c)}};
 NumericProcessingGraph g;g.graph_id="numeric-values";g.nodes={n};Running r(g);Observation o;const auto before=allocations;
 check(r.plan->process_clock(1,tick(),*r.state,{},o.callbacks())==Status::ok,"constant publishes configured scalar");
 check(allocations==before && o.values[1]==expected && o.available[1]==1,"constant is exact and allocation-free including UInt32 maximum");
 check(!o.stamp.has_sample_interval,"numeric constant is not a PCM sample interval");
 check(r.plan->process_clock(1,tick(),*r.state,{},o.callbacks())==Status::would_block,"duplicate constant event refused");
 auto bad=g;std::get<EndpointProcessingParameters>(bad.nodes[0].parameters).values[0].value=NumericLiteral{4294967296ULL};
 std::shared_ptr<const PreparedGraph> rejected=r.plan;std::unique_ptr<RuntimeContext> state;
 check(PreparedGraph::prepare_numeric(bad,profile(),rejected,state)!=Status::ok,"out-of-range scalar parameter refused");
 }}
ConfiguredProcessingNode metadata(NodeId id,const char* kind,const NumericContract& input,unsigned axis=0){
 ConfiguredProcessingNode n;n.id=id;n.kind=kind;n.parameters=TransformProcessingParameters{};
 if(n.kind=="ArrayLength")std::get<TransformProcessingParameters>(n.parameters).values={{"axis",NumericLiteral{std::uint64_t(axis)}}};
 n.inputs={{0,PortDirection::Input,own(input)}};OwnedProcessingContract scalar;
 check(numeric_metadata_output_contract(input,n.kind=="SampleRate",scalar)==Status::ok,"derive scalar metadata");n.outputs={{0,PortDirection::Output,std::move(scalar)}};return n;
}
void lengths(){
 auto c=base();c.dtype=DType::UInt16;c.semantic=Semantic::Numeric;c.clock={"source",{1,44100},44100};c.representation.alignment=alignof(std::uint16_t);
 c.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{1,16},ExtentBounds{}}};
 ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{44100}}}}};source.outputs={{0,PortDirection::Output,own(c)}};
 check(attach_numeric_parameter_ports(source,limits)==Status::ok,"source exposes ordinary Fs output");
 NumericProcessingGraph g;g.graph_id="numeric-metadata";g.nodes={source,metadata(2,"ArrayLength",c),metadata(3,"SampleRate",c)};g.edges={{1,2,0,0},{1,3,0,0}};
 NumericProcessingGraph lowered;std::vector<RoutingAlias> aliases;
 check(bool(lower_numeric_metadata_outputs(g,limits,lowered,aliases)),"compile declared source Fs to ordinary scalar constant");
 const auto alias=std::find_if(aliases.begin(),aliases.end(),[](const auto& value){return value.node==1 && value.port==sample_rate_output;});
 check(alias!=aliases.end(),"source Fs retains its explicit public endpoint alias");
 const auto constant=std::find_if(lowered.nodes.begin(),lowered.nodes.end(),[&](const auto& value){return value.id==alias->source;});
 check(constant!=lowered.nodes.end() && constant->kind=="Number","source Fs lowering uses ordinary Number producer");
 check(std::get<NumericContract>(constant->outputs[0].contract.view()).unit==NumericUnit::Hertz,"declared Fs scalar has Hertz unit");
 Running r(g);Observation o;std::array<std::uint16_t,16> samples{};const PlaneLayout plane{0,{sizeof(std::uint16_t),0}};
 const auto configuration_allocations=allocations;
 const auto configured_status=r.plan->process_clock(alias->source,tick(),*r.state,{},o.callbacks());
 if(configured_status!=Status::ok)std::fprintf(stderr,"Fs constant alias source=%d port=%u -> node=%d port=%u status=%u events=%u value=%u contract_present=%d\n",int(alias->source),unsigned(alias->source_port),int(alias->node),unsigned(alias->port),unsigned(configured_status),o.available[1],o.values[1],int(r.plan->port_contract(alias->source,PortDirection::Output,0).has_value()));
 check(configured_status==Status::ok,"declared Fs constant publishes at configuration cadence");
 check(allocations==configuration_allocations && o.values[1]==44100 && o.available[1]==1 && !o.stamp.has_sample_interval,"configuration Fs alias is exact and allocation-free without PCM interval");
 for(unsigned count:{7u,3u}){
  auto stamp=tick(count==7?1:2);stamp.sample_begin=count==7?0:7;stamp.sample_count=count;
  NumericView v{&c,std::as_bytes(std::span(samples)),std::span(&plane,1),{count,1},{},{}};const auto before=allocations;
  check(r.plan->process_numeric_ports(1,{stamp,v},*r.state,{},o.callbacks())==Status::ok,"metadata accepts ordinary non-Float32 vector");
  check(allocations==before && o.values[2]==count && o.values[3]==44100,"actual packet length and source Fs are exact, allocation-free");
  check(o.values[1]==44100 && o.available[1]==1,"PCM events do not republish or infer declared configuration Fs");
 }
 auto foreign=c;foreign.clock.sample_rate=48000;foreign.clock.tick_seconds={1,48000};
 NumericView foreign_frame{&foreign,std::as_bytes(std::span(samples)),std::span(&plane,1),{1,1},{},{}};
 check(r.plan->process_numeric_ports(1,{tick(3),foreign_frame},*r.state,{},o.callbacks())==Status::incompatible_contract,"foreign frame clock cannot replace declared source Fs");
 check(o.values[1]==44100 && o.available[1]==1,"foreign clock refusal leaves declared scalar unchanged");
 auto stamp=tick(3);stamp.available=false;NumericView missing{&c,{},std::span(&plane,1),{1,1},{},{}};
 check(r.plan->process_numeric_ports(1,{stamp,missing},*r.state,{},o.callbacks())==Status::ok,"unavailable metadata propagates");
 check(o.unavailable[2]==1 && o.unavailable[3]==1 && o.available[2]==2 && o.available[3]==2,"metadata does not reuse a stale scalar");
}
void matrix(){auto c=base();c.dtype=DType::Float64;c.semantic=Semantic::Numeric;c.shape={Rank::Matrix,{Axis::Time,Axis::Band},{ExtentBounds{1,3},ExtentBounds{4,4}}};c.representation.alignment=alignof(double);
 ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={{0,PortDirection::Output,own(c)}};
 NumericProcessingGraph g;g.graph_id="matrix-dimensions";g.nodes={source,metadata(2,"ArrayLength",c,0),metadata(3,"ArrayLength",c,1)};g.edges={{1,2,0,0},{1,3,0,0}};Running r(g);Observation o;
 std::array<double,12> data{};const PlaneLayout plane{0,{4*sizeof(double),sizeof(double)}};NumericView v{&c,std::as_bytes(std::span(data)),std::span(&plane,1),{2,4},{},{}};auto stamp=tick();stamp.has_sample_interval=false;
 check(r.plan->process_numeric_ports(1,{stamp,v},*r.state,{},o.callbacks())==Status::ok,"matrix metadata executes");check(o.values[2]==2 && o.values[3]==4,"matrix row/column lengths stay distinct");
}
void parameter_bindings(){
 auto input=base();input.dtype=DType::Float32;input.semantic=Semantic::PCM;input.representation.alignment=alignof(float);
 input.representation.normalization=Normalization::FloatUnit;input.quantity=Quantity::Amplitude;input.unit=NumericUnit::NormalizedAmplitude;
 input.clock={"source",{1,48000},48000};input.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{1,256},ExtentBounds{}}};
 ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={{0,PortDirection::Output,own(input)}};
 auto output=input;output.semantic=Semantic::Window;output.shape.extents[0]={4096,4096};
 ConfiguredProcessingNode window;window.id=2;window.kind="Window";window.parameters=TransformProcessingParameters{{{"size",NumericLiteral{std::uint64_t{4096}}},{"input_rate",NumericLiteral{std::uint64_t{48000}}}}};
 window.inputs={{0,PortDirection::Input,own(input)}};window.outputs={{0,PortDirection::Output,own(output)}};
 check(attach_numeric_parameter_ports(window,limits)==Status::ok,"attach explicit Fs and size parameter inputs");
 check(std::count_if(window.inputs.begin(),window.inputs.end(),[](const auto& port){return port.id==window_size_input;})==1,"factory exposes exactly one size parameter input");
 ConfiguredProcessingNode constant;constant.id=3;constant.kind="UInt32Constant";constant.parameters=EndpointProcessingParameters{{{"value",NumericLiteral{std::uint64_t{2048}}}}};constant.outputs={{0,PortDirection::Output,own(base())}};
 NumericProcessingGraph graph;graph.graph_id="parameter-bindings";graph.nodes={source,window,constant};graph.edges={{1,2,0,0},{3,2,0,window_size_input}};
 NumericProcessingGraph evaluated,executable;
 check(bool(resolve_numeric_parameter_bindings(graph,limits,evaluated,executable)),"evaluate N connection");
 check(evaluated.nodes[1].inputs.size()==3 && executable.nodes[1].inputs.size()==1 && executable.edges.size()==1,"parameter wire retained in editable graph and removed from execution schedule");
 check(std::get<NumericContract>(evaluated.nodes[1].outputs[0].contract.view()).shape.extents[0].maximum==2048,"N wire changes prepared window geometry");
 std::array<std::size_t,64> workspace{};ProcessingGraphAdmission admission;
 const auto admitted=admit_processing_graph(executable,limits,PreparedGraph::numeric_capabilities(profile()),workspace,admission);
 if(!admitted)std::fprintf(stderr,"parameter admission error=%u node=%d port=%u\n",unsigned(admitted.error),int(admitted.node),unsigned(admitted.port));
 check(bool(admitted),"bound executable graph admission");
 NumericProcessingGraph converted,lowered;std::vector<RoutingAlias> aliases;
 const auto conversion=lower_dtype_edges(executable,limits,converted);
 if(!conversion)std::fprintf(stderr,"conversion error=%u node=%d\n",unsigned(conversion.error),int(conversion.node));
 check(bool(conversion),"bound graph dtype lowering");
 check(bool(lower_routing_points(converted,limits,lowered,aliases)),"bound graph routing lowering");
 Running run(graph);check(run.plan->port_contract(2,PortDirection::Input,window_size_input).has_value(),"prepared plan exposes parameter endpoint");
 auto rejected=graph;std::get<EndpointProcessingParameters>(rejected.nodes[2].parameters).values[0].value=NumericLiteral{std::uint64_t{0}};
 const auto previous=evaluated.nodes[1].outputs[0].contract;
 check(!resolve_numeric_parameter_bindings(rejected,limits,evaluated,executable) && same_contract(previous.view(),evaluated.nodes[1].outputs[0].contract.view()),"invalid N rejected atomically");
 rejected=graph;rejected.edges.push_back({3,2,0,window_size_input});check(!resolve_numeric_parameter_bindings(rejected,limits,evaluated,executable),"multiple scalar producers refused");
 auto target=input;target.clock.sample_rate=24000;target.clock.tick_seconds={1,24000};target.shape.extents[0]={1,129};
 ConfiguredProcessingNode resampler;resampler.id=2;resampler.kind="PcmResampler";resampler.parameters=TransformProcessingParameters{{{"input_rate",NumericLiteral{std::uint64_t{input.clock.sample_rate}}},{"output_rate",NumericLiteral{std::uint64_t{24000}}},{"mode",std::string{"existing_fir"}}}};
 resampler.inputs={{0,PortDirection::Input,own(input)}};resampler.outputs={{0,PortDirection::Output,own(target)}};check(attach_numeric_parameter_ports(resampler,limits)==Status::ok,"attach ordinary Fs output target input");
 graph.nodes={source,resampler};graph.edges={{1,2,0,0}};
 check(bool(resolve_numeric_parameter_bindings(graph,limits,evaluated,executable)),"evaluate local resampler Fs_out");
 check(evaluated.nodes[1].inputs.size()==2 && evaluated.nodes[1].outputs.back().id==sample_rate_output,"resampler has Fs input and right Fs output, no target control input");
 check(std::get<NumericContract>(evaluated.nodes[1].outputs[0].contract.view()).clock.sample_rate==24000,"local setting owns resampling target");
 Running resampled(graph);
}
void native_selector(){
 auto a=base();a.dtype=DType::Float32;a.semantic=Semantic::PCM;a.representation.alignment=alignof(float);a.clock={"native",{1,48000},48000};a.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{1,256},ExtentBounds{}}};auto b=a;b.clock.tick_seconds={1,32000};b.clock.sample_rate=32000;
 ConfiguredProcessingNode first;first.id=1;first.kind="NetworkInput";first.outputs={{0,PortDirection::Output,own(a)}};auto second=first;second.id=2;second.outputs[0].contract=own(b);
 second.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{32000}}}}};
 check(attach_numeric_parameter_ports(first,limits)==Status::ok && attach_numeric_parameter_ports(second,limits)==Status::ok,"source Fs outputs");
 const auto fs=first.outputs.back().contract,fs2=second.outputs.back().contract;
 ConfiguredProcessingNode selector;selector.id=3;selector.kind="InputSelector";selector.parameters=TransformProcessingParameters{{{"selected_input",NumericLiteral{std::uint64_t{0}}},{"channels_per_stream",NumericLiteral{std::uint64_t{2}}}}};
 selector.inputs={{0,PortDirection::Input,own(a)},{1,PortDirection::Input,fs},{2,PortDirection::Input,own(b)},{3,PortDirection::Input,fs2}};
 selector.outputs={{0,PortDirection::Output,own(a)},{1,PortDirection::Output,fs}};
 ConfiguredProcessingNode resampler;resampler.id=4;resampler.kind="PcmResampler";resampler.parameters=TransformProcessingParameters{{{"output_rate",NumericLiteral{std::uint64_t{48000}}},{"mode",std::string{"existing_fir"}}}};resampler.inputs={{0,PortDirection::Input,own(a)}};auto output=a;output.shape.extents[0]={1,257};resampler.outputs={{0,PortDirection::Output,own(output)}};check(attach_numeric_parameter_ports(resampler,limits)==Status::ok,"resampler Fs inputs");
 NumericProcessingGraph graph;graph.graph_id="native-selector";graph.nodes={first,second,selector,resampler};graph.edges={{1,3,0,0},{1,3,sample_rate_output,1},{2,3,0,2},{2,3,sample_rate_output,3},{3,4,0,0},{3,4,1,input_sample_rate_input}};
 std::get<TransformProcessingParameters>(graph.nodes[2].parameters).values[0].value=NumericLiteral{std::uint64_t{1}};
 NumericProcessingGraph evaluated,executable;check(bool(resolve_numeric_parameter_bindings(graph,limits,evaluated,executable)),"selector switches native Fs and explicit resampler input atomically");
 check(std::get<NumericContract>(evaluated.nodes[2].outputs[0].contract.view()).clock.sample_rate==32000 && std::get<NumericContract>(evaluated.nodes[3].outputs[0].contract.view()).clock.sample_rate==48000,"selector does not resample, following resampler owns target Fs");
 auto wrongFs=graph;for(auto& edge:wrongFs.edges)if(edge.to==3 && edge.input_port==3)edge.from=1;
 check(!resolve_numeric_parameter_bindings(wrongFs,limits,evaluated,executable),"selector follows explicit Fs wire: wrong source rate is rejected rather than synthesized");
 Running running(graph);
 struct Selected {unsigned values=0,rates=0;std::uint32_t fs=0;static void output(void* context,const NumericPortFrameView& event){auto& self=*static_cast<Selected*>(context);if(event.identity.node!=3 || !event.frame.stamp.available)return;const auto& view=std::get<NumericView>(event.frame.payload);if(event.identity.port==1){check(load_element(view,{0,0,0},self.fs)==Status::ok,"selected Fs load");++self.rates;}else {check(view.contract->clock.sample_rate==32000,"selected data preserves native Fs");++self.values;}}} selected;
 NumericProcessingGraph configured,processing,constants;std::vector<RoutingAlias> aliases;
 check(bool(resolve_numeric_parameter_bindings(graph,limits,configured,processing)),"selected configuration evaluates independently of PCM");
 check(bool(lower_numeric_metadata_outputs(processing,limits,constants,aliases)),"selected Fs endpoints lower to explicit constants");
 NumericRuntimeCallbacks callbacks{Selected::output,nullptr,nullptr,&selected};
 for(const NodeId owner:{1,2}) {
  const auto alias=std::find_if(aliases.begin(),aliases.end(),[&](const auto& item){return item.node==owner&&item.port==sample_rate_output;});
  check(alias!=aliases.end(),"each native source exposes its declared Fs constant alias");
  const auto before=allocations;
  check(running.plan->process_clock(alias->source,tick(),*running.state,{},callbacks)==Status::ok && allocations==before,"declared Fs config delivery remains allocation free");
  check(selected.values==0 && selected.rates==(owner==2?1u:0u),"selector excludes unselected configuration and forwards only selected Fs lane");
 }
 check(selected.fs==32000,"selected Fs comes from explicit32000 configuration, independently of PCM");
 std::array<float,256> data{};data.fill(.25f);const PlaneLayout plane{0,{sizeof(float),0}};auto stamp=tick();stamp.sample_count=256;
 NumericView view{&a,std::as_bytes(std::span(data)),std::span(&plane,1),{256,1},{},{}};auto before=allocations;check(running.plan->process_numeric_ports(1,{stamp,view},*running.state,{},callbacks)==Status::ok && allocations==before && !selected.values && selected.rates==1,"unselected source cannot update selected data or declared Fs");
 view.contract=&b;before=allocations;check(running.plan->process_numeric_ports(2,{stamp,view},*running.state,{},callbacks)==Status::ok && allocations==before && selected.values==1 && selected.rates==1 && selected.fs==32000,"native data preserves selected configuration Fs without republishing it or allocating");
}
void spectrum_pipeline(){
 auto input=base();input.dtype=DType::Float32;input.semantic=Semantic::PCM;input.representation.alignment=alignof(float);input.clock={"spectrum",{1,48000},48000};input.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{512,512},ExtentBounds{}}};
 ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={{0,PortDirection::Output,own(input)}};check(attach_numeric_parameter_ports(source,limits)==Status::ok,"spectral source Fs");
 ConfiguredProcessingNode fft;fft.id=2;fft.kind="RealFourier";fft.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{512}}}}};fft.inputs={{0,PortDirection::Input,own(input)}};
 std::array<OwnedProcessingContract,4> transformed;check(fourier_output_contracts(fft,input,transformed)==Status::ok,"spectral transform outputs");for(PortId p=0;p<4;++p)fft.outputs.push_back({p,PortDirection::Output,transformed[p]});
 ConfiguredProcessingNode polar;polar.id=3;polar.kind="ComplexPolar";polar.parameters=TransformProcessingParameters{};polar.inputs={{0,PortDirection::Input,transformed[0]},{1,PortDirection::Input,transformed[1]}};
 std::array<OwnedProcessingContract,2> projected;check(complex_polar_output_contracts(std::get<NumericContract>(transformed[0].view()),projected)==Status::ok,"spectral polar outputs");polar.outputs={{0,PortDirection::Output,projected[0]},{1,PortDirection::Output,projected[1]}};
 ConfiguredProcessingNode amplitude;amplitude.id=4;amplitude.kind="SpectrumAmplitude";amplitude.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{512}}}}};amplitude.inputs={{0,PortDirection::Input,projected[0]}};OwnedProcessingContract peak;check(spectrum_amplitude_output_contract(amplitude,std::get<NumericContract>(projected[0].view()),peak)==Status::ok,"spectral peak geometry");amplitude.outputs={{0,PortDirection::Output,peak}};check(attach_numeric_parameter_ports(amplitude,limits)==Status::ok,"amplitude N");
 ConfiguredProcessingNode step;step.id=6;step.kind="FrequencyStep";step.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{512}}}}};step.inputs={{0,PortDirection::Input,source.outputs.back().contract}};OwnedProcessingContract delta;check(frequency_step_output_contract(std::get<NumericContract>(step.inputs[0].contract.view()),delta)==Status::ok,"ordinary frequency step contract");step.outputs={{0,PortDirection::Output,delta}};check(attach_numeric_parameter_ports(step,limits)==Status::ok,"step accepts measured N");
 NumericProcessingGraph graph;graph.graph_id="spectrum-pipeline";graph.nodes={source,fft,polar,amplitude,step};graph.edges={{1,2,0,0},{2,3,0,0},{2,3,1,1},{3,4,0,0},{1,6,sample_rate_output,0}};Running running(graph);
 struct Spectrum {float tone=0,nyquist=0,dc=0;unsigned count=0;const std::byte* amplitude_data=nullptr;static void output(void* context,const NumericPortFrameView& event){auto& self=*static_cast<Spectrum*>(context);if(!event.frame.stamp.available)return;const auto& data=std::get<NumericView>(event.frame.payload);
   if(event.identity.node==6){double delta=0;check(load_element(data,{0,0,0},delta)==Status::ok && delta==93.75,"runtime divides actual Fs by N into ordinary Float64");}
   if(event.identity.node==4)self.amplitude_data=data.storage.data();
   if(event.identity.node==2 && event.identity.port==2)check(load_element(data,{0,0,0},self.dc)==Status::ok,"raw DC load");
   if(event.identity.node==4){check(data.actual[0]==256,"nonzero half includes exactly N/2 amplitudes");check(load_element(data,{0,4,0},self.tone)==Status::ok && load_element(data,{0,255,0},self.nyquist)==Status::ok,"peak amplitude loads");check(data.contract->coordinates[0].mode==CoordinateMode::None,"amplitude stays indexed without coordinate annotation");++self.count;}
 }} observed;
 std::array<float,512> values{};for(unsigned i=0;i<512;++i)values[i]=float(.125+.3*std::cos(2.*3.14159265358979323846*5*i/512.)+.2*(i%2?-1.:1.));const PlaneLayout plane{0,{sizeof(float),0}};
 NumericView view{&input,std::as_bytes(std::span(values)),std::span(&plane,1),{512,1},{},{}};auto stamp=tick();stamp.sample_count=512;const auto before=allocations;
 check(running.plan->process_numeric_ports(1,{stamp,view},*running.state,{},NumericRuntimeCallbacks{Spectrum::output,nullptr,nullptr,&observed})==Status::ok,"full explicit spectrum pipeline");
 check(allocations==before && observed.count==1 && std::abs(observed.tone-.3)<1e-5 && std::abs(observed.nyquist-.2)<1e-5 && std::abs(observed.dc/512-.125)<1e-5,"tone, unpaired Nyquist amplitude, separate signed DC and noalloc");
}
void array_correction(){
 auto data=base();data.dtype=DType::Float32;data.semantic=Semantic::Numeric;data.representation.alignment=alignof(float);data.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{3,3},ExtentBounds{}}};data.clock={"data",{1,48000},48000};
 data.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Values};
 auto gains=data;gains.coordinates={};gains.clock={"profile",{1,1},0};
 ConfiguredProcessingNode a;a.id=1;a.kind="NetworkInput";a.outputs={{0,PortDirection::Output,own(data)}};
 ConfiguredProcessingNode b;b.id=2;b.kind="NetworkInput";b.outputs={{0,PortDirection::Output,own(gains)}};
 ConfiguredProcessingNode product;product.id=3;product.kind="ArrayMultiply";product.parameters=TransformProcessingParameters{};product.inputs={{0,PortDirection::Input,own(data)},{1,PortDirection::Input,own(gains)}};product.outputs={{0,PortDirection::Output,own(data)}};
 NumericProcessingGraph graph;graph.graph_id="array-correction-independent-coefficients";graph.nodes={a,b,product};graph.edges={{1,3,0,0},{2,3,0,1}};
 auto minimal=profile();minimal.music_enabled=false;std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
 check(PreparedGraph::prepare_numeric(graph,minimal,plan,state)==Status::ok,"array correction is primitive numeric processing without music capability");
 struct Result{unsigned count=0;std::array<float,3> values{};static void output(void* p,const NumericPortFrameView& e){if(e.identity.node!=3 || !e.frame.stamp.available)return;auto& r=*static_cast<Result*>(p);++r.count;check(e.frame.stamp.stream_id=="signal" && e.frame.stamp.timestamp_ticks==42,"correction retains data clock and owned data stream identity");const auto& view=std::get<NumericView>(e.frame.payload);std::array<double,3> expected_coordinates{10,20,30};check(view.coordinates.axes[0].size()==24 && std::equal(view.coordinates.axes[0].begin(),view.coordinates.axes[0].end(),std::as_bytes(std::span(expected_coordinates)).begin()),"array product owns delayed data coordinates");for(std::size_t i=0;i<3;++i)check(load_element(std::get<NumericView>(e.frame.payload),{0,i,0},r.values[i])==Status::ok,"array product values");}} result;
 std::array<double,3> x_coordinates{10,20,30};
 std::array<float,3> samples{1,-2,4},coefficients{2,.5f,0};const PlaneLayout plane{0,{sizeof(float),0}};
 NumericView values{&data,std::as_bytes(std::span(samples)),std::span(&plane,1),{3,1},{},{}},weights{&gains,std::as_bytes(std::span(coefficients)),std::span(&plane,1),{3,1},{},{}};
 values.coordinates.axes[0]=std::as_bytes(std::span(x_coordinates));
 auto signal=tick();signal.timestamp_ticks=42;signal.sample_count=3;signal.stream_id="signal";
 auto profile_stamp=tick(99);profile_stamp.timestamp_ticks=99;profile_stamp.producer_epoch=99;profile_stamp.stream_id="profile";profile_stamp.has_sample_interval=false;
 const auto before=allocations;const NumericRuntimeCallbacks callbacks{Result::output,nullptr,nullptr,&result};
 check(plan->process_numeric_ports(1,{signal,values},*state,{},callbacks)==Status::ok && result.count==0,"data waits for first coefficient array");
 x_coordinates={99,99,99};
 check(plan->process_numeric_ports(2,{profile_stamp,weights},*state,{},callbacks)==Status::ok && result.count==1 && result.values==std::array<float,3>{2,-1,0},"independent coefficient clock, nonuniform multiplication");
 x_coordinates={10,20,30};
 ++signal.sequence;signal.sample_begin+=3;
 check(plan->process_numeric_ports(1,{signal,values},*state,{},callbacks)==Status::ok && result.count==2,"retained coefficient array applies to next data frame");
 coefficients={3,2,-1};++profile_stamp.sequence;
 check(plan->process_numeric_ports(2,{profile_stamp,weights},*state,{},callbacks)==Status::ok && result.count==2,"coefficient update does not duplicate prior data");
 ++signal.sequence;signal.sample_begin+=3;
 check(plan->process_numeric_ports(1,{signal,values},*state,{},callbacks)==Status::ok && result.count==3 && result.values==std::array<float,3>{3,-4,-4} && allocations==before,"updated coefficients and no hot allocations");
 auto unavailable=profile_stamp;unavailable.available=false;++unavailable.sequence;auto no_weights=weights;no_weights.storage={};
 check(plan->process_numeric_ports(2,{unavailable,no_weights},*state,{},callbacks)==Status::ok && result.count==3,"unavailable coefficients invalidate retained cache");
 ++signal.sequence;signal.sample_begin+=3;check(plan->process_numeric_ports(1,{signal,values},*state,{},callbacks)==Status::ok && result.count==3,"no stale coefficient multiplication after unavailable marker");
 profile_stamp.sequence=unavailable.sequence+1;check(plan->process_numeric_ports(2,{profile_stamp,weights},*state,{},callbacks)==Status::ok && result.count==4,"fresh coefficients resume pending current data");
 auto missing_data=signal;missing_data.available=false;++missing_data.sequence;auto no_data=values;no_data.storage={};check(plan->process_numeric_ports(1,{missing_data,no_data},*state,{},callbacks)==Status::ok && result.count==4,"unavailable data clears pending data only");
 ++profile_stamp.sequence;check(plan->process_numeric_ports(2,{profile_stamp,weights},*state,{},callbacks)==Status::ok && result.count==4,"coefficient update cannot revive unavailable data");
 signal.sequence=missing_data.sequence+1;signal.sample_begin+=3;check(plan->process_numeric_ports(1,{signal,values},*state,{},callbacks)==Status::ok && result.count==5 && allocations==before,"retained healthy coefficients survive unavailable data without allocations");
}
void array_product_bypass(){
 auto c=base();c.dtype=DType::Float32;c.semantic=Semantic::Numeric;c.representation.alignment=alignof(float);c.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{3,3},ExtentBounds{}}};c.clock={"data",{1,48000},48000};
 ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={{0,PortDirection::Output,own(c)}};
 ConfiguredProcessingNode product;product.id=2;product.kind="ArrayMultiply";product.enabled=false;product.parameters=TransformProcessingParameters{};product.inputs={{0,PortDirection::Input,own(c)},{1,PortDirection::Input,own(c)}};product.outputs={{0,PortDirection::Output,own(c)}};
 auto coefficient_source=source;coefficient_source.id=3;
 NumericProcessingGraph graph;graph.graph_id="array-bypass";graph.nodes={source,product,coefficient_source};graph.edges={{1,2,0,0},{3,2,0,1}};Running running(graph);
 struct Result{unsigned count=0,unavailable=0;static void output(void* user,const NumericPortFrameView& e){if(e.identity.node!=2)return;if(!e.frame.stamp.available){++static_cast<Result*>(user)->unavailable;return;}++static_cast<Result*>(user)->count;const auto& v=std::get<NumericView>(e.frame.payload);for(unsigned i=0;i<3;++i){float x=0;check(load_element(v,{0,i,0},x)==Status::ok && x==float(i+1),"bypass preserves every data element without coefficient input");}}} result;
 std::array<float,3> samples{1,2,3};const PlaneLayout plane{0,{4,0}};NumericView values{&c,std::as_bytes(std::span(samples)),std::span(&plane,1),{3,1},{},{}};auto stamp=tick();stamp.sample_count=3;const auto before=allocations;
 check(running.plan->process_numeric_ports(1,{stamp,values},*running.state,{},NumericRuntimeCallbacks{Result::output,nullptr,nullptr,&result})==Status::ok && result.count==1 && allocations==before,"array bypass is immediate identity, no coefficient wait or allocation");
 check(running.plan->process_numeric_ports(3,{stamp,values},*running.state,{},NumericRuntimeCallbacks{Result::output,nullptr,nullptr,&result})==Status::ok && result.count==1,"coefficient updates cannot duplicate bypassed data");
 auto missing=values;missing.storage={};auto marker=stamp;++marker.sequence;marker.available=false;
 check(running.plan->process_numeric_ports(3,{marker,missing},*running.state,{},NumericRuntimeCallbacks{Result::output,nullptr,nullptr,&result})==Status::ok && result.count==1 && result.unavailable==0,"unavailable auxiliary factors cannot invalidate bypassed data");
 ++stamp.sequence;stamp.sample_begin+=3;
 check(running.plan->process_numeric_ports(1,{stamp,values},*running.state,{},NumericRuntimeCallbacks{Result::output,nullptr,nullptr,&result})==Status::ok && result.count==2 && allocations==before,"bypass survives coefficient unavailability without allocations");
}
void variadic_product(){
 for(unsigned driver=0;driver<4;++driver)for(bool data_first:{false,true}){
    auto data=base();data.dtype=DType::Float32;data.representation.alignment=alignof(float);data.semantic=Semantic::Numeric;data.quantity=Quantity::SpectrumReal;data.unit=NumericUnit::RawAmplitude;data.clock={"signal",{1,48000},48000};data.shape={Rank::Vector,{Axis::Bin,Axis::None},{ExtentBounds{3,3},ExtentBounds{}}};data.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz};
    auto coefficient=data;coefficient.quantity=Quantity::Unspecified;coefficient.unit=NumericUnit::Values;coefficient.clock={"coefficient",{1,1},0};coefficient.coordinates={};coefficient.shape.axes[0]=Axis::Sample;
    ConfiguredProcessingNode product;product.id=8;product.kind="ArrayMultiply";product.parameters=TransformProcessingParameters{};NumericProcessingGraph graph;graph.graph_id="variadic-product";constexpr std::array<PortId,4> ports{0,2,5,9};
    product.parameters=TransformProcessingParameters{{{"frame_input",NumericLiteral{std::uint64_t{ports[driver]}}}}};
    for(unsigned i=0;i<4;++i){auto contract=own(i==driver?data:coefficient);ConfiguredProcessingNode source;source.id=NodeId(i+1);source.kind="NetworkInput";source.outputs={{0,PortDirection::Output,contract}};graph.nodes.push_back(source);product.inputs.push_back({ports[i],PortDirection::Input,contract});graph.edges.push_back({source.id,8,0,ports[i]});}
    OwnedProcessingContract output;std::size_t selected=SIZE_MAX;check(array_product_output_contract(product,output,&selected)==Status::ok && selected==driver,"explicit frame port retains signal coordinates at any factor position");product.outputs={{0,PortDirection::Output,output}};graph.nodes.push_back(product);Running running(graph);
    struct Result{unsigned count=0,unavailable=0;std::array<float,3> values{};static void output(void* user,const NumericPortFrameView& event){if(event.identity.node!=8)return;auto& result=*static_cast<Result*>(user);if(!event.frame.stamp.available){++result.unavailable;return;}++result.count;check(event.frame.stamp.stream_id=="signal" && event.frame.stamp.timestamp_ticks==42,"factor permutation retains frame input time and owning stream identity");const auto& view=std::get<NumericView>(event.frame.payload);check(view.contract->quantity==Quantity::SpectrumReal && view.contract->unit==NumericUnit::RawAmplitude && view.contract->clock.sample_rate==48000,"factor permutation retains frame context");std::array<double,3> expected{10,20,30};check(view.coordinates.axes[0].size()==24 && std::equal(view.coordinates.axes[0].begin(),view.coordinates.axes[0].end(),std::as_bytes(std::span(expected)).begin()),"variadic product owns delayed coordinates");for(unsigned i=0;i<3;++i)check(load_element(view,{0,i,0},result.values[i])==Status::ok,"read variadic product");}} result;
    const NumericRuntimeCallbacks callbacks{Result::output,nullptr,nullptr,&result};const PlaneLayout plane{0,{4,0}};std::array<std::array<float,3>,4> values{};std::array<double,3> x{10,20,30};unsigned ordinal=0;for(unsigned i=0;i<4;++i)if(i==driver)values[i]={1,2,3};else{const float factor=ordinal==0?2.f:ordinal==1?.5f:-4.f;values[i].fill(factor);++ordinal;}
    auto stamp=tick();stamp.has_sample_interval=false;stamp.timestamp_ticks=42;stamp.stream_id="signal";
    auto send=[&](unsigned lane,bool available=true){auto current=stamp;if(lane!=driver){current.producer_epoch=99;current.timestamp_ticks=99;current.stream_id="coefficient";}current.available=available;NumericView view{lane==driver?&data:&coefficient,std::as_bytes(std::span(values[lane])),std::span(&plane,1),{3,1},{},{}};if(lane==driver)view.coordinates.axes[0]=std::as_bytes(std::span(x));if(!available)view.storage={};return running.plan->process_numeric_ports(NodeId(lane+1),{current,view},*running.state,{},callbacks);};
    const auto before=allocations;
    if(data_first){check(send(driver)==Status::ok && result.count==0,"frame waits for all factors");x={99,99,99};}
    for(unsigned i=0;i<4;++i)if(i!=driver)check(send(i)==Status::ok,"accept independently clocked factor snapshot");
    if(!data_first)check(send(driver)==Status::ok,"accept frame after factors");
    check(result.count==1 && result.values==std::array<float,3>{-4,-8,-12},"four-factor multiplication commutes with input order and arrival order");x={10,20,30};++stamp.sequence;const auto auxiliary=driver==0?1u:0u;
    check(send(auxiliary)==Status::ok && result.count==1,"factor update does not duplicate a consumed frame");
    check(send(driver)==Status::ok && result.count==2,"retained factors apply to subsequent frames");++stamp.sequence;
    check(send(auxiliary,false)==Status::ok && result.unavailable==1,"unavailable factor invalidates the complete product");
    check(send(driver)==Status::ok && result.count==2,"unavailable factor cannot be reused");++stamp.sequence;
    check(send(auxiliary)==Status::ok && result.count==3 && allocations==before,"fresh factor releases only the post-unavailable frame, no hot allocations");
 }
}
void concat_intervals(){
 auto left=base();left.dtype=DType::Float32;left.representation.alignment=4;left.clock={"join",{1,48000},48000};left.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{2,2},ExtentBounds{}}};auto right=left;right.shape.extents[0]={3,3};
 ConfiguredProcessingNode a;a.id=1;a.kind="NetworkInput";a.outputs={{0,PortDirection::Output,own(left)}};ConfiguredProcessingNode b;b.id=2;b.kind="NetworkInput";b.outputs={{0,PortDirection::Output,own(right)}};
 ConfiguredProcessingNode join;join.id=3;join.kind="ArrayConcat";join.parameters=TransformProcessingParameters{};join.inputs={{0,PortDirection::Input,a.outputs[0].contract},{1,PortDirection::Input,b.outputs[0].contract}};OwnedProcessingContract together;check(array_concat_output_contract(join,together)==Status::ok,"concatenate ordinary arrays of different sizes");join.outputs={{0,PortDirection::Output,together}};
 NumericProcessingGraph graph;graph.graph_id="concat-intervals";graph.nodes={a,b,join};graph.edges={{1,3,0,0},{2,3,0,1}};Running running(graph);
 struct Result{unsigned count=0,missing=0;std::array<float,5> values{};static void output(void* user,const NumericPortFrameView& event){if(event.identity.node!=3)return;auto& r=*static_cast<Result*>(user);if(!event.frame.stamp.available){++r.missing;return;}++r.count;const auto& value=std::get<NumericView>(event.frame.payload);for(unsigned i=0;i<5;++i)check(load_element(value,{0,i,0},r.values[i])==Status::ok,"read joined ordinary array");}} result;
 std::array<float,2> l{1,2};std::array<float,3> r{3,4,5};const PlaneLayout plane{0,{4,0}};
 auto send=[&](bool first,std::uint64_t sequence,std::int64_t time,bool available=true){auto stamp=tick(sequence);stamp.has_sample_interval=false;stamp.timestamp_ticks=time;stamp.available=available;
    NumericView value{first?&left:&right,first?std::span<const std::byte>(std::as_bytes(std::span(l))):std::span<const std::byte>(std::as_bytes(std::span(r))),{&plane,1},{first?2u:3u,1},{},{}};if(!available)value.storage={};return running.plan->process_numeric_ports(first?1:2,{stamp,value},*running.state,{},NumericRuntimeCallbacks{Result::output,nullptr,nullptr,&result});};
 const auto before=allocations;check(send(true,1,10)==Status::ok && send(false,1,11)==Status::ok && result.count==0,"concat never combines mismatched intervals");r.fill(99);l={6,7};
 check(send(true,2,11)==Status::ok && result.count==1 && result.values==std::array<float,5>{6,7,3,4,5},"concat owns a delayed right row while waiting for matching time");
 check(send(true,3,12,false)==Status::ok && result.missing==1,"concat propagates unavailable and clears both pending rows");r={10,11,12};l={8,9};
 check(send(false,2,12)==Status::ok && result.count==1 && send(true,4,12)==Status::ok && result.count==2 && result.values==std::array<float,5>{8,9,10,11,12} && allocations==before,"concat recovery emits one fresh matching frame without allocation");
}
void frequency_step_math(){
 auto input=base();input.unit=NumericUnit::Hertz;
 ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={{0,PortDirection::Output,own(input)}};
 ConfiguredProcessingNode step;step.id=2;step.kind="FrequencyStep";step.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{2048}}}}};step.inputs={{0,PortDirection::Input,own(input)}};
 OwnedProcessingContract delta;check(frequency_step_output_contract(input,delta)==Status::ok,"step scalar Float64 output contract");step.outputs={{0,PortDirection::Output,delta}};
 NumericProcessingGraph graph;graph.graph_id="actual-frequency-step";graph.nodes={source,step};graph.edges={{1,2,0,0}};Running running(graph);
 struct Result{double value=0;unsigned count=0,missing=0;static void output(void* user,const NumericPortFrameView& event){if(event.identity.node!=2)return;auto& r=*static_cast<Result*>(user);if(!event.frame.stamp.available){++r.missing;return;}++r.count;check(load_element(std::get<NumericView>(event.frame.payload),{0,0,0},r.value)==Status::ok,"read ordinary Float64 step");}} result;
 const PlaneLayout plane{0,{0,0}};auto stamp=tick();stamp.has_sample_interval=false;
 const auto before=allocations;
 for(const std::uint32_t fs:{44100u,48000u,0u,32000u}){auto value=fs;NumericView frame{&input,std::as_bytes(std::span(&value,1)),std::span(&plane,1),{1,1},{},{}};
    const auto status=running.plan->process_numeric_ports(1,{stamp,frame},*running.state,{},NumericRuntimeCallbacks{Result::output,nullptr,nullptr,&result});
    check(status==(fs?Status::ok:Status::unavailable),"frequency step handles actual Fs events");
    if(fs)check(result.value==double(fs)/2048,"frequency step uses actual scalar Fs, independent of clock.sample_rate");++stamp.sequence;
 }
 check(result.count==3 && result.missing==1 && allocations==before,"zero Fs unavailable and recovered step, no hot allocation");
}
void generic_number(){
 auto scalar=base();scalar.dtype=DType::Float64;scalar.representation.alignment=alignof(double);scalar.clock={"configuration",{1,120},0};
 ConfiguredProcessingNode n;n.id=1;n.kind="Number";n.parameters=EndpointProcessingParameters{{{"value",NumericLiteral{-1.25}}}};n.outputs={{0,PortDirection::Output,own(scalar)}};
 NumericProcessingGraph graph;graph.graph_id="generic-number";graph.nodes={n};Running running(graph);
 struct Result{double number=0;static void output(void* p,const NumericPortFrameView& e){if(e.frame.stamp.available)check(load_element(std::get<NumericView>(e.frame.payload),{0,0,0},static_cast<Result*>(p)->number)==Status::ok,"typed number scalar");}} result;
 const auto before=allocations;check(running.plan->process_clock(1,tick(),*running.state,{},NumericRuntimeCallbacks{Result::output,nullptr,nullptr,&result})==Status::ok && allocations==before && result.number==-1.25,"Number publishes negative float64 without integer conversion or allocation");
}
std::array<float,48> expected_note_row(const std::array<float,256>& real,
    const std::array<float,256>& imaginary,double window_gain,bool normalize,std::string_view mode){
    std::array<float,257> power{};
    for(std::size_t i=0;i<real.size();++i){
        const double squared=double(real[i])*real[i]+double(imaginary[i])*imaginary[i];
        const bool endpoint=i+1==real.size();
        const bool amplitude=mode=="magnitude" || mode=="peak_amplitude";
        const double factor=!normalize && !amplitude?2.:
            (amplitude?(endpoint?4.:8.):(endpoint?2.:4.))/(window_gain*window_gain);
        power[i+1]=float(squared*factor);
    }
    dsp_kernels::NoteSalience kernel;
    check(kernel.prepare(power.size(),48000./512.,48,48,440.,1),"prepare independent note reference");
    std::array<float,48> expected{};
    check(kernel.process(power,expected),"evaluate independent note reference");
    return expected;
}

void corrected_musical_pipeline(){
 for(const auto* function:{"hann","hamming","blackman","rectangular","bartlett","blackman_harris","flat_top","bypass"})for(const auto* representation:{"magnitude","peak_amplitude","power","psd"})for(const bool normalize:{false,true}){
    auto input=base();input.dtype=DType::Float32;input.semantic=Semantic::Window;input.representation.alignment=alignof(float);input.clock={"window",{1,48000},48000};input.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{512,512},ExtentBounds{}}};
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={{0,PortDirection::Output,own(input)}};
    ConfiguredProcessingNode window;window.id=2;window.kind="WindowFunction";window.enabled=std::string_view(function)!="bypass";window.parameters=TransformProcessingParameters{{{"function",std::string(window.enabled?function:"hann")}}};window.inputs={{0,PortDirection::Input,own(input)}};
    auto windowed=input;auto length=metadata(7,"ArrayLength",input);window.inputs={{0,PortDirection::Input,length.outputs[0].contract}};std::get<TransformProcessingParameters>(window.parameters).values.push_back({"size",NumericLiteral{std::uint64_t{512}}});OwnedProcessingContract window_coefficients;check(array_generator_output_contract(window,std::get<NumericContract>(length.outputs[0].contract.view()),window_coefficients)==Status::ok,"window generator numeric N contract");window.outputs={{0,PortDirection::Output,window_coefficients}};
    ConfiguredProcessingNode apply_window;apply_window.id=9;apply_window.kind="ArrayMultiply";apply_window.parameters=TransformProcessingParameters{};apply_window.inputs={{0,PortDirection::Input,own(input)},{1,PortDirection::Input,window_coefficients}};apply_window.outputs={{0,PortDirection::Output,own(windowed)}};check(attach_numeric_parameter_ports(window,limits)==Status::ok,"window exposes float64 coherent gain");
    ConfiguredProcessingNode fft;fft.id=3;fft.kind="RealFourier";fft.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{512}}}}};fft.inputs={{0,PortDirection::Input,own(windowed)}};std::array<OwnedProcessingContract,4> frequency;check(fourier_output_contracts(fft,windowed,frequency)==Status::ok,"fused FFT contract");for(PortId p=0;p<4;++p)fft.outputs.push_back({p,PortDirection::Output,frequency[p]});
    ConfiguredProcessingNode polar;polar.id=4;polar.kind="AmplitudePhase";polar.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{512}}},{"representation",std::string{representation}},{"normalize",normalize}}};polar.inputs={{0,PortDirection::Input,frequency[0]},{1,PortDirection::Input,frequency[1]}};std::array<OwnedProcessingContract,2> projection;check(amplitude_phase_output_contracts(polar,std::get<NumericContract>(frequency[0].view()),projection)==Status::ok,"fused amplitude phase contract");polar.outputs={{0,PortDirection::Output,projection[0]},{1,PortDirection::Output,projection[1]}};check(attach_numeric_parameter_ports(polar,limits)==Status::ok,"fused numeric inputs");
    auto coefficients=std::get<NumericContract>(projection[0].view());coefficients.semantic=Semantic::Numeric;coefficients.quantity=Quantity::Unspecified;coefficients.unit=NumericUnit::Values;
    ConfiguredProcessingNode fill;fill.id=8;fill.kind="ArrayFill";fill.parameters=TransformProcessingParameters{{{"value",NumericLiteral{2.}},{"values",NumericArray{}}}};auto spectrum_length=metadata(10,"ArrayLength",std::get<NumericContract>(frequency[0].view()));std::get<TransformProcessingParameters>(fill.parameters).values.push_back({"size",NumericLiteral{std::uint64_t{256}}});fill.inputs={{0,PortDirection::Input,spectrum_length.outputs[0].contract}};OwnedProcessingContract fill_coefficients;check(array_generator_output_contract(fill,std::get<NumericContract>(spectrum_length.outputs[0].contract.view()),fill_coefficients)==Status::ok,"coefficient generator numeric N contract");fill.outputs={{0,PortDirection::Output,fill_coefficients}};
    ConfiguredProcessingNode norm;norm.id=5;norm.kind="ArrayMultiply";norm.parameters=TransformProcessingParameters{};norm.inputs={{0,PortDirection::Input,projection[0]},{1,PortDirection::Input,fill_coefficients}};const auto normalized=projection[0];norm.outputs={{0,PortDirection::Output,normalized}};
    ResolvedProcessingNode resolved;check(bool(resolve_processing_node(norm,limits,resolved)),"generic two-array correction has no Fs/N requirements");
    auto invalid=norm;auto mismatched=coefficients;mismatched.shape.extents[0]={255,255};invalid.inputs[1].contract=own(mismatched);check(!resolve_processing_node(invalid,limits,resolved),"array correction rejects incompatible dimensions");
    const std::string_view mode=representation;
    const auto row_representation=!normalize?(mode=="power" || mode=="psd"?"power":"magnitude"):representation;
    const double window_gain=processing_window_gain(window,512);
    const double row_gain=normalize?1.:window_gain;
    const double row_bandwidth=mode=="psd" && normalize?48000.*processing_window_gain(window,512,true)/(window_gain*window_gain):1.;
    ConfiguredProcessingNode row;row.id=6;row.kind="NoteSalience";row.parameters=TransformProcessingParameters{{{"first_midi",NumericLiteral{std::int64_t{48}}},{"notes",NumericLiteral{std::int64_t{48}}},{"harmonics",NumericLiteral{std::int64_t{1}}},{"input_representation",std::string(row_representation)},{"first_bin",NumericLiteral{std::uint64_t{1}}},{"bin_hz",NumericLiteral{48000./512.}},{"power_bandwidth_hz",NumericLiteral{row_bandwidth}},{"coherent_gain",NumericLiteral{row_gain}}}};row.inputs={{0,PortDirection::Input,normalized}};OwnedProcessingContract notes;check(music_output_contract(row,std::get<NumericContract>(normalized.view()),notes)==Status::ok,"musical row accepts calibrated amplitude");row.outputs={{0,PortDirection::Output,notes}};
    NumericProcessingGraph graph;graph.graph_id="window-corrected-musical-row";graph.nodes={source,window,fft,polar,norm,row,length,fill,apply_window,spectrum_length};graph.edges={{7,2,0,0},{1,9,0,0},{2,9,0,1},{9,3,0,0},{3,4,0,0},{3,4,1,1},{2,4,coherent_gain_output,coherent_gain_input},{1,7,0,0},{3,10,0,0},{10,8,0,0},{8,5,0,1},{5,6,0,0},{2,4,power_gain_output,power_gain_input}};
    graph.edges.push_back({4,5,0,0});
    NumericProcessingGraph evaluated,executable;const auto evaluated_status=resolve_numeric_parameter_bindings(graph,limits,evaluated,executable);
    if(!evaluated_status)std::fprintf(stderr,"window=%s parameter error=%u node=%d port=%u\n",function,unsigned(evaluated_status.error),evaluated_status.node,unsigned(evaluated_status.port));
    check(bool(evaluated_status),"corrected graph parameter admission");
    NumericProcessingGraph lowered_meta,converted,lowered_routes;std::vector<RoutingAlias> aliases;
    const auto lm=lower_numeric_metadata_outputs(executable,limits,lowered_meta,aliases);if(!lm)std::fprintf(stderr,"metadata error=%u node=%d port=%u\n",unsigned(lm.error),lm.node,unsigned(lm.port));check(bool(lm),"metadata lowering");
    const auto calibration_aliases=aliases;
    const auto ld=lower_dtype_edges(lowered_meta,limits,converted);if(!ld)std::fprintf(stderr,"dtype error=%u node=%d port=%u\n",unsigned(ld.error),ld.node,unsigned(ld.port));check(bool(ld),"dtype lowering");
    const auto lr=lower_routing_points(converted,limits,lowered_routes,aliases);if(!lr)std::fprintf(stderr,"routing error=%u node=%d port=%u\n",unsigned(lr.error),lr.node,unsigned(lr.port));check(bool(lr),"routing lowering");
    std::array<std::size_t,128> workspace{};ProcessingGraphAdmission admitted;const auto admission=admit_processing_graph(lowered_routes,limits,PreparedGraph::numeric_capabilities(profile()),workspace,admitted);
    if(!admission)std::fprintf(stderr,"full admission error=%u node=%d port=%u\n",unsigned(admission.error),admission.node,unsigned(admission.port));check(bool(admission),"full graph admission");
    Running running(graph);
    struct Result{double gain=0,power_gain=0;float amplitude=0,nyquist=0,normalized=0,phase=0;std::array<float,256> re{},im{},projected{};std::array<float,48> notes{};unsigned rows=0,gain_events=0,power_events=0;static void output(void* p,const NumericPortFrameView& e){auto& r=*static_cast<Result*>(p);if(!e.frame.stamp.available)return;const auto& v=std::get<NumericView>(e.frame.payload);
      if(e.identity.node==3 && e.identity.port<2)for(std::size_t i=0;i<256;++i)check(load_element(v,{0,i,0},e.identity.port?r.im[i]:r.re[i])==Status::ok,"retain same Re/Im for batch/graph comparison");
      if(e.identity.node==4 && e.identity.port==0)for(std::size_t i=0;i<256;++i)check(load_element(v,{0,i,0},r.projected[i])==Status::ok,"retain graph spectral values");
      if(e.identity.node==2 && e.identity.port==0){float first=0,middle=0;check(v.actual[0]==512 && load_element(v,{0,0,0},first)==Status::ok && load_element(v,{0,256,0},middle)==Status::ok && std::isfinite(first) && middle>.99f,"generator publishes full coefficient bank, not multiplied signal");}
      if(e.identity.node==2 && e.identity.port==coherent_gain_output){check(load_element(v,{0,0,0},r.gain)==Status::ok,"window gain load");++r.gain_events;}
      if(e.identity.node==2 && e.identity.port==power_gain_output){check(load_element(v,{0,0,0},r.power_gain)==Status::ok,"window power gain load");++r.power_events;}
      if(e.identity.node==4 && e.identity.port==0){check(load_element(v,{0,4,0},r.amplitude)==Status::ok,"corrected amplitude load");check(load_element(v,{0,255,0},r.nyquist)==Status::ok,"corrected Nyquist load");}
      if(e.identity.node==4 && e.identity.port==1)check(load_element(v,{0,4,0},r.phase)==Status::ok,"fused phase load");
      if(e.identity.node==5)check(load_element(v,{0,4,0},r.normalized)==Status::ok,"manual amplitude normalization");
      if(e.identity.node==6){check(v.actual[0]==48 && v.contract->coordinates[0].unit==NumericUnit::Values,"musical row has MIDI coordinates, not Hz");float peak=0;for(std::size_t i=0;i<48;++i){float x=0;check(load_element(v,{0,i,0},x)==Status::ok && std::isfinite(x),"finite musical row");r.notes[i]=x;peak=std::max(peak,x);}check(peak>0,"nonempty musical row");++r.rows;}
    }} result;
    for(const auto port:{coherent_gain_output,power_gain_output}) {
      const auto alias=std::find_if(calibration_aliases.begin(),calibration_aliases.end(),[&](const auto& item){return item.node==2&&item.port==port;});
      check(alias!=calibration_aliases.end(),"both window calibration outputs have explicit prepared constant aliases");
      const auto before=allocations;
      check(running.plan->process_clock(alias->source,tick(),*running.state,{},NumericRuntimeCallbacks{Result::output,nullptr,nullptr,&result})==Status::ok && allocations==before,"window calibration publishes on configuration clock without allocation");
    }
    check(result.gain_events==1 && result.power_events==1 && result.rows==0,"calibration is observable before signal delivery without running spectral DSP");
    std::array<float,512> data{};for(std::size_t i=0;i<data.size();++i)data[i]=.3f*float(std::cos(2*3.14159265358979323846*5*i/512))+.07f*float(std::cos(2*3.14159265358979323846*13*i/512))+.05f*float(std::sin(2*3.14159265358979323846*27*i/512))+.03f*float(std::cos(2*3.14159265358979323846*61*i/512))+(i%2?-.2f:.2f);
    const PlaneLayout plane{0,{sizeof(float),0}};NumericView frame{&input,std::as_bytes(std::span(data)),std::span(&plane,1),{512,1},{},{}};auto stamp=tick();stamp.sample_count=512;
    const auto before=allocations;check(running.plan->process_numeric_ports(1,{stamp,frame},*running.state,{},NumericRuntimeCallbacks{Result::output,nullptr,nullptr,&result})==Status::ok && allocations==before,"window-corrected musical path has no hot allocations");
    const auto expected_notes=expected_note_row(result.re,result.im,window_gain,normalize,mode);
    for(std::size_t i=0;i<expected_notes.size();++i)check(std::abs(result.notes[i]-expected_notes[i])<=std::max(1e-5f,std::abs(expected_notes[i])*.002f),"explicit salience representation, coherent gain and ENBW match raw FFT reference");
    const auto cg=processing_window_gain(window,512),pg=processing_window_gain(window,512,true);
    dsp_kernels::SpectrumProjection kind;check(dsp_kernels::parse_spectrum_projection(representation,kind),"shared representation registry");
    dsp_kernels::PreparedSpectrumProjection batch;check(batch.prepare(512,48000,{512,cg,pg},{kind,normalize}),"batch uses graph compensation options");
    std::array<float,514> interleaved{};std::array<float,257> batchOutput{};
    for(std::size_t i=0;i<256;++i){interleaved[2*(i+1)]=result.re[i];interleaved[2*(i+1)+1]=result.im[i];}
    check(batch.process(interleaved,batchOutput),"project same FFT with batch layout adapter");
    for(std::size_t i=0;i<256;++i)check(result.projected[i]==batchOutput[i+1],"graph and batch agree exactly for every bin, mode, window and compensation state");
    check(cg>1 && pg>1,"window scalar ports carry sums, not means");
    const auto expected=[&](double amplitude,bool nyquist){const std::string_view mode=representation;const double paired=nyquist?1.:2.;const double magnitude=amplitude*cg/paired;
        if(!normalize)return mode=="power" || mode=="psd"?magnitude*magnitude:magnitude;
        if(mode=="magnitude")return magnitude/cg;
        if(mode=="peak_amplitude")return amplitude;
        const double power=amplitude*amplitude/paired,psd=magnitude*magnitude*paired/(48000*pg);
        if(mode=="power")return power;return psd;};
    const auto tone=expected(.3,false),nyquist=expected(.2,true);
    if(std::abs(result.amplitude-tone)>std::max(.001,std::abs(tone)*.001))std::fprintf(stderr,"function=%s representation=%s actual=%g expected=%g\n",function,representation,result.amplitude,tone);
    check(std::abs(result.amplitude-tone)<std::max(.001,std::abs(tone)*.001) && std::abs(result.nyquist-nyquist)<std::max(.001,std::abs(nyquist)*.001) && std::abs(result.normalized-2*tone)<std::max(.002,std::abs(tone)*.002) && result.rows==1 && std::abs(result.phase)<.0001f,"four linear representations, window correction, Nyquist, array multiplication and musical row");
    check(result.gain_events==1 && result.power_events==1,"PCM does not republish configuration calibration outputs");
    check(std::abs(result.power_gain-pg)<1e-12,"published power window correction equals prepared squared coefficient sum");
    check(std::abs(result.gain-processing_window_gain(window,512))<1e-12,"published window correction equals prepared parameter");
 }
}

struct NumberHistoryFixture {
    ConfiguredProcessingNode accumulator,source,view;
    NumericProcessingGraph graph;
    std::shared_ptr<const PreparedGraph> plan;
    std::unique_ptr<RuntimeContext> state;
    NumericContract ingress;
    PlaneLayout plane{0,{sizeof(float),0}};
    unsigned sink_rows=0,compact_rows=0;
    static void compact(void* context,const NumericHistoryRowView& event) {
        auto& fixture=*static_cast<NumberHistoryFixture*>(context);
        if(event.endpoint.identity.node!=2 || event.event!=NumericHistoryEventKind::Row)return;
        const auto& value=std::get<NumericView>(event.compact.frame.payload);
        float actual=0;
        check(value.contract->shape.rank==Rank::Scalar && value.storage.size()==sizeof(float) &&
              load_element(value,{0,0,0},actual)==Status::ok &&
              actual==float(event.accepted_row_ordinal) && event.capacity==4 &&
              event.filled_count==std::min<std::uint64_t>(event.accepted_row_ordinal,4),
              "number history emits one borrowed scalar per append");
        ++fixture.compact_rows;
    }
    static void sink(void* context,const NumericPortFrameView& event) {
        auto& fixture=*static_cast<NumberHistoryFixture*>(context);
        if(event.identity.node!=3 || event.direction!=PortDirection::Input)return;
        const auto& value=std::get<NumericView>(event.frame.payload);
        check(event.status==Status::ok && value.contract->shape.rank==Rank::Vector &&
              value.contract->semantic==Semantic::History,"NumberAccumulator delivers vector History to generic view");
        ++fixture.sink_rows;
    }
    NumberHistoryFixture() {
        NodeFactorySettings settings;settings.history_capacity=4;
        check(bool(make_processing_node("NumberAccumulator",2,limits,accumulator,settings)),"number history factory");
        source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};
        source.outputs={{0,PortDirection::Output,accumulator.inputs[0].contract}};
        check(bool(make_processing_node("VectorLineView",3,limits,view,settings)),"generic vector view factory");
        view.inputs[0].contract=accumulator.outputs[0].contract;
        graph.graph_id="number-history";graph.nodes={source,accumulator,view};
        graph.edges={{1,2,0,0},{2,3,0,0}};
        ingress=std::get<NumericContract>(graph.nodes[0].outputs[0].contract.view());
        check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"number history prepare");
    }
    void append(std::uint64_t ordinal) {
        const float value=float(ordinal);
        auto stamp=tick(ordinal);stamp.timestamp_ticks=std::int64_t(ordinal);
        NumericView frame{&ingress,std::as_bytes(std::span(&value,1)),std::span(&plane,1),{1,1},{},{}};
        const auto before=allocations;
        check(plan->process_numeric_ports(1,{stamp,frame},*state,{},{nullptr,sink,compact,this})==Status::ok &&
              allocations==before,"number history O(1) append without allocation");
    }
    void expect_latest() {
        DataFrameView frame;std::uint64_t filled=0;
        check(plan->history(2,0,*state,frame,&filled)==Status::ok && filled==4,"number history occupancy");
        const auto& values=std::get<NumericView>(frame.payload);
        check(values.actual==std::array<std::size_t,2>{4,1} &&
              validate_view(frame,frames)==Status::ok,"number history bounded vector ring");
        for(std::size_t i=0;i<4;++i) {
            float actual=0;
            check(load_element(values,{0,i,0},actual)==Status::ok &&
                  actual==float(i+3),"number history preserves order after wrap");
        }
        check(sink_rows==6 && compact_rows==6,"generic vector view and compact delivery receive each update");
    }
};
void number_history() {
    NumberHistoryFixture fixture;
    for(std::uint64_t ordinal=1;ordinal<=6;++ordinal)fixture.append(ordinal);
    fixture.expect_latest();
}

}
void* operator new(std::size_t n){++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int main(){number_history();constants();constant_materialization();constant_capability_admission();lengths();matrix();parameter_bindings();native_selector();spectrum_pipeline();corrected_musical_pipeline();generic_number();array_correction();array_product_bypass();variadic_product();frequency_step_math();concat_intervals();std::puts("ordinary UInt32 Fs/N, native selector, explicit spectrum pipeline, parameter bindings, metadata, noalloc PASS");}
