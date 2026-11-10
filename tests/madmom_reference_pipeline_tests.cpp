#include <core_app/numeric_madmom_reference_graph.h>
#include <dataflow_runtime/runtime.h>
#include <dsp_graph_adapters/profile.h>
#include <dsp_kernels/fourier_transform.h>
#include <domain/graph/node_factory.h>
#include <domain/graph/preparation_values.h>
#include <domain/numeric/atomic_operation_ports.h>
#include "numeric_test_builders.h"
#include "test_aligned_allocation.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <string_view>

namespace {
using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
namespace beat_ports=domain::atomic_ports::decoder;
std::size_t allocations=0,provider_calls=0,provider_clones=0;
constexpr FrameLimits bounds{8,2000000,16000000,65536,4096,65536};
constexpr std::size_t history=2000,stride=40;
void check(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"FAIL madmom combined pipeline: %s\n",message);std::exit(1);}
}
NumericRuntimeProfile profile() {
    NumericRuntimeProfile p{{128,256,16,128,1048576,16777216,bounds},
        {{33554432,134217728,134217728,1048576},0,536870912},
        {2000000,8,16000000,16000000,16777216},{true,true,true}};
    return dsp_graph_adapters::with_dsp_operations(std::move(p));
}
OwnedProcessingContract own(const NumericContract& c) {
    OwnedProcessingContract result;check(own_processing_contract(c,bounds,16777216,result)==Status::ok,"own test ingress");return result;
}
ConfiguredProcessingNode& node(NumericProcessingGraph& g,NodeId id) {
    auto found=std::find_if(g.nodes.begin(),g.nodes.end(),[id](const auto& n){return n.id==id;});
    check(found!=g.nodes.end(),"stable live preset node exists");return *found;
}
void field(ConfiguredProcessingNode& n,std::string key,ProcessingParameterValue value) {
    auto& fields=std::get<TransformProcessingParameters>(n.parameters).values;
    auto found=std::find_if(fields.begin(),fields.end(),[&](const auto& entry){return entry.key==key;});
    if(found==fields.end())fields.push_back({std::move(key),std::move(value)});else found->value=std::move(value);
}
NumericProcessingGraph graph(bool provider=false) {
    auto g=numeric_madmom_combined_reference_example(48000).graph;
    for(const auto& n:g.nodes)if(const auto* p=std::get_if<TransformProcessingParameters>(&n.parameters))
        for(const auto& entry:p->values)if(const auto* values=std::get_if<NumericArray>(&entry.value))check(values->values.size()<=32768,"coefficient parameter admission unchanged");
    std::erase_if(g.nodes,[](const auto& n){const auto kind=node_kind(n.kind);return kind && is_view(*kind);});
    std::erase_if(g.edges,[&](const auto& e){return std::none_of(g.nodes.begin(),g.nodes.end(),[&](const auto& n){return n.id==e.to;});});
    auto& source=node(g,1);auto c=std::get<NumericContract>(source.outputs.front().contract.view());c.shape.extents[0]={1,8192};
    source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{48000}}}}};
    source.outputs={{0,PortDirection::Output,own(c)}};
    check(attach_numeric_parameter_ports(source,profile().admission)==Status::ok,"explicit source rate");
    if(provider)field(node(g,24),"backend_id",std::string{"madmom-test-real-forward"});
    NumericProcessingGraph evaluated,executable;
    const auto result=resolve_numeric_parameter_bindings(g,profile().admission,evaluated,executable,source.id);
    if(!result)std::fprintf(stderr,"madmom resolve error=%d node=%d port=%u\n",int(result.error),int(result.node),unsigned(result.port));
    check(bool(result),"propagate packet capacity through live combined graph");return evaluated;
}
std::vector<float> signal(std::size_t samples,bool silence=false) {
    std::vector<float> values(samples);constexpr float hz[]{110,251,503,997,1999,4001,6007,10007};
    if(silence)return values;
    for(std::size_t i=0;i<samples;++i){
        const double t=double(i)/48000,pulse=std::exp(-double(i%24000)/290.);
        for(unsigned k=0;k<8;++k)values[i]+=float(.06*pulse*std::sin(6.283185307179586*(hz[k]*t+.3*std::sin(t*(k+1)))+.17*k));
    }
    return values;
}
template<typename T> void append(const NumericView& value,std::vector<T>& output) {
    const auto count=value.actual[0]*value.actual[1];check(count<=output.capacity()-output.size(),"observer scratch reserved before processing");
    for(std::size_t i=0;i<count;++i){T sample{};
        check(load_element(value,{0,i/value.actual[1],i%value.actual[1]},sample)==Status::ok,"read exact stage dtype");output.push_back(sample);
    }
}
template<typename T> void replace(const NumericView& value,std::vector<T>& output) {output.clear();append(value,output);}
struct Beats {
    std::vector<float> times,scores;std::vector<std::uint64_t> positions,trials,offsets{0},trial_offsets{0};
    std::array<std::size_t,4> publications{},rejections{};std::vector<std::uint8_t> outcomes;
    void reserve(std::size_t snapshots) {
        times.reserve(snapshots*(history+1));positions.reserve(snapshots*(history+1));scores.reserve(snapshots*5);trials.reserve(snapshots*5);
        offsets.reserve(snapshots+1);trial_offsets.reserve(snapshots+1);outcomes.reserve(snapshots);
    }
    void capture(PortId port,const NumericView& value) {
        ++publications[port];
        if(port==beat_ports::times){append(value,times);outcomes.push_back(0);}
        if(port==beat_ports::positions){append(value,positions);offsets.push_back(positions.size());}
        if(port==beat_ports::trials){append(value,trials);trial_offsets.push_back(trials.size());}
        if(port==beat_ports::scores)append(value,scores);
    }
    void reject(PortId port) {
        ++rejections[port];
        if(port==beat_ports::times)outcomes.push_back(1);
        if(port==beat_ports::positions)offsets.push_back(positions.size());
        if(port==beat_ports::trials)trial_offsets.push_back(trials.size());
    }

};
struct Observation {
    std::vector<float> magnitude,filtered,logarithmic,onset,snapshots,fixed_activation,fixed_raw,fixed_smoothed,fixed_intervals,local_intervals,decoder_activation;
    std::vector<double> crf_activation,crf_raw,crf_smoothed;
    std::vector<std::uint64_t> ends,supplied_trials,supplied_offsets{0},snapshot_offsets{0},fixed_offsets{0},crf_offsets{0};
    std::array<Beats,3> beats;
    std::array<std::array<std::size_t,4>,125> publications{};
    std::size_t frames=0,bands=0,boundaries=0,rejected=0;
    explicit Observation(std::size_t maximum_frames){reserve(maximum_frames);}
    void reserve(std::size_t maximum_frames) {
        const auto count=1+(maximum_frames-1)/stride;
        magnitude.reserve(maximum_frames*1024);filtered.reserve(maximum_frames*137);logarithmic.reserve(maximum_frames*137);onset.reserve(maximum_frames);
        for(auto* values:{&snapshots,&fixed_activation,&local_intervals,&decoder_activation})values->reserve(count*history);
        fixed_raw.reserve(count*253);fixed_smoothed.reserve(count*253);fixed_intervals.reserve(count);
        crf_activation.reserve(count*history);crf_raw.reserve(count*551);crf_smoothed.reserve(count*551);
        ends.reserve(count);supplied_trials.reserve(count*5);
        for(auto* offsets:{&supplied_offsets,&snapshot_offsets,&fixed_offsets,&crf_offsets})offsets->reserve(count+1);
        for(auto& trace:beats)trace.reserve(count);
    }
    void frontend(NodeId id,const NumericView& value) {
        if(id==20){check(value.actual[0]==2048,"complete fixed PCM frame");++frames;}
        if(id==25)append(value,magnitude);
        if(id==26){bands=value.actual[0];append(value,filtered);}
        if(id==29)append(value,logarithmic);
        if(id==3)append(value,onset);
    }
    void temporal(NodeId id,const NumericView& value) {
        switch(id){
        case 31:check(value.actual[0]<=history,"bounded filled rolling context");append(value,snapshots);snapshot_offsets.push_back(snapshots.size());ends.push_back(onset.size());break;
        case 92:append(value,fixed_activation);fixed_offsets.push_back(fixed_activation.size());break;
        case 122:append(value,fixed_raw);break;
        case 96:append(value,fixed_smoothed);break;
        case 98:append(value,fixed_intervals);break;
        case 106:append(value,local_intervals);break;
        case 111:append(value,crf_activation);crf_offsets.push_back(crf_activation.size());break;
        case 124:append(value,crf_raw);break;
        case 115:append(value,crf_smoothed);break;
        case 119:append(value,supplied_trials);supplied_offsets.push_back(supplied_trials.size());break;
        case 120:append(value,decoder_activation);break;
        default:break;
        }
    }
    static bool observed(NodeId id) noexcept {
        constexpr NodeId ids[]{20,25,26,29,3,31,92,122,96,98,106,111,124,115,119,120};
        return std::find(std::begin(ids),std::end(ids),id)!=std::end(ids);
    }
    static void output(void* context,const NumericPortFrameView& event) {
        if(event.direction!=PortDirection::Output)return;
        const auto id=event.identity.node;const auto port=event.identity.port;
        const bool decoder=id==80 || id==81 || id==4;
        if(!decoder && (!observed(id) || port))return;
        auto& self=*static_cast<Observation*>(context);
        check(port<4,"live observed output ID");
        if(event.status==Status::unavailable && !event.frame.stamp.available){++self.boundaries;return;}
        if(decoder && event.status==Status::invalid_argument){
            self.beats[id==80?0:id==81?1:2].reject(port);if(!port)++self.rejected;return;
        }
        if(event.status!=Status::ok)std::fprintf(stderr,"madmom node=%d port=%u status=%d end=%zu\n",int(id),unsigned(port),int(event.status),self.onset.size());
        check(event.status==Status::ok && event.frame.stamp.available,"all subscribed live stages available");
        const auto& value=std::get<NumericView>(event.frame.payload);++self.publications[std::size_t(id)][port];
        if(decoder)self.beats[id==80?0:id==81?1:2].capture(port,value);
        else {self.frontend(id,value);self.temporal(id,value);}
    }
};
// A named provider delegates only the transform to the portable mathematical
// primitive. No DSP recipe or tempo state is recreated by this test adapter.
class Provider final:public dsp_kernels::FourierBackend {
    dsp_kernels::PreparedFourierTransform transform_;std::vector<float> imaginary_,complex_;
public:
    bool prepare() {imaginary_.resize(2048);complex_.resize(4096);return transform_.prepare(2048);}
    bool process(std::span<const float> input,std::span<float> output) noexcept override {
        if(!dsp_kernels::fourier_backend_call_valid({2048,dsp_kernels::FourierBackendTransform::RealForward},input,output))return false;
        if(!transform_.process(input,imaginary_,complex_))return false;
        std::copy_n(complex_.begin(),output.size(),output.begin());++provider_calls;return true;
    }
    dsp_kernels::FourierBackendResources resources() const noexcept override {
        const auto bytes=sizeof(*this)+transform_.workspace_bytes()+(imaginary_.capacity()+complex_.capacity())*sizeof(float);
        return {bytes,bytes,0,131072};
    }
    std::unique_ptr<dsp_kernels::FourierBackend> clone() const override {++provider_clones;return std::make_unique<Provider>(*this);}
};
class ProviderFactory final:public dsp_kernels::FourierBackendFactory {
    const char* id_;
public:
    explicit ProviderFactory(const char* id="madmom-test-real-forward"):id_(id) {}
    const char* id() const noexcept override {return id_;}
    bool query(dsp_kernels::FourierBackendRequest request,dsp_kernels::FourierBackendResources& result) const noexcept override {
        if(request!=dsp_kernels::FourierBackendRequest{2048,dsp_kernels::FourierBackendTransform::RealForward})return false;
        std::size_t bytes=0;if(!dsp_kernels::PreparedFourierTransform::storage_bytes(2048,bytes))return false;
        bytes+=sizeof(Provider)+6144*sizeof(float);result={bytes,bytes,0,131072};return true;
    }
    std::unique_ptr<dsp_kernels::FourierBackend> prepare(dsp_kernels::FourierBackendRequest request) const override {
        dsp_kernels::FourierBackendResources resources;if(!query(request,resources))return {};
        auto value=std::make_unique<Provider>();return value->prepare()?std::move(value):nullptr;
    }
};
struct Execution {
    NumericProcessingGraph g;std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    std::size_t processed=0;std::uint64_t sequence=0,epoch=1;
    explicit Execution(bool provider=false):g(graph(provider)) {
        auto p=profile();if(provider)p.fourier_backend=std::make_shared<ProviderFactory>();
        const auto result=PreparedGraph::prepare_numeric(g,p,plan,state);
        if(result!=Status::ok)std::fprintf(stderr,"madmom prepare status=%d provider=%d\n",int(result),int(provider));
        check(result==Status::ok,"prepare current combined preset");seed_coefficients();
    }
    void seed_coefficients() {
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=1;stamp.producer_epoch=epoch;
        stamp.has_sample_interval=true;stamp.sample_count=1;
        const auto before=allocations;
        for(const auto id:{90,94,109,113})check(plan->process_clock(id,stamp,*state,{})==Status::ok,"publish explicit Hamming size constants through runtime clock API");
        check(allocations==before,"coefficient-source publication allocates nothing");
    }
    Execution(const Execution& other):g(other.g),plan(other.plan),state(std::make_unique<RuntimeContext>(*other.state)),processed(other.processed),sequence(other.sequence),epoch(other.epoch) {}
    void reset() {plan->reset_history(27,*state);processed=0;sequence=0;++epoch;seed_coefficients();}
    void packet(std::span<const float> values,Observation& seen) {
        const auto c=std::get<NumericContract>(node(g,1).outputs.front().contract.view());
        std::array<float,8192> storage{};std::copy(values.begin(),values.end(),storage.begin());
        const PlaneLayout layout{0,{sizeof(float),0}};FrameStamp stamp;
        stamp.sequence=stamp.source_sequence=++sequence;stamp.producer_epoch=epoch;stamp.discontinuity=processed==0;
        stamp.has_sample_interval=true;stamp.sample_begin=stamp.timestamp_ticks=std::int64_t(processed);stamp.sample_count=values.size();
        const auto input=numeric_test_builders::frame(stamp,c,std::as_bytes(std::span(storage)),std::span(&layout,1),{values.size(),1});
        const auto rejected=seen.rejected;
        const auto result=plan->process_numeric_ports(1,0,input,*state,{},NumericRuntimeCallbacks{Observation::output,nullptr,nullptr,&seen});
        check(result==Status::ok || (result==Status::invalid_argument && seen.rejected>rejected),"process packet or explicitly observed decoder rejection");
        processed+=values.size();
    }
    void feed(std::span<const float> values,std::size_t packet_size,Observation& seen) {
        check(packet_size && packet_size<=8192,"bounded packet size");const auto before=allocations;
        for(std::size_t offset=0;offset<values.size();){const auto count=std::min(packet_size,values.size()-offset);packet(values.subspan(offset,count),seen);offset+=count;}
        check(allocations==before,"runtime and observer allocate nothing during processing");
    }
};
void equal(const Observation& a,const Observation& b) {
    check(a.publications==b.publications && a.frames==b.frames && a.rejected==b.rejected,"packet invariant publication/rejection counts");
    check(a.magnitude==b.magnitude && a.filtered==b.filtered && a.logarithmic==b.logarithmic && a.onset==b.onset,"packet invariant complete frontend");
    check(a.snapshots==b.snapshots && a.ends==b.ends && a.snapshot_offsets==b.snapshot_offsets,"packet invariant chronological filled contexts");
    check(a.fixed_offsets==b.fixed_offsets && a.crf_offsets==b.crf_offsets,"packet invariant smoothing lengths during warmup");
    check(a.fixed_activation==b.fixed_activation && a.fixed_raw==b.fixed_raw && a.fixed_smoothed==b.fixed_smoothed,"packet invariant Fixed preprocessing");
    check(a.fixed_intervals==b.fixed_intervals && a.local_intervals==b.local_intervals,"packet invariant supplied Fixed/Local intervals");
    check(a.crf_activation==b.crf_activation && a.crf_raw==b.crf_raw && a.crf_smoothed==b.crf_smoothed && a.decoder_activation==b.decoder_activation,"packet invariant Double CRF preprocessing");
    check(a.supplied_trials==b.supplied_trials && a.supplied_offsets==b.supplied_offsets,"packet invariant supplied CRF trials");
    for(std::size_t i=0;i<3;++i){const auto& x=a.beats[i];const auto& y=b.beats[i];
        check(x.times==y.times && x.positions==y.positions && x.offsets==y.offsets,"packet invariant stable beat outputs");
        check(x.trials==y.trials && x.scores==y.scores && x.trial_offsets==y.trial_offsets,"packet invariant CRF diagnostics");
        check(x.outcomes==y.outcomes && x.rejections==y.rejections,"packet invariant source-unsupported snapshots");
    }
}
void complete_activations(const Observation& seen,std::size_t attempts) {
    if(seen.fixed_offsets.size()!=attempts+1 || seen.crf_offsets.size()!=attempts+1){
        std::fprintf(stderr,"madmom snapshots=%zu fixed=%zu crf=%zu attempts=%zu unavailable=%zu\n",seen.ends.size(),seen.fixed_offsets.size()-1,seen.crf_offsets.size()-1,attempts,seen.boundaries);
        for(const auto id:{31,92,122,96,98,106,111,124,115,119,120,80,81,4})std::fprintf(stderr,"madmom node=%d publications=%zu\n",id,seen.publications[std::size_t(id)][0]);
    }
    check(seen.fixed_offsets.size()==attempts+1 && seen.crf_offsets.size()==attempts+1,"every warmup activation span exported");
    check(seen.fixed_intervals.size()==attempts && seen.local_intervals.size()==seen.fixed_activation.size(),"one explicit interval per Fixed batch or Local prediction");
    check(seen.decoder_activation.size()==seen.crf_activation.size(),"CRF narrowing preserves actual activation length");
}
void complete_snapshots(const Observation& seen,std::size_t attempts) {
    for(std::size_t i=0;i<attempts;++i){
        const auto end=std::size_t(seen.ends[i]),filled=std::min(history,end);
        check(end==1+i*stride && seen.snapshot_offsets[i+1]-seen.snapshot_offsets[i]==filled,"actual occupied context and 40-frame cadence");
        const auto first=seen.snapshots.begin()+std::ptrdiff_t(seen.snapshot_offsets[i]);
        check(std::equal(first,first+std::ptrdiff_t(filled),seen.onset.begin()+std::ptrdiff_t(end-filled)),"filled history is chronological without artificial zero padding");
        check(seen.fixed_offsets[i+1]-seen.fixed_offsets[i]==std::max(filled,std::size_t{28}),"Fixed largest convolution warmup length");
        check(seen.crf_offsets[i+1]-seen.crf_offsets[i]==std::max(filled,std::size_t{18}),"CRF largest convolution warmup length");
    }
}
void complete_beats(const Observation& seen,std::size_t mode,std::size_t attempts) {
    const auto& beats=seen.beats[mode];
    for(std::size_t port=0;port<(mode==2?4u:2u);++port)check(beats.publications[port]+beats.rejections[port]==attempts,"every stable decoder output attempted");
    check(beats.times.size()==beats.positions.size() && beats.offsets.size()==attempts+1 && beats.outcomes.size()==attempts,"typed beat data and explicit outcomes agree in shape");
    for(std::size_t i=0;i<attempts;++i)if(beats.outcomes[i])check(mode==0 && seen.fixed_intervals[i]>float(seen.fixed_offsets[i+1]-seen.fixed_offsets[i]),"only source-unsupported short Fixed snapshots may reject");
    for(std::size_t i=0;i<beats.times.size();++i)check(beats.times[i]==float(beats.positions[i])/200.f,"beat seconds use snapshot-relative frame origin");
}
void complete(const Observation& seen,std::size_t samples) {
    const auto count=(samples-2048)/240+1,attempts=1+(count-1)/stride;
    check(seen.frames==count && seen.onset.size()==count && seen.magnitude.size()==count*1024,"one frontend evaluation per complete frame");
    check(seen.bands==137 && seen.filtered.size()==count*137 && seen.logarithmic.size()==count*137,"all shared filter bands retained");
    check(seen.ends.size()==attempts && seen.snapshot_offsets.size()==attempts+1,"every live rolling snapshot exported");
    complete_activations(seen,attempts);complete_snapshots(seen,attempts);
    for(const auto id:{92,122,96,98,106,111,124,115,119,120})check(seen.publications[std::size_t(id)][0]==attempts,"every explicit stage actually executed");
    for(std::size_t mode=0;mode<3;++mode)complete_beats(seen,mode,attempts);
}

template<typename T> void write(const std::filesystem::path& root,const char* file,const std::vector<T>& values) {
    std::ofstream output(root/file,std::ios::binary);output.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(T)));check(bool(output),"write numerical evidence");
}
const char* dtype_text(DType type) {
    switch(type){
    case DType::Int8:return "Int8";case DType::UInt8:return "UInt8";
    case DType::Int16:return "Int16";case DType::UInt16:return "UInt16";
    case DType::Int32:return "Int32";case DType::UInt32:return "UInt32";
    case DType::Int64:return "Int64";case DType::UInt64:return "UInt64";
    case DType::Float32:return "Float32";case DType::Float64:return "Float64";
    }
    check(false,"export explicit numeric dtype");return "invalid";
}
void export_ports(std::ostream& output,const std::vector<ProcessingPort>& ports) {
    output<<'[';bool first=true;
    for(const auto& port:ports){const auto view=port.contract.view();const auto* c=std::get_if<NumericContract>(&view);check(c,"live preset numeric port");
        if(!first)output<<',';first=false;
        output<<"{\"id\":"<<port.id<<",\"dtype\":\""<<dtype_text(c->dtype)<<"\",\"rank\":"<<int(c->shape.rank)<<",\"maximum\":["<<c->shape.extents[0].maximum<<','<<c->shape.extents[1].maximum<<"]}";
    }
    output<<']';
}
void export_modes(std::ostream& output,const ConfiguredProcessingNode& node) {
    output<<'{';bool first=true;const auto* parameters=std::get_if<TransformProcessingParameters>(&node.parameters);
    if(parameters)for(const auto& entry:parameters->values){
        if(entry.key!="mode" && entry.key!="accumulation")continue;
        const auto* literal=std::get_if<NumericLiteral>(&entry.value);if(!literal)continue;
        if(!first)output<<',';first=false;output<<'"'<<entry.key<<"\":";
        std::visit([&](auto value){output<<+value;},*literal);
    }
    output<<'}';
}
void export_graph(const std::filesystem::path& root,const NumericProcessingGraph& graph) {
    std::ofstream output(root/"graph.json");output<<"{\"graph_id\":\""<<graph.graph_id<<"\",\"nodes\":[";bool first=true;
    for(const auto& n:graph.nodes){if(!first)output<<',';first=false;
        output<<"{\"id\":"<<n.id<<",\"kind\":\""<<n.kind<<"\",\"inputs\":";export_ports(output,n.inputs);
        output<<",\"outputs\":";export_ports(output,n.outputs);output<<",\"settings\":";export_modes(output,n);output<<'}';
    }
    output<<"],\"edges\":[";first=true;
    for(const auto& edge:graph.edges){if(!first)output<<',';first=false;output<<'['<<edge.from<<','<<edge.output_port<<','<<edge.to<<','<<edge.input_port<<']';}
    output<<"]}";check(bool(output),"export actual prepared graph contracts and edges");
}
void export_values(const char* directory,const char* name,const std::vector<float>& pcm,const Observation& s,const NumericProcessingGraph& graph,bool provider=false) {
    if(!directory)return;const auto root=std::filesystem::path(directory)/name;
    check(std::filesystem::create_directories(root),"preserve prior evidence directories");
    write(root,"pcm.f32",pcm);write(root,"magnitude.f32",s.magnitude);write(root,"filtered.f32",s.filtered);write(root,"log.f32",s.logarithmic);write(root,"onset.f32",s.onset);
    write(root,"snapshots.f32",s.snapshots);write(root,"snapshot_ends.u64",s.ends);write(root,"snapshot_offsets.u64",s.snapshot_offsets);
    write(root,"fixed_offsets.u64",s.fixed_offsets);write(root,"crf_offsets.u64",s.crf_offsets);write(root,"fixed_activation.f32",s.fixed_activation);
    write(root,"fixed_histogram.f32",s.fixed_raw);write(root,"fixed_smoothed.f32",s.fixed_smoothed);write(root,"fixed_intervals.f32",s.fixed_intervals);write(root,"local_intervals.f32",s.local_intervals);
    write(root,"crf_activation.f64",s.crf_activation);write(root,"crf_histogram.f64",s.crf_raw);write(root,"crf_smoothed.f64",s.crf_smoothed);write(root,"decoder_activation.f32",s.decoder_activation);
    write(root,"supplied_trials.u64",s.supplied_trials);write(root,"supplied_offsets.u64",s.supplied_offsets);
    constexpr const char* modes[]{"fixed","local","crf"};
    for(std::size_t mode=0;mode<3;++mode){const auto target=root/modes[mode];check(std::filesystem::create_directory(target),"new decoder evidence directory");const auto& trace=s.beats[mode];
        write(target,"outcomes.u8",trace.outcomes);write(target,"beats.f32",trace.times);write(target,"positions.u64",trace.positions);write(target,"offsets.u64",trace.offsets);
        write(target,"trials.u64",trace.trials);write(target,"trial_offsets.u64",trace.trial_offsets);write(target,"scores.f32",trace.scores);
    }
    std::ofstream meta(root/"capture.json");meta<<"{\"version\":1,\"graph_id\":\"madmom-superflux-reference\",\"sample_rate\":48000,\"hop\":240,\"frame\":2048,\"analysis_rate\":200,\"stride\":40,\"history\":2000,\"frames\":"<<s.frames<<",\"samples\":"<<pcm.size()<<",\"bands\":"<<s.bands<<",\"snapshots\":"<<s.ends.size()<<",\"byte_order\":\"little\",\"beat_nodes\":[80,81,4],\"provider\":\""<<(provider?"madmom-test-real-forward":"builtin")<<"\"}";
    check(bool(meta),"write live graph capture identity");export_graph(root,graph);
}
void packet_reset_clone_case(const char* directory) {
    const auto pcm=signal(3*48000);const auto count=(pcm.size()-2048)/240+1;
    Execution small,large;Observation a(count),b(count),reset(count),repeat(count);
    small.feed(pcm,97,a);large.feed(pcm,8192,b);equal(a,b);complete(a,pcm.size());
    small.reset();small.feed(pcm,511,reset);equal(a,reset);
    small.reset();small.feed(pcm,2048,repeat);equal(a,repeat);
    Execution prefix;Observation original(count);constexpr std::size_t cut=48017;
    prefix.feed(std::span(pcm).first(cut),97,original);Execution cloned(prefix);auto fork=original;fork.reserve(count);
    prefix.feed(std::span(pcm).subspan(cut),511,original);cloned.feed(std::span(pcm).subspan(cut),8192,fork);
    equal(a,original);equal(original,fork);export_values(directory,"combined",pcm,a,small.g);
}
void rollover_silence_case(const char* directory) {
    const auto pcm=signal(514761);Observation seen((pcm.size()-2048)/240+1);Execution execution;
    execution.feed(pcm,8192,seen);complete(seen,pcm.size());check(seen.ends.back()>history,"live context actually rolls over");
    export_values(directory,"rollover",pcm,seen,execution.g);
    const auto zero=signal(48000,true);Observation silent((zero.size()-2048)/240+1);Execution no_result;
    no_result.feed(zero,97,silent);complete(silent,zero.size());
    check(std::all_of(silent.onset.begin(),silent.onset.end(),[](float x){return x==0;}),"silence has zero ODF");
    check(silent.beats[2].times.empty() && silent.beats[2].trials.empty() && silent.supplied_trials.empty(),"CRF silence publishes successful empty results without stale trials");
    export_values(directory,"silence",zero,silent,no_result.g);
}
void provider_case(const char* directory) {
    const auto pcm=signal(48000);const auto count=(pcm.size()-2048)/240+1;
    const auto calls=provider_calls,clones=provider_clones;Execution execution(true),fork(execution);Observation a(count),b(count);
    check(provider_clones>clones,"cold runtime context copy clones named provider");
    execution.feed(pcm,97,a);fork.feed(pcm,8192,b);equal(a,b);complete(a,pcm.size());
    check(provider_calls-calls==2*count,"named provider executes every Fourier frame, including cloned context");
    export_values(directory,"provider",pcm,a,execution.g,true);
    auto g=graph(true);std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,profile(),plan,state)!=Status::ok && !plan && !state,"missing named provider rejects before publication");
    auto wrong=profile();wrong.fourier_backend=std::make_shared<ProviderFactory>("madmom-test-wrong-id");
    check(PreparedGraph::prepare_numeric(g,wrong,plan,state)!=Status::ok && !plan && !state,"mismatched provider cannot silently fall back");
}
NumericProcessingGraph coordinate_graph() {
    auto g=graph();auto decoder=node(g,80);auto c=std::get<NumericContract>(decoder.inputs[0].contract.view());
    c.semantic=c.history_element=Semantic::Numeric;c.shape.axes[0]=Axis::Time;
    c.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,200}};
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";
    source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{48000}}}}};source.outputs={{0,PortDirection::Output,own(c)}};
    ConfiguredProcessingNode interval;interval.id=2;interval.kind="NetworkInput";
    interval.parameters=source.parameters;interval.outputs={{0,PortDirection::Output,decoder.inputs[1].contract}};
    decoder.inputs[0].contract=source.outputs[0].contract;g.nodes={source,interval,decoder};g.edges={{1,80,0,0},{2,80,0,1}};
    g.graph_id="madmom-explicit-decoder-coordinates";return g;
}
struct Coordinates {
    std::vector<float> values;std::vector<std::uint64_t> positions;std::size_t calls=0,errors=0;
    Coordinates(){values.reserve(history+1);positions.reserve(history+1);}
    static void output(void* context,const NumericPortFrameView& event) {
        if(event.identity.node!=80)return;auto& self=*static_cast<Coordinates*>(context);
        if(event.status==Status::unavailable && !event.frame.stamp.available)return;
        if(event.status!=Status::ok || !event.frame.stamp.available){++self.errors;return;}
        const auto& value=std::get<NumericView>(event.frame.payload);
        if(event.identity.port==beat_ports::times)replace(value,self.values);else replace(value,self.positions);++self.calls;
    }
};
Coordinates coordinate_case(double origin,double step,bool nonuniform,bool accepted) {
    const auto g=coordinate_graph();const auto c=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,profile(),plan,state)==Status::ok,"prepare actual decoder coordinate regression");
    std::array<float,2000> values{};std::array<double,121> times{};
    for(std::size_t i=0;i<times.size();++i){values[i]=i%40==10?1.f:.01f;times[i]=origin+double(i)*step;}
    if(nonuniform)times[60]+=.001;
    const PlaneLayout plane{0,{sizeof(float),0}};FrameStamp stamp;stamp.sequence=stamp.source_sequence=stamp.producer_epoch=1;
    auto input=numeric_test_builders::frame(stamp,c,std::as_bytes(std::span(values)),std::span(&plane,1),{times.size(),1});
    std::get<NumericView>(input.payload).coordinates.axes[0]=std::as_bytes(std::span(times));
    const auto interval_contract=std::get<NumericContract>(g.nodes[1].outputs[0].contract.view());
    const std::array<float,1> interval{100};
    const auto supplied=numeric_test_builders::frame(stamp,interval_contract,std::as_bytes(std::span(interval)),std::span(&plane,1),{1,1});
    Coordinates seen;const auto before=allocations;
    const auto supplied_status=plan->process_numeric_ports(2,0,supplied,*state,{},NumericRuntimeCallbacks{Coordinates::output,nullptr,nullptr,&seen});
    check((supplied_status==Status::ok || supplied_status==Status::would_block) && !seen.calls,"retain supplied decoder interval before activations");
    const auto result=plan->process_numeric_ports(1,0,input,*state,{},NumericRuntimeCallbacks{Coordinates::output,nullptr,nullptr,&seen});
    check(allocations==before,"coordinate processing allocates nothing");
    if(accepted)check(result==Status::ok && seen.calls==2 && !seen.errors,"uniform grid with arbitrary origin accepted");
    else check(result==Status::incompatible_contract && !seen.calls && seen.errors,"wrong or nonuniform grid rejected by metadata contract");return seen;
}
void coordinate_regression() {
    const auto zero=coordinate_case(0,1./200,false,true);
    for(const auto origin:{17.,-17.}){const auto shifted=coordinate_case(origin,1./200,false,true);check(zero.values==shifted.values && zero.positions==shifted.positions,"origin invariant explicit beat decoding");}
    coordinate_case(17,1./180,false,false);coordinate_case(-17,1./200,true,false);
}
}
void* operator new(std::size_t bytes) {++allocations;if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t bytes,std::align_val_t alignment) {
    ++allocations;if(auto* p=test_allocation::allocate_aligned(bytes,std::size_t(alignment)))return p;throw std::bad_alloc();
}
void operator delete(void* p,std::align_val_t) noexcept {test_allocation::deallocate_aligned(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept {test_allocation::deallocate_aligned(p);}
void* operator new[](std::size_t bytes,std::align_val_t alignment) {return ::operator new(bytes,alignment);}
void operator delete[](void* p,std::align_val_t alignment) noexcept {::operator delete(p,alignment);}
void operator delete[](void* p,std::size_t,std::align_val_t alignment) noexcept {::operator delete(p,alignment);}
int main(int argc,char** argv) {
    check(std::endian::native==std::endian::little,"evidence declares little endian");
    const auto directory=argc>1?argv[1]:nullptr;
    coordinate_regression();packet_reset_clone_case(directory);rollover_silence_case(directory);provider_case(directory);
    if(directory){
        const auto path=std::filesystem::path(directory)/"regression.json";check(!std::filesystem::exists(path),"preserve prior regression record");
        std::ofstream report(path);report<<"{\"version\":1,\"cases\":[\"combined\",\"rollover\",\"silence\",\"provider\"],\"packet_reset_clone\":true,\"no_hot_cpp_allocation\":true,\"provider_clone\":true,\"coordinate_validation\":true}";
        check(bool(report),"write completed runtime regression record");
    }
    std::puts("madmom combined graph packet/reset/clone/provider/noalloc regression PASS");
}
