#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <dsp_kernels/rhythm.h>
#include <dsp_kernels/musical_features.h>
#include <core_app/numeric_default_graph.h>
#ifdef BARELIEF_TEST_DESKTOP_SPEEX
#include <speex_backend.h>
#endif
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <cmath>

namespace {
using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
constexpr FrameLimits frames{1,65536,1048576,65536,1024,65536};
constexpr ProcessingAdmissionLimits limits{64,128,16,32,1048576,4194304,frames};
template<class T> void check(const T& condition,const std::source_location where=std::source_location::current()) {
    if(!condition){std::fprintf(stderr,"rhythm runtime check failed line %u\n",where.line());std::exit(1);}
}
NumericRuntimeProfile profile() {
    NumericRuntimeProfile p{limits,{{4194304,4194304,4194304,131072},0,16777216},{65536,8,1048576,1048576,1048576},{true,true,true}};
    p.music_enabled=true;return p;
}
ConfiguredProcessingNode analyzer_node() {
    ConfiguredProcessingNode node;
    check(make_processing_node("RhythmAnalyzer",2,limits,node,NodeFactorySettings{32768,4096,256,512}));
    return node;
}
NumericProcessingGraph graph() {
    auto analyzer=analyzer_node();ConfiguredProcessingNode source;
    source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};
    auto input=std::get<NumericContract>(analyzer.inputs[0].contract.view());
    input.shape={Rank::Vector,{Axis::Band,Axis::None},{ExtentBounds{64,64},ExtentBounds{}}};input.semantic=Semantic::Numeric;
    OwnedProcessingContract owned;check(own_processing_contract(input,frames,65536,owned)==Status::ok);
    source.outputs={{0,PortDirection::Output,owned}};
    ConfiguredProcessingNode history;check(make_processing_node("Spectrogram",3,limits,history,NodeFactorySettings{32768,4096,256,512}));
    check(configure_processing_input(history,input,limits));
    check(configure_processing_input(analyzer,history.outputs[0].contract.view(),limits));
    NumericProcessingGraph result;result.graph_id="rhythm-runtime";result.execution_interval={1,60};
    result.nodes={source,history,analyzer};result.edges={{1,3,0,0},{3,2,0,0}};return result;
}
struct Observation {
    std::array<float,rhythm_output_count> values{};
    std::array<bool,rhythm_output_count> available{};
    static void receive(void* user,const NumericPortFrameView& event) {
        if(event.identity.node!=2 || event.direction!=PortDirection::Output)return;
        auto& self=*static_cast<Observation*>(user);const auto port=event.identity.port;
        check(port<rhythm_output_count);check(event.status==Status::ok || event.status==Status::unavailable);
        check(validate_view(event.frame,frames)==event.status);
        self.available[port]=event.frame.stamp.available;
        const auto& view=std::get<NumericView>(event.frame.payload);
        self.values[port]=0;
        if(!view.storage.empty())check(load_element(view,{0,0,0},self.values[port])==Status::ok);
        if(!self.available[port])check(self.values[port]==0);
    }
};
struct Fixture {
    NumericProcessingGraph definition=graph();
    NumericContract contract=std::get<NumericContract>(definition.nodes[0].outputs[0].contract.view());
    std::shared_ptr<const PreparedGraph> plan;
    std::unique_ptr<RuntimeContext> context;
    std::array<std::uint8_t,64> storage{};
    PlaneLayout plane{0,{1,0}};
    Observation result;
    Fixture(){check(PreparedGraph::prepare_numeric(definition,profile(),plan,context)==Status::ok);}
    void step(unsigned index,bool available=true) {
        storage.fill(index%30==0?200:0);
        NumericView view{&contract,std::as_bytes(std::span{storage}),{&plane,1},{64,1}};
        if(!available)view.storage={};
        FrameStamp stamp;stamp.sequence=index;stamp.epoch=1;stamp.producer_epoch=1;stamp.available=available;
        stamp.timestamp_ticks=std::int64_t(std::llround(double(index)*32768/60));
        const auto status=plan->process_numeric_ports(1,{stamp,view},*context,{}, {Observation::receive,nullptr,nullptr,&result});
        if(status!=Status::ok && (available || status!=Status::unavailable))std::fprintf(stderr,"frame %u status %u\n",index,unsigned(status));
        check(status==Status::ok || (!available && status==Status::unavailable));
    }
};
void ring_and_unavailable() {
    Fixture f;
    for(unsigned i=0;i<620;++i)f.step(i);
    check(f.result.available[0] && f.result.values[0]>=60 && f.result.values[0]<=180);
    check(!f.result.available[1]);check(f.result.values[3]==1);check(!f.result.available[4]);
    f.step(620,false);check(!f.result.available[0] && !f.result.available[1]);
    f.step(621);check(!f.result.available[0]);check(f.result.values[3]==0);
}
void refuses_wrong_input() {
    auto node=analyzer_node();auto input=std::get<NumericContract>(node.inputs[0].contract.view());
    std::array<OwnedProcessingContract,rhythm_output_count> output;
    input.dtype=DType::Float32;check(rhythm_output_contracts(node,input,output)!=Status::ok);
    input.dtype=DType::UInt8;input.shape.extents[1]={65,65};check(rhythm_output_contracts(node,input,output)!=Status::ok);
    input.shape.extents[1]={64,64};input.clock.sample_rate=48000;check(rhythm_output_contracts(node,input,output)!=Status::ok);
    auto p=profile();p.music_enabled=false;std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    check(PreparedGraph::prepare_numeric(graph(),p,plan,context)!=Status::ok);
}
bool has_measured_profile(const domain::ConfiguredProcessingNode& node){
    const auto& parameters=std::get<TransformProcessingParameters>(node.parameters).values;
    for(const auto& f:parameters)if(f.key=="values"){
        if(const auto* profile=std::get_if<NumericArray>(&f.value))return profile->values.size()==2048 && profile->values[0]>1;
    }
    return false;
}
struct ResearchTopology {
    unsigned real_fourier=0,duplicate_rfft=0;bool shared_history=false,combined_96=false,quantized=false;
    bool no_custom_features=true,existing_broadband=false,existing_notes=false,measured_profile=false;
    void include(const domain::ConfiguredProcessingNode& node){
        real_fourier+=unsigned(node.kind=="RealFourier");
        duplicate_rfft+=unsigned(node.kind=="RFFT");
        no_custom_features=no_custom_features && node.kind!="RhythmFeatures" && node.id!=1101;
        if(node.id==6 && !node.outputs.empty()){
            const auto& c=std::get<NumericContract>(node.outputs[0].contract.view());
            existing_broadband=c.shape.extents[0]==ExtentBounds{48,48};
        }
        if(node.id==1006 && !node.outputs.empty()){
            const auto& c=std::get<NumericContract>(node.outputs[0].contract.view());
            existing_notes=c.shape.extents[0]==ExtentBounds{48,48};
        }
        if(node.id==7 && !node.outputs.empty()){
            const auto& c=std::get<NumericContract>(node.outputs[0].contract.view());
            combined_96=c.dtype==DType::UInt8 && c.shape.extents[0].maximum==512 &&
                c.shape.extents[1]==ExtentBounds{96,96};
        }
        if(node.id==1005)measured_profile=has_measured_profile(node);
    }
    bool valid()const{return real_fourier==1 && duplicate_rfft==0 && shared_history && combined_96 &&
          quantized && no_custom_features && existing_broadband && existing_notes && measured_profile;}
};
void example_is_preparable() {
    const auto example=numeric_rhythm_graph();ResearchTopology topology;
    for(const auto& node:example.nodes)topology.include(node);
    for(const auto& edge:example.edges){
        if(edge.from==7 && edge.to==1102)topology.shared_history=true;
        if(edge.from==1007 && edge.to==1111)topology.quantized=true;
    }
    check(topology.valid());
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    // The shipped desktop example uses the external SpeexDSP backend.
    // Portable SDK tests retain the same topology with its allowed FIR fallback.
    auto p=profile();p.experimental_resampling_enabled=true;
#ifdef BARELIEF_TEST_DESKTOP_SPEEX
    p.resampler_backend=&desktop_resampling::speex_backend_factory();
    check(PreparedGraph::prepare_numeric(example,p,plan,context)==Status::ok);
#else
    auto portable=example;
    for(auto& node:portable.nodes)if(node.kind=="PcmResampler"){
        auto& params=std::get<TransformProcessingParameters>(node.parameters).values;
        for(auto& entry:params)if(entry.key=="mode")entry.value=std::string{"existing_fir"};
    }
    check(PreparedGraph::prepare_numeric(portable,p,plan,context)==Status::ok);
#endif
}
void research_history_contract() {
    const auto research=numeric_music_graph();
    check((research.execution_interval==Ratio{1,60}));
    bool found_history=false,found_quantizer=false,found_concat=false;
    for(const auto& node:research.nodes){
        if(node.id==7){
            check(node.kind=="Spectrogram" && !node.outputs.empty());
            const auto& c=std::get<NumericContract>(node.outputs.front().contract.view());
            check((c.dtype==DType::UInt8 && c.shape.extents[0].maximum==512 && c.shape.extents[1]==ExtentBounds{64,64}));
            found_history=true;
        }
        if(node.id==33)found_quantizer=node.kind=="SpectralQuantize";
        if(node.id==35)found_concat=node.kind=="ArrayConcat";
        check(node.id!=17 && node.id!=34 && node.id!=18);
    }
    check(found_history && found_quantizer && found_concat);
    bool broadband=false,notes=false,quantized=false;
    for(const auto& edge:research.edges){
        if(edge.from==31 && edge.to==35)broadband=true;
        if(edge.from==32 && edge.to==35)notes=true;
        if(edge.from==35 && edge.to==33)quantized=true;
    }
    check(broadband && notes && quantized);
}
double research_parameter(const domain::ConfiguredProcessingNode& node,std::string_view key){
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView value;check(resolve_processing_parameter(resolved,key,value)==Status::ok);
    const auto* numeric=std::get_if<NumericLiteral>(&value);check(numeric);
    return std::visit([](auto v){return double(v);},*numeric);
}
void research_window_calibration(){
    const auto graph=numeric_music_graph();
    const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.kind=="NoteSalience";});
    check(found!=graph.nodes.end());
    check(std::abs(research_parameter(*found,"coherent_gain")-4095./2)<1e-5);
    check(std::abs(research_parameter(*found,"power_bandwidth_hz")-1.5*32768./4095)<1e-6);
}
void research_quantization_scales(){
    for(const auto& graph:{numeric_music_graph(),numeric_startup_graph()}){
        const auto quantizer=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.kind=="SpectralQuantize";});
        check(quantizer!=graph.nodes.end());const auto expected=graph.execution_interval==Ratio{1,60} && quantizer->id==33?.5:1.;
        check(research_parameter(*quantizer,"min_value")==0 && research_parameter(*quantizer,"max_value")==expected);
        const auto contract=std::get<NumericContract>(quantizer->outputs.front().contract.view());
        check(contract.dtype==DType::UInt8 && contract.quantity==Quantity::Unspecified && contract.unit==NumericUnit::Values);
        dsp_kernels::SpectralQuantizer kernel;check(kernel.prepare(0,expected,8));
        const std::array<float,3> values{0,float(expected/2),float(expected)};std::array<std::uint8_t,3> codes{};
        check(kernel.process(values,codes));check(codes[0]==0 && codes[1]==128 && codes[2]==255);
    }
}
void explicit_rhythm_rate_binding(){
    auto requested=numeric_startup_graph();
    const auto is_rate=[](const Edge& edge){return edge.from==332&&edge.to==1102&&edge.output_port==sample_rate_output&&edge.input_port==target_sample_rate_input;};
    check(std::any_of(requested.edges.begin(),requested.edges.end(),is_rate));
    for(auto& node:requested.nodes){
        auto* parameters=std::get_if<TransformProcessingParameters>(&node.parameters);if(!parameters)continue;
        for(auto& field:parameters->values){
            if(node.id==332&&field.key=="output_rate")field.value=NumericLiteral{std::uint64_t{36000}};
            if(node.id==1005&&field.key=="values")field.value=NumericArray{};
            if(node.id==1005&&field.key=="frontend_declared")field.value=false;
        }
    }
    NumericProcessingGraph evaluated,executable;
    check(resolve_numeric_parameter_bindings(requested,limits,evaluated,executable,332));
    const auto analyzer=std::find_if(evaluated.nodes.begin(),evaluated.nodes.end(),[](const auto& node){return node.id==1102;});
    check(analyzer!=evaluated.nodes.end()&&research_parameter(*analyzer,"sample_rate")==36000);
    std::erase_if(requested.edges,is_rate);
    // Without the explicit wire, a stale manual Fs is rejected rather than inferred.
    check(!resolve_numeric_parameter_bindings(requested,limits,evaluated,executable,332));
}
}
int main(){explicit_rhythm_rate_binding();ring_and_unavailable();refuses_wrong_input();example_is_preparable();research_history_contract();research_window_calibration();research_quantization_scales();}
