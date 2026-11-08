#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <dsp_kernels/window_math.h>
#include "numeric_test_builders.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <limits>
namespace {
using namespace signal_contracts;using namespace domain;using namespace dataflow_runtime;
constexpr std::size_t count=8;
constexpr FrameLimits bounds{1,32768,262144,16384,256,32768};
std::size_t allocations=0;
void check(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL precise frontend: %s\n",message);std::exit(1);}}
NumericRuntimeProfile profile(){
    NumericRuntimeProfile value{{32,64,8,32,1048576,4*1048576,bounds},{{4*1048576,4*1048576,4*1048576,131072},0,32*1048576},{32768,1,262144,262144,1048576},{true,true,true}};
    value.music_enabled=true;return value;
}
ProcessingPort port(PortDirection direction,PortId id,const NumericContract& value){return numeric_test_builders::port(id,direction,value,bounds,65536,check,"own port");}
NumericContract samples(){
    static constexpr std::uint32_t mono=0;NumericContract value;
    value.channel_order={&mono,1};value.clock={"values",{1,1},0};value.representation.alignment=alignof(float);
    value.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{count,count},ExtentBounds{}}};return value;
}
ConfiguredProcessingNode coefficients(const ConfiguredProcessingNode& size){
    ConfiguredProcessingNode node;check(bool(make_processing_node("WindowFunction",3,profile().admission,node)),"window factory");
    node.parameters=TransformProcessingParameters{{{"function",std::string("hann")},{"size",NumericLiteral{std::uint64_t(count)}},{"precision",NumericLiteral{std::uint64_t(1)}}}};
    node.inputs[0].contract=size.outputs[0].contract;
    check(bool(derive_processing_node_contracts(node,profile().admission)),"Float64 window derivation");return node;
}
void direct_parameters(ConfiguredProcessingNode& node,double pi=3.14159265358979){
    auto& fields=std::get<TransformProcessingParameters>(node.parameters).values;
    fields.push_back({"evaluation",NumericLiteral{std::uint64_t(1)}});
    fields.push_back({"pi",NumericLiteral{pi}});
}
NumericProcessingGraph definition(){
    const auto input=samples();
    auto source=numeric_test_builders::node(1,"NetworkInput",EndpointProcessingParameters{}, {},{port(PortDirection::Output,0,input)});
    ConfiguredProcessingNode size;check(bool(make_processing_node("UInt32Constant",4,profile().admission,size,{48000,count,256,32})),"size factory");
    auto window=coefficients(size);
    ConfiguredProcessingNode product;product.id=6;product.kind="ArrayMultiply";product.parameters=TransformProcessingParameters{};
    product.inputs={port(PortDirection::Input,0,input),{1,PortDirection::Input,window.outputs[0].contract}};
    OwnedProcessingContract output;check(array_product_output_contract(product,output)==Status::ok,"mixed product contract");product.outputs={{0,PortDirection::Output,output}};
    ConfiguredProcessingNode fft;fft.id=2;fft.kind="RealFourier";fft.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t(count)}}}};
    fft.inputs={{0,PortDirection::Input,output}};std::vector<OwnedProcessingContract> spectra;
    check(fourier_output_contracts(fft,std::get<NumericContract>(output.view()),spectra)==Status::ok,"Double FFT contracts");
    for(PortId id=0;id<spectra.size();++id)fft.outputs.push_back({id,PortDirection::Output,spectra[id]});
    return numeric_test_builders::graph("precise-frontend",{1,120},{source,size,window,product,fft},
        {numeric_test_builders::edge(4,0,3,0),numeric_test_builders::edge(3,0,6,1),numeric_test_builders::edge(1,0,6,0),numeric_test_builders::edge(6,0,2,0)});
}
struct Seen {
    std::array<double,count> window{},product{};std::array<float,5> real{},imaginary{};
    std::size_t fft_ports=0;
    bool float_window=false;
    static void output(void* user,const NumericPortFrameView& event){
        if(event.direction!=PortDirection::Output || event.status==Status::unavailable)return;
        if(event.identity.node!=3 && event.identity.node!=6 && event.identity.node!=2)return;
        auto& seen=*static_cast<Seen*>(user);check(event.status==Status::ok,"available stage status");
        const auto& value=std::get<NumericView>(event.frame.payload);
        if(event.identity.node==2){seen.spectrum(event.identity.port,value);return;}
        if(seen.float_window && event.identity.node==3){
            check(value.contract->dtype==DType::Float32,"Float32 direct window publication");
            for(std::size_t i=0;i<count;++i){float sample=0;check(load_element(value,{0,i,0},sample)==Status::ok,"Float32 coefficient");seen.window[i]=sample;}
            return;
        }
        check(value.contract->dtype==DType::Float64,"Double coefficient/product publication");
        auto& destination=event.identity.node==3?seen.window:seen.product;
        for(std::size_t i=0;i<count;++i)check(load_element(value,{0,i,0},destination[i])==Status::ok,"Double stage value");
    }
    void spectrum(PortId port,const NumericView& value){
        check(value.contract->dtype==DType::Float32,"single Float32 FFT boundary");
        check(value.actual[0]==5u,"complete positive real FFT shape");
        for(std::size_t i=0;i<value.actual[0];++i){
            float sample=0;check(load_element(value,{0,i,0},sample)==Status::ok,"Float32 spectrum load");
            (port==0?real:imaginary)[i]=sample;
        }
        ++fft_ports;
    }
};
void verify(const Seen& seen,float input,bool direct){
    const double coefficient=direct?.5*(1-std::cos(2*3.14159265358979*(1./7.))):.5+.5*std::cos(2*std::numbers::pi*(1.-3.5)/7.);
    const double product=double(input)*coefficient;
    check(seen.fft_ports==2,"one complete FFT publication");
    check(std::abs(seen.window[1]-coefficient)<1e-16,"source Double Hann coefficient");
    check(seen.product[1]==product && seen.product[1]!=double(float(product)),"mixed product preserves sub-Float32 precision");
    check(seen.real[0]==float(product),"DC rounds at FFT publication");
    for(std::size_t k=1;k<=4;++k){
        const double angle=2*std::numbers::pi*double(k)/count;
        check(std::abs(seen.real[k]-float(product*std::cos(angle)))<2e-8f,"Double FFT real impulse oracle");
        if(k<4)check(std::abs(seen.imaginary[k]-float(-product*std::sin(angle)))<2e-8f,"Double FFT imaginary impulse oracle");
    }
}
void processing(bool direct){
    auto graph=definition();if(direct)direct_parameters(graph.nodes[2]);std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"prepare explicit mixed precision pipeline");
    Seen seen;const NumericRuntimeCallbacks callbacks{Seen::output,nullptr,nullptr,&seen};FrameStamp stamp;stamp.sequence=stamp.producer_epoch=1;
    stamp.has_sample_interval=true;stamp.sample_count=count;
    std::array<float,count> values{};values[1]=1.00000011920928955078125f;
    const auto contract=graph.nodes[0].outputs[0].contract.view();const auto& input=std::get<NumericContract>(contract);
    const PlaneLayout plane{0,{sizeof(float),0}};const auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(values)),std::span(&plane,1),{count,1});
    const auto before=allocations;
    check(plan->process_clock(4,stamp,*state,{},callbacks)==Status::ok,"publish Double Hann once");
    check(plan->process_numeric_ports(1,0,frame,*state,{},callbacks)==Status::ok,"process mixed precision frame");
    check(allocations==before,"no processing allocation");verify(seen,values[1],direct);
    auto tight=profile();tight.resources.maximum.scratch_bytes=1;
    check(PreparedGraph::prepare_numeric(graph,tight,plan,state)!=Status::ok,"reject insufficient Double FFT construction budget");
}
void direct_source_arithmetic(){
    auto node=definition().nodes[2];direct_parameters(node);
    auto& fields=std::get<TransformProcessingParameters>(node.parameters).values;
    fields[1].value=NumericLiteral{std::uint64_t(1024)};
    std::array<double,1024> values{};std::array<float,1024> rounded{};
    check(window_function_coefficients(node,values)==Status::ok,"direct source Double coefficients");
    check(window_function_coefficients(node,rounded)==Status::ok,"direct source Float32 coefficients");
    for(std::size_t i=0;i<values.size();++i){
        const double expected=.5*(1-std::cos(2*3.14159265358979*(double(i)/double(1024-1))));
        check(values[i]==expected,"exact source expression, literal pi and operation order");
        check(rounded[i]==float(expected),"Float32 rounds direct Double coefficient once");
    }
    fields[4].value=NumericLiteral{std::numbers::pi};
    check(window_function_coefficients(node,values)==Status::ok,"explicit pi selects source arithmetic");
    check(values[1023]==.5*(1-std::cos(2*std::numbers::pi)),"direct window uses supplied pi");
    fields[4].value=NumericLiteral{3.14159265358979};
    fields.push_back({"periodic",true});
    check(window_function_coefficients(node,values)==Status::ok,"direct periodic window");
    check(values[1023]==.5*(1-std::cos(2*3.14159265358979*(1023./1024.))),"explicit periodic denominator");
    node.enabled=false;
    check(window_function_coefficients(node,values)==Status::ok && values.front()==1 && values.back()==1,"disabled direct window is identity");
}
void invalid_direct_parameters(){
    auto node=definition().nodes[2];direct_parameters(node);
    auto& fields=std::get<TransformProcessingParameters>(node.parameters).values;
    std::array<double,count> output;output.fill(17);
    fields[3].value=NumericLiteral{std::uint64_t(3)};
    check(!derive_processing_node_contracts(node,profile().admission),"reject unknown window evaluation");
    check(window_function_coefficients(node,output)==Status::invalid_argument && output[0]==17 && output.back()==17,"invalid mode leaves coefficients untouched");
    fields[3].value=NumericLiteral{std::uint64_t(1)};
    for(const double pi:{0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
        fields[4].value=NumericLiteral{pi};
        check(!derive_processing_node_contracts(node,profile().admission),"reject invalid pi during inference");
        check(window_function_coefficients(node,output)==Status::invalid_argument && output[0]==17 && output.back()==17,"invalid pi leaves coefficients untouched");
    }
}
void direct_float_publication(){
    auto graph=definition();
    graph.nodes.erase(std::remove_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.id!=3 && node.id!=4;}),graph.nodes.end());
    graph.edges.erase(std::remove_if(graph.edges.begin(),graph.edges.end(),[](const auto& edge){return edge.from!=4;}),graph.edges.end());
    auto& node=graph.nodes[1];direct_parameters(node);
    std::get<TransformProcessingParameters>(node.parameters).values[2].value=NumericLiteral{std::uint64_t(0)};
    check(bool(derive_processing_node_contracts(node,profile().admission)),"direct Float32 derivation");
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"prepare direct Float32 generator");
    Seen seen;seen.float_window=true;FrameStamp stamp;stamp.sequence=stamp.producer_epoch=1;
    stamp.has_sample_interval=true;stamp.sample_count=count;
    const auto before=allocations;
    check(plan->process_clock(4,stamp,*state,{},NumericRuntimeCallbacks{Seen::output,nullptr,nullptr,&seen})==Status::ok,"publish direct Float32 window");
    check(allocations==before,"direct Float32 publication allocates nothing");
    for(std::size_t i=0;i<count;++i)check(seen.window[i]==float(.5*(1-std::cos(2*3.14159265358979*(double(i)/double(count-1))))),"direct Float32 runtime coefficients");
}
void hamming_publication(bool precise,bool series){
    auto graph=definition();
    graph.nodes.erase(std::remove_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.id!=3 && node.id!=4;}),graph.nodes.end());
    graph.edges.erase(std::remove_if(graph.edges.begin(),graph.edges.end(),[](const auto& edge){return edge.from!=4;}),graph.edges.end());
    auto& window=graph.nodes[1];direct_parameters(window);
    auto& fields=std::get<TransformProcessingParameters>(window.parameters).values;
    fields[0].value=std::string{"hamming"};fields[2].value=NumericLiteral{std::uint64_t(precise)};
    fields[3].value=NumericLiteral{std::uint64_t(series?2:0)};
    check(bool(derive_processing_node_contracts(window,profile().admission)),"Hamming evaluation contract");
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"prepare declared Hamming arithmetic");
    Seen seen;seen.float_window=!precise;FrameStamp stamp;stamp.sequence=stamp.producer_epoch=1;
    stamp.has_sample_interval=true;stamp.sample_count=count;
    const auto before=allocations;
    check(plan->process_clock(4,stamp,*state,{},NumericRuntimeCallbacks{Seen::output,nullptr,nullptr,&seen})==Status::ok,"publish Hamming coefficients");
    check(allocations==before,"Hamming publication has no hot allocation");
    std::array<double,count> doubles{};std::array<float,count> floats{};
    check(window_function_coefficients(window,doubles)==Status::ok && window_function_coefficients(window,floats)==Status::ok,"domain Hamming evaluation");
    std::array<double,count> source_double{};std::array<float,count> source_float{};
    check(dsp_kernels::symmetric_hamming_window(source_double) && dsp_kernels::symmetric_hamming_window(source_float),"source cosine-series Hamming");
    for(std::size_t i=0;i<count;++i){
        const double previous=.54-.46*std::cos(double(i)*6.2831853071795864769/double(count-1));
        check(doubles[i]==(series?source_double[i]:previous) && floats[i]==(series?source_float[i]:float(previous)),"Default preserves old coefficients; explicit series preserves source arithmetic");
        check(seen.window[i]==(precise?doubles[i]:double(floats[i])),"domain coefficients equal actual runtime publication");
    }
}
void contracts(){
    auto graph=definition();auto& window=graph.nodes[2];
    auto& fields=std::get<TransformProcessingParameters>(window.parameters).values;fields.back().value=NumericLiteral{std::uint64_t(2)};
    check(!derive_processing_node_contracts(window,profile().admission),"reject unknown precision");
    auto& fft=graph.nodes.back();fft.kind="Fourier";std::vector<OwnedProcessingContract> outputs;
    check(fourier_output_contracts(fft,std::get<NumericContract>(fft.inputs[0].contract.view()),outputs)==Status::incompatible_contract,"complex Float64 is outside explicit real contract");
}
}
void* operator new(std::size_t bytes){++allocations;if(auto* value=std::malloc(bytes?bytes:1))return value;throw std::bad_alloc();}
void operator delete(void* value) noexcept{std::free(value);}
void operator delete(void* value,std::size_t) noexcept{std::free(value);}
int main(){processing(false);processing(true);direct_source_arithmetic();invalid_direct_parameters();direct_float_publication();hamming_publication(false,false);hamming_publication(true,false);hamming_publication(false,true);hamming_publication(true,true);contracts();std::puts("precise frontend: mixed product, Double real FFT/Float32 boundary, compact ports, admission and no allocations PASS");}
