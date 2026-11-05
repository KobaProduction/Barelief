#include <core_app/numeric_madmom_reference_graph.h>
#include <dataflow_runtime/runtime.h>
#include <dsp_graph_adapters/profile.h>
#include <domain/graph/node_factory.h>
#include <domain/graph/preparation_values.h>
#include "numeric_test_builders.h"
#include "madmom_whole_fixture.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <malloc.h>
#include <new>
#include <string_view>
#include <stdexcept>

namespace {
using namespace domain;
using namespace signal_contracts;
using namespace dataflow_runtime;
std::size_t allocations=0;
constexpr FrameLimits bounds{8,800000,3200000,65536,4096,65536};
void check(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"FAIL madmom pipeline: %s\n",message);std::exit(1);}
}
NumericRuntimeProfile profile() {
    NumericRuntimeProfile p{{64,128,16,64,1048576,16777216,bounds},
        {{16777216,67108864,33554432,1048576},0,268435456},
        {800000,8,3200000,3200000,16777216},{true,true,true}};
    return dsp_graph_adapters::with_dsp_operations(std::move(p));
}
OwnedProcessingContract own(const NumericContract& c) {
    OwnedProcessingContract result;
    check(own_processing_contract(c,bounds,16777216,result)==Status::ok,"own source");return result;
}
NumericProcessingGraph graph(MadmomReferenceMode mode) {
    auto g=numeric_madmom_reference_example(48000,mode).graph;
    for(const auto& node:g.nodes)if(const auto* parameters=std::get_if<TransformProcessingParameters>(&node.parameters))
        for(const auto& field:parameters->values)if(const auto* values=std::get_if<NumericArray>(&field.value))
            check(values->values.size()<=32768,"each explicit coefficient parameter fits unchanged admission limit");
    std::erase_if(g.nodes,[](const auto& n){const auto kind=node_kind(n.kind);return kind && is_view(*kind);});
    std::erase_if(g.edges,[&](const auto& e){return std::none_of(g.nodes.begin(),g.nodes.end(),[&](const auto& n){return n.id==e.to;});});
    auto& source=g.nodes.front();auto c=std::get<NumericContract>(source.outputs.front().contract.view());
    c.shape.extents[0]={1,8192};
    source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{48000}}}}};
    source.outputs={{0,PortDirection::Output,own(c)}};
    check(attach_numeric_parameter_ports(source,profile().admission)==Status::ok,"explicit source rate");
    NumericProcessingGraph evaluated,executable;
    const auto diagnostic=resolve_numeric_parameter_bindings(g,profile().admission,evaluated,executable,source.id);
    if(!diagnostic)std::fprintf(stderr,"madmom resolve error=%d node=%d port=%u reason=%u\n",
        int(diagnostic.error),int(diagnostic.node),unsigned(diagnostic.port),unsigned(diagnostic.reason));
    check(bool(diagnostic),"propagate changed packet capacity");
    return evaluated;
}
std::vector<float> signal(std::size_t samples,bool silence=false) {
    std::vector<float> result(samples);
    constexpr float hz[]{110,251,503,997,1999,4001,6007,10007};
    if(silence)return result;
    for(std::size_t i=0;i<samples;++i){
        const auto pulse=std::exp(-double(i%24000)/290.);const double t=double(i)/48000;
        for(unsigned k=0;k<8;++k)result[i]+=float(.06*pulse*std::sin(6.283185307179586*hz[k]*t+.17*k));
    }
    return result;
}
void append(const NumericView& value,std::vector<float>& out) {
    const auto count=value.actual[0]*value.actual[1];
    check(out.size()+count<=out.capacity(),"observer preallocated");
    for(std::size_t k=0;k<count;++k){float x=0;
        check(load_element(value,{0,k/value.actual[1],k%value.actual[1]},x)==Status::ok,"read stage");out.push_back(x);
    }
}
void replace(const NumericView& value,std::vector<float>& out) {out.clear();append(value,out);}
void indices(const NumericView& value,std::vector<std::uint64_t>& out) {
    out.clear();check(value.actual[0]<=out.capacity(),"index observer preallocated");
    for(std::size_t k=0;k<value.actual[0];++k){std::uint64_t x=0;
        check(load_element(value,{0,k,0},x)==Status::ok,"read exact indices");out.push_back(x);
    }
}
double time_coordinate(const NumericView& value,std::size_t index) {
    const auto& coordinate=value.contract->coordinates[0];
    if(coordinate.mode==CoordinateMode::Regular)
        return double(coordinate.origin.numerator)/double(coordinate.origin.denominator)+
            double(index)*double(coordinate.step.numerator)/double(coordinate.step.denominator);
    const auto bytes=value.coordinates.axes[0];
    if(bytes.size()/8<=index)return std::numeric_limits<double>::quiet_NaN();
    std::uint64_t bits=0;
    for(std::size_t byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned>(bytes[index*8+byte]))<<(byte*8);
    return std::bit_cast<double>(bits);
}
struct Observation {
    std::vector<float> magnitude,filtered,logarithmic,onset,snapshot,tempo,raw_histogram,smoothed_histogram,beats;
    std::vector<std::uint64_t> peaks,beat_positions,trials,rejected_ends;
    std::vector<float> trial_scores,decoder_histogram,decoder_tempo,coordinates;
    std::array<std::array<std::size_t,5>,37> publications{};
    double snapshot_first_time=0,snapshot_last_time=0;
    std::size_t frame_count=0,grids=0,rejected_grids=0,snapshot_end=0,band_count=0,boundary_markers=0;
    bool crf=false;
    explicit Observation(bool is_crf=false):crf(is_crf) {
        magnitude.reserve(2400*1024);filtered.reserve(2400*256);logarithmic.reserve(2400*256);onset.reserve(2400);
        snapshot.reserve(2000);tempo.reserve(1102);raw_histogram.reserve(551);smoothed_histogram.reserve(551);beats.reserve(2001);
        peaks.reserve(2000);beat_positions.reserve(2001);trials.reserve(5);trial_scores.reserve(5);
        rejected_ends.reserve(60);
        decoder_histogram.reserve(253);decoder_tempo.reserve(1102);coordinates.reserve(551);
    }
    void feature(NodeId node,const NumericView& v) {
        if(node==20){check(v.actual[0]==2048,"complete fixed length PCM frame");++frame_count;}
        if(node==25)append(v,magnitude);
        if(node==26){band_count=v.actual[0];append(v,filtered);}
        if(node==29)append(v,logarithmic);
        if(node==3)append(v,onset);
    }
    void analysis(NodeId node,PortId port,const NumericView& v) {
        if(node==31){replace(v,snapshot);snapshot_end=onset.size();
            snapshot_first_time=time_coordinate(v,0);snapshot_last_time=time_coordinate(v,v.actual[0]?v.actual[0]-1:0);}
        if(node==35 && port==0)replace(v,tempo);
        if(node==35 && port==1)replace(v,raw_histogram);
        if(node==35 && port==2)replace(v,coordinates);
        if(node==35 && port==3)replace(v,smoothed_histogram);
        if(node==4 && port==0){replace(v,beats);++grids;}
        if(node==4 && port==1)indices(v,beat_positions);
        if(node==4 && port==2 && crf)indices(v,trials);
        if(node==4 && port==2 && !crf)replace(v,decoder_histogram);
        if(node==4 && port==3 && crf)replace(v,trial_scores);
        if(node==4 && port==4 && crf)replace(v,decoder_tempo);
        if(node==36 && port==0)indices(v,peaks);
    }
    void reject() {
        check(rejected_ends.size()<rejected_ends.capacity(),"rejection observer preallocated");
        ++rejected_grids;rejected_ends.push_back(snapshot_end);
        beats.clear();beat_positions.clear();trials.clear();trial_scores.clear();
        decoder_histogram.clear();decoder_tempo.clear();
    }
    static void output(void* user,const NumericPortFrameView& e) {
        if(e.identity.node!=20 && e.identity.node!=25 && e.identity.node!=26 && e.identity.node!=29 &&
            e.identity.node!=3 && e.identity.node!=31 && e.identity.node!=35 && e.identity.node!=4 && e.identity.node!=36)return;
        auto& self=*static_cast<Observation*>(user);
        if(e.identity.port>=self.publications[0].size())return;
        auto& publications=self.publications[std::size_t(e.identity.node)][e.identity.port];
        if(e.status==Status::unavailable && !e.frame.stamp.available && !publications){
            ++self.boundary_markers;return; // Explicit reset/warm-up; not a decoded/rejected CRF snapshot.
        }
        if(e.identity.node==4 && e.status==Status::invalid_argument){
            if(!e.identity.port)self.reject();return;
        }
        if(e.status!=Status::ok)std::fprintf(stderr,"madmom node=%d port=%u status=%d snapshot_count=%zu first_time=%.17g last_time=%.17g\n",
            int(e.identity.node),unsigned(e.identity.port),int(e.status),self.snapshot.size(),self.snapshot_first_time,self.snapshot_last_time);
        check(e.status==Status::ok && e.frame.stamp.available,"available graph stages");++publications;
        const auto& v=std::get<NumericView>(e.frame.payload);
        if(!e.identity.port)self.feature(e.identity.node,v);
        self.analysis(e.identity.node,e.identity.port,v);
    }
};
struct Execution {
    NumericProcessingGraph g;
    std::shared_ptr<const PreparedGraph> plan;
    std::unique_ptr<RuntimeContext> state;
    explicit Execution(MadmomReferenceMode mode):g(graph(mode)) {
        const auto status=PreparedGraph::prepare_numeric(g,profile(),plan,state);
        if(status!=Status::ok)std::fprintf(stderr,"madmom prepare mode=%d status=%d\n",int(mode),int(status));
        check(status==Status::ok,"prepare graph");
    }
    void feed(std::span<const float> values,std::size_t packet,Observation& seen,std::uint64_t epoch=1) {
        const auto c=std::get<NumericContract>(g.nodes.front().outputs.front().contract.view());
        const PlaneLayout layout{0,{sizeof(float),0}};std::uint64_t sequence=0;
        std::array<float,8192> storage{};const auto before=allocations;
        for(std::size_t offset=0;offset<values.size();){
            const auto count=std::min(packet,values.size()-offset);FrameStamp stamp;
            stamp.sequence=stamp.source_sequence=++sequence;stamp.producer_epoch=epoch;stamp.discontinuity=offset==0;
            stamp.has_sample_interval=true;stamp.sample_begin=std::int64_t(offset);stamp.sample_count=count;
            stamp.timestamp_ticks=std::int64_t(offset);
            std::copy_n(values.begin()+std::ptrdiff_t(offset),count,storage.begin());
            const auto input=numeric_test_builders::frame(stamp,c,std::as_bytes(std::span(storage)),std::span(&layout,1),{count,1});
            const auto rejects=seen.rejected_grids;
            const auto status=plan->process_numeric_ports(1,0,input,*state,{},NumericRuntimeCallbacks{Observation::output,nullptr,nullptr,&seen});
            check(status==Status::ok || (status==Status::invalid_argument && seen.rejected_grids>rejects),"process packet or explicitly observed beat kernel rejection");offset+=count;
        }
        check(allocations==before,"processing and observer allocate nothing");
    }
};
void equal(const Observation& a,const Observation& b) {
    check(a.publications==b.publications,"chunk invariant successful publication counts");
    check(a.magnitude==b.magnitude && a.filtered==b.filtered && a.logarithmic==b.logarithmic && a.onset==b.onset,"chunk invariant frontend");
    if(a.snapshot!=b.snapshot || a.raw_histogram!=b.raw_histogram || a.smoothed_histogram!=b.smoothed_histogram || a.beats!=b.beats)
        std::fprintf(stderr,"madmom temporal snapshots=%zu/%zu end=%zu/%zu grids=%zu/%zu raw=%zu/%zu smoothed=%zu/%zu beats=%zu/%zu\n",
            a.snapshot.size(),b.snapshot.size(),a.snapshot_end,b.snapshot_end,a.grids,b.grids,
            a.raw_histogram.size(),b.raw_histogram.size(),a.smoothed_histogram.size(),b.smoothed_histogram.size(),a.beats.size(),b.beats.size());
    check(a.snapshot==b.snapshot,"chunk invariant ODF snapshot");
    check(a.raw_histogram==b.raw_histogram,"chunk invariant raw tempo histogram");
    check(a.smoothed_histogram==b.smoothed_histogram,"chunk invariant smoothed tempo histogram");
    check(a.beats==b.beats,"chunk invariant decoded beats");
    check(a.tempo.size()==b.tempo.size(),"candidate shapes");
    check(a.rejected_grids==b.rejected_grids && a.rejected_ends==b.rejected_ends,"chunk invariant CRF rejection count and snapshots");
    check(a.peaks==b.peaks && a.beat_positions==b.beat_positions && a.trials==b.trials && a.trial_scores==b.trial_scores,"chunk invariant event/CRF diagnostics");
    check(a.decoder_histogram==b.decoder_histogram && a.decoder_tempo==b.decoder_tempo && a.coordinates==b.coordinates,"chunk invariant typed final diagnostics");
    for(std::size_t k=0;k<a.tempo.size();++k)check(a.tempo[k]==b.tempo[k] || (std::isnan(a.tempo[k]) && std::isnan(b.tempo[k])),"candidate values including source NaN sentinel");
}
void complete(const Observation& seen,std::size_t attempts) {
    for(const auto node:{35,36})for(std::size_t port=0;port<(node==35?4u:2u);++port)
        check(seen.publications[std::size_t(node)][port]==attempts,"every tempo and peak snapshot actually published");
    for(std::size_t port=0;port<(seen.crf?5u:3u);++port)
        check(seen.publications[4][port]==seen.grids,"every decoded beat output actually published");
    check(seen.grids>0 && !seen.beats.empty(),"final rhythmic snapshot has a decoded path");
}
void write(const std::filesystem::path& root,const char* name,std::span<const float> data) {
    std::ofstream f(root/name,std::ios::binary);f.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size_bytes()));check(bool(f),"write evidence");
}
void write_indices(const std::filesystem::path& root,const char* name,std::span<const std::uint64_t> data) {
    std::ofstream f(root/name,std::ios::binary);f.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size_bytes()));check(bool(f),"write exact evidence");
}
void export_values(const std::filesystem::path& root,std::span<const float> pcm,const Observation& seen) {
    check(std::filesystem::create_directories(root),"evidence mode directory must be new; preserve prior exports");
    write(root,"pcm.f32",pcm);write(root,"magnitude.f32",seen.magnitude);write(root,"filtered.f32",seen.filtered);
    write(root,"log.f32",seen.logarithmic);write(root,"onset.f32",seen.onset);write(root,"snapshot.f32",seen.snapshot);
    write(root,"tempo.f32",seen.tempo);write(root,"histogram.f32",seen.raw_histogram);write(root,"smoothed.f32",seen.smoothed_histogram);write(root,"beats.f32",seen.beats);
    write_indices(root,"peaks.u64",seen.peaks);write_indices(root,"beat_positions.u64",seen.beat_positions);write_indices(root,"trials.u64",seen.trials);write(root,"trial_scores.f32",seen.trial_scores);
    write_indices(root,"rejected_ends.u64",seen.rejected_ends);
    write(root,"decoder_histogram.f32",seen.decoder_histogram);write(root,"crf_tempo.f32",seen.decoder_tempo);write(root,"coordinates.f32",seen.coordinates);
    std::ofstream meta(root/"geometry.txt");meta<<48000<<' '<<240<<' '<<2048<<' '<<seen.frame_count<<' '<<seen.band_count<<' '<<seen.snapshot_end<<' '<<seen.snapshot.size();
}
void mode_case(MadmomReferenceMode mode,const char* directory) {
    const auto pcm=signal(3*48000);Execution first(mode),second(mode);
    const bool crf=mode==MadmomReferenceMode::Crf;Observation a(crf),b(crf),reset(crf);
    first.feed(pcm,97,a);second.feed(pcm,8192,b);equal(a,b);
    const auto count=(pcm.size()-2048)/240+1;
    check(a.frame_count==count && a.onset.size()==count && a.magnitude.size()==count*1024,"one FFT and novelty per complete frame");
    const auto attempts=a.grids+a.rejected_grids;
    check(attempts==1+(count-1)/40 && a.snapshot_end==1+(attempts-1)*40,"independent fixed temporal cadence");
    check(a.filtered.size()==count*a.band_count && a.band_count>24,"24 bands per octave, not 24 total bands");
    complete(a,attempts);
    if(directory)export_values(std::filesystem::path(directory)/std::to_string(int(mode)),pcm,a);
    // A new independent recording explicitly resets owned histories; a source
    // epoch alone intentionally preserves compatible History in the runtime.
    first.plan->reset_history(27,*first.state);first.plan->reset_history(32,*first.state);
    first.feed(pcm,511,reset,2);equal(a,reset);
    std::printf("madmom mode=%d frames=%zu bands=%zu grids=%zu rejected=%zu beats=%zu\n",int(mode),count,a.band_count,a.grids,a.rejected_grids,a.beats.size());
}
void rollover_case() {
    const auto pcm=signal(11*48000);Execution execution(MadmomReferenceMode::Fixed);Observation seen;
    execution.feed(pcm,8192,seen);
    check(seen.snapshot.size()==2000 && seen.snapshot_end>2000,"bounded context rolls over");
    check(std::equal(seen.snapshot.begin(),seen.snapshot.end(),seen.onset.begin()+std::ptrdiff_t(seen.snapshot_end-2000)),"context retains newest ODF in chronological order");
}
void silence_case() {
    const auto pcm=signal(48000,true);Execution execution(MadmomReferenceMode::Fixed);Observation seen;
    execution.feed(pcm,97,seen);
    check(std::all_of(seen.onset.begin(),seen.onset.end(),[](float x){return x==0;}),"silence has zero spectral onset evidence");
    check(seen.tempo.size()==2 && std::isnan(seen.tempo[0]) && seen.tempo[1]==0,"source no-tempo sentinel remains explicit");
}
NumericProcessingGraph coordinate_graph() {
    auto g=numeric_madmom_reference_example().graph;
    auto tempo=*std::find_if(g.nodes.begin(),g.nodes.end(),[](const auto& node){return node.id==35;});
    auto c=std::get<NumericContract>(tempo.inputs[0].contract.view());
    c.semantic=c.history_element=Semantic::Numeric;
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";
    source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{48000}}}}};
    source.outputs={{0,PortDirection::Output,own(c)}};tempo.inputs[0].contract=source.outputs[0].contract;
    g.nodes={source,tempo};g.edges={{1,35,0,0}};g.graph_id="madmom-tempo-coordinate-regression";return g;
}
struct CoordinateObservation {
    std::array<std::vector<float>,4> values;
    std::size_t calls=0,errors=0;
    CoordinateObservation(){for(auto& output:values)output.reserve(1102);}
    static void output(void* context,const NumericPortFrameView& event) {
        if(event.identity.node!=35)return;
        auto& self=*static_cast<CoordinateObservation*>(context);
        if(event.status!=Status::ok || !event.frame.stamp.available){++self.errors;return;}
        check(event.identity.port<self.values.size(),"coordinate regression output port");
        replace(std::get<NumericView>(event.frame.payload),self.values[event.identity.port]);++self.calls;
    }
};
CoordinateObservation coordinate_case(double origin,double step,bool nonuniform,bool accepted) {
    const auto g=coordinate_graph();const auto c=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,profile(),plan,state)==Status::ok,"prepare tempo coordinate regression");
    std::array<float,2000> values{};std::array<double,121> times{};
    for(std::size_t k=0;k<times.size();++k){values[k]=k%40==10?1.f:.01f;times[k]=origin+double(k)*step;}
    if(nonuniform)times[60]+=.001;
    const PlaneLayout plane{0,{sizeof(float),0}};FrameStamp stamp;
    stamp.sequence=stamp.source_sequence=stamp.producer_epoch=1;
    auto input=numeric_test_builders::frame(stamp,c,std::as_bytes(std::span(values)),std::span(&plane,1),{times.size(),1});
    std::get<NumericView>(input.payload).coordinates.axes[0]=std::as_bytes(std::span(times));
    CoordinateObservation seen;const auto before=allocations;
    const auto status=plan->process_numeric_ports(1,0,input,*state,{},NumericRuntimeCallbacks{CoordinateObservation::output,nullptr,nullptr,&seen});
    check(allocations==before,"tempo coordinate regression processing allocates nothing");
    if(accepted)check(status==Status::ok && seen.calls==4 && !seen.errors,"uniform tempo grid with arbitrary origin accepted");
    else check(status==Status::invalid_argument && !seen.calls && seen.errors,"wrong or nonuniform tempo grid rejected");
    return seen;
}
void coordinate_regression() {
    const auto zero=coordinate_case(0,1./200,false,true);
    for(const auto origin:{17.,-17.}){
        const auto shifted=coordinate_case(origin,1./200,false,true);
        for(std::size_t port=0;port<zero.values.size();++port){
            check(zero.values[port].size()==shifted.values[port].size(),"origin invariant tempo output geometry");
            for(std::size_t k=0;k<zero.values[port].size();++k)
                check(zero.values[port][k]==shifted.values[port][k] ||
                    (std::isnan(zero.values[port][k]) && std::isnan(shifted.values[port][k])),"origin invariant exact tempo analytics");
        }
    }
    coordinate_case(17,1./180,false,false);coordinate_case(-17,1./200,true,false);
}
void whole_precision(const NumericProcessingGraph& graph,MadmomReferenceMode mode) {
    const bool reference=mode==MadmomReferenceMode::Crf;
    const auto expected=reference?DType::Float64:DType::Float32;
    for(const auto& node:graph.nodes){
        if(node.id==22 || node.id==23)
            check(std::get<NumericContract>(node.outputs.front().contract.view()).dtype==expected,"whole window and product retain selected precision");
        if(node.id==24){
            check(std::get<NumericContract>(node.inputs.front().contract.view()).dtype==expected,"whole real FFT receives unrounded selected input");
            for(const auto& output:node.outputs)if(output.id<3)
                check(std::get<NumericContract>(output.contract.view()).dtype==DType::Float32,"whole FFT publishes Float32 before magnitude");
        }
    }
    const bool calibration=std::any_of(graph.edges.begin(),graph.edges.end(),[](const auto& edge){return edge.from==22 && edge.to==25;});
    check(calibration!=reference,"raw whole reference magnitude does not consume window calibration");
}
NumericProcessingGraph whole_graph(MadmomReferenceMode mode,std::size_t samples) {
    auto g=numeric_madmom_whole_reference_example(samples,mode).graph;
    std::erase_if(g.nodes,[](const auto& n){const auto kind=node_kind(n.kind);return kind && is_view(*kind);});
    std::erase_if(g.edges,[&](const auto& e){return std::none_of(g.nodes.begin(),g.nodes.end(),[&](const auto& n){return n.id==e.to;});});
    const auto c=std::get<NumericContract>(g.nodes.front().outputs.front().contract.view());
    check(c.shape.extents[0]==ExtentBounds{1,8192} && c.clock.sample_rate==48000,"public whole PCM ingress is bounded before preparation");
    whole_precision(g,mode);
    return g;
}
void whole_reserve(Observation& seen,std::size_t count) {
    seen.snapshot.reserve(count);seen.beats.reserve(count+1);seen.beat_positions.reserve(count+1);
}
void whole_outputs(const Observation& seen,std::size_t count) {
    check(seen.frame_count==count && seen.onset.size()==count,"every whole complete frame collected");
    check(seen.snapshot==seen.onset && seen.snapshot_end==count,"whole ODF is complete, not a last rolling snapshot");
    check(seen.snapshot_first_time==0 && seen.snapshot_last_time==double(count-1)/200,"whole left-edge time coordinates");
    check(seen.publications[31][0]==1 && seen.grids==1 && !seen.beats.empty(),"one whole EOF input and defined beat result");
    for(std::size_t port=0;port<4;++port)check(seen.publications[35][port]==1,"one whole tempo output per port");
    for(std::size_t port=0;port<(seen.crf?5u:3u);++port)check(seen.publications[4][port]==1,"one whole decoder output per port");
    check(seen.publications[36][0]==0 && seen.peaks.empty(),"whole recipe does not claim rolling peaks");
    if(seen.crf)check(seen.decoder_histogram.empty() && seen.decoder_tempo==seen.tempo,"CRF publishes own candidate matrix and UInt64 trials");
    else check(seen.trials.empty() && seen.decoder_histogram.size()==253 &&
        std::all_of(seen.decoder_histogram.begin(),seen.decoder_histogram.end(),[](float x){return std::isfinite(x) && x>=0;}),"Fixed/Local 4:2 is a complete Float32 histogram, not trials");
}
void export_whole(const char* directory,MadmomReferenceMode mode,std::span<const float> pcm,const Observation& seen) {
    if(!directory)return;
    const auto root=std::filesystem::path(directory)/std::to_string(int(mode));
    export_values(root,pcm,seen);
    std::ofstream meta(root/"whole.json");
    meta<<"{\"whole_input\":true,\"pcm_samples\":"<<pcm.size()<<",\"frames\":"<<seen.frame_count
        <<",\"tail_samples\":"<<pcm.size()-((seen.frame_count-1)*240+2048)
        <<",\"odf_source\":31,\"producer\":3,\"frame_rate\":200,\"eof_batches\":1}";
    check(bool(meta),"write whole session identity");
}
void whole_mode_case(MadmomReferenceMode mode,const char* directory) {
    constexpr std::size_t samples=514761,count=2137,tail=73;
    const auto pcm=signal(samples);const auto g=whole_graph(mode,samples);
    for(const auto& node:g.nodes)check(node.id!=27 && node.id!=32 && node.id!=33 && node.id!=36,"whole has no rolling or peak branches");
    Observation a(mode==MadmomReferenceMode::Crf),b(mode==MadmomReferenceMode::Crf);whole_reserve(a,count);whole_reserve(b,count);
    madmom_whole_fixture::Session<Observation> first(g,profile(),samples,count,a,check),second(g,profile(),samples,count,b,check);
    const auto before=allocations;
    check(first.feed(std::span(pcm).first(2048),97)==Status::ok,"whole first frame feed");
    check(first.finish()==Status::invalid_argument && !a.publications[31][0],"incomplete ODF is never submitted");
    check(first.feed(std::span(pcm).subspan(2048,samples-2048-tail),97)==Status::ok,"whole prefix feed");
    check(first.frames()==count && first.finish()==Status::invalid_argument,"EOF waits for all real PCM, including tail");
    check(first.feed(std::span(pcm).last(tail),97)==Status::ok && first.finish()==Status::ok,"whole EOF after real short tail");
    check(first.finish()==Status::invalid_argument && first.feed({},97)==Status::invalid_argument,"finite session accepts EOF only once");
    check(second.feed(pcm,8192)==Status::ok && second.finish()==Status::ok,"whole large partition EOF");
    check(allocations==before,"whole prepared processing and collector allocate nothing");
    whole_outputs(a,count);whole_outputs(b,count);equal(a,b);export_whole(directory,mode,pcm,a);
    std::printf("madmom WHOLE mode=%d N=%zu F=%zu tail=%zu EOF=1 beats=%zu\n",int(mode),samples,count,tail,a.beats.size());
}
void whole_contract_rejections() {
    for(const auto rate:{44100u,96000u}){
        bool rejected=false;
        try{numeric_madmom_whole_reference_example(514761,MadmomReferenceMode::Crf,rate);}
        catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"whole variant rejects unimplemented native rate");
    }
    bool rejected=false;
    try{numeric_madmom_whole_reference_example(2047);}
    catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"whole variant requires a complete frame");
}
void whole_no_result_case(const char* directory) {
    constexpr std::size_t samples=48000,count=192;
    const auto pcm=signal(samples,true);const auto g=whole_graph(MadmomReferenceMode::Crf,samples);
    Observation seen(true);whole_reserve(seen,count);
    madmom_whole_fixture::Session<Observation> session(g,profile(),samples,count,seen,check);const auto before=allocations;
    check(session.feed(pcm,97)==Status::ok && session.finish()==Status::ok,"whole no-evidence EOF is successful computation");
    check(allocations==before,"whole no-result processing allocates nothing");
    check(seen.grids==1 && seen.rejected_grids==0 && seen.beats.empty() && seen.beat_positions.empty(),"available empty beats, not invalid_argument");
    for(std::size_t port=0;port<5;++port)check(seen.publications[4][port]==1,"empty result publishes every CRF diagnostic port once");
    for(std::size_t port=0;port<4;++port)check(seen.publications[35][port]==1,"no-tempo publishes every tempo port once");
    check(seen.tempo.size()==2 && std::isnan(seen.tempo[0]) && seen.tempo[1]==0,"no-tempo source sentinel retained");
    check(seen.decoder_tempo.size()==2 && std::isnan(seen.decoder_tempo[0]) && seen.decoder_tempo[1]==0,"CRF own no-tempo sentinel is current");
    check(seen.trials.empty() && seen.trial_scores.empty(),"no evidence does not reuse prior trial values");
    if(directory)export_values(std::filesystem::path(directory)/"no-result",pcm,seen);
}
}
void* operator new(std::size_t bytes) {++allocations;if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t bytes,std::align_val_t alignment) {
    ++allocations;if(auto* p=_aligned_malloc(bytes?bytes:1,std::size_t(alignment)))return p;throw std::bad_alloc();
}
void operator delete(void* p,std::align_val_t) noexcept {_aligned_free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept {_aligned_free(p);}
void* operator new[](std::size_t bytes,std::align_val_t alignment) {return ::operator new(bytes,alignment);}
void operator delete[](void* p,std::align_val_t alignment) noexcept {::operator delete(p,alignment);}
void operator delete[](void* p,std::size_t,std::align_val_t alignment) noexcept {::operator delete(p,alignment);}
int main(int argc,char** argv) {
    if(argc>1 && std::string_view(argv[1])=="--whole-no-result"){
        whole_no_result_case(argc>2?argv[2]:nullptr);
        std::puts("madmom whole no-result regression PASS");return 0;
    }
    if(argc>1 && std::string_view(argv[1])=="--whole"){
        const auto directory=argc>2?argv[2]:nullptr;whole_contract_rejections();
        whole_mode_case(MadmomReferenceMode::Fixed,directory);whole_mode_case(MadmomReferenceMode::Local,directory);
        whole_mode_case(MadmomReferenceMode::Crf,directory);whole_no_result_case(directory);
        std::puts("madmom whole graph regression PASS");return 0;
    }
    const auto directory=argc>1?argv[1]:nullptr;
    coordinate_regression();
    mode_case(MadmomReferenceMode::Fixed,directory);mode_case(MadmomReferenceMode::Local,directory);mode_case(MadmomReferenceMode::Crf,directory);
    rollover_case();silence_case();std::puts("madmom graph regression PASS");
}
