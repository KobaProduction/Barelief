#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <domain/graph/preparation_values.h>
#include <dsp_kernels/detail/fused_square_plus_one.h>
#include <dsp_kernels/dsp.h>
#include "numeric_test_builders.h"
#include "owned_history_fixture.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <bit>
#include <string_view>
namespace {
using namespace signal_contracts;using namespace domain;using namespace dataflow_runtime;
constexpr std::size_t maximum=4096;
constexpr std::array<std::uint32_t,1> mono{0};
constexpr FrameLimits bounds{1,32768,262144,16384,256,32768};
std::size_t allocations=0;
void check(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL Fourier runtime: %s\n",message);std::exit(1);}}
NumericRuntimeProfile profile(){auto p=NumericRuntimeProfile{{32,64,8,32,1048576,4*1048576,bounds},{{4*1048576,4*1048576,4*1048576,131072},0,32*1048576},{32768,1,262144,262144,1048576},{true,true,true}};p.music_enabled=true;return p;}
ProcessingPort port(PortDirection side,PortId id,const NumericContract& value){return numeric_test_builders::port(id,side,value,bounds,65536,check,"own port");}
NumericProcessingGraph graph(bool real,std::size_t size,const char* radix="radix2",bool imaginary_connected=true){
    NumericContract input;input.channel_order=mono;input.shape={Rank::Vector,{Axis::Declared,Axis::None},{ExtentBounds{size,size},ExtentBounds{}}};
    input.clock={"values",{1,1},0};input.representation.alignment=alignof(float);
    ConfiguredProcessingNode source;source.id=1;source.kind="AudioSource";source.parameters=EndpointProcessingParameters{};source.outputs={port(PortDirection::Output,0,input),port(PortDirection::Output,1,input)};
    ConfiguredProcessingNode fft;fft.id=2;fft.kind=real?"RealFourier":"Fourier";
    fft.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t(size)}},{"radix",std::string(radix)}}};
    fft.inputs={port(PortDirection::Input,0,input)};if(!real)fft.inputs.push_back(port(PortDirection::Input,1,input));
    std::vector<OwnedProcessingContract> output;check(fourier_output_contracts(fft,input,output)==Status::ok,"derive raw Fourier without Fs");
    for(PortId p=0;p<output.size();++p)fft.outputs.push_back({p,PortDirection::Output,std::move(output[p])});
    auto result=numeric_test_builders::graph("raw-fourier",{1,120},{source,fft},{numeric_test_builders::edge(1,0,2,0)});
    if(!real && imaginary_connected)result.edges.push_back(numeric_test_builders::edge(1,1,2,1));return result;
}
struct Observed {
    std::array<std::array<float,maximum>,3> values{};
    std::array<std::size_t,3> counts{},calls{};
    std::array<FrameStamp,3> stamps{};
    static void output(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=2 || event.direction!=PortDirection::Output)return;
        auto& self=*static_cast<Observed*>(user);const auto p=event.identity.port;check(p<3,"canonical output ID");
        ++self.calls[p];self.stamps[p]=event.frame.stamp;
        if(event.status!=Status::ok){self.counts[p]=0;return;}
        const auto& value=std::get<NumericView>(event.frame.payload);self.counts[p]=value.actual[0];
        check(value.contract->clock.sample_rate==0,"raw FFT needs no sampling frequency");
        const auto axis=value.contract->coordinates[0];
        check(axis.mode==CoordinateMode::Regular && axis.unit==NumericUnit::Values && axis.origin==Ratio{0,1} && axis.step==Ratio{1,1},"raw FFT uses bin indices");
        for(std::size_t i=0;i<value.actual[0];++i)check(load_element(value,{0,i,0},self.values[p][i])==Status::ok,"read Fourier output");
    }
};
struct Running {
    NumericProcessingGraph definition;
    std::shared_ptr<const PreparedGraph> plan;
    std::unique_ptr<RuntimeContext> state;
    Observed seen;
    std::array<std::uint64_t,2> sequence{};
    Running(bool real,std::size_t size,const char* radix="radix2",bool imaginary_connected=true)
        :definition(graph(real,size,radix,imaginary_connected)){
        const auto status=PreparedGraph::prepare_numeric(definition,profile(),plan,state);
        if(status!=Status::ok){
            NumericProcessingGraph evaluated,executable;
            const auto bound=resolve_numeric_parameter_bindings(definition,profile().admission,evaluated,executable);
            std::fprintf(stderr,"prepare status=%d binding=%d node=%d port=%u\n",int(status),int(bound.error),int(bound.node),unsigned(bound.port));
            for(const auto& node:executable.nodes){ResolvedProcessingNode resolved;const auto diagnostic=resolve_processing_node(node,profile().admission,resolved);std::fprintf(stderr,"node %d resolve=%d port=%u\n",int(node.id),int(diagnostic.error),unsigned(diagnostic.port));}
        }
        check(status==Status::ok,"prepare raw Fourier");
    }
    void feed(PortId lane,std::span<const float> samples,std::int64_t begin=0,bool discontinuity=false,std::uint64_t epoch=7,std::uint64_t generation=1){
        const auto data=definition.nodes[0].outputs[lane].contract.view();const auto& input=std::get<NumericContract>(data);
        const PlaneLayout plane{0,{sizeof(float),0}};FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence[lane];
        stamp.producer_epoch=epoch;stamp.generation=generation;stamp.timestamp_ticks=begin;stamp.sample_begin=begin;stamp.sample_count=samples.size();stamp.has_sample_interval=true;stamp.discontinuity=discontinuity;
        const auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(samples),std::span(&plane,1),{samples.size(),1});
        const auto before=allocations;
        check(plan->process_numeric_ports(1,lane,frame,*state,{},NumericRuntimeCallbacks{Observed::output,nullptr,nullptr,&seen})==Status::ok,"feed raw Fourier");
        check(before==allocations,"Fourier processing allocates no memory");
    }
};
void complex_case(){
    constexpr std::size_t size=512;Running run(false,size);std::array<float,size> re{},im{};
    for(std::size_t i=0;i<size;++i){re[i]=float(std::cos(6.283185307179586*3*i/size));im[i]=float(.2+std::sin(6.283185307179586*3*i/size)+.1*(i%2?-1:1));}
    run.feed(0,re);check(run.seen.calls[0]==0,"wait for matching imaginary input");
    const auto original=re;re.fill(0);run.feed(1,im);
    check(run.seen.counts[0]==size && run.seen.counts[1]==size && !run.seen.calls[2],"C2C has two full N planes");
    check(std::abs(run.seen.values[0][3]-size)<.002,"positive complex tone");
    check(std::abs(run.seen.values[0][size-3])<.002,"negative complex spectrum remains independent");
    check(std::abs(run.seen.values[1][0]-.2*size)<.002,"complex DC remains in bin zero");
    check(std::abs(run.seen.values[1][size/2]-.1*size)<.002,"complex Nyquist imaginary part is retained");
    run.feed(0,original,size);run.feed(1,im,2*size);check(run.seen.calls[0]==1,"different intervals are not paired");
    run.feed(0,original,2*size);check(run.seen.calls[0]==2,"matching interval recovers");
    run.feed(1,im,3*size,true);run.feed(0,original,3*size,true);
    check(run.seen.calls[0]==3 && run.seen.stamps[0].discontinuity,"imaginary-first boundary pair works");
}
void real_case(const char* radix){
    constexpr std::size_t size=4096;Running run(true,size,radix);std::array<float,size> samples{};
    for(std::size_t i=0;i<size;++i)samples[i]=float(.25+.5*(i%2?-1:1)+.125*std::sin(6.283185307179586*3*i/size));
    run.feed(0,samples);
    check(run.seen.counts[0]==2049 && run.seen.counts[1]==2049 && !run.seen.calls[2],"RFFT4096 has two 2049-bin planes");
    check(std::abs(run.seen.values[0][2048]-.5*size)<.002,"Nyquist is the final real value");
    check(std::abs(run.seen.values[1][3]+.0625*size)<.002,"interior imaginary sign and index");
    check(std::abs(run.seen.values[0][0]-.25*size)<.002,"real DC at bin zero");
    Running complex(false,size,"radix2",false);complex.feed(0,samples);
    for(std::size_t i=0;i<2049;++i)check(std::abs(complex.seen.values[0][i]-run.seen.values[0][i])<.002,"complex and real FFT positive bins agree");
    for(std::size_t i=0;i<2049;++i)check(std::abs(complex.seen.values[1][i]-run.seen.values[1][i])<.002,"imaginary interior agrees");
}
void pairing_boundaries(){
    std::array<float,8> re{},im{};re.fill(1);
    Running epochs(false,8);epochs.feed(0,re,0,false,8);epochs.feed(1,im,0,false,7);
    check(!epochs.seen.calls[0],"producer epochs are not mixed");
    Running generations(false,8);generations.feed(0,re,0,false,7,2);generations.feed(1,im,0,false,7,1);
    check(!generations.seen.calls[0],"generations are not mixed");
}
void algorithm_geometry(){
    for(const auto* kind:{"Fourier","RealFourier"}){
        ConfiguredProcessingNode node;
        check(bool(make_processing_node(kind,2,profile().admission,node,{0,8,0,0})),"FFT factory does not require audio settings");
        check(parameter_descriptor(*node_kind(kind),"sample_rate")==nullptr && numeric_parameter_key(kind,target_sample_rate_input).empty(),"FFT has no Fs setting or input");
    }
    auto invalid=graph(false,8);auto& fft=invalid.nodes[1];
    std::get<TransformProcessingParameters>(fft.parameters).values[1].value=std::string("radix4");
    std::vector<OwnedProcessingContract> output;const auto input=std::get<NumericContract>(fft.inputs[0].contract.view());
    check(fourier_output_contracts(fft,input,output)!=Status::ok,"pure complex radix4 rejects a non-power-of-four length");
    Running complex(false,16,"radix4");std::array<float,16> re{},im{};re.fill(1);complex.feed(0,re);complex.feed(1,im);
    check(complex.seen.values[0][0]==16 && complex.seen.counts[0]==16,"complex radix4 retains N values and DC");
    Running real(true,8,"radix4");std::array<float,8> small{};small.fill(1);real.feed(0,small);
    check(real.seen.values[0][0]==8 && real.seen.counts[0]==5 && real.seen.counts[1]==5,"real radix4 minimum geometry");
}
void explicit_transform_parameters(){
    for(const bool real:{false,true})for(const std::uint64_t size:{0u,16u}){
        auto definition=graph(real,8);
        std::get<TransformProcessingParameters>(definition.nodes[1].parameters).values[0].value=NumericLiteral{size};
        std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
        check(PreparedGraph::prepare_numeric(definition,profile(),plan,state)!=Status::ok,"payload length cannot repair invalid or mismatched declared FFT size");
    }
}
void explicit_amplitude_frequency(){
    const auto definition=graph(true,8);
    auto source=std::get<NumericContract>(definition.nodes[1].outputs[0].contract.view());
    source.clock={"audio",{1,48000},48000};
    source.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{6000,1}};
    ConfiguredProcessingNode node;node.id=2;node.kind="AmplitudePhase";
    node.inputs={port(PortDirection::Input,0,source)};
    source.quantity=Quantity::SpectrumImaginary;node.inputs.push_back(port(PortDirection::Input,1,source));
    auto& fields=node.parameters.emplace<TransformProcessingParameters>().values;
    fields={{"fft_size",NumericLiteral{std::uint64_t{8}}},{"sample_rate",NumericLiteral{std::uint64_t{0}}},
        {"representation",std::string(dsp_kernels::spectrum_projection_name(dsp_kernels::SpectrumProjection::Psd))}};
    const auto input=std::get<NumericContract>(node.inputs[0].contract.view());
    std::array<OwnedProcessingContract,3> output;
    check(amplitude_phase_output_contracts(node,input,output)==Status::invalid_argument,"PSD cannot obtain missing Fs from transport clock or frequency coordinates");
    fields[2].value=std::string(dsp_kernels::spectrum_projection_name(dsp_kernels::SpectrumProjection::Magnitude));
    check(amplitude_phase_output_contracts(node,input,output)==Status::ok,"magnitude works without Fs");
    const auto index=std::get<NumericContract>(output[0].view()).coordinates[0];
    check(index.unit==NumericUnit::Values && index.origin==Ratio{1,1} && index.step==Ratio{1,1},"missing explicit Fs yields bin indices, not inherited Hz");
    fields[1].value=NumericLiteral{std::uint64_t{48000}};
    check(amplitude_phase_output_contracts(node,input,output)==Status::ok,"explicit Fs supplies the frequency axis");
    const auto frequency=std::get<NumericContract>(output[0].view()).coordinates[0];
    check(frequency.unit==NumericUnit::Hertz && frequency.origin==Ratio{48000,8} && frequency.step==Ratio{48000,8},"frequency axis is calculated from explicit Fs and N");
}
NumericProcessingGraph matrix_graph(bool real){
    auto result=graph(real,8);const auto original=result.nodes[0].outputs[0].contract;
    auto input=std::get<NumericContract>(original.view());
    input.shape={Rank::Matrix,{Axis::Time,Axis::Sample},{ExtentBounds{0,3},ExtentBounds{8,8}}};
    input.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}};
    auto& source=result.nodes[0];source.kind="NetworkInput";source.outputs={port(PortDirection::Output,0,input)};
    auto& fft=result.nodes[1];fft.inputs={port(PortDirection::Input,0,input)};if(!real)fft.inputs.push_back(port(PortDirection::Input,1,input));
    std::vector<OwnedProcessingContract> outputs;check(fourier_output_contracts(fft,input,outputs)==Status::ok,"row FFT contracts");
    fft.outputs.clear();for(PortId id=0;id<outputs.size();++id)fft.outputs.push_back({id,PortDirection::Output,outputs[id]});
    result.edges={numeric_test_builders::edge(1,0,2,0)};if(!real)result.edges.push_back(numeric_test_builders::edge(1,0,2,1));return result;
}
void endpoint_contracts(bool matrix) {
    auto definition=matrix?matrix_graph(true):graph(true,8);auto& fft=definition.nodes[1];
    const auto input=std::get<NumericContract>(fft.inputs[0].contract.view());
    std::vector<OwnedProcessingContract> outputs;
    check(fourier_output_contracts(fft,input,outputs)==Status::ok && outputs.size()==2,"derive full positive real planes");
    const auto axis=matrix?1u:0u;
    for(std::size_t port=0;port<2;++port){
        const auto value=std::get<NumericContract>(outputs[port].view());
        check(value.shape.extents[axis]==ExtentBounds{5,5},"both real planes include DC and Nyquist");
        check(value.coordinates[axis]==AxisCoordinates{CoordinateMode::Regular,NumericUnit::Values,{0,1},{1,1}},"full positive planes use bins0 throughN/2");
        if(matrix)check(value.shape.extents[0]==input.shape.extents[0] && value.coordinates[0]==input.coordinates[0],"matrix row bounds and coordinates preserved");
    }

}
struct MatrixObservation {
    bool real=false;std::size_t calls=0;unsigned missing=2;
    static void output(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=2)return;auto& self=*static_cast<MatrixObservation*>(user);
        check(event.status==Status::ok,"matrix Fourier output");const auto& value=std::get<NumericView>(event.frame.payload);const auto port=event.identity.port;
        check(value.actual[0]==2 && value.actual[1]==(self.real?5:8),"row FFT actual shapes");
        const double times[]{-1,0};check(value.coordinates.axes[0].size()==sizeof(times) && std::equal(value.coordinates.axes[0].begin(),value.coordinates.axes[0].end(),std::as_bytes(std::span(times)).begin()),"row coordinates remain explicit and ordered");
        for(std::size_t row=0;row<2;++row){float number=0;check(load_element(value,{0,row,0},number)==Status::ok,"matrix coefficient");
            const auto expected=(self.real?port==1:port==self.missing)?0.f:float((row+1)*8);check(std::abs(number-expected)<1e-5,"each row has independent DC");}
        ++self.calls;
    }
};
void matrix_case(bool real,unsigned missing=2){
    auto definition=matrix_graph(real);std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    if(!real)std::erase_if(definition.edges,[=](const auto& edge){return edge.input_port==missing;});
    const auto prepared=PreparedGraph::prepare_numeric(definition,profile(),plan,state);
    if(prepared!=Status::ok)std::fprintf(stderr,"prepare matrix real=%d status=%d\n",int(real),int(prepared));
    check(prepared==Status::ok,"prepare shared vector/matrix Fourier");
    std::array<float,24> storage{};std::fill_n(storage.begin()+16,8,1.f);std::fill_n(storage.begin(),8,2.f);
    const auto data=definition.nodes[0].outputs[0].contract.view();const auto& input=std::get<NumericContract>(data);
    const PlaneLayout plane{0,{8*sizeof(float),sizeof(float)}};const double times[]{-1,0};CoordinateView coordinates;coordinates.axes[0]=std::as_bytes(std::span(times));
    auto frame=numeric_test_builders::frame({},input,std::as_bytes(std::span(storage)),std::span(&plane,1),{2,8},coordinates);
    std::get<NumericView>(frame.payload).ring={3,2,{}};MatrixObservation seen{real,0,missing};
    auto arena=std::make_shared<history_fixture::Arena>();OwnedDataFrame owned;
    check(retain_frame(frame,bounds,arena->values,arena->descriptors,owned)==Status::ok,"retain external ring");const auto before=allocations;
    check(validate_view(frame,bounds)==Status::ok,"valid matrix input");
    const auto status=plan->process_numeric_owned_ports(1,owned,*state,{},NumericRuntimeCallbacks{MatrixObservation::output,nullptr,nullptr,&seen},2,arena);
    if(status!=Status::ok)std::fprintf(stderr,"matrix real=%d status=%d\n",int(real),int(status));
    check(status==Status::ok,"FFT reads logical ring rows");
    check(before==allocations && seen.calls==2,"matrix uses one prepared FFT without audio-path allocations");
}
struct JoinedObservation {
    std::size_t calls=0;
    static void output(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=3)return;auto& self=*static_cast<JoinedObservation*>(user);check(event.status==Status::ok,"DC prepend status");
        const auto& value=std::get<NumericView>(event.frame.payload);check(value.actual[0]==5,"DC + amplitude restores N/2+1 values");
        for(std::size_t i=0;i<5;++i){float number=0;check(load_element(value,{0,i,0},number)==Status::ok && number==(i?0.f:8.f),"DC is prepended without scaling or reordered bins");}
        ++self.calls;
    }
};
void concatenate_dc(){
    auto definition=graph(true,8);ConfiguredProcessingNode join;join.id=3;join.kind="ArrayConcat";join.parameters=TransformProcessingParameters{};
    ConfiguredProcessingNode polar;
    check(bool(make_processing_node("AmplitudePhase",4,profile().admission,polar,{48000,8,8,8})),"make amplitude/phase");
    std::get<TransformProcessingParameters>(polar.parameters).values.push_back({"normalize",false});
    for(PortId port=0;port<2;++port)polar.inputs[port].contract=definition.nodes[1].outputs[port].contract;
    check(bool(derive_processing_node_contracts(polar,profile().admission)),"derive amplitude/phase and DC");
    join.inputs={{0,PortDirection::Input,polar.outputs[2].contract},{1,PortDirection::Input,polar.outputs[0].contract}};
    definition.nodes.push_back(std::move(polar));
    definition.edges.insert(definition.edges.end(),{{2,4,0,0},{2,4,1,1}});
    OwnedProcessingContract output;check(array_concat_output_contract(join,output)==Status::ok,"scalar DC joins a vector through the existing primitive");join.outputs={{0,PortDirection::Output,output}};
    definition.nodes.push_back(std::move(join));definition.edges.push_back(numeric_test_builders::edge(4,2,3,0));definition.edges.push_back(numeric_test_builders::edge(4,0,3,1));
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;check(PreparedGraph::prepare_numeric(definition,profile(),plan,state)==Status::ok,"prepare DC concatenation");
    std::array<float,8> samples;samples.fill(1.f);const auto data=definition.nodes[0].outputs[0].contract.view();const auto& input=std::get<NumericContract>(data);const PlaneLayout plane{0,{sizeof(float),0}};
    const auto frame=numeric_test_builders::frame({},input,std::as_bytes(std::span(samples)),std::span(&plane,1),{8,1});JoinedObservation seen;const auto before=allocations;
    check(plan->process_numeric_ports(1,0,frame,*state,{},NumericRuntimeCallbacks{JoinedObservation::output,nullptr,nullptr,&seen})==Status::ok && seen.calls==1 && before==allocations,"separate DC is reusable by N/2+1 consumers without allocation");
}
NumericProcessingGraph norm_graph(const char* norm){
    auto definition=graph(false,8);auto& polar=definition.nodes[1];polar.kind="ComplexPolar";
    polar.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{8}}},{"first_bin",NumericLiteral{std::uint64_t{0}}},{"positive_half",false},{"real_endpoints",false}}};
    if(norm)std::get<TransformProcessingParameters>(polar.parameters).values.push_back({"magnitude_norm",std::string(norm)});
    std::array<OwnedProcessingContract,2> output;
    check(complex_polar_output_contracts(polar,std::get<NumericContract>(polar.inputs[0].contract.view()),output)==Status::ok,"explicit magnitude norm contract");
    polar.outputs={{0,PortDirection::Output,output[0]},{1,PortDirection::Output,output[1]}};return definition;
}
struct NormObserved {
    float magnitude=0;std::size_t calls=0;
    static void output(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=2 || event.direction!=PortDirection::Output || event.identity.port)return;
        auto& self=*static_cast<NormObserved*>(user);check(event.status==Status::ok,"norm output status");
        check(load_element(std::get<NumericView>(event.frame.payload),{0,0,0},self.magnitude)==Status::ok,"norm output value");++self.calls;
    }
};
void norm_case(const char* norm,std::uint32_t expected,std::uint32_t re=0xbdc93b0fu,std::uint32_t im=0x3d8ae443u){
    const auto definition=norm_graph(norm);std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(definition,profile(),plan,state)==Status::ok,"prepare explicit magnitude norm");
    // Actual same-operand NumPy complex64 witness from PCM bee194... / v56.
    std::array<float,8> real{},imaginary{};real[0]=std::bit_cast<float>(re);imaginary[0]=std::bit_cast<float>(im);
    const auto data=definition.nodes[0].outputs[0].contract.view();const auto& input=std::get<NumericContract>(data);
    const PlaneLayout plane{0,{sizeof(float),0}};FrameStamp stamp;stamp.sequence=stamp.producer_epoch=1;stamp.has_sample_interval=true;stamp.sample_count=8;
    const auto first=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(real)),std::span(&plane,1),{8,1});
    const auto second=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(imaginary)),std::span(&plane,1),{8,1});
    NormObserved seen;const NumericRuntimeCallbacks callbacks{NormObserved::output,nullptr,nullptr,&seen};const auto before=allocations;
    check(plan->process_numeric_ports(1,0,first,*state,{},callbacks)==Status::ok,"retain real norm operand");
    check(plan->process_numeric_ports(1,1,second,*state,{},callbacks)==Status::ok,"publish selected magnitude norm");
    check(allocations==before && seen.calls==1,"norm binding makes one publication without allocations");
    check(std::bit_cast<std::uint32_t>(seen.magnitude)==expected,"selected norm matches native witness bits");
}
void bounded_fma_edges(){
    // Exact IEEE boundary results, plus the independently recorded CRT/native
    // failing ratio from v63. The test contains no replay of the implementation.
    constexpr std::array<std::array<std::uint32_t,2>,8> cases{{
        {0x00000000u,0x3f800000u},{0x00000001u,0x3f800000u},
        {0x3f800000u,0x40000000u},{0x3f7fffffu,0x3fffffffu},
        {0x397fffffu,0x3f800000u},{0x39800000u,0x3f800000u},
        {0x39800001u,0x3f800001u},{0x3e0451b7u,0x3f822323u}}};
    for(const auto& item:cases)check(std::bit_cast<std::uint32_t>(dsp_kernels::detail::fused_square_plus_one(std::bit_cast<float>(item[0])))==item[1],"bounded fused square/one native and IEEE boundary bits");
}
void scaled_ratio_binding(){
    bounded_fma_edges();norm_case("scaled_ratio_fma",0x3df48253u);norm_case(nullptr,0x3df48252u);
    norm_case("scaled_ratio_fma",0x3eb02834u,0xbd349987u,0x3eaeb45cu);
    norm_case("scaled_ratio_fma",0x40076f21u,0xbf4305eeu,0xbffcb51du);
    norm_case("scaled_ratio_fma",0x3f8e87aau,0xbf7cf98cu,0x3f03636bu);
}
}
void* operator new(std::size_t bytes){++allocations;if(void* pointer=std::malloc(bytes?bytes:1))return pointer;throw std::bad_alloc();}
void operator delete(void* pointer) noexcept{std::free(pointer);}
void operator delete(void* pointer,std::size_t) noexcept{std::free(pointer);}
int main(int argc,char** argv){
    if(argc==2 && std::string_view(argv[1])=="--scaled-ratio-only"){scaled_ratio_binding();std::puts("explicit ScaledRatioFma and unchanged default Hypot native witness/noalloc PASS");return 0;}
    complex_case();real_case("radix2");real_case("radix4");pairing_boundaries();algorithm_geometry();explicit_transform_parameters();explicit_amplitude_frequency();endpoint_contracts(false);endpoint_contracts(true);matrix_case(true);matrix_case(false);matrix_case(false,0);matrix_case(false,1);concatenate_dc();scaled_ratio_binding();std::puts("raw FFT: separate Re/Im, full C2C, full positive RFFT bins, radix2/4, Fs0, explicit parameters, owning pairs, shared matrix/ring processing and DC concatenation PASS");
}
