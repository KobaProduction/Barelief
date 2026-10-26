#include <dataflow_runtime/runtime.h>
#include <dataflow_runtime/pcm_packet_queue.h>
#include "numeric_test_builders.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string_view>

namespace {
using namespace signal_contracts;using namespace domain;using namespace dataflow_runtime;
std::size_t allocations=0;
void check(bool yes,const char* reason){if(!yes){std::fprintf(stderr,"FAIL music runtime: %s\n",reason);std::exit(1);}}
constexpr FrameLimits limits{1,32768,262144,16384,256,32768};
constexpr std::array<std::uint32_t,1> mono{0};
NumericRuntimeProfile profile(){auto p=NumericRuntimeProfile{{32,64,8,32,1048576,4*1048576,limits},{{4*1048576,4*1048576,4*1048576,131072},0,32*1048576},{32768,1,262144,262144,1048576},{true,true,true}};p.music_enabled=true;return p;}
ProcessingPort port(PortDirection direction,PortId id,const NumericContract& c){return numeric_test_builders::port(id,direction,c,limits,65536,check,"own fixture port");}
ProcessingParameter number(const char* key,std::uint64_t value){return {key,NumericLiteral{value}};}
ProcessingParameter text(const char* key,const char* value){return {key,std::string(value)};}
ConfiguredProcessingNode transform(NodeId id,const char* kind,const NumericContract& input,std::vector<ProcessingParameter> params={}){
    auto n=numeric_test_builders::node(id,kind,TransformProcessingParameters{std::move(params)},
        {port(PortDirection::Input,0,input)},{});
    OwnedProcessingContract result;check(music_output_contract(n,input,result)==Status::ok,"derive music output");
    n.outputs.push_back(numeric_test_builders::port(0,PortDirection::Output,std::move(result)));return n;
}
NumericContract output(const ConfiguredProcessingNode& node){return std::get<NumericContract>(node.outputs[0].contract.view());}
ConfiguredProcessingNode history(NodeId id,const NumericContract& input){
    auto c=input;c.shape.rank=Rank::Matrix;c.shape.axes={Axis::Time,input.shape.axes[0]};c.shape.extents={ExtentBounds{0,400},input.shape.extents[0]};
    c.semantic=Semantic::History;c.history_element=input.semantic;c.coordinates={AxisCoordinates{CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds},input.coordinates[0]};
    NumericContract count;count.dtype=DType::UInt64;count.channel_order=mono;count.clock=input.clock;count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;
    ConfiguredProcessingNode n;n.id=id;n.kind="Spectrogram";n.parameters=HistoryProcessingParameters{400};n.inputs={port(PortDirection::Input,0,input)};
    n.outputs={port(PortDirection::Output,0,c),port(PortDirection::Output,1,count)};return n;
}
void music_branches(NumericProcessingGraph& g,unsigned bits){
    g.nodes.push_back(transform(5,"SpectrumProjection",output(g.nodes.back()),{text("representation","power"),number("sample_rate",32768)}));g.edges.push_back({4,5});
    g.nodes.push_back(transform(6,"FrequencyNormalizer",output(g.nodes.back())));g.edges.push_back({5,6});
    const auto power=output(g.nodes.back());g.nodes.push_back(transform(7,"BroadbandFeatures",power,{text("statistic","rms"),{"bin_hz",NumericLiteral{float{8}}}}));g.edges.push_back({6,7});
    g.nodes.push_back(transform(8,"SpectralQuantize",output(g.nodes.back()),{number("bits",bits)}));g.edges.push_back({7,8});
    g.nodes.push_back(history(9,output(g.nodes.back())));g.edges.push_back({8,9});
    g.nodes.push_back(transform(10,"NoteSalience",power,{{"bin_hz",NumericLiteral{float{8}}}}));g.edges.push_back({6,10});
    g.nodes.push_back(transform(11,"SpectralQuantize",output(g.nodes.back()),{number("bits",bits)}));g.edges.push_back({10,11});
    g.nodes.push_back(history(12,output(g.nodes.back())));g.edges.push_back({11,12});}
std::pair<ConfiguredProcessingNode,ConfiguredProcessingNode> window_coefficients(NumericProcessingGraph& g,const ConfiguredProcessingNode& window,const NumericContract& pcm){
    ConfiguredProcessingNode length;length.id=20;length.kind="ArrayLength";length.parameters=TransformProcessingParameters{};length.inputs={port(PortDirection::Input,0,pcm)};OwnedProcessingContract size;check(numeric_metadata_output_contract(pcm,false,size)==Status::ok,"window array size");length.outputs={{0,PortDirection::Output,size}};
    auto function=window;function.id=3;function.kind="WindowFunction";function.parameters=TransformProcessingParameters{{text("function","hann"),number("size",4096)}};function.inputs={{0,PortDirection::Input,size}};OwnedProcessingContract coefficients;check(array_generator_output_contract(function,std::get<NumericContract>(size.view()),coefficients)==Status::ok,"window coefficient generator");function.outputs={{0,PortDirection::Output,coefficients}};g.nodes.push_back(function);
    ConfiguredProcessingNode product;product.id=21;product.kind="ArrayMultiply";product.parameters=TransformProcessingParameters{};product.inputs={port(PortDirection::Input,0,pcm),{1,PortDirection::Input,coefficients}};product.outputs={port(PortDirection::Output,0,pcm)};
    return {std::move(length),std::move(product)};
}
NumericProcessingGraph graph(unsigned cadence=60,unsigned bits=8){
    NumericContract pcm;pcm.channel_order=mono;pcm.semantic=Semantic::PCM;pcm.quantity=Quantity::Amplitude;pcm.unit=NumericUnit::NormalizedAmplitude;
    pcm.representation.normalization=Normalization::FloatUnit;pcm.representation.alignment=alignof(float);
    pcm.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{1,1024},ExtentBounds{}}};pcm.clock={"music-clock",{1,32768},32768};
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={port(PortDirection::Output,0,pcm)};
    auto g=numeric_test_builders::graph("music-runtime-regression",{1,cadence},{source},{});
    auto window=source;window.id=2;window.kind="Window";window.parameters=TransformProcessingParameters{{number("size",4096),number("input_rate",32768)}};window.inputs={port(PortDirection::Input,0,pcm)};
    pcm.shape.extents[0]={4096,4096};pcm.semantic=Semantic::Window;window.outputs={port(PortDirection::Output,0,pcm)};g.nodes.push_back(window);g.edges.push_back({1,2});
    auto [length,product]=window_coefficients(g,window,pcm);
    g.nodes.push_back(transform(4,"RFFT",pcm));g.edges.insert(g.edges.end(),{{2,20},{20,3},{2,21,0,0},{3,21,0,1},{21,4}});
    music_branches(g,bits);g.nodes.push_back(std::move(length));g.nodes.push_back(std::move(product));return g;
}
struct Observation {
    std::size_t count=0;std::array<std::int64_t,600> ticks{};std::array<std::uint64_t,600> hashes{};std::array<bool,600> boundaries{};bool discontinuity=false;std::size_t discontinuities=0;
    std::size_t flux_count=0;std::array<std::uint64_t,600> flux_hashes{};std::array<bool,600> flux_boundaries{},flux_nonzero{};
    static void observe(void* user,const NumericPortFrameView& event){auto& self=*static_cast<Observation*>(user);if(event.status!=Status::ok)return;
        if(event.identity.node==7){const auto& v=std::get<NumericView>(event.frame.payload);check(self.flux_count<self.flux_hashes.size(),"bounded flux observations");
            std::uint64_t hash=1469598103934665603ull;bool nonzero=false;for(std::size_t i=0;i<v.actual[0];++i){float x=0;check(load_element(v,{0,i,0},x)==Status::ok && std::isfinite(x),"finite broad feature");nonzero=nonzero||x!=0;hash=(hash^std::bit_cast<std::uint32_t>(x))*1099511628211ull;}
            self.flux_hashes[self.flux_count]=hash;self.flux_boundaries[self.flux_count]=event.frame.stamp.discontinuity;self.flux_nonzero[self.flux_count++]=nonzero;return;
        }
        if(event.identity.node!=5)return;
        const auto& v=std::get<NumericView>(event.frame.payload);check(v.planes.size()==1 && v.actual[0]==2049,"projection includes DC/Nyquist in one plane");
        check(self.count<self.ticks.size(),"bounded observations");std::uint64_t hash=1469598103934665603ull;
        for(std::size_t i=0;i<v.actual[0];++i){float x=0;check(load_element(v,{0,i,0},x)==Status::ok && std::isfinite(x) && x>=0,"finite power");hash=(hash^std::bit_cast<std::uint32_t>(x))*1099511628211ull;}
        self.ticks[self.count]=event.frame.stamp.timestamp_ticks;self.boundaries[self.count]=event.frame.stamp.discontinuity;self.hashes[self.count++]=hash;self.discontinuity=event.frame.stamp.discontinuity;if(self.discontinuity)++self.discontinuities;
    }
    NumericRuntimeCallbacks callbacks(){return {observe,nullptr,nullptr,this};}
};
struct Running {
    NumericProcessingGraph graph;std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    std::uint64_t sequence=0;Observation observation;
    explicit Running(unsigned cadence=60,unsigned bits=8,bool flux=false):graph(::graph(cadence,bits)){
        if(flux){
            graph.nodes[6]=transform(7,"BroadbandFeatures",output(graph.nodes[5]),{text("statistic","flux"),{"bin_hz",NumericLiteral{float{8}}}});
            graph.nodes[7]=transform(8,"SpectralQuantize",output(graph.nodes[6]),{number("bits",bits)});
            graph.nodes[8]=history(9,output(graph.nodes[7]));
        }
        const auto status=PreparedGraph::prepare_numeric(graph,profile(),plan,context);
        if(status!=Status::ok)std::fprintf(stderr,"prepare status=%d\n",int(status));check(status==Status::ok,"prepare PC music graph");
    }
    void replace(NumericProcessingGraph changed){std::shared_ptr<const PreparedGraph> next;std::unique_ptr<RuntimeContext> state;
        check(PreparedGraph::prepare_numeric(changed,profile(),next,state,plan.get(),context.get())==Status::ok,"replace music graph");
        graph=std::move(changed);plan=std::move(next);context=std::move(state);
    }
    void feed(std::int64_t begin,std::size_t count,bool discontinuity=false){
        const auto c=output(graph.nodes.front());std::array<float,1024> samples{};constexpr double tau=6.2831853071795864769;
        for(std::size_t i=0;i<count;++i)for(unsigned tone=0;tone<7;++tone){const auto hz=128.+tone*176.;samples[i]+=float((.025+.004*std::sin(double(begin+i)/32768.+tone))*std::sin(tau*hz*double(begin+i)/32768.));}
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence;stamp.producer_epoch=17;stamp.generation=1;stamp.timestamp_ticks=stamp.sample_begin=begin;stamp.sample_count=count;stamp.has_sample_interval=true;stamp.discontinuity=discontinuity;
        const PlaneLayout plane{0,{sizeof(float),0}};
        auto frame=numeric_test_builders::frame(stamp,c,std::as_bytes(std::span(samples)),
            std::span(&plane,1),{count,1});
        const auto before=allocations;check(plan->process_numeric_ports(1,frame,*context,{},observation.callbacks())==Status::ok,"process packet");check(allocations==before,"no processing allocation");
    }
    std::uint64_t filled(NodeId id) const {DataFrameView f;std::uint64_t filled=0;check(plan->history(id,0,*context,f,&filled)==Status::ok,"History available");return filled;}
};
void packet_queue(){
    PcmPacketQueue queue;check(queue.prepare(32768,16,4),"prepare bounded packet queue");std::array<float,8> samples{0,1,2,3,4,5,6,7},copy{};
    auto stamp=[](std::int64_t begin,std::size_t count,std::uint64_t sequence){FrameStamp s;s.sequence=sequence;s.generation=1;s.producer_epoch=9;s.timestamp_ticks=s.sample_begin=begin;s.sample_count=count;s.has_sample_interval=true;return s;};
    const auto before=allocations;check(queue.push(samples,stamp(0,8,1))==Status::ok,"enqueue packet");std::size_t count=0;FrameStamp out;
    check(queue.pop(std::span(copy).first(3),count,out)==Status::ok && count==3 && out.sample_begin==0 && out.discontinuity,"first fragment stamp");
    check(queue.pop(copy,count,out)==Status::ok && count==5 && out.sample_begin==3 && !out.discontinuity,"continuing fragment stamp");
    check(queue.push(samples,stamp(8,8,2))==Status::ok && queue.push(samples,stamp(16,8,3))==Status::ok,"two queued packets");
    check(queue.push(samples,stamp(24,8,4))==Status::exhausted && queue.stats().overflow_packets==1,"bounded overflow explicit");
    check(queue.push(samples,stamp(32,8,5))==Status::ok && queue.stats().queued_samples==8,"overflow recovery discards stale queue");
    check(queue.pop(copy,count,out)==Status::ok && out.discontinuity && out.sample_begin==32,"overflow gap provenance");
    check(queue.push(samples,stamp(100,8,6))==Status::ok && queue.pop(copy,count,out)==Status::ok && out.discontinuity,"unhinted packet interval gap");
    check(queue.push(samples,stamp(108,8,6))==Status::would_block,"duplicate stamp rejected");check(allocations==before,"packet queue allocation-free");
    std::printf("PCM packet queue PASS: capacity_samples16 capacity_packets4 owned_heap_bytes%zu, fragment timestamps/overflow/gap/no allocations\n",queue.workspace_bytes());
}
void retained_feature_geometry(Running& whole){
    for(std::int64_t at=32768;at<8*32768;at+=1024)whole.feed(at,1024);
    check(whole.filled(9)==400 && whole.filled(12)==400,"typed History capacity400 retains bounded rows");
    DataFrameView broad,notes;check(whole.plan->history(9,0,*whole.context,broad)==Status::ok && whole.plan->history(12,0,*whole.context,notes)==Status::ok,"compact snapshots");
    const auto& b=std::get<NumericView>(broad.payload);const auto& n=std::get<NumericView>(notes.payload);check(b.contract->dtype==DType::UInt8 && n.contract->dtype==DType::UInt8 && b.actual[1]==16 && n.actual[1]==48,"compact feature geometry");
    check(400*(b.actual[1]+n.actual[1])==25600,"feature payload25600 bytes");
    const auto before=whole.filled(9);const auto prior_boundaries=whole.observation.discontinuities;whole.feed(8*32768+97,1024);check(whole.observation.discontinuities==prior_boundaries+1,"unhinted source gap resets only first window");check(whole.filled(9)==before,"compatible History preserved at capacity");
}
void changed_feature_partition(Running& whole){
    auto changed=whole.graph;std::get<TransformProcessingParameters>(changed.nodes[6].parameters).values.push_back(ProcessingParameter{"boundaries_hz",NumericArray{{0,70,120,250,400,600,900,1300,1800,2500,3500,4800,6500,8500,11000,14000,16384}}});
    std::shared_ptr<const PreparedGraph> replacement;std::unique_ptr<RuntimeContext> replacement_context;
    check(PreparedGraph::prepare_numeric(changed,profile(),replacement,replacement_context,whole.plan.get(),whole.context.get())==Status::ok,"prepare changed frequency partition");
    DataFrameView retained;std::uint64_t filled=999;
    check(replacement->history(9,0,*replacement_context,retained,&filled)==Status::ok && filled==0,"same-size changed band centers invalidate affected History");
    check(replacement->history(12,0,*replacement_context,retained,&filled)==Status::ok && filled==400,"unaffected note History remains intact");
}
void refused_feature_profiles(Running& whole){
    auto profile_embedded=profile();profile_embedded.music_enabled=false;std::shared_ptr<const PreparedGraph> refused;std::unique_ptr<RuntimeContext> context;
    check(PreparedGraph::prepare_numeric(whole.graph,profile_embedded,refused,context)!=Status::ok && !refused,"new embedded ops unsupported");
    auto wrong=whole.graph;auto& projection=std::get<TransformProcessingParameters>(wrong.nodes[4].parameters);projection.values={text("representation","phase")};
    OwnedProcessingContract phase;check(music_output_contract(wrong.nodes[4],std::get<NumericContract>(wrong.nodes[4].inputs[0].contract.view()),phase)==Status::ok,"derive honest phase metadata");
    auto phase_input=std::get<NumericContract>(phase.view());OwnedProcessingContract invalid;check(music_output_contract(wrong.nodes[6],phase_input,invalid)!=Status::ok,"phase cannot masquerade as power features");
}
void partition_and_cadence(){
    Running whole,fragmented;
    for(std::int64_t at=0;at<32768;at+=1024)whole.feed(at,1024);
    constexpr std::array<std::size_t,5> chunks{1,31,547,1009,17};std::size_t part=0;
    for(std::int64_t at=0;at<32768;){const auto count=std::min(chunks[part++%chunks.size()],std::size_t(32768-at));fragmented.feed(at,count);at+=count;}
    check(whole.observation.count==60 && fragmented.observation.count==60,"32768/60 fractional hop emits 60 windows");
    for(std::size_t i=0;i<60;++i){check(whole.observation.ticks[i]==std::int64_t((i+1)*32768/60)-1,"fractional hop exact timestamp");check(whole.observation.ticks[i]==fragmented.observation.ticks[i] && whole.observation.hashes[i]==fragmented.observation.hashes[i],"fragmentation preserves spectrum/timestamps");}
    check(whole.filled(9)==60 && whole.filled(12)==60,"two feature History clocks agree");
    Running integral(64,16);for(std::int64_t at=0;at<32768;at+=1024)integral.feed(at,1024);
    check(integral.observation.count==64,"32768/64 integral hop emits64");for(std::size_t i=0;i<64;++i)check(integral.observation.ticks[i]==std::int64_t((i+1)*512)-1,"integral hop512 exact");
    retained_feature_geometry(whole);
    changed_feature_partition(whole);
    refused_feature_profiles(whole);
    std::printf("music runtime PASS: fractional60/integral64, fragmentation, discontinuity, UInt8/16 History, payload25600, no hot allocations; plan buffers=%zu scratch=%zu\n",whole.plan->memory_requirements().buffer_bytes,whole.plan->memory_requirements().scratch_bytes);
}
struct SalienceObservation {
    std::vector<float> values;
    static void observe(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=2 || event.status!=Status::ok)return;
        auto& values=static_cast<SalienceObservation*>(user)->values;
        const auto& view=std::get<NumericView>(event.frame.payload);
        for(std::size_t i=0;i<values.size();++i)check(load_element(view,{0,i,0},values[i])==Status::ok,"salience result");
    }
};
struct SalienceInput {std::string_view mode;Quantity quantity;NumericUnit unit;float value;};
SalienceInput salience_input(std::string_view representation){
    constexpr SalienceInput cases[]{
        {"peak_amplitude",Quantity::SpectrumPeakAmplitude,NumericUnit::RawAmplitude,.5f},
        {"magnitude",Quantity::SpectrumMagnitude,NumericUnit::RawAmplitude,.5f},
        {"psd",Quantity::SpectrumPsd,NumericUnit::SquaredAmplitudePerHertz,1.f/96.f},
        {"rms_amplitude",Quantity::SpectrumRmsAmplitude,NumericUnit::RawAmplitude,.3535533905932738f},
        {"bin_power",Quantity::SpectrumBinPower,NumericUnit::SquaredAmplitude,1.f/12.f},
        {"window_scaled_magnitude",Quantity::SpectrumWindowScaledMagnitude,NumericUnit::RawAmplitude,.25f},
        {"power",Quantity::SpectrumPower,NumericUnit::SquaredAmplitude,.125f}};
    for(const auto& item:cases)if(item.mode==representation)return item;
    check(false,"unknown salience fixture representation");return {};
}
std::vector<float> salience_values(bool no_dc,const char* representation){
    const auto source_graph=graph();
    auto input=output(source_graph.nodes[4]);
    const auto settings=salience_input(representation);input.quantity=settings.quantity;input.unit=settings.unit;
    if(no_dc){input.shape.extents[0]={2048,2048};input.coordinates[0].origin=input.coordinates[0].step;}
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{{number("sample_rate",32768)}};source.outputs={port(PortDirection::Output,0,input)};
    std::vector<ProcessingParameter> policy{text("input_representation",representation),
        {"first_bin",NumericLiteral{std::uint64_t(no_dc)}},
        {"bin_hz",NumericLiteral{float{8}}},
        {"coherent_gain",NumericLiteral{float{2}}},
        {"power_bandwidth_hz",NumericLiteral{float{12}}}};
    auto fixture=numeric_test_builders::graph("salience-units",{1,120},
        {source,transform(2,"NoteSalience",input,std::move(policy))},
        {numeric_test_builders::edge(1,0,2,0)});
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    check(PreparedGraph::prepare_numeric(fixture,profile(),plan,context)==Status::ok,"prepare salience units");
    std::vector<float> samples(input.shape.extents[0].maximum);
    for(std::size_t bin:{55,110,165})samples[bin-(no_dc?1:0)]=settings.value;
    SalienceObservation observed;observed.values.resize(output(fixture.nodes.back()).shape.extents[0].maximum);
    const PlaneLayout plane{0,{sizeof(float),0}};FrameStamp stamp;stamp.sequence=1;stamp.producer_epoch=1;stamp.generation=1;
    auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(samples)),
        std::span(&plane,1),{samples.size(),1});
    check(plan->process_numeric_ports(1,frame,*context,{},NumericRuntimeCallbacks{SalienceObservation::observe,nullptr,nullptr,&observed})==Status::ok,"process salience units");
    return observed.values;
}
void salience_grid_and_units(){
    const auto full=salience_values(false,"peak_amplitude"),positive=salience_values(true,"peak_amplitude");
    const auto power=salience_values(false,"power"),magnitude=salience_values(false,"magnitude");
    const auto density=salience_values(false,"psd");
    const auto rms=salience_values(false,"rms_amplitude"),bin=salience_values(false,"bin_power"),scaled=salience_values(false,"window_scaled_magnitude");
    check(std::any_of(full.begin(),full.end(),[](float value){return value>0;}),"salience fixture is meaningful");
    for(std::size_t i=0;i<full.size();++i)check(std::abs(full[i]-positive[i])<1e-6f &&
        std::abs(full[i]-power[i])<1e-6f && std::abs(full[i]-magnitude[i])<1e-6f &&
        std::abs(full[i]-density[i])<1e-6f && std::abs(full[i]-rms[i])<1e-6f &&
        std::abs(full[i]-bin[i])<1e-6f && std::abs(full[i]-scaled[i])<1e-6f,"salience explicit policy unit equivalence");
}
void new_projection_contracts(){
    const auto fixture=graph();auto input=output(fixture.nodes[4]);OwnedProcessingContract result;
    auto broad=*std::find_if(fixture.nodes.begin(),fixture.nodes.end(),[](const auto& n){return n.kind=="BroadbandFeatures";});
    input.quantity=Quantity::SpectrumBinPower;input.unit=NumericUnit::SquaredAmplitude;
    check(music_output_contract(broad,input,result)==Status::ok,"BroadbandFeatures accepts bin power without converting its units");
    input.quantity=Quantity::SpectrumRmsAmplitude;input.unit=NumericUnit::RawAmplitude;
    check(music_output_contract(broad,input,result)==Status::incompatible_contract,"BroadbandFeatures refuses amplitude instead of silently squaring it");
    for(const auto mode:{"rms_amplitude","bin_power","window_scaled_magnitude"}){
        auto source=output(fixture.nodes[3]);
        auto projected=transform(100,"SpectrumProjection",source,{text("representation",mode),number("sample_rate",32768)});
        auto normalized=transform(101,"FrequencyNormalizer",output(projected),{text("mode","manual"),text("representation",mode),number("manual_gain",2)});
        check(output(normalized).quantity==output(projected).quantity && output(normalized).unit==output(projected).unit,"manual normalizer preserves each explicit new representation");
    }
}
void salience_endpoint_policy(){
    dsp_kernels::PreparedSpectrumPower converter;
    dsp_kernels::SpectrumPowerPolicy policy;
    policy.representation=dsp_kernels::SpectrumProjection::PeakAmplitude;
    policy.bin_hz=8;policy.power_bandwidth_hz=12;
    const std::array input{.4f,.5f,.6f};
    std::array<float,3> result{};
    check(converter.prepare(input.size(),policy) && converter.process(input,result),"full-grid endpoint conversion");
    check(std::abs(result[0]-.16f)<1e-6f && std::abs(result[1]-.125f)<1e-6f &&
        std::abs(result[2]-.36f)<1e-6f,"DC and Nyquist are unpaired");
    policy.first_bin=1;policy.include_nyquist=false;
    const std::array positive{.5f,.6f};
    check(converter.prepare(positive.size(),policy) && converter.process(positive,result),"no-DC conversion");
    check(result[0]==0 && std::abs(result[1]-.125f)<1e-6f &&
        std::abs(result[2]-.18f)<1e-6f,"last bin is paired when Nyquist is omitted");
    policy.representation=dsp_kernels::SpectrumProjection::Magnitude;
    policy.first_bin=0;policy.include_nyquist=true;policy.coherent_gain=1024;
    const std::array raw{0.f,512.f,0.f};
    check(converter.prepare(raw.size(),policy) && converter.process(raw,result),"raw magnitude calibration");
    check(std::abs(result[1]-.5f)<1e-6f,"unit sine raw magnitude yields half mean-square");
}
void disconnected_product_after_clear(){
    Running running;auto changed=running.graph;
    std::erase_if(changed.edges,[](const auto& edge){return edge.to==21 && edge.input_port==1;});
    running.replace(std::move(changed));running.feed(0,1024);
    const auto before=running.observation.count;check(before>0,"disconnected coefficient emits typed zeros before clear");
    running.plan->clear_ingress(1,*running.context);running.feed(1024,1024);
    check(running.observation.count>before,"disconnected coefficient stays ready after ingress clear");
}
void declared_spectral_window(){
    auto direct=graph();
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(direct,profile(),plan,state)==Status::ok,"declared frontend prepares");
    const auto found=std::find_if(direct.nodes.begin(),direct.nodes.end(),[](const auto& node){return node.kind=="SpectrumProjection";});
    check(found!=direct.nodes.end(),"projection fixture exists");
    std::get<TransformProcessingParameters>(found->parameters).values.push_back(text("window_function","rectangular"));
    check(PreparedGraph::prepare_numeric(direct,profile(),plan,state)==Status::ok,"explicit calibration accepted without ancestry inference");
}
void complex_frequency_binding(){
    auto g=graph();NumericProcessingGraph resolved;
    ConfiguredProcessingNode view;view.id=30;view.kind="FFTView";
    view.inputs={port(PortDirection::Input,0,output(g.nodes[4]))};g.nodes.push_back(view);
    g.configuration_bindings={{4,30,ConfigurationRole::FrequencyGrid,ConfigurationRole::FrequencyGrid}};
    check(bool(resolve_configuration_bindings(g,profile().admission,resolved)),"RFFT Bin/Component exports the actual bin Hz axis");
    auto mismatched=output(g.nodes[4]);mismatched.coordinates[0].step={9,1};
    g.nodes.back().inputs={port(PortDirection::Input,0,mismatched)};
    const auto prior_id=resolved.graph_id;
    check(resolve_configuration_bindings(g,profile().admission,resolved).error==ProcessingError::IncompatiblePorts,"RFFT frequency grid rejects unequal Hz spacing");
    check(resolved.graph_id==prior_id && output(resolved.nodes[3]).shape.axes[1]==Axis::Component,"failed frequency binding leaves resolved graph unchanged");
    auto matrix=output(::graph().nodes[4]);matrix.shape.rank=Rank::Matrix;
    matrix.shape.axes={Axis::Time,Axis::Bin};matrix.shape.extents={ExtentBounds{0,8},ExtentBounds{2049,2049}};
    matrix.history_element=matrix.semantic;matrix.semantic=Semantic::History;
    matrix.coordinates={AxisCoordinates{CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds},output(g.nodes[4]).coordinates[0]};
    g.nodes.back().kind="SpectrogramView";g.nodes.back().inputs={port(PortDirection::Input,0,matrix)};
    check(bool(resolve_configuration_bindings(g,profile().admission,resolved)),"RFFT grid binds to History frequency columns");
}
void flux_boundaries(){
    Running burst(64,8,true),fragments(64,8,true);burst.feed(0,1024,true);fragments.feed(0,512,true);fragments.feed(512,512);
    burst.feed(10000,1024,true);fragments.feed(10000,512,true);fragments.feed(10512,512);
    check(burst.observation.flux_count==4 && fragments.observation.flux_count==4,"two hops per boundary packet");
    for(std::size_t i=0;i<4;++i){check(burst.observation.flux_hashes[i]==fragments.observation.flux_hashes[i],"flux independent of discontinuous packet partitioning");
        check(burst.observation.flux_boundaries[i]==(i%2==0) && fragments.observation.flux_boundaries[i]==(i%2==0),"only first hop carries packet boundary");}
    check(!burst.observation.flux_nonzero[0] && burst.observation.flux_nonzero[1] && !burst.observation.flux_nonzero[2] && burst.observation.flux_nonzero[3],"boundary resets once and following flux remains meaningful");
    auto disabled=burst.graph;disabled.nodes[6].enabled=false;burst.replace(disabled);burst.feed(11024,1024);
    check(!burst.observation.flux_nonzero[4] && !burst.observation.flux_nonzero[5],"disabled temporal feature outputs typed zero");
    disabled.nodes[6].enabled=true;burst.replace(disabled);burst.feed(12048,512);
    check(!burst.observation.flux_nonzero[6],"reenabled flux does not compare stale pre-disable spectrum");
    std::puts("music provenance/flux regressions PASS: no-window, identity converter, grouped-route refusal, one boundary per packet, disabled-state reset");
}
}
void* operator new(std::size_t n){++allocations;if(auto* p=std::malloc(n?n:1))return p;std::abort();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
int main(){packet_queue();partition_and_cadence();declared_spectral_window();complex_frequency_binding();flux_boundaries();salience_grid_and_units();salience_endpoint_policy();new_projection_contracts();disconnected_product_after_clear();}
