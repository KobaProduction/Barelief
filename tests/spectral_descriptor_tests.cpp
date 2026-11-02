#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <vector>

namespace {
using namespace signal_contracts;using namespace domain;using namespace dataflow_runtime;
using namespace dsp_kernels;
std::size_t allocations=0;
void check(bool value,const char* why) {if(!value){std::fprintf(stderr,"FAIL spectral descriptor: %s\n",why);std::exit(1);}}
void near(float value,double expected,const char* why) {
    if(std::abs(double(value)-expected)>2e-5*std::max(1.,std::abs(expected))){
        std::fprintf(stderr,"got %.9g, expected %.12g\n",double(value),expected);check(false,why);
    }
}
SpectralDescriptorConfig config(unsigned variant) {
    SpectralDescriptorConfig c;c.method=SpectralDescriptor(variant/2);c.half_wave=variant%2!=0;
    c.weighted=variant%2!=0;c.squared=variant%2!=0;c.magnitude_threshold=.005f;return c;
}
// Independent double oracle uses the direct complex prediction (DAFx03 eq.10),
// rather than the rotated real-axis distance used by the production kernel.
struct Reference {
    std::vector<std::complex<double>> previous,older;
    explicit Reference(std::size_t bins):previous(bins),older(bins){}
    static double phase(std::complex<double> z) {return std::abs(z)>0?std::arg(z):0.;}
    double term(std::complex<double> z,std::size_t i,const SpectralDescriptorConfig& c) const {
        const auto magnitude=std::abs(z),old=std::abs(previous[i]);
        const auto prediction=2*phase(previous[i])-phase(older[i]);
        switch(c.method){
        case SpectralDescriptor::Flux:return c.half_wave?std::max(0.,magnitude-old):std::abs(magnitude-old);
        case SpectralDescriptor::ComplexDifference:return c.half_wave && magnitude<old?0:std::abs(z-std::polar(old,prediction));
        case SpectralDescriptor::PhaseDeviation:return magnitude>c.magnitude_threshold?
            std::abs(std::arg(std::polar(1.,phase(z)-prediction)))*(c.weighted?magnitude:1):0;
        case SpectralDescriptor::HighFrequencyContent:return double(i+1)*(c.squared?std::norm(z):magnitude);
        }
        return 0;
    }
    double process(std::span<const float> input,const SpectralDescriptorConfig& c) {
        double sum=0;
        for(std::size_t i=0;i<previous.size();++i){
            const std::complex<double> z{input[2*i],input[2*i+1]};sum+=term(z,i,c);older[i]=previous[i];previous[i]=z;
        }
        return c.method==SpectralDescriptor::PhaseDeviation?sum/double(previous.size()):sum;
    }
};
void mathematical_reference() {
    for(const auto bins:{1u,257u,2049u})for(unsigned variant=0;variant<8;++variant){
        const auto c=config(variant);PreparedSpectralDescriptor kernel;Reference reference(bins);
        check(kernel.prepare(bins,c),"prepare every method and variant");std::vector<float> input(bins*2);
        for(unsigned frame=0;frame<40;++frame){
            for(std::size_t k=0;k<bins;++k){
                const auto amplitude=frame%9==0?0.f:float((k*17+frame*13)%101)/7.f;
                const auto phase=float(k)*.31f+float(frame)*.79f+float(frame%5)*.04f;
                input[2*k]=amplitude*std::cos(phase);input[2*k+1]=amplitude*std::sin(phase);
            }
            float result=-1;const auto before=allocations;
            check(kernel.process(input,result) && allocations==before,"prepared Float32 descriptor performs no allocations");
            near(result,reference.process(input,c),"double mathematical oracle including silence and phase wraps");
        }
    }
}
void phase_changes() {
    SpectralDescriptorConfig c;c.method=SpectralDescriptor::ComplexDifference;c.half_wave=false;
    PreparedSpectralDescriptor kernel;check(kernel.prepare(1,c),"complex detector");float out=0;
    check(kernel.process(std::array<float,2>{1,0},out),"first frame");near(out,1,"zero-history onset");
    check(kernel.process(std::array<float,2>{0,1},out),"linear phase advance");
    check(kernel.process(std::array<float,2>{-1,0},out),"steady oscillator");near(out,0,"steady phase advance is predicted");
    check(kernel.process(std::array<float,2>{-1,0},out),"phase break without energy increase");near(out,std::sqrt(2.),"complex detector responds to phase-only changes");
    c.method=SpectralDescriptor::Flux;check(kernel.prepare(1,c),"flux detector");
    check(kernel.process(std::array<float,2>{1,0},out) && kernel.process(std::array<float,2>{0,1},out),"same amplitude, different phase");
    near(out,0,"flux ignores phase-only changes");
}
void source_raw_cosine_cancellation() {
    SpectralDescriptorConfig c;c.method=SpectralDescriptor::ComplexDifference;
    c.half_wave=false;c.complex_distance=ComplexDistanceMode::SquaredCosineRaw;
    PreparedSpectralDescriptor kernel;check(kernel.prepare(1,c),"raw source complex distance");
    const std::array<float,1> previous{0x1.391028p+3f},current{0x1.39102ap+3f},phase{0};
    float output=0;check(kernel.process_polar(previous,phase,output),"prime source magnitude");
    const auto before=allocations;
    check(kernel.process_polar(current,phase,output) && allocations==before,"adjacent Float32 magnitudes remain finite without allocations");
    const double expected=double(current[0])-double(previous[0]);
    check(output==float(expected),"double source radicand preserves the nonzero adjacent-magnitude distance");
    check(kernel.process_polar(current,phase,output) && output==0,"unchanged source magnitude has zero distance");
}
void invalid_and_state() {
    PreparedSpectralDescriptor kernel,copy;float out=0;
    check(kernel.prepare(2) && copy.prepare(2),"state preparation");
    const std::array<float,4> a{2,0,0,1},b{3,0,0,3};
    check(kernel.process(a,out),"prime history");const auto before=allocations;
    check(copy.copy_state_from(kernel) && allocations==before,"compatible reprepare copies only prepared history");
    auto bad=b;bad[3]=std::numeric_limits<float>::quiet_NaN();out=123;
    check(!kernel.process(bad,out) && out==123,"NaN leaves history and output unchanged");
    check(!kernel.process(std::span(b).first(3),out) && out==123,"wrong shape is atomic");
    bad.fill(std::numeric_limits<float>::max());check(!kernel.process(bad,out) && out==123,"Float32 overflow is atomic");
    check(!kernel.prepare(0),"invalid preparation preserves old history");
    float copied=0;check(kernel.process(b,out) && copy.process(b,copied) && out==copied,"failed operations do not poison following frame");
    kernel.reset();check(kernel.process(b,out),"explicit reset");near(out,6,"reset restores zero previous magnitudes");
}
NumericRuntimeProfile profile() {
    constexpr FrameLimits frames{1,131072,524288,65536,256,65536};
    NumericRuntimeProfile p{{32,64,8,16,256,131072,frames},{{4194304,4194304,4194304,131072},0,16777216},
        {131072,1,524288,524288,524288},{true,true,true}};p.music_enabled=true;return p;
}
NumericProcessingGraph graph(std::string_view kind,bool enabled) {
    ConfiguredProcessingNode method;check(bool(make_processing_node(kind,2,profile().admission,method)),"factory creates independently named block");
    auto c=std::get<NumericContract>(method.inputs[0].contract.view());c.shape.extents[0]={3,3};
    check(own_processing_contract(c,profile().admission.frame,131072,method.inputs[0].contract)==Status::ok,"declare complex geometry");
    check(bool(derive_processing_node_contracts(method,profile().admission)),"derive scalar Float32 output");method.enabled=enabled;
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};
    source.outputs={{0,PortDirection::Output,method.inputs[0].contract}};
    NumericProcessingGraph g;g.graph_id="spectral-descriptors";g.nodes={source,method};g.edges={{1,2,0,0}};return g;
}
struct Capture {
    float value=0;bool available=false;unsigned received=0;
    static void receive(void* user,const NumericPortFrameView& event) {
        if(event.identity.node!=2 || event.direction!=PortDirection::Output)return;
        auto& self=*static_cast<Capture*>(user);self.available=event.frame.stamp.available;++self.received;
        if(!self.available)return;
        const auto& view=std::get<NumericView>(event.frame.payload);
        check(view.contract->dtype==DType::Float32 && view.contract->shape.rank==Rank::Scalar,"typed Float32 descriptor publication");
        check(load_element(view,{0,0,0},self.value)==Status::ok,"read scalar descriptor");
    }
};
void streaming(std::string_view kind,unsigned variant,bool enabled) {
    const auto g=graph(kind,enabled);std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    check(PreparedGraph::prepare_numeric(g,profile(),plan,context)==Status::ok,"prepare descriptor in production runtime");
    const auto contract=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    const std::array<PlaneLayout,1> planes{{{0,{8,4}}}};Capture capture;
    auto c=config(variant);c.magnitude_threshold=0;Reference reference(3);
    for(unsigned time=0;time<5;++time){
        const std::array<float,6> values{float(time+1),0,0,float(5-time),-2,1};
        FrameStamp stamp;stamp.sequence=time+1;stamp.timestamp_ticks=time*400;stamp.discontinuity=time==3;
        if(stamp.discontinuity)reference=Reference(3);
        const DataFrameView frame{stamp,NumericView{&contract,std::as_bytes(std::span(values)),planes,{3,2}}};
        const auto before=allocations;
        check(plan->process_numeric_ports(1,frame,*context,{}, {Capture::receive,nullptr,nullptr,&capture})==Status::ok,"streaming complex frame");
        check(allocations==before && capture.available,"graph descriptor is available without allocations");
        near(capture.value,enabled?reference.process(values,c):0.,"graph preserves method parameters and discontinuity reset");
    }
    check(capture.received==5,"exactly one output per input, no hidden pairing latency");
    FrameStamp stamp;stamp.sequence=6;stamp.available=false;
    const DataFrameView missing{stamp,NumericView{&contract,{},planes,{3,2}}};
    const auto status=plan->process_numeric_ports(1,missing,*context,{}, {Capture::receive,nullptr,nullptr,&capture});
    check((status==Status::ok || status==Status::unavailable) && !capture.available,"unavailable source cannot publish stale descriptor");
}
void admission() {
    auto g=graph("PhaseDeviation",true);auto p=profile();p.music_enabled=false;
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    check(PreparedGraph::prepare_numeric(g,p,plan,context)!=Status::ok,"music profile gates descriptors");
    p=profile();p.resources.maximum.scratch_bytes=1;
    check(PreparedGraph::prepare_numeric(g,p,plan,context)!=Status::ok,"state storage is charged before processing");
    auto c=std::get<NumericContract>(g.nodes.back().inputs[0].contract.view());c.quantity=Quantity::SpectrumPower;
    check(own_processing_contract(c,profile().admission.frame,131072,g.nodes.back().inputs[0].contract)==Status::ok,"misleading same-size matrix");
    check(!derive_processing_node_contracts(g.nodes.back(),profile().admission),"power matrix cannot masquerade as complex input");
}
}
void* operator new(std::size_t n){++allocations;auto* p=std::malloc(n?n:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
int main() {
    mathematical_reference();phase_changes();source_raw_cosine_cancellation();invalid_and_state();admission();
    constexpr std::array kinds{"SpectralFlux","ComplexSpectralDifference","PhaseDeviation","HighFrequencyContent"};
    constexpr std::array variants{1u,2u,4u,6u};
    for(std::size_t i=0;i<kinds.size();++i){streaming(kinds[i],variants[i],true);streaming(kinds[i],variants[i],false);}
    std::puts("spectral/complex/phase/HFC Float32 kernel and native streaming PASS");
}
