#include <dataflow_runtime/runtime.h>
#include "numeric_test_builders.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

using namespace domain;
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
std::size_t allocations=0;
void require(bool condition,const char* message){if(!condition){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
constexpr FrameLimits limits{1,131072,524288,32768,256,32768};
constexpr std::array<std::uint32_t,1> channels{0};
NumericRuntimeProfile profile(){return {{32,64,8,16,256,131072,limits},{{4194304,4194304,4194304,131072},0,16777216},{131072,1,524288,524288,524288},{true,true,true}};}
ProcessingPort port(PortDirection direction,const NumericContract& input,PortId id=0){
    return numeric_test_builders::port(id,direction,input,limits,131072,require,"own matrix contract");
}
NumericContract matrix_contract(){NumericContract input;input.dtype=DType::Float32;input.semantic=Semantic::Numeric;input.channel_order=channels;
    input.shape={Rank::Matrix,{Axis::Band,Axis::Time},{ExtentBounds{0,3},ExtentBounds{0,8}}};
    input.clock={"center-test",{1,48000},48000};input.coordinates[0]={CoordinateMode::Regular,NumericUnit::Values,{0,1},{1,1}};
    input.coordinates[1]={CoordinateMode::Regular,NumericUnit::Seconds,{0,1},{1,120}};return input;}
TransformProcessingParameters parameters(unsigned method,double offset=0){return {{{"method",NumericLiteral{std::uint64_t(method)}},{"offset",NumericLiteral{offset}}}};}
NumericProcessingGraph center_graph(const NumericContract& input,unsigned method=0,double offset=0,bool enabled=true,bool fft=false){
    auto source=numeric_test_builders::node(1,"NetworkInput",EndpointProcessingParameters{},
        {},{port(PortDirection::Output,input)});
    OwnedProcessingContract centered;
    require(matrix_center_output_contract(input,centered)==Status::ok,"derive centered matrix");
    auto center=numeric_test_builders::node(2,"MatrixCenter",parameters(method,offset),
        {port(PortDirection::Input,input)},
        {numeric_test_builders::port(0,PortDirection::Output,centered)},enabled);
    auto graph=numeric_test_builders::graph("center-test",{1,120},
        {source,center},{numeric_test_builders::edge(1,0,2,0)});
    if(fft){ConfiguredProcessingNode window;window.id=3;window.kind="MatrixWindowFunction";
        window.parameters=TransformProcessingParameters{{{"function",std::string{"hann"}}}};window.inputs={{0,PortDirection::Input,centered}};
        OwnedProcessingContract windowed,spectrum;require(matrix_window_output_contract(std::get<NumericContract>(centered.view()),windowed)==Status::ok,"derive downstream window");
        window.outputs={{0,PortDirection::Output,windowed}};graph.nodes.push_back(window);
        ConfiguredProcessingNode transform;transform.id=4;transform.kind="MatrixFFT";transform.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{256}}}}};
        transform.inputs={{0,PortDirection::Input,windowed}};require(matrix_fft_output_contract(std::get<NumericContract>(windowed.view()),256,spectrum)==Status::ok,"derive downstream FFT");
        transform.outputs={{0,PortDirection::Output,spectrum}};graph.nodes.push_back(transform);graph.edges.insert(graph.edges.end(),{{2,3},{3,4}});}
    return graph;
}
struct Probe {
    NodeId center_node=2,fft_node=4;bool available=false,fft_available=false,numeric_semantic=true;Status status=Status::ok;
    std::size_t ring_capacity=0;
    const NumericContract* expected_contract=nullptr;
    std::array<float,1024> values{},spectrum{};std::array<std::size_t,2> actual{};
    std::array<std::array<std::byte,512>,2> coordinates{};std::array<std::size_t,2> coordinate_bytes{};
    static void output(void* user,const NumericPortFrameView& frame){auto& probe=*static_cast<Probe*>(user);
        if(frame.identity.node!=probe.center_node && frame.identity.node!=probe.fft_node)return;
        const bool fft=frame.identity.node==probe.fft_node;
        if(fft)probe.fft_available=frame.frame.stamp.available;
        else {probe.available=frame.frame.stamp.available;probe.status=frame.status;}
        const auto& value=std::get<NumericView>(frame.frame.payload);
        if(!frame.frame.stamp.available){require(value.storage.empty(),"unavailable output has no stale payload");return;}
        require(value.actual[0]*value.actual[1]<=probe.values.size(),"bounded matrix observation");
        if(!fft){probe.actual=value.actual;probe.ring_capacity=value.ring.capacity;require((!probe.numeric_semantic || value.contract->semantic==Semantic::Numeric) && value.contract->representation.normalization==Normalization::None,"center output is ordinary unrestricted numeric");
            if(probe.expected_contract)require(same_contract(DataContract{*value.contract},DataContract{*probe.expected_contract}),"bypass publishes its declared output contract");
            for(std::size_t axis=0;axis<2;++axis){const auto bytes=value.coordinates.axes[axis];require(bytes.size()<=probe.coordinates[axis].size(),"bounded coordinates");
                probe.coordinate_bytes[axis]=bytes.size();std::copy(bytes.begin(),bytes.end(),probe.coordinates[axis].begin());}}
        auto& destination=fft?probe.spectrum:probe.values;
        for(std::size_t row=0;row<value.actual[0];++row)for(std::size_t column=0;column<value.actual[1];++column)
            require(load_element(value,{0,row,column},destination[row*value.actual[1]+column])==Status::ok && std::isfinite(destination[row*value.actual[1]+column]),"read finite matrix output");
    }
    NumericRuntimeCallbacks callbacks(){return {output,nullptr,nullptr,this};}
};
struct Running {
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    explicit Running(const NumericProcessingGraph& graph){require(PreparedGraph::prepare_numeric(graph,profile(),plan,context)==Status::ok,"prepare MatrixCenter graph");}
    void replace(const NumericProcessingGraph& graph){std::shared_ptr<const PreparedGraph> next;std::unique_ptr<RuntimeContext> state;
        require(PreparedGraph::prepare_numeric(graph,profile(),next,state,plan.get(),context.get())==Status::ok,"replace compatible MatrixCenter graph");plan=std::move(next);context=std::move(state);}
};
Status push(Running& running,const NumericContract& input,std::span<const float> values,std::array<std::size_t,2> actual,std::size_t ordinal,Probe& probe,CoordinateView coordinates={}){
    const std::array<PlaneLayout,1> planes{{{0,{std::size_t(input.shape.extents[1].maximum)*4,4}}}};
    FrameStamp stamp;stamp.sequence=stamp.source_sequence=ordinal+1;stamp.producer_epoch=7;stamp.generation=1;stamp.timestamp_ticks=std::int64_t(ordinal*400);
    const auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(values),planes,actual,coordinates);
    const auto before=allocations;const auto status=running.plan->process_numeric_ports(1,frame,*running.context,{},probe.callbacks());
    require(allocations==before,"MatrixCenter processing never allocates");return status;
}
void near(float value,long double expected){const auto rounded=float(expected);require(std::abs(double(value)-double(rounded))<=1e-6*std::max(1.,std::abs(double(rounded))),"independent per-row centering oracle");}
void oracle(const Probe& probe,std::span<const float> data,std::size_t stride,unsigned method,double offset=0,bool bypass=false){
    require(probe.available && probe.status==Status::ok,"finite input is available immediately");
    for(std::size_t row=0;row<probe.actual[0];++row){const auto samples=data.subspan(row*stride,probe.actual[1]);long double reference=offset;
        if(bypass)reference=0;
        else if(samples.empty())reference=0;
        else if(method==0){reference=0;for(float value:samples)reference+=value;reference/=samples.size();}
        else if(method==1){const auto extrema=std::minmax_element(samples.begin(),samples.end());reference=(static_cast<long double>(*extrema.first)+static_cast<long double>(*extrema.second))/2;}
        for(std::size_t column=0;column<samples.size();++column)near(probe.values[row*probe.actual[1]+column],static_cast<long double>(samples[column])-reference);
    }
}
void methods_and_bypass(){
    const auto input=matrix_contract();std::array<float,24> data{1,2,3,10,0,0,0,0,-5,-2,6,9,0,0,0,0,100,100,100,104};
    for(unsigned method=0;method<3;++method){auto graph=center_graph(input,method,2.25);Running running(graph);Probe probe;
        require(push(running,input,data,{3,4},0,probe)==Status::ok,"all centering methods process");oracle(probe,data,8,method,2.25);
        if(method==0)require(probe.values[0]==-3,"mean uses each asymmetric row independently");
        if(method==1)require(probe.values[0]==-4.5f,"midrange differs from mean on asymmetric data");}
    auto bypassed=center_graph(input,2,10,false);Running bypass(bypassed);Probe probe;
    require(push(bypass,input,data,{3,4},0,probe)==Status::ok,"compatible disabled center forwards");oracle(probe,data,8,2,10,true);
    auto unit=input;unit.representation.normalization=Normalization::FloatUnit;std::fill(data.begin(),data.end(),.75f);
    Running zeros(center_graph(unit,0,0,false));require(push(zeros,unit,data,{3,4},0,probe)==Status::ok && probe.available,"incompatible disabled center publishes typed zeros");
    for(std::size_t index=0;index<12;++index)require(probe.values[index]==0,"FloatUnit-to-None disabled center follows generic typed-zero policy");
}
void variable_geometry_and_coordinates(){
    auto input=matrix_contract();input.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{0,1}};
    input.coordinates[1]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{0,1}};
    for(unsigned method:{0u,1u,2u}){Running running(center_graph(input,method,.125));Probe probe;std::array<float,24> data{};
        for(std::size_t length=0;length<=8;++length)for(std::size_t rows=0;rows<=3;++rows){
            for(std::size_t index=0;index<data.size();++index)data[index]=float(std::sin(double(index)+.3)*7);
            std::array<double,3> frequencies{10,100,1000};std::array<double,8> times{0,.1,.2,.3,.4,.5,.6,.7};
            const CoordinateView coordinates{{std::as_bytes(std::span(frequencies).first(rows)),std::as_bytes(std::span(times).first(length))}};
            require(push(running,input,data,{rows,length},length*4+rows,probe,coordinates)==Status::ok,"empty/singleton/variable geometry processes");
            require(probe.actual==std::array{rows,length},"actual geometry preserved");oracle(probe,data,8,method,.125);
            for(std::size_t axis=0;axis<2;++axis)require(probe.coordinate_bytes[axis]==coordinates.axes[axis].size() &&
                std::equal(coordinates.axes[axis].begin(),coordinates.axes[axis].end(),probe.coordinates[axis].begin()),"coordinates preserved exactly");}
    }
}
void constants_before_window_fft(){
    const auto input=matrix_contract();Running running(center_graph(input,0,0,true,true));Probe probe;std::array<float,24> data{};
    for(std::size_t row=0;row<3;++row)std::fill_n(data.begin()+row*8,8,row==0?7.f:row==1?-3.f:std::numeric_limits<float>::max());
    require(push(running,input,data,{3,8},0,probe)==Status::ok && probe.available && probe.fft_available,"constant signals reach center/window/FFT without warmup");
    for(std::size_t index=0;index<24;++index)require(probe.values[index]==0,"constant row centers to exact zero before window");
    for(std::size_t index=0;index<3*128;++index)require(probe.spectrum[index]==0,"constant centered row has exactly zero FFT");
    std::fill(data.begin(),data.end(),0);require(push(running,input,data,{3,8},1,probe)==Status::ok && probe.fft_available,"zero signals stay available");
}
void finite_and_overflow(){
    const auto input=matrix_contract();const auto maximum=std::numeric_limits<float>::max();std::array<float,24> data{};Probe probe;
    for(unsigned method:{0u,1u}){Running running(center_graph(input,method));data[0]=-maximum;data[1]=maximum;
        require(push(running,input,data,{1,2},0,probe)==Status::ok,"balanced Float32 extremes do not overflow reference");oracle(probe,data,8,method);
        data[0]=data[1]=maximum;require(push(running,input,data,{1,2},1,probe)==Status::ok && probe.values[0]==0 && probe.values[1]==0,"extreme constant reference uses double arithmetic");
        data[0]=std::numeric_limits<float>::quiet_NaN();require(push(running,input,data,{1,2},2,probe)==Status::invalid_argument && !probe.available,"nonfinite input rejected without stale publication");}
    Running manual(center_graph(input,2,maximum));data[0]=-maximum;
    require(push(manual,input,data,{1,1},0,probe)==Status::overflow && !probe.available,"manual subtraction overflow rejected");
    Running mean(center_graph(input));data[0]=-maximum;data[1]=data[2]=maximum;
    require(push(mean,input,data,{1,3},0,probe)==Status::overflow && !probe.available,"centered result beyond Float32 range rejected");
    for(auto invalid:center_graph(input,2,std::numeric_limits<double>::infinity()).nodes)if(invalid.kind=="MatrixCenter"){
        ResolvedProcessingNode resolved;require(!resolve_processing_node(invalid,profile().admission,resolved),"manual offset must be finite and bounded");}
    auto invalid=center_graph(input,3);ResolvedProcessingNode resolved;require(!resolve_processing_node(invalid.nodes[1],profile().admission,resolved),"unknown method rejected");
}
NumericProcessingGraph history_graph(){
    auto vector=matrix_contract();vector.shape={Rank::Vector,{Axis::Band,Axis::None},{ExtentBounds{4,4},ExtentBounds{1,1}}};vector.coordinates[1]={};
    auto history=vector;history.semantic=Semantic::History;history.history_element=vector.semantic;
    history.shape={Rank::Matrix,{Axis::Time,Axis::Band},{ExtentBounds{0,3},ExtentBounds{4,4}}};
    history.coordinates={AxisCoordinates{CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{0,1}},vector.coordinates[0]};
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={port(PortDirection::Output,vector)};
    ConfiguredProcessingNode retained;retained.id=2;retained.kind="Spectrogram";retained.parameters=HistoryProcessingParameters{3};
    retained.inputs={port(PortDirection::Input,vector)};retained.outputs={port(PortDirection::Output,history)};
    auto count=vector;count.dtype=DType::UInt64;count.semantic=Semantic::Scalar;count.shape={};count.coordinates={};count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;
    retained.outputs.push_back(port(PortDirection::Output,count,1));
    ConfiguredProcessingNode center;center.id=3;center.kind="MatrixCenter";center.parameters=parameters(0);center.inputs={port(PortDirection::Input,history)};
    OwnedProcessingContract centered;require(matrix_center_output_contract(history,centered)==Status::ok,"derive retained matrix center");center.outputs={{0,PortDirection::Output,centered}};
    return numeric_test_builders::graph("center-history",{1,120},{source,retained,center},
        {numeric_test_builders::edge(1,0,2,0),numeric_test_builders::edge(2,0,3,0)});
}
void history_ring_and_compatible_changes(){
    auto graph=history_graph();const auto centered_owner=graph.nodes[2].outputs[0].contract;
    const auto declared=std::get<NumericContract>(centered_owner.view());
    Running running(graph);Probe probe;probe.center_node=3;probe.fft_node=-1;probe.expected_contract=&declared;
    const auto input_owner=graph.nodes[0].outputs[0].contract;
    const auto input=std::get<NumericContract>(input_owner.view());
    auto event=[&](std::size_t ordinal){const std::array<float,4> values{float(ordinal+1),float(ordinal+2),float(ordinal+8),float(ordinal+10)};
        const std::array<PlaneLayout,1> planes{{{0,{4,0}}}};FrameStamp stamp;stamp.sequence=stamp.source_sequence=ordinal+1;stamp.producer_epoch=7;stamp.generation=1;stamp.timestamp_ticks=std::int64_t(ordinal*400);
        const auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(values)),planes,{4,1});const auto before=allocations;
        require(running.plan->process_numeric_ports(1,frame,*running.context,{},probe.callbacks())==Status::ok,"retained ring centers successfully");require(allocations==before,"ring centering never allocates");};
    for(std::size_t ordinal=0;ordinal<5;++ordinal)event(ordinal);
    for(std::size_t row=0;row<3;++row){const std::array<float,4> expected{-4.25f,-3.25f,2.75f,4.75f};
        for(std::size_t column=0;column<4;++column)require(probe.values[row*4+column]==expected[column],"wrapped History centered in logical order");
        double time=0;std::memcpy(&time,probe.coordinates[0].data()+row*8,8);require(std::abs(time-double(row+2)/120.)<1e-12,"logical ring Time coordinates preserved");}
    DataFrameView snapshot;std::uint64_t filled=0,ordinal=0;
    require(running.plan->history(2,0,*running.context,snapshot,&filled,&ordinal)==Status::ok && filled==3 && ordinal==5,"History full before center edit");
    graph.nodes[2].parameters=parameters(1);running.replace(graph);
    require(running.plan->history(2,0,*running.context,snapshot,&filled,&ordinal)==Status::ok && filled==3 && ordinal==5,"center method edit preserves compatible History");event(5);
    const std::array<float,4> midpoint{-4.5f,-3.5f,2.5f,4.5f};for(std::size_t index=0;index<12;++index)require(probe.values[index]==midpoint[index%4],"midrange applied after compatible change");
    graph.nodes[2].enabled=false;running.replace(graph);event(6);
    require(probe.values[0]==5 && probe.values[8]==7,"disabled compatible center forwards logical History rows");
    require(probe.ring_capacity==0 && running.plan->history(3,0,*running.context,snapshot)==Status::incompatible_contract,"disabled center materializes ordinary matrix without false History identity");
    require(running.plan->history(2,0,*running.context,snapshot)==Status::ok,"parent History remains available after center bypass");
    const auto& bypass_source=std::get<NumericView>(snapshot.payload);
    require(probe.coordinate_bytes[0]==bypass_source.coordinates.axes[0].size() && std::equal(bypass_source.coordinates.axes[0].begin(),bypass_source.coordinates.axes[0].end(),probe.coordinates[0].begin()),"center bypass preserves logical ring coordinates");
    graph.nodes[2].enabled=true;const auto retained=graph.nodes[1].outputs[0].contract;
    ConfiguredProcessingNode gate;gate.id=4;gate.kind="Gate";gate.parameters=RouteProcessingParameters{1,0,false,1};
    gate.inputs={{0,PortDirection::Input,retained}};gate.outputs={{0,PortDirection::Output,retained}};
    graph.nodes.push_back(gate);graph.edges={{1,2},{2,4},{4,3}};running.replace(graph);event(7);
    for(std::size_t index=0;index<12;++index)require(probe.values[index]==0,"center honors ring silence masks");
    require(running.plan->history(2,0,*running.context,snapshot,&filled,&ordinal)==Status::ok && filled==3 && ordinal==8,"route change preserves and advances History");
    graph.nodes[2].enabled=false;running.replace(graph);event(8);
    require(probe.available && probe.ring_capacity==0,"disabled masked center publishes ordinary available matrix");
    for(std::size_t index=0;index<12;++index)require(probe.values[index]==0,"disabled center materialization honors ring masks");
}
void window_ring_bypass(){
    auto graph=history_graph();const auto history=graph.nodes[1].outputs[0].contract;
    const auto data=history.view();OwnedProcessingContract windowed;
    require(matrix_window_output_contract(std::get<NumericContract>(data),windowed)==Status::ok,"derive direct ring window");
    graph.nodes[2].kind="MatrixWindowFunction";graph.nodes[2].parameters=TransformProcessingParameters{{{"function",std::string{"hann"}}}};
    graph.nodes[2].enabled=false;graph.nodes[2].outputs={{0,PortDirection::Output,windowed}};
    const auto declared=std::get<NumericContract>(windowed.view());
    Running running(graph);Probe probe;probe.center_node=3;probe.fft_node=-1;probe.numeric_semantic=false;probe.expected_contract=&declared;
    const auto input_owner=graph.nodes[0].outputs[0].contract;
    const auto input=std::get<NumericContract>(input_owner.view());
    auto event=[&](std::size_t ordinal){const std::array<float,4> values{float(ordinal+1),float(ordinal+2),float(ordinal+8),float(ordinal+10)};
        const std::array<PlaneLayout,1> planes{{{0,{4,0}}}};FrameStamp stamp;stamp.sequence=stamp.source_sequence=ordinal+1;stamp.producer_epoch=7;stamp.generation=1;stamp.timestamp_ticks=std::int64_t(ordinal*400);
        const auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(values)),planes,{4,1});const auto before=allocations;
        require(running.plan->process_numeric_ports(1,frame,*running.context,{},probe.callbacks())==Status::ok,"disabled ring window processes");
        require(allocations==before,"disabled ring window materialization never allocates");};
    for(std::size_t ordinal=0;ordinal<5;++ordinal)event(ordinal);
    require(probe.available && probe.ring_capacity==0 && probe.values[0]==3 && probe.values[8]==5,"disabled window materializes logical wrapped rows");
    DataFrameView snapshot;require(running.plan->history(3,0,*running.context,snapshot)==Status::incompatible_contract,"disabled window is not an ancestor History alias");
    require(running.plan->history(2,0,*running.context,snapshot)==Status::ok,"retained parent History remains available");
    const auto& retained=std::get<NumericView>(snapshot.payload);
    require(probe.coordinate_bytes[0]==retained.coordinates.axes[0].size() && std::equal(retained.coordinates.axes[0].begin(),retained.coordinates.axes[0].end(),probe.coordinates[0].begin()),"disabled window preserves logical ring coordinates");
    ConfiguredProcessingNode gate;gate.id=4;gate.kind="Gate";gate.parameters=RouteProcessingParameters{1,0,false,1};
    gate.inputs={{0,PortDirection::Input,history}};gate.outputs={{0,PortDirection::Output,history}};
    graph.nodes.push_back(gate);graph.edges={{1,2},{2,4},{4,3}};running.replace(graph);event(5);
    require(probe.ring_capacity==0,"masked disabled window output is ordinary");
    for(std::size_t index=0;index<12;++index)require(probe.values[index]==0,"disabled window materialization honors ring masks");
}
void contract_tests(){
    auto input=matrix_contract();input.representation.normalization=Normalization::FloatUnit;OwnedProcessingContract output;
    require(matrix_center_output_contract(input,output)==Status::ok,"derive FloatUnit matrix center");
    const auto result=std::get<NumericContract>(output.view());require(result.shape.extents==input.shape.extents && result.shape.axes==input.shape.axes && result.coordinates==input.coordinates && result.clock.sample_rate==48000,"center preserves geometry coordinates and clock");
    require(result.semantic==Semantic::Numeric && result.representation.normalization==Normalization::None,"center declares unrestricted output");
    input.dtype=DType::Float64;require(matrix_center_output_contract(input,output)!=Status::ok,"non-Float32 input refused");
    auto defaults=center_graph(matrix_contract());defaults.nodes[1].parameters=TransformProcessingParameters{};ResolvedProcessingNode node;
    require(bool(resolve_processing_node(defaults.nodes[1],profile().admission,node)),"registered center defaults resolve");
    ProcessingParameterView method,offset;require(resolve_processing_parameter(node,"method",method)==Status::ok && resolve_processing_parameter(node,"offset",offset)==Status::ok,"registered center parameter keys");
    require(std::visit([](auto value){return value==0;},std::get<NumericLiteral>(method)) && std::visit([](auto value){return value==0;},std::get<NumericLiteral>(offset)),"mean and zero offset are defaults");
}
} // namespace
void* operator new(std::size_t bytes){++allocations;auto* data=std::malloc(bytes?bytes:1);if(!data)std::abort();return data;}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* data) noexcept{std::free(data);}void operator delete[](void* data) noexcept{std::free(data);}
void operator delete(void* data,std::size_t) noexcept{std::free(data);}void operator delete[](void* data,std::size_t) noexcept{std::free(data);}
int main(){contract_tests();methods_and_bypass();variable_geometry_and_coordinates();constants_before_window_fft();finite_and_overflow();history_ring_and_compatible_changes();window_ring_bypass();
    std::puts("MatrixCenter mean/midrange/manual/bypass/variable/ring/coordinates/finite/overflow/History/window/FFT/no-allocation PASS");}
