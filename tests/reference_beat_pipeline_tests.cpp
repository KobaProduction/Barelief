#include <core_app/numeric_default_graph.h>
#include <dataflow_runtime/runtime.h>
#include <dsp_graph_adapters/profile.h>
#include <domain/graph/node_factory.h>
#include <domain/graph/preparation_values.h>
#include "numeric_test_builders.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
namespace {
using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
std::size_t allocations=0;
void check(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL live reference: %s\n",message);std::exit(1);}}
constexpr FrameLimits bounds{8,800000,3200000,65536,4096,65536};
NumericRuntimeProfile profile(){
    NumericRuntimeProfile value{{64,128,16,64,1048576,16777216,bounds},{{16777216,67108864,33554432,1048576},0,268435456},
        {800000,8,3200000,3200000,16777216},{true,true,true}};
    return dsp_graph_adapters::with_dsp_operations(std::move(value));
}
OwnedProcessingContract own(const NumericContract& value){OwnedProcessingContract result;check(own_processing_contract(value,bounds,16777216,result)==Status::ok,"own contract");return result;}
NumericProcessingGraph live_graph(std::uint32_t rate,std::size_t context=0){
    auto graph=numeric_reference_beat_example(rate).graph;
    std::erase_if(graph.nodes,[](const auto& node){const auto kind=node_kind(node.kind);return kind && is_view(*kind);});
    std::erase_if(graph.edges,[&](const auto& edge){return std::none_of(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==edge.to;});});
    const auto original=graph.nodes[0].outputs[0].contract;const auto input=std::get<NumericContract>(original.view());
    auto& source=graph.nodes[0];source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t(rate)}}}};
    source.outputs={{0,PortDirection::Output,own(input)}};
    check(attach_numeric_parameter_ports(source,profile().admission)==Status::ok,"source exposes explicit Fs output");
    if(context)for(auto& node:graph.nodes)if(node.id==27){
        auto& values=std::get<TransformProcessingParameters>(node.parameters).values;
        for(auto& value:values)if(value.key=="size")value.value=NumericLiteral{std::uint64_t(context)};
    }
    if(context){
        NumericProcessingGraph evaluated,executable;
        check(bool(resolve_numeric_parameter_bindings(graph,profile().admission,evaluated,executable,27)),"rebind after editing Window size");
        graph=std::move(evaluated);
    }
    return graph;
}
struct Observation {
    std::span<const float> samples;std::uint32_t rate;std::size_t frames=0,ffts=0,mel_count=0,onsets=0,masks=0;
    float bpm=0;std::size_t beat_count=0,count=0,onset_end=0,tracker_end=0;
    std::array<float,2048> envelope{},tracking{},acf{};std::array<std::uint8_t,2048> mask{};
    std::vector<float> mel_values,onset_values;
    std::chrono::steady_clock::time_point onset_start,tracker_start;
    double max_onset_ms=0,max_tracker_ms=0;
    Observation(std::span<const float> input,std::uint32_t fs):samples(input),rate(fs),mel_values(input.size()/512*128),onset_values(input.size()/512){}
    void frame(const NumericPortFrameView& event){
        const auto& value=std::get<NumericView>(event.frame.payload);const auto begin=std::size_t(event.frame.stamp.sample_begin);
        check(begin==frames*512 && event.frame.stamp.sample_count==512 && value.actual[0]==512,"source already emits fixed 512-sample blocks");
        for(std::size_t i=0;i<512;++i){float actual=0;check(load_element(value,{0,i,0},actual)==Status::ok && actual==samples[begin+i],"source sample order");}
        ++frames;
    }
    void window(const NumericView& value){
        check(value.actual[0]==2048,"only 2048 PCM samples retained for the spectrum");
        const auto first=std::int64_t(frames*512)-2048;
        for(std::size_t i=0;i<2048;++i){float actual=0;const auto at=first+std::int64_t(i);
            check(load_element(value,{0,i,0},actual)==Status::ok && actual==(at<0?0.f:samples[std::size_t(at)]),"Window receives source samples directly");}
    }
    void mel(const NumericView& value){
        check(value.actual[0]==128 && (mel_count+1)*128<=mel_values.size(),"linear Mel shape");
        for(std::size_t i=0;i<128;++i)check(load_element(value,{0,i,0},mel_values[mel_count*128+i])==Status::ok,"Mel output");
        ++mel_count;onset_start=std::chrono::steady_clock::now();
    }
    void onset(const NumericView& value){
        check(value.contract->shape.rank==Rank::Scalar && onsets<onset_values.size(),"one scalar onset for each new spectrum");
        check(load_element(value,{0,0,0},onset_values[onsets])==Status::ok,"new onset value");++onsets;
        max_onset_ms=std::max(max_onset_ms,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-onset_start).count());
    }
    void history(const NumericView& value){
        count=value.actual[0];check(count<=envelope.size(),"onset capacity");
        for(std::size_t i=0;i<count;++i)check(load_element(value,{0,i,0},envelope[i])==Status::ok,"rolling onset history");
        onset_end=mel_count;
    }
    void beats(const NumericView& value){
        beat_count=0;check(value.actual[0]==count,"beat grid aligns with selected onset snapshot");
        for(std::size_t i=0;i<count;++i){check(load_element(value,{0,i,0},mask[i])==Status::ok && mask[i]<=1,"boolean beat mask");beat_count+=mask[i];}
        ++masks;max_tracker_ms=std::max(max_tracker_ms,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-tracker_start).count());
    }
    void correlation(const NumericView& value){
        check(value.actual[0]==count,"one current correlation profile");
        for(std::size_t i=0;i<count;++i)check(load_element(value,{0,i,0},acf[i])==Status::ok,"correlation values");
    }
    static void output(void* user,const NumericPortFrameView& event){
        auto& self=*static_cast<Observation*>(user);
        if(event.status!=Status::ok)std::fprintf(stderr,"stage node=%d port=%u status=%d\n",int(event.identity.node),unsigned(event.identity.port),int(event.status));
        check(event.status==Status::ok,"no unavailable/error stage");const auto& value=std::get<NumericView>(event.frame.payload);
        const auto id=event.identity.node;const auto port=event.identity.port;
        if(id==1 && port==0)self.frame(event);
        if(id==20)self.window(value);
        if(id==24 && port==0)++self.ffts;
        if(id==26)self.mel(value);
        if(id==3)self.onset(value);
        if(id==27 && port==0)self.history(value);
        if(id==31){self.tracker_start=std::chrono::steady_clock::now();self.tracking=self.envelope;self.tracker_end=self.mel_count;}
        if(id==34 && port==0)self.correlation(value);
        if(id==4 && port==0)self.beats(value);
        if(id==35){check(value.contract->shape.rank==Rank::Scalar,"BPM is a scalar");check(load_element(value,{0,0,0},self.bpm)==Status::ok,"tempo output");}
    }
};
std::vector<float> signal(std::uint32_t rate,bool silence){
    std::vector<float> values(12*rate);if(silence)return values;
    constexpr float frequencies[]{110,251,503,997,1999,4001,6007,10007};
    for(std::size_t i=0;i<values.size();++i){const auto phase=i%(rate/2);const double t=double(i)/rate,envelope=std::exp(-double(phase)/(rate*.006));
        for(unsigned k=0;k<8;++k)values[i]+=float(.07*envelope*std::sin(6.283185307179586*frequencies[k]*t+.17*k));}
    return values;
}
void feed(const NumericProcessingGraph& graph,const PreparedGraph& plan,RuntimeContext& state,std::span<const float> samples,Observation& seen){
    const auto data=graph.nodes[0].outputs[0].contract.view();const auto& contract=std::get<NumericContract>(data);const PlaneLayout plane{0,{sizeof(float),0}};
    std::uint64_t sequence=0;std::size_t offset=0;
    std::array<float,512> packet{};
    const auto before=allocations;
    while(offset+512<=samples.size()){
        const std::size_t count=512;FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence;stamp.producer_epoch=1;
        stamp.has_sample_interval=true;stamp.sample_begin=std::int64_t(offset);stamp.sample_count=count;stamp.timestamp_ticks=std::int64_t(offset+count-1);
        std::copy_n(samples.begin()+offset,count,packet.begin());
        const auto frame=numeric_test_builders::frame(stamp,contract,std::as_bytes(std::span(packet)),std::span(&plane,1),{count,1});
        check(plan.process_numeric_ports(1,0,frame,state,{},NumericRuntimeCallbacks{Observation::output,nullptr,nullptr,&seen})==Status::ok,"process source packet");offset+=count;
    }
    check(allocations==before,"live pipeline processing allocates no memory");
}
void export_values(const char* directory,std::span<const float> pcm,const Observation& seen){
    if(!directory)return;const std::string root=directory;
    std::ofstream samples(root+"/pcm.f32",std::ios::binary);samples.write(reinterpret_cast<const char*>(pcm.data()),std::streamsize(pcm.size_bytes()));
    std::ofstream envelope(root+"/onset.f32",std::ios::binary);envelope.write(reinterpret_cast<const char*>(seen.envelope.data()),std::streamsize(seen.count*sizeof(float)));
    std::ofstream mask(root+"/beats.u8",std::ios::binary);mask.write(reinterpret_cast<const char*>(seen.mask.data()),std::streamsize(seen.count));
    std::ofstream result(root+"/bpm.txt");result.precision(9);result<<seen.bpm;
    std::ofstream mel(root+"/mel.f32",std::ios::binary);mel.write(reinterpret_cast<const char*>(seen.mel_values.data()),std::streamsize(seen.mel_count*128*sizeof(float)));
    std::ofstream tracking(root+"/tracker_onset.f32",std::ios::binary);tracking.write(reinterpret_cast<const char*>(seen.tracking.data()),std::streamsize(seen.count*sizeof(float)));
    std::ofstream meta(root+"/geometry.txt");meta<<seen.rate<<' '<<seen.mel_count<<' '<<seen.onset_end<<' '<<seen.tracker_end<<' '<<seen.count;
    std::ofstream raw(root+"/raw_onset.f32",std::ios::binary);raw.write(reinterpret_cast<const char*>(seen.onset_values.data()),std::streamsize(seen.onsets*sizeof(float)));
    std::ofstream acf(root+"/acf.f32",std::ios::binary);acf.write(reinterpret_cast<const char*>(seen.acf.data()),std::streamsize(seen.count*sizeof(float)));
}
void live_case(std::uint32_t rate,bool silence,const char* directory,std::size_t context=0){
    const auto graph=live_graph(rate,context);std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    const auto status=PreparedGraph::prepare_numeric(graph,profile(),plan,state);
    if(status!=Status::ok){NumericProcessingGraph evaluated,executable;const auto p=profile();
        const auto diagnostic=resolve_numeric_parameter_bindings(graph,p.admission,evaluated,executable);
        std::fprintf(stderr,"prepare rate=%u status=%d bindings=%u node=%d port=%u\n",rate,int(status),unsigned(diagnostic.error),int(diagnostic.node),unsigned(diagnostic.port));
        for(const auto& node:graph.nodes){ResolvedProcessingNode resolved;const auto d=resolve_processing_node(node,p.admission,resolved);
            const auto support=d?p.operation_factory->assess(resolved,p):ProcessingSupport{};
            std::fprintf(stderr," node=%d %s resolve=%u support=%d\n",int(node.id),node.kind.c_str(),unsigned(d.error),int(support.supported));}}
    check(status==Status::ok,"prepare full reference pipeline");
    const auto values=signal(rate,silence);Observation seen{values,rate};const auto start=std::chrono::steady_clock::now();
    feed(graph,*plan,*state,values,seen);
    check(seen.frames==values.size()/512 && seen.ffts==seen.frames && seen.mel_count==seen.frames,"one FFT and Mel projection per short frame, never reprocess PCM history");
    check(seen.onsets==seen.frames && seen.masks==1+(seen.onsets-1)/20,"one new onset per source frame and independent tempo cadence");
    if(silence)check(seen.bpm==0 && seen.beat_count==0,"silence is zero tempo and no invented grid");
    else check(seen.bpm>100 && seen.bpm<140 && seen.beat_count>=10,"tempo and beats on rhythmic changing-tone signal");
    std::printf("rate=%u silence=%d bpm=%g beats=%zu FFTs=%zu onsets=%zu grids=%zu max_onset_ms=%.2f max_tracker_ms=%.2f processing_ms=%.1f\n",rate,int(silence),seen.bpm,seen.beat_count,seen.ffts,seen.onsets,seen.masks,seen.max_onset_ms,seen.max_tracker_ms,
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    if(directory && !silence)export_values(directory,values,seen);
}
struct FramingObservation {
    std::size_t count=0,base=0,markers=0;bool boundary=false;
    static void output(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=2)return;auto& self=*static_cast<FramingObservation*>(user);
        if(event.status==Status::unavailable){++self.markers;return;}
        check(event.status==Status::ok && event.frame.stamp.sample_begin==std::int64_t(self.base+self.count*3) && event.frame.stamp.sample_count==5,"fixed hop independent of input packet size");
        check(event.frame.stamp.discontinuity==(self.count==0),"only the first complete frame carries the boundary across warmup");
        const auto& value=std::get<NumericView>(event.frame.payload);
        for(std::size_t i=0;i<5;++i){float number=0;check(load_element(value,{0,i,0},number)==Status::ok && number==float(self.base+self.count*3+i),"framer includes every sample in logical order");}
        ++self.count;
    }
};
void reset_framer(const PreparedGraph& plan,RuntimeContext& state,DataFrameView input,std::span<float> values,FramingObservation& seen){
    const auto before=allocations;const NumericRuntimeCallbacks callbacks{FramingObservation::output,nullptr,nullptr,&seen};
    input.stamp.available=false;input.stamp.sequence=input.stamp.source_sequence=2;
    const auto storage=std::get<NumericView>(input.payload).storage;std::get<NumericView>(input.payload).storage={};
    const auto unavailable=plan.process_numeric_ports(1,0,input,state,{},callbacks);
    check(unavailable==Status::ok && seen.markers==1,"unavailable propagates a marker");
    std::get<NumericView>(input.payload).storage=storage;
    seen.count=0;seen.base=100;seen.boundary=true;
    for(std::size_t i=0;i<5;++i)values[i]=float(100+i);
    input.stamp.available=true;input.stamp.sample_begin=100;input.stamp.sample_count=3;input.stamp.sequence=input.stamp.source_sequence=3;
    std::get<NumericView>(input.payload).actual[0]=3;
    check(plan.process_numeric_ports(1,0,input,state,{},callbacks)==Status::ok && seen.count==0,"restart requires a full new window");
    values[0]=103;values[1]=104;input.stamp.sample_begin=103;input.stamp.sample_count=2;input.stamp.sequence=input.stamp.source_sequence=4;
    std::get<NumericView>(input.payload).actual[0]=2;
    check(plan.process_numeric_ports(1,0,input,state,{},callbacks)==Status::ok && seen.count==1 && allocations==before,"restart has no old samples or allocations");
}
void multiple_frames_per_packet(){
    auto graph=live_graph(48000);graph.nodes.resize(2);graph.edges.resize(1);
    auto input_contract=std::get<NumericContract>(graph.nodes[0].outputs[0].contract.view());input_contract.shape.extents[0]={1,8192};
    graph.nodes[0].outputs[0].contract=own(input_contract);auto& frame=graph.nodes[1];
    check(bool(make_processing_node("FrameBuffer",2,profile().admission,frame)),"standalone optional framer");
    frame.parameters=TransformProcessingParameters{{{"size",NumericLiteral{std::uint64_t{5}}},{"hop_samples",NumericLiteral{std::uint64_t{3}}}}};
    frame.inputs[0].contract=graph.nodes[0].outputs[0].contract;graph.edges={{1,2,0,0}};
    check(bool(derive_processing_node_contracts(frame,profile().admission)),"general framer accepts non-FFT lengths");
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"prepare small sample framer");
    std::array<float,8192> values{};for(std::size_t i=0;i<11;++i)values[i]=float(i);
    const auto data=graph.nodes[0].outputs[0].contract.view();const auto& contract=std::get<NumericContract>(data);const PlaneLayout plane{0,{sizeof(float),0}};
    FrameStamp stamp;stamp.producer_epoch=1;stamp.sequence=stamp.source_sequence=1;stamp.has_sample_interval=true;stamp.sample_count=11;
    const auto input=numeric_test_builders::frame(stamp,contract,std::as_bytes(std::span(values)),std::span(&plane,1),{11,1});FramingObservation seen;const auto before=allocations;
    const auto result=plan->process_numeric_ports(1,0,input,*state,{},NumericRuntimeCallbacks{FramingObservation::output,nullptr,nullptr,&seen});
    check(result==Status::ok && seen.count==3 && allocations==before,"one packet emits all three complete frames without allocation");
    reset_framer(*plan,*state,input,values,seen);
}
struct AggregationResult {
    float value=0;unsigned calls=0;
    static void output(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=3)return;auto& result=*static_cast<AggregationResult*>(user);
        check(event.status==Status::ok && std::get<NumericView>(event.frame.payload).contract->shape.rank==Rank::Scalar,"vector aggregation produces scalar");
        check(load_element(std::get<NumericView>(event.frame.payload),{0,0,0},result.value)==Status::ok,"aggregation scalar value");++result.calls;
    }
};
void feature_aggregation(){
    const auto basis=live_graph(48000);const auto original=basis.nodes[0].outputs[0].contract;
    auto contract=std::get<NumericContract>(original.view());contract.shape={Rank::Vector,{Axis::Declared,Axis::None},{ExtentBounds{3,3},ExtentBounds{1,1}}};contract.coordinates={};
    for(std::uint64_t method=1;method<=3;++method){
        auto source=basis.nodes[0];source.outputs={{0,PortDirection::Output,own(contract)}};
        ConfiguredProcessingNode aggregate;
        check(bool(make_processing_node("FeatureAggregate",3,profile().admission,aggregate)),"create generic feature reducer");
        aggregate.parameters=TransformProcessingParameters{{{"method",NumericLiteral{method}}}};
        check(bool(configure_processing_input(aggregate,contract,profile().admission)),"derive vector-to-scalar aggregation");
        NumericProcessingGraph graph;graph.graph_id="aggregate";graph.nodes={source,aggregate};graph.edges={{1,3,0,0}};
        std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
        check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"prepare generic aggregation");
        const std::array<float,3> values{8,1,12};const PlaneLayout layout{0,{4,0}};FrameStamp stamp;stamp.producer_epoch=1;stamp.sequence=1;
        const auto frame=numeric_test_builders::frame(stamp,contract,std::as_bytes(std::span(values)),std::span(&layout,1),{3,1});AggregationResult result;const auto before=allocations;
        check(plan->process_numeric_ports(1,0,frame,*state,{},NumericRuntimeCallbacks{AggregationResult::output,nullptr,nullptr,&result})==Status::ok,"process generic aggregation");
        check(result.calls==1 && result.value==(method==1?7.f:method==2?8.f:21.f) && allocations==before,"mean, median, sum and no allocations");
    }
}
struct CadenceResult {
    unsigned frames=0,markers=0;std::uint64_t last=0;
    static void output(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=2)return;auto& result=*static_cast<CadenceResult*>(user);
        if(event.status==Status::unavailable){++result.markers;return;}
        check(event.status==Status::ok,"cadence output status");float value=0;
        check(load_element(std::get<NumericView>(event.frame.payload),{0,1,0},value)==Status::ok && value==1,"cadence forwards values unchanged");
        ++result.frames;result.last=event.frame.stamp.sequence;
    }
};
void cadence_reset(){
    const auto basis=live_graph(48000);const auto original=basis.nodes[0].outputs[0].contract;
    auto contract=std::get<NumericContract>(original.view());contract.shape={Rank::Vector,{Axis::Declared,Axis::None},{ExtentBounds{3,3},ExtentBounds{1,1}}};contract.coordinates={};
    auto source=basis.nodes[0];source.outputs={{0,PortDirection::Output,own(contract)}};ConfiguredProcessingNode cadence;
    check(bool(make_processing_node("FrameStride",2,profile().admission,cadence)),"create generic cadence block");
    cadence.parameters=TransformProcessingParameters{{{"every_n",NumericLiteral{std::uint64_t{3}}}}};
    check(bool(configure_processing_input(cadence,contract,profile().admission)),"derive unchanged cadence contract");
    NumericProcessingGraph graph;graph.graph_id="cadence";graph.nodes={source,cadence};graph.edges={{1,2,0,0}};
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"prepare cadence block");
    const std::array<float,3> values{8,1,12};const PlaneLayout layout{0,{4,0}};CadenceResult result;const auto before=allocations;
    for(std::uint64_t sequence=1;sequence<=12;++sequence){
        FrameStamp stamp;stamp.producer_epoch=1;stamp.sequence=stamp.source_sequence=sequence;stamp.available=sequence!=8;
        std::span<const std::byte> bytes=std::as_bytes(std::span(values));if(!stamp.available)bytes={};
        const auto input=numeric_test_builders::frame(stamp,contract,bytes,std::span(&layout,1),{3,1});
        check(plan->process_numeric_ports(1,0,input,*state,{},NumericRuntimeCallbacks{CadenceResult::output,nullptr,nullptr,&result})==Status::ok,"feed cadence or unavailable marker");
        if(sequence==7)check(result.frames==3 && result.last==7,"first frame and every third frame");
        if(sequence==9)check(result.frames==4 && result.last==9,"recovery restarts cadence immediately");
    }
    check(result.frames==5 && result.markers==1 && result.last==12 && allocations==before,"cadence reset and no allocations");
}
struct StreamingDifference {
    unsigned rows=0,markers=0;
    static void output(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=30)return;auto& result=*static_cast<StreamingDifference*>(user);
        if(event.status==Status::unavailable){++result.markers;return;}
        check(event.status==Status::ok,"stream difference status");
        const auto& value=std::get<NumericView>(event.frame.payload);
        const std::array<float,3> expected=result.rows==1?std::array{3.f,0.f,4.f}:result.rows==3?std::array{5.f,0.f,4.f}:std::array{0.f,0.f,0.f};
        for(std::size_t i=0;i<3;++i){float v=0;check(load_element(value,{0,i,0},v)==Status::ok && v==expected[i],"positive differences reuse lag and reflected frequency maximum");}
        ++result.rows;
    }
};
void streaming_onset_reset(){
    auto source=live_graph(48000).nodes[0];const auto original=source.outputs[0].contract;auto input=std::get<NumericContract>(original.view());
    input.shape={Rank::Vector,{Axis::Band,Axis::None},{ExtentBounds{3,3},ExtentBounds{1,1}}};input.coordinates={};input.unit=NumericUnit::Decibels;
    source.outputs={{0,PortDirection::Output,own(input)}};ConfiguredProcessingNode difference;
    check(bool(make_processing_node("OnsetStrength",30,profile().admission,difference)),"create existing onset block");
    difference.parameters=TransformProcessingParameters{{{"lag",NumericLiteral{std::uint64_t{1}}},{"max_size",NumericLiteral{std::uint64_t{3}}},{"center",false},{"detrend",false},{"aggregate",std::string{"none"}}}};
    check(bool(configure_processing_input(difference,input,profile().admission)),"derive streaming vector difference");
    NumericProcessingGraph graph;graph.graph_id="streaming-difference";graph.nodes={source,difference};graph.edges={{1,30,0,0}};
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"prepare streaming difference");
    const std::array<std::array<float,3>,5> values{{{1,2,3},{5,2,7},{0,0,0},{2,4,1},{9,2,8}}};const PlaneLayout plane{0,{4,0}};
    StreamingDifference result;const auto before=allocations;
    for(std::size_t i=0;i<values.size();++i){
        FrameStamp stamp;stamp.producer_epoch=1;stamp.sequence=stamp.source_sequence=i+1;stamp.available=i!=2;
        std::span<const std::byte> bytes=std::as_bytes(std::span(values[i]));if(!stamp.available)bytes={};
        const auto frame=numeric_test_builders::frame(stamp,input,bytes,std::span(&plane,1),{3,1});
        check(plan->process_numeric_ports(1,0,frame,*state,{},NumericRuntimeCallbacks{StreamingDifference::output,nullptr,nullptr,&result})==Status::ok,"feed new spectrum or missing marker");
    }
    check(result.rows==4 && result.markers==1 && allocations==before,"gap resets reference frame without allocations");
}
}
void* operator new(std::size_t bytes){++allocations;if(auto* pointer=std::malloc(bytes?bytes:1))return pointer;throw std::bad_alloc();}
void operator delete(void* pointer) noexcept{std::free(pointer);}
void operator delete(void* pointer,std::size_t) noexcept{std::free(pointer);}
int main(int argc,char** argv){multiple_frames_per_packet();feature_aggregation();cadence_reset();streaming_onset_reset();live_case(48000,false,argc>1?argv[1]:nullptr);live_case(44100,false,nullptr);live_case(48000,true,nullptr);live_case(48000,false,nullptr,1024);std::puts("live reference pipeline PASS");}
