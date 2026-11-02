#include <domain/graph/node_factory.h>
#include <dataflow_runtime/runtime.h>
#include <dsp_kernels/correlation.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <limits>

namespace {
using namespace domain;
using namespace signal_contracts;
using namespace dataflow_runtime;
constexpr double tau=6.2831853071795864769;
std::size_t allocations=0;
void check(bool value,const char* reason) {
    if(!value){std::fprintf(stderr,"FAIL %s\n",reason);std::exit(1);}
}
NumericRuntimeProfile profile() {
    constexpr FrameLimits frames{1,131072,524288,65536,256,65536};
    return {{32,64,8,16,256,131072,frames},
        {{4194304,4194304,4194304,131072},0,16777216},
        {131072,1,524288,524288,524288},{true,true,true}};
}
ConfiguredProcessingNode scalar_source() {
    ConfiguredProcessingNode node;
    check(bool(make_processing_node("NetworkInput",1,profile().admission,node,{32768,512,1,400})),"create isolated scalar source");
    auto value=std::get<NumericContract>(node.outputs[0].contract.view());
    value.semantic=Semantic::Scalar;value.shape={Rank::Scalar,{Axis::None,Axis::None},{ExtentBounds{1,1},ExtentBounds{1,1}}};
    OwnedProcessingContract owned;check(own_processing_contract(value,profile().admission.frame,16777216,owned)==Status::ok,"own scalar source");
    node.outputs={{0,PortDirection::Output,std::move(owned)}};return node;
}
void append_stage(NumericProcessingGraph& graph,std::string_view kind,NodeId id,NodeId from,ProcessingParameters parameters,bool paired=false) {
    ConfiguredProcessingNode node;check(bool(make_processing_node(kind,id,profile().admission,node)),"create isolated temporal stage");
    node.parameters=std::move(parameters);node.enabled=true;
    const auto source=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& value){return value.id==from;});
    check(source!=graph.nodes.end(),"isolated temporal source exists");
    if(paired)node.inputs[1].contract=source->outputs[1].contract;
    check(bool(configure_processing_input(node,source->outputs[0].contract.view(),profile().admission)),"derive isolated temporal stage");
    graph.nodes.push_back(std::move(node));graph.edges.push_back({from,id});
    if(paired)graph.edges.push_back({from,id,1,1});
}

NumericProcessingGraph branch(std::size_t size) {
    NumericProcessingGraph graph;graph.graph_id="scalar-tempogram";graph.execution_interval={1,60};
    graph.nodes.push_back(scalar_source());const NodeId first=size==512?1200:1210;
    append_stage(graph,"Spectrogram",first,1,HistoryProcessingParameters{size});
    append_stage(graph,"MatrixTranspose",first+1,first,TransformProcessingParameters{});
    append_stage(graph,"MatrixCenter",first+2,first+1,TransformProcessingParameters{{{"method",NumericLiteral{std::uint64_t{0}}}}});
    append_stage(graph,"MatrixWindowFunction",first+3,first+2,TransformProcessingParameters{{{"function",std::string{"hann"}}}});
    append_stage(graph,"MatrixFFT",first+4,first+3,TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t(size)}},{"sample_rate",NumericLiteral{std::uint64_t{60}}}}});
    append_stage(graph,"ComplexPolar",first+50,first+4,TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t(size)}},{"first_bin",NumericLiteral{std::uint64_t{0}}}}},true);
    append_stage(graph,"MatrixRow",first+5,first+50,TransformProcessingParameters{{{"row",NumericLiteral{std::uint64_t{0}}}}});
    append_stage(graph,"MatrixAutocorrelation",first+7,first+2,TransformProcessingParameters{{{"max_lag",NumericLiteral{std::uint64_t{120}}},{"frame_rate_fps",NumericLiteral{std::uint64_t{60}}}}});
    append_stage(graph,"MatrixRow",first+8,first+7,TransformProcessingParameters{{{"row",NumericLiteral{std::uint64_t{0}}}}});
    return graph;
}
float signal(std::size_t index,std::size_t size) {
    const auto primary=std::size_t(std::llround(double(size)/30));
    double value=.5+.2*std::cos(tau*double(primary)*double(index)/double(size));
    for(std::size_t tone=0;tone<4;++tone)
        value+=.04*std::cos(tau*double(primary+7+tone*11)*double(index)/double(size)+double(tone));
    return float(value);
}
double coordinate(std::span<const std::byte> bytes,std::size_t index) {
    std::uint64_t bits=0;
    for(std::size_t byte=0;byte<8;++byte)
        bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[index*8+byte]))<<(byte*8);
    double result=0;std::memcpy(&result,&bits,8);return result;
}
double coordinate(const NumericView& view,std::size_t axis,std::size_t index) {
    const auto& c=view.contract->coordinates[axis];
    if(c.mode==CoordinateMode::ExplicitFloat64LE)return coordinate(view.coordinates.axes[axis],index);
    check(c.mode==CoordinateMode::Regular && c.unit==NumericUnit::Hertz && c.origin.denominator && c.step.denominator,"declared Fourier frequency grid");
    return double(c.origin.numerator)/double(c.origin.denominator)+double(index)*double(c.step.numerator)/double(c.step.denominator);
}
struct Capture {
    NodeId first=0;std::size_t size=0;std::uint64_t filled=0;
    std::array<float,2049> spectrum{};
    std::array<float,2049> phase{};
    std::array<float,121> acf{};
    static void output(void* user,const NumericPortFrameView& event) {
        auto& self=*static_cast<Capture*>(user);
        if(event.identity.node==self.first && event.identity.port==1){
            check(event.status==Status::ok,"history count available");
            check(load_element(std::get<NumericView>(event.frame.payload),{0,0,0},self.filled)==Status::ok,"read filled count");
        }
        if(event.identity.node==self.first+5)self.row(event);
        if(event.identity.node==self.first+50 && event.identity.port==1)self.phases(event);
        if(event.identity.node==self.first+8)self.correlation(event);
    }
    void phases(const NumericPortFrameView& event) {
        check(event.status==Status::ok && event.frame.stamp.available,"Fourier phase available with magnitude");
        const auto& view=std::get<NumericView>(event.frame.payload);
        check(view.actual==std::array{std::size_t{1},size/2+1},"phase has identical bin geometry including Nyquist");
        check(view.contract->quantity==Quantity::SpectrumPhase && view.contract->unit==NumericUnit::Radians,"phase has explicit quantity and radians");
        for(std::size_t bin=0;bin<=size/2;++bin){
            check(load_element(view,{0,0,bin},phase[bin])==Status::ok && std::isfinite(phase[bin]) && std::abs(phase[bin])<=3.141593f,"finite principal phase");
            check(std::abs(coordinate(view,1,bin)-double(bin)*60/double(size))<1e-12,"phase frequency axis matches magnitude");
        }
    }
    void row(const NumericPortFrameView& event) {
        check(event.status==Status::ok && event.frame.stamp.available,"FFT row available");
        const auto& view=std::get<NumericView>(event.frame.payload);
        check(view.actual==std::array{size/2+1,std::size_t{1}},"FFT row has N/2+1 bins including DC and Nyquist");
        check(view.contract->coordinates[0].unit==NumericUnit::Hertz,"frequency coordinate is Hz");
        for(std::size_t bin=0;bin<=size/2;++bin){
            check(load_element(view,{0,bin,0},spectrum[bin])==Status::ok,"read FFT bin");
            check(std::abs(coordinate(view,0,bin)-double(bin)*60/double(size))<1e-12,"explicit Fourier sample_rate determines Hz independently of audio Fs");
        }
    }
    void correlation(const NumericPortFrameView& event) {
        check(event.status==Status::ok && event.frame.stamp.available,"ACF row available");
        const auto& view=std::get<NumericView>(event.frame.payload);
        check(view.actual==std::array{std::size_t{121},std::size_t{1}},"explicit maximum lag controls row length");
        const auto& axis=view.contract->coordinates[0];
        check(axis.mode==CoordinateMode::Regular && axis.unit==NumericUnit::Seconds && axis.origin.numerator==0 &&
            axis.step.numerator==1 && axis.step.denominator==60,"lag seconds derive from explicit frame_rate_fps");
        for(std::size_t lag=0;lag<acf.size();++lag)
            check(load_element(view,{0,lag,0},acf[lag])==Status::ok && std::isfinite(acf[lag]) && std::abs(acf[lag])<=1,"finite signed coefficient");
    }
};
struct Running {
    NumericProcessingGraph graph;
    std::shared_ptr<const PreparedGraph> plan;
    std::unique_ptr<RuntimeContext> state;
    Capture capture;
    explicit Running(std::size_t size):graph(branch(size)),capture{size==512?1200:1210,size} {
        check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"prepare shipped scalar tempogram branch");
    }
    void push(std::size_t index,bool silence=false) {
        const auto contract=std::get<NumericContract>(graph.nodes[0].outputs[0].contract.view());
        const float sample=silence?0:signal(index,capture.size);
        check(sample>=0 && sample<=1,"five tone source stays normalized");
        const std::array<PlaneLayout,1> planes{{{0,{4,0}}}};
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=index+1;stamp.producer_epoch=3;
        stamp.timestamp_ticks=std::int64_t(std::llround(double(index)*32768/60));
        const DataFrameView frame{stamp,NumericView{&contract,std::as_bytes(std::span{&sample,1}),planes,{1,1}}};
        const auto before=allocations;
        check(plan->process_numeric_ports(1,frame,*state,{}, {Capture::output,nullptr,nullptr,&capture})==Status::ok,"scalar history/center/window/Fourier executes");
        check(allocations==before,"tempogram processing performs no allocations");
    }
};
double expected_sample(std::size_t position,std::size_t count,std::size_t size) {
    return count<size && position<size-count?0:signal(position+count-size,size);
}
std::array<double,2> direct_bin(std::size_t bin,std::size_t count,std::size_t size) {
    double mean=0;
    for(std::size_t i=0;i<size;++i)mean+=expected_sample(i,count,size);
    mean/=double(size);double real=0,imaginary=0;
    for(std::size_t i=0;i<size;++i){
        const auto centered=float(expected_sample(i,count,size)-mean);
        const auto hann=float(.5-.5*std::cos(tau*double(i)/double(size-1)));
        const auto value=double(centered*hann);
        const auto phase=tau*double(bin)*double(i)/double(size);
        real+=value*std::cos(phase);imaginary-=value*std::sin(phase);
    }
    return {real/double(size),imaginary/double(size)};
}
void verify(Running& run,std::size_t count) {
    const auto size=run.capture.size;
    check(run.capture.filled==std::min(count,size),"filled count distinguishes padding from real history");
    std::array<float,4096> centered{};double mean=0;
    for(std::size_t i=0;i<size;++i)mean+=expected_sample(i,count,size);
    for(std::size_t i=0;i<size;++i)centered[i]=float(expected_sample(i,count,size)-mean/double(size));
    std::array<float,121> reference{};
    check(dsp_kernels::linear_autocorrelation(std::span(centered).first(size),reference),"shared portable correlation on independently reconstructed chronological history");
    for(std::size_t lag=0;lag<reference.size();++lag)
        check(std::abs(reference[lag]-run.capture.acf[lag])<2e-6,"shipped ACF uses centered time history without Fourier Hann or circular wrap");
    const auto primary=std::size_t(std::llround(double(size)/30));
    for(const auto bin:std::array<std::size_t,8>{0,1,primary-1,primary,primary+1,primary+7,primary+18,size/2-1}){
        const auto expected=direct_bin(bin,count,size);
        // Divide both DFT planes by N only for the existing numerical error metric.
        const auto magnitude=double(run.capture.spectrum[bin])/double(size),phase=double(run.capture.phase[bin]);
        check(std::abs(magnitude-std::hypot(expected[0],expected[1]))<2e-6,"magnitude agrees with independent DFT through partial history and wrap");
        check(std::abs(magnitude*std::cos(phase)-expected[0])<2e-6 && std::abs(magnitude*std::sin(phase)-expected[1])<2e-6,"phase preserves complex DFT including rolling-window origin");
    }
    if(count<size)return;
    const auto begin=run.capture.spectrum.begin();
    const auto peak=std::max_element(begin+1,begin+std::ptrdiff_t(size/2));
    check(std::size_t(peak-begin)==primary,"dominant normalized modulation tone has the expected bin");
}
void numerical_case(std::size_t size) {
    Running run(size);
    for(std::size_t index=0;index<size+19;++index){
        run.push(index);
        if(index==0 || index+1==size/2 || index+1==size || index+1==size+19)verify(run,index+1);
    }
    for(std::size_t index=size+19;index<2*size+19;++index)run.push(index,true);
    check(run.capture.filled==size,"silence keeps compatible history full");
    for(std::size_t bin=0;bin<=size/2;++bin)check(run.capture.spectrum[bin]==0,"old tones leave no stale FFT after a full silent history");
    for(std::size_t bin=0;bin<=size/2;++bin)check(run.capture.phase[bin]==0,"undefined silent-bin phase is exactly zero");
    for(const auto value:run.capture.acf)check(value==0,"full silence has zero correlation support, not unit confidence");
}
void correlation_admission() {
    const auto definition=branch(4096);
    const auto input=std::get<NumericContract>(definition.nodes.back().inputs[0].contract.view());
    OwnedProcessingContract output;
    check(matrix_autocorrelation_output_contract(input,120,0,output)!=Status::ok,"zero frame rate rejected");
    check(matrix_autocorrelation_output_contract(input,4096,60,output)!=Status::ok,"excess lag rejected");
    auto wrong=input;wrong.dtype=DType::Float64;
    check(matrix_autocorrelation_output_contract(wrong,120,60,output)!=Status::ok,"wrong primitive dtype rejected");
    auto graph=definition;const auto correlation=graph.nodes[graph.nodes.size()-2];
    graph.nodes={definition.nodes[0],correlation};graph.nodes[0].outputs[0].contract=correlation.inputs[0].contract;
    graph.edges={{1,correlation.id}};
    auto constrained=profile();constrained.kernels.max_elements=2048;
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph,constrained,plan,state)!=Status::ok,"ACF row size must fit declared execution profile");
}
void phase_kernel_failures() {
    dsp_kernels::MagnitudeSpectrum fft;check(fft.prepare(512),"prepare existing Fourier for atomic phase failures");
    std::array<float,512> input{};std::array<float,256> magnitude{},phase{};
    for(std::size_t i=0;i<input.size();++i)input[i]=signal(i,input.size());
    check(fft.process(input,magnitude,phase),"five changing tones produce both output planes");
    const auto saved_magnitude=magnitude,saved_phase=phase;
    input[27]=std::numeric_limits<float>::quiet_NaN();
    check(!fft.process(input,magnitude,phase) && magnitude==saved_magnitude && phase==saved_phase,"invalid input preserves both outputs");
    input[27]=0;
    check(!fft.process(input,magnitude,std::span(phase).first(255)) && magnitude==saved_magnitude && phase==saved_phase,"invalid phase geometry preserves both outputs");
    check(!fft.process(input,magnitude,magnitude) && magnitude==saved_magnitude,"aliased output planes rejected before publication");
}
void phase_contract_admission() {
    auto node=branch(512).nodes[5];ResolvedProcessingNode resolved;
    check(node.kind=="MatrixFFT" && node.outputs.size()==2,"shipped Fourier declares raw real and imaginary ports");
    check(bool(resolve_processing_node(node,profile().admission,resolved)),"both Fourier outputs admitted");
    const auto saved=node.outputs[1];node.outputs.pop_back();
    check(!resolve_processing_node(node,profile().admission,resolved),"missing imaginary contract rejected atomically");
    node.outputs.push_back(saved);
    auto wrong=std::get<NumericContract>(node.outputs[1].contract.view());wrong.unit=NumericUnit::Radians;
    check(own_processing_contract(wrong,profile().admission.frame,131072,node.outputs[1].contract)==Status::ok,"build malformed phase fixture");
    check(!resolve_processing_node(node,profile().admission,resolved),"raw imaginary values cannot masquerade as radians");
}
}
void* operator new(std::size_t bytes){++allocations;auto* data=std::malloc(bytes?bytes:1);if(!data)std::abort();return data;}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* data) noexcept{std::free(data);}
void operator delete[](void* data) noexcept{std::free(data);}
void operator delete(void* data,std::size_t) noexcept{std::free(data);}
void operator delete[](void* data,std::size_t) noexcept{std::free(data);}
int main(){phase_contract_admission();phase_kernel_failures();correlation_admission();numerical_case(512);numerical_case(4096);std::puts("shipped scalar history/center/Hann/FFT magnitude+phase/linear-ACF 512/4096 DFT/Hz/lag/wrap/silence/admission/no-allocation PASS");}
