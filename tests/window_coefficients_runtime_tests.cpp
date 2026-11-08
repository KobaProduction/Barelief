#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <domain/graph/preparation_values.h>
#include <dsp_kernels/dsp.h>
#include "numeric_test_builders.h"
#include "owned_history_fixture.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
namespace {
using namespace signal_contracts;using namespace domain;using namespace dataflow_runtime;
constexpr std::size_t size=512;
constexpr FrameLimits bounds{1,32768,262144,16384,256,32768};
std::size_t allocations=0;
void check(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL window coefficients: %s\n",message);std::exit(1);}}
NumericRuntimeProfile profile(){auto value=NumericRuntimeProfile{{32,64,8,32,1048576,4*1048576,bounds},{{4*1048576,4*1048576,4*1048576,131072},0,32*1048576},{32768,1,262144,262144,1048576},{true,true,true}};value.music_enabled=true;return value;}
ProcessingPort port(PortDirection direction,PortId id,const NumericContract& value){return numeric_test_builders::port(id,direction,value,bounds,65536,check,"owned port");}
ConfiguredProcessingNode window_node(){
    ConfiguredProcessingNode node;check(bool(make_processing_node("WindowFunction",3,profile().admission,node)),"window factory");
    node.parameters=TransformProcessingParameters{{{"function",std::string("hann")},{"size",NumericLiteral{std::uint64_t(size)}},{"periodic",true}}};
    check(bool(derive_processing_node_contracts(node,profile().admission)),"window derivation");
    check(node.outputs.size()==1,"window exposes one coefficient array");return node;
}
NumericProcessingGraph graph(){
    static constexpr std::uint32_t mono=0;
    NumericContract input;input.channel_order={&mono,1};input.clock={"values",{1,1},0};input.representation.alignment=alignof(float);
    input.shape={Rank::Vector,{Axis::Declared,Axis::None},{ExtentBounds{size,size},ExtentBounds{}}};
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs={port(PortDirection::Output,0,input)};
    ConfiguredProcessingNode fft;fft.id=2;fft.kind="RealFourier";fft.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t(size)}}}};fft.inputs={port(PortDirection::Input,0,input)};
    std::vector<OwnedProcessingContract> spectra;check(fourier_output_contracts(fft,input,spectra)==Status::ok,"FFT contracts");
    for(PortId id=0;id<spectra.size();++id)fft.outputs.push_back({id,PortDirection::Output,spectra[id]});
    ConfiguredProcessingNode amplitude;amplitude.id=5;amplitude.kind="AmplitudePhase";amplitude.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t(size)}}}};
    amplitude.inputs={{0,PortDirection::Input,spectra[0]},{1,PortDirection::Input,spectra[1]}};
    std::array<OwnedProcessingContract,3> outputs;check(amplitude_phase_output_contracts(amplitude,std::get<NumericContract>(spectra[0].view()),outputs)==Status::ok,"compact polar contracts");
    amplitude.outputs={{0,PortDirection::Output,outputs[0]},{1,PortDirection::Output,outputs[1]},{2,PortDirection::Output,outputs[2]}};
    check(attach_numeric_parameter_ports(amplitude,profile().admission)==Status::ok,"one coefficient input");
    ConfiguredProcessingNode count;check(bool(make_processing_node("UInt32Constant",4,profile().admission,count,{48000,size,256,32})),"size factory");
    auto window=window_node();window.inputs[0].contract=count.outputs[0].contract;
    check(bool(derive_processing_node_contracts(window,profile().admission)),"window scalar source contract");
    return numeric_test_builders::graph("window-coefficients",{1,120},{source,fft,window,count,amplitude},
        {numeric_test_builders::edge(1,0,2,0),numeric_test_builders::edge(2,0,5,0),numeric_test_builders::edge(2,1,5,1),numeric_test_builders::edge(4,0,3,0),numeric_test_builders::edge(3,0,5,window_coefficients_input)});
}
struct Observation {
    float tone=0,phase=0;std::size_t frames=0;
    static void output(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=5 || event.direction!=PortDirection::Output)return;
        auto& self=*static_cast<Observation*>(user);check(event.status==Status::ok,"amplitude output status");
        const auto& value=std::get<NumericView>(event.frame.payload);
        if(event.identity.port==2){check(value.contract->shape.rank==Rank::Scalar,"DC is scalar");return;}
        check(value.actual[0]==256,"amplitude and phase have identical non-DC bins");
        check(value.contract->clock.sample_rate==0 && value.contract->coordinates[0].unit==NumericUnit::Values,"no implicit sampling frequency");
        check(load_element(value,{0,2,0},event.identity.port?self.phase:self.tone)==Status::ok,"read tone");
        if(!event.identity.port)++self.frames;
    }
};
struct Provider {
    std::size_t calls=0;
    static Status resolve(const void* context,const NumericProcessingGraph& graph,NodeId id,PortId output,NumericArray& out){
        if(id!=3 || output)return Status::unavailable;
        ++const_cast<Provider*>(static_cast<const Provider*>(context))->calls;
        const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& value){return value.id==id;});
        out.values.resize(size);return window_function_coefficients(*node,out.values);
    }
};
void feed(const NumericProcessingGraph& definition,const PreparedGraph& plan,RuntimeContext& state,Observation& seen){
    std::array<float,size> values{},coefficients{};
    check(window_function_coefficients(definition.nodes[2],coefficients)==Status::ok,"producer coefficients");
    for(std::size_t i=0;i<size;++i)values[i]=float(.4*std::cos(6.283185307179586*3*i/size))*coefficients[i];
    const auto data=definition.nodes[0].outputs[0].contract.view();const auto& input=std::get<NumericContract>(data);const PlaneLayout plane{0,{sizeof(float),0}};
    FrameStamp stamp;stamp.sequence=stamp.source_sequence=seen.frames+1;stamp.timestamp_ticks=seen.frames;stamp.producer_epoch=1;
    const auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(values)),std::span(&plane,1),{size,1});
    const auto before=allocations;
    check(plan.process_numeric_ports(1,0,frame,state,{},NumericRuntimeCallbacks{Observation::output,nullptr,nullptr,&seen})==Status::ok,"process windowed frame");
    check(allocations==before,"no coefficient sums/storage allocations on audio path");
}
void coefficients_and_disconnect(){
    auto definition=graph();Provider provider;auto limits=profile();limits.preparation_values={&provider,nullptr,Provider::resolve};
    NumericProcessingGraph evaluated,executable;
    const auto bound=resolve_numeric_parameter_bindings(definition,limits.admission,evaluated,executable,-1,&limits.preparation_values);
    if(!bound)std::fprintf(stderr,"binding error=%d node=%d port=%u\n",int(bound.error),int(bound.node),unsigned(bound.port));
    check(bool(bound),"evaluate vector binding");
    check(std::none_of(std::get<TransformProcessingParameters>(evaluated.nodes.back().parameters).values.begin(),std::get<TransformProcessingParameters>(evaluated.nodes.back().parameters).values.end(),[](const auto& field){return field.key=="window_coefficients";}),"saved graph retains wire, not duplicated array");
    check(std::get<TransformProcessingParameters>(executable.nodes.back().parameters).values.size()==2,"executable owns cold coefficients");
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(definition,limits,plan,state)==Status::ok,"prepare coefficient pipeline");
    auto prepared_calls=provider.calls;Observation seen;
    feed(definition,*plan,*state,seen);feed(definition,*plan,*state,seen);
    check(std::abs(seen.tone-.4f)<1e-6f && std::abs(seen.phase)<1e-5f,"Hann compensated peak and phase");
    check(provider.calls==prepared_calls,"coefficients evaluated only at preparation");
    std::get<TransformProcessingParameters>(definition.nodes[2].parameters).values[0].value=std::string("hamming");
    std::shared_ptr<const PreparedGraph> changed;std::unique_ptr<RuntimeContext> changed_state;
    check(PreparedGraph::prepare_numeric(definition,limits,changed,changed_state,plan.get(),state.get())==Status::ok,"changed window rebuilds correction cache");
    feed(definition,*changed,*changed_state,seen);
    check(std::abs(seen.tone-.4f)<1e-6f && provider.calls>prepared_calls,"Hamming recalibrates after a window change");
    prepared_calls=provider.calls;
    std::erase_if(definition.edges,[](const auto& edge){return edge.input_port==window_coefficients_input;});
    std::shared_ptr<const PreparedGraph> disconnected;std::unique_ptr<RuntimeContext> next;
    check(PreparedGraph::prepare_numeric(definition,limits,disconnected,next,changed.get(),changed_state.get())==Status::ok,"disconnect coefficient correction");
    feed(definition,*disconnected,*next,seen);
    check(std::abs(seen.tone-.216f)<1e-6f && provider.calls==prepared_calls,"disconnect removes cached window gain");
}
void scaling_without_rate(){
    dsp_kernels::PreparedSpectrumScaling scaling;dsp_kernels::SpectrumProjectionOptions options;options.compensate=true;options.representation=dsp_kernels::SpectrumProjection::PeakAmplitude;
    std::array<float,8> coefficients;coefficients.fill(.5f);
    check(scaling.prepare(0,coefficients,options,8),"amplitude has no Fs dependency");
    coefficients.fill(0);float value=0;
    check(scaling.project(2,0,true,value) && value==1,"cached sums do not borrow coefficients");
    check(scaling.prepare(0,{},options,8) && scaling.project(2,0,true,value) && value==.5f,"empty coefficients remove only window correction");
    options.representation=dsp_kernels::SpectrumProjection::Psd;
    check(!scaling.prepare(0,{},options,8) && scaling.prepare(48000,{},options,8),"PSD explicitly requires Fs");
}
NumericProcessingGraph matrix_pipeline(){
    auto definition=graph();const auto original=definition.nodes.front().outputs[0].contract;
    auto input=std::get<NumericContract>(original.view());
    input.shape={Rank::Matrix,{Axis::Time,Axis::Sample},{ExtentBounds{0,3},ExtentBounds{size,size}}};
    definition.nodes.front().outputs[0]=port(PortDirection::Output,0,input);
    auto& fft=definition.nodes[1];fft.inputs={port(PortDirection::Input,0,input)};
    std::vector<OwnedProcessingContract> spectra;check(fourier_output_contracts(fft,input,spectra)==Status::ok,"matrix FFT contracts");
    fft.outputs.clear();for(PortId id=0;id<spectra.size();++id)fft.outputs.push_back({id,PortDirection::Output,spectra[id]});
    auto& amplitude=definition.nodes.back();amplitude.inputs[0].contract=spectra[0];amplitude.inputs[1].contract=spectra[1];
    check(bool(derive_processing_node_contracts(amplitude,profile().admission)),"matrix amplitude contracts");
    ConfiguredProcessingNode product;product.id=6;product.kind="ArrayMultiply";product.parameters=TransformProcessingParameters{};
    product.inputs={port(PortDirection::Input,0,input),{1,PortDirection::Input,definition.nodes[2].outputs[0].contract}};
    OwnedProcessingContract output;check(array_product_output_contract(product,output)==Status::ok,"row broadcast uses generic multiplication");product.outputs={{0,PortDirection::Output,output}};
    definition.nodes.push_back(std::move(product));definition.edges.front()=numeric_test_builders::edge(1,0,6,0);
    definition.edges.push_back(numeric_test_builders::edge(3,0,6,1));definition.edges.push_back(numeric_test_builders::edge(6,0,2,0));return definition;
}
struct MatrixObservation {
    std::size_t frames=0,rows=2;
    static void output(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=5)return;auto& self=*static_cast<MatrixObservation*>(user);check(event.status==Status::ok,"matrix window pipeline status");
        const auto& view=std::get<NumericView>(event.frame.payload);const auto phase=event.identity.port==1;
        if(event.identity.port==2){check(view.actual==std::array<std::size_t,2>{self.rows,1},"one DC per matrix row");return;}
        check(view.actual==std::array<std::size_t,2>{self.rows,256},"matrix magnitude/phase preserve actual rows and shared non-DC width");
        for(std::size_t row=0;row<self.rows;++row){float value=0;check(load_element(view,{0,row,2},value)==Status::ok,"matrix amplitude value");
            const auto expected=phase?(row?-1.57079632679f:0.f):(row?.2f:.4f);check(std::abs(value-expected)<1e-5f,"broadcast window normalization and phase match each row");}
        if(!phase)++self.frames;
    }
};
void matrix_window_broadcast(){
    const auto definition=matrix_pipeline();std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    const auto prepared=PreparedGraph::prepare_numeric(definition,profile(),plan,state);
    if(prepared!=Status::ok)std::fprintf(stderr,"prepare matrix pipeline status=%d\n",int(prepared));
    check(prepared==Status::ok,"prepare one primitive family for matrix windows/FFT/amplitude");
    const auto contract=definition.nodes.front().outputs[0].contract.view();const auto& input=std::get<NumericContract>(contract);
    std::array<float,3*size> samples{};
    for(std::size_t i=0;i<size;++i){samples[2*size+i]=float(.4*std::cos(6.283185307179586*3*i/size));samples[i]=float(.2*std::sin(6.283185307179586*3*i/size));}
    const PlaneLayout plane{0,{size*sizeof(float),sizeof(float)}};FrameStamp stamp;stamp.producer_epoch=1;stamp.sequence=1;stamp.has_sample_interval=true;stamp.sample_count=size;
    auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(samples)),std::span(&plane,1),{2,size});
    std::get<NumericView>(frame.payload).ring={3,2,{}};MatrixObservation seen;const NumericRuntimeCallbacks callbacks{MatrixObservation::output,nullptr,nullptr,&seen};
    auto arena=std::make_shared<history_fixture::Arena>();OwnedDataFrame owned;
    check(retain_frame(frame,bounds,arena->values,arena->descriptors,owned)==Status::ok,"retain external matrix");const auto before=allocations;
    check(plan->process_clock(4,stamp,*state,{},callbacks)==Status::ok,"publish one shared coefficient vector");
    check(plan->process_numeric_owned_ports(1,owned,*state,{},callbacks,2,arena)==Status::ok,"process matrix through shared primitives");
    seen.rows=1;std::get<NumericView>(frame.payload).actual[0]=1;++frame.stamp.sequence;++frame.stamp.timestamp_ticks;++frame.stamp.producer_epoch;
    check(retain_frame(frame,bounds,arena->values,arena->descriptors,owned)==Status::ok,"retain new matrix generation");
    check(plan->process_numeric_owned_ports(1,owned,*state,{},callbacks,1,arena)==Status::ok,"matrix row count changes without preparing FFT again");
    check(allocations==before && seen.frames==2,"matrix primitive pipeline processes ring rows without allocations");
}
}
void* operator new(std::size_t bytes){++allocations;if(auto* pointer=std::malloc(bytes?bytes:1))return pointer;throw std::bad_alloc();}
void operator delete(void* pointer) noexcept{std::free(pointer);}
void operator delete(void* pointer,std::size_t) noexcept{std::free(pointer);}
int main(){coefficients_and_disconnect();scaling_without_rate();matrix_window_broadcast();std::puts("window coefficients: one wire, cold owning cache, compact phase, disconnect, Fs0 amplitude, explicit PSD Fs and shared matrix/ring primitives PASS");}
