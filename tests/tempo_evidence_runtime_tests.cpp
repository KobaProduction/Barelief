#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <dsp_kernels/dsp.h>
#include <dsp_kernels/correlation.h>
#include <dsp_kernels/tempo_evidence.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
using namespace domain;
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace r=dsp_kernels::rhythm;
std::size_t allocations=0;
void check(bool value,const char* why){if(!value){std::fprintf(stderr,"FAIL tempo runtime: %s\n",why);std::exit(1);}}
NumericRuntimeProfile profile() {
    constexpr FrameLimits frames{1,131072,524288,65536,256,65536};
    NumericRuntimeProfile result{{32,64,8,16,256,131072,frames},{{4194304,4194304,4194304,131072},0,16777216},
        {131072,1,524288,524288,524288},{true,true,true}};
    result.music_enabled=true;return result;
}
NumericProcessingGraph graph(bool enabled=true) {
    ConfiguredProcessingNode tempo;
    check(bool(make_processing_node("TempoEvidence",7,profile().admission,tempo)),"create six-input evidence block");
    tempo.enabled=enabled;NumericProcessingGraph g;g.graph_id="explicit-tempo-evidence";g.execution_interval={1,60};
    for(PortId port=0;port<tempo_evidence_inputs;++port){
        ConfiguredProcessingNode input;input.id=port+1;input.kind="NetworkInput";input.parameters=EndpointProcessingParameters{};
        input.outputs={{0,PortDirection::Output,tempo.inputs[port].contract}};g.nodes.push_back(std::move(input));
        g.edges.push_back({NodeId(port+1),7,0,port});
    }
    g.nodes.push_back(std::move(tempo));return g;
}
struct Inputs {
    std::array<std::vector<float>,4> curves;
    std::array<std::uint64_t,2> filled{512,4096};
    std::size_t short_lags=121,long_lags=121;
    Inputs(){generate(512,0);generate(4096,1);}
    void generate(std::size_t size,std::size_t branch) {
        constexpr double tau=6.2831853071795864769;
        std::vector<float> centered(size),windowed(size);double sum=0;
        for(std::size_t sample=0;sample<size;++sample){
            double value=.5+.3*std::cos(tau*2*double(sample)/60);
            for(unsigned tone=0;tone<4;++tone)value+=.02*std::cos(tau*(4.3+2.1*tone)*double(sample)/60+.17*tone);
            centered[sample]=float(value);sum+=centered[sample];
        }
        for(std::size_t sample=0;sample<size;++sample){centered[sample]=float(centered[sample]-sum/double(size));windowed[sample]=centered[sample]*float(.5-.5*std::cos(tau*double(sample)/double(size-1)));}
        curves[branch].resize(size/2);curves[branch+2].resize(121);
        dsp_kernels::MagnitudeSpectrum fft;check(fft.prepare(size) && fft.process(windowed,curves[branch]),"existing Fourier kernel on five changing tones");
        check(dsp_kernels::linear_autocorrelation(centered,curves[branch+2]),"existing linear ACF kernel");
    }
    r::TempoEvidenceOutput expected() const {
        r::PreparedTempoEvidence kernel;check(kernel.prepare() && kernel.process({curves[0],std::span(curves[2]).first(short_lags),std::size_t(filled[0])},
            {curves[1],std::span(curves[3]).first(long_lags),std::size_t(filled[1])}),"shared kernel reference");return kernel.output();
    }
};
struct Capture {
    std::array<std::array<float,tempo_candidate_columns>,8> matrix{};
    std::uint32_t count=0;float maturity=0;std::uint8_t ambiguity=0;
    unsigned publications=0,unavailable=0;
    FrameStamp stamp{};
    static void receive(void* user,const NumericPortFrameView& event) {
        if(event.identity.node!=7)return;
        auto& self=*static_cast<Capture*>(user);
        if(!event.frame.stamp.available){++self.unavailable;return;}
        const auto& view=std::get<NumericView>(event.frame.payload);
        switch(event.identity.port){
        case 0:self.read_matrix(view);break;
        case 1:check(load_element(view,{},self.count)==Status::ok,"count output");break;
        case 2:check(load_element(view,{},self.maturity)==Status::ok,"maturity output");break;
        case 3:check(load_element(view,{},self.ambiguity)==Status::ok,"ambiguity output");++self.publications;self.stamp=event.frame.stamp;self.stamp.stream_id={};break;
        }
    }
    void read_matrix(const NumericView& view) {
        check(view.contract->dtype==DType::Float32 && view.actual==std::array{std::size_t{8},tempo_candidate_columns},"explicit candidate matrix");
        for(std::size_t row=0;row<matrix.size();++row)for(std::size_t column=0;column<matrix[row].size();++column)
            check(load_element(view,{0,row,column},matrix[row][column])==Status::ok,"candidate field");
    }
    void verify(const r::TempoEvidenceOutput& expected) const {
        check(count==expected.count && maturity==expected.maturity && ambiguity==unsigned(expected.harmonic_ambiguity),"same count/maturity/ambiguity as portable kernel");
        for(std::size_t row=0;row<matrix.size();++row){
            const auto c=row<count?expected.candidates[row]:r::TempoEvidenceCandidate{};
            const std::array<float,tempo_candidate_columns> wanted{c.bpm,c.score,c.fourier_short,c.fourier_long,c.acf_short,c.acf_long,float(c.origin_curves)};
            check(matrix[row]==wanted,"bit-exact candidate fields and zero padding");
        }
    }
};
struct Running {
    NumericProcessingGraph definition;
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    Inputs inputs;Capture capture;
    explicit Running(NumericProcessingGraph g=graph()):definition(std::move(g)) {
        const auto status=PreparedGraph::prepare_numeric(definition,profile(),plan,state);
        if(status!=Status::ok){
            ResolvedProcessingNode resolved;const auto diagnostic=resolve_processing_node(definition.nodes.back(),profile().admission,resolved);
            TempoEvidenceParameters parameters;const bool settings=tempo_evidence_parameters(definition.nodes.back(),parameters);
            std::fprintf(stderr,"prepare status=%u node error=%u port=%u settings=%u inputs=%u\n",unsigned(status),unsigned(diagnostic.error),unsigned(diagnostic.port),unsigned(settings),unsigned(settings && tempo_evidence_inputs_valid(definition.nodes.back(),parameters)));
            NumericProcessingGraph evaluated,lowered;const auto bound=resolve_numeric_parameter_bindings(definition,profile().admission,evaluated,lowered);
            std::fprintf(stderr,"bindings node=%d error=%u port=%u\n",bound.node,unsigned(bound.error),unsigned(bound.port));
            const auto p=profile();std::array<std::size_t,32> work{};ProcessingGraphAdmission admission;
            const auto admitted=admit_processing_graph(definition,p.admission,PreparedGraph::numeric_capabilities(p),work,admission);
            std::fprintf(stderr,"admission node=%d error=%u port=%u\n",admitted.node,unsigned(admitted.error),unsigned(admitted.port));
        }
        check(status==Status::ok,"prepare explicit evidence graph");
    }
    Status feed(PortId port,std::uint64_t sequence,bool available=true,std::uint64_t generation=0,bool discontinuity=false) {
        const auto contract=std::get<NumericContract>(definition.nodes[port].outputs[0].contract.view());
        const auto bytes=port<4?std::as_bytes(std::span(inputs.curves[port])):std::as_bytes(std::span(&inputs.filled[port-4],1));
        const auto size=port==2?inputs.short_lags:port==3?inputs.long_lags:port<4?inputs.curves[port].size():1;
        const std::array<PlaneLayout,1> planes{{{0,{port<4?4u:8u,0}}}};
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=sequence;stamp.timestamp_ticks=std::int64_t(sequence*800);
        stamp.producer_epoch=3;stamp.generation=generation;stamp.available=available;stamp.discontinuity=discontinuity;
        const DataFrameView frame{stamp,NumericView{&contract,available?bytes:std::span<const std::byte>{},planes,{size,1}}};
        const auto before=allocations;
        const auto result=plan->process_numeric_ports(NodeId(port+1),frame,*state,{}, {Capture::receive,nullptr,nullptr,&capture});
        check(allocations==before,"six-input retain/compute/publish performs no allocations");return result;
    }
    void frame(std::uint64_t sequence,std::uint64_t generation=0,bool discontinuity=false) {
        for(PortId port=0;port<tempo_evidence_inputs;++port){
            const auto status=feed(port,sequence,true,generation,discontinuity);
            if(status!=Status::ok)std::fprintf(stderr,"frame sequence=%llu generation=%llu port=%u status=%u\n",static_cast<unsigned long long>(sequence),static_cast<unsigned long long>(generation),unsigned(port),unsigned(status));
            check(status==Status::ok,"complete frame input accepted");
        }
    }
};
void borrowed_inputs_and_order() {
    Running run;const auto expected=run.inputs.expected();check(expected.count>0,"reference has hypotheses");
    const std::array<PortId,6> order{5,2,0,4,3,1};
    for(std::size_t position=0;position<order.size();++position){
        const auto port=order[position];check(run.feed(port,1)==Status::ok,"arbitrary input order");
        if(port<4)std::fill(run.inputs.curves[port].begin(),run.inputs.curves[port].end(),0.f);else run.inputs.filled[port-4]=0;
        check(run.capture.publications==unsigned(position+1==order.size()),"publish once, only after six matching inputs");
    }
    run.capture.verify(expected);
}
void freshness_gap_and_context_copy() {
    Running run;
    for(PortId port=0;port<3;++port)check(run.feed(port,1)==Status::ok,"partial interval");
    RuntimeContext copied=*run.state;run.frame(2);
    check(run.capture.publications==1,"new interval discards incomplete older inputs");run.capture.verify(run.inputs.expected());
    run.state=std::make_unique<RuntimeContext>(copied);run.capture={};
    for(PortId port=3;port<6;++port)check(run.feed(port,1)==Status::ok,"copied context retains its own unfinished join");
    check(run.capture.publications==1,"context copy owns independent buffers");run.capture.verify(run.inputs.expected());
    check(run.feed(0,2,false)==Status::ok && run.capture.unavailable==4,"unavailable input invalidates all outputs");
    run.frame(3,1,true);check(run.capture.publications==2 && run.capture.stamp.discontinuity,"restart joins marked frames without repeatedly clearing sibling inputs");
}
void disabled_and_missing() {
    Running disabled(graph(false));disabled.frame(1);
    check(disabled.capture.publications==1 && disabled.capture.count==0 && disabled.capture.maturity==0,"disabled block publishes typed zero outputs");
    auto g=graph();g.edges.erase(g.edges.begin());Running missing(std::move(g));
    for(PortId port=1;port<6;++port)check(missing.feed(port,1)==Status::ok,"known missing Fourier input is zero");
    std::fill(missing.inputs.curves[0].begin(),missing.inputs.curves[0].end(),0.f);
    check(missing.capture.publications==1,"known disconnected lane does not stall connected histories");missing.capture.verify(missing.inputs.expected());
}
void stable_port_ids() {
    auto g=graph();auto& ports=g.nodes.back().inputs;
    std::rotate(ports.begin(),ports.begin()+2,ports.end());
    Running reordered(std::move(g));reordered.frame(1);
    check(reordered.capture.publications==1,"input storage order does not change stable port identities");
    reordered.capture.verify(reordered.inputs.expected());
}
void variable_acf_extent() {
    auto g=graph();
    auto c=std::get<NumericContract>(g.nodes.back().inputs[2].contract.view());c.shape.extents[0].minimum=0;
    check(own_processing_contract(c,profile().admission.frame,131072,g.nodes.back().inputs[2].contract)==Status::ok,"bound variable ACF");
    g.nodes[2].outputs[0].contract=g.nodes.back().inputs[2].contract;
    Running run(std::move(g));run.inputs.short_lags=80;run.frame(1);
    check(run.capture.publications==1,"variable ACF publishes its actual lag extent");run.capture.verify(run.inputs.expected());
}
void invalid_data_and_admission() {
    Running run;run.inputs.filled[0]=513;
    for(PortId port=0;port<5;++port)check(run.feed(port,1)==Status::ok,"retain invalid occupancy until matching curves arrive");
    check(run.feed(5,1)==Status::invalid_argument && run.capture.publications==0,"impossible occupancy cannot publish stale candidates");
    run.inputs.filled[0]=512;run.frame(2);check(run.capture.publications==1,"valid next frame recovers");
    auto g=graph();auto& port=g.nodes.back().inputs[2];auto bad=std::get<NumericContract>(port.contract.view());bad.coordinates[0].step={1,45};
    check(own_processing_contract(bad,profile().admission.frame,131072,port.contract)==Status::ok,"build wrong ACF cadence");
    ResolvedProcessingNode node;check(!resolve_processing_node(g.nodes.back(),profile().admission,node),"wrong explicit lag cadence rejected");
    auto constrained=profile();constrained.resources.maximum.payload_bytes=64;
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph(),constrained,plan,state)!=Status::ok,"owned join and outputs must fit profile");
    auto numeric_only=profile();numeric_only.music_enabled=false;
    check(PreparedGraph::prepare_numeric(graph(),numeric_only,plan,state)!=Status::ok,"musical interpretation remains opt-in");
}
}
void* operator new(std::size_t bytes){++allocations;auto* p=std::malloc(bytes?bytes:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
int main(){borrowed_inputs_and_order();freshness_gap_and_context_copy();disabled_and_missing();stable_port_ids();variable_acf_extent();invalid_data_and_admission();std::puts("explicit tempo evidence graph/kernel parity/owned join/context copy/freshness/gap/disabled/profile/no-allocation PASS");}
