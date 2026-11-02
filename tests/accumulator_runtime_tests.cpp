#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
using namespace signal_contracts;
using namespace domain;
using namespace dataflow_runtime;
namespace {
std::size_t allocations=0;
constexpr std::array<std::uint32_t,1> mono{0};
constexpr FrameLimits frames{8,32768,262144,16384,256,8192};
constexpr ProcessingAdmissionLimits limits{32,64,16,16,256,65536,frames};
void require(bool ok,const char* text){if(!ok){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
NumericRuntimeProfile profile(){return {limits,{{4194304,4194304,4194304,131072},0,16777216},{32768,8,262144,262144,262144},{true,true,true}};}
OwnedProcessingContract own(const NumericContract& input){
    OwnedProcessingContract result;require(own_processing_contract(input,frames,65536,result)==Status::ok,"own accumulator contract");return result;
}
NumericContract input_contract(DType dtype,bool scalar,std::uint32_t rate){
    NumericContract input;input.dtype=dtype;input.semantic=Semantic::Numeric;input.channel_order=mono;
    input.representation.alignment=dtype_descriptor(dtype)->alignment;input.clock={"values",{1,1},rate};
    if(!scalar)input.shape={Rank::Vector,{Axis::Declared,Axis::None},{ExtentBounds{0,1024},ExtentBounds{}}};
    return input;
}
NumericProcessingGraph graph(const NumericContract& input,std::size_t capacity){
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};
    source.outputs={{0,PortDirection::Output,own(input)}};
    ConfiguredProcessingNode accumulator;
    require(bool(make_processing_node("Window",2,limits,accumulator,{0,0,0,0})),"create generic accumulator without audio/FFT settings");
    std::get<TransformProcessingParameters>(accumulator.parameters).values={{"size",NumericLiteral{std::uint64_t(capacity)}}};
    require(bool(configure_processing_input(accumulator,input,limits)),"configure scalar/vector without Fs");
    require(accumulator.inputs.size()==2 && numeric_parameter_key("Window",input_sample_rate_input).empty(),"data and N only; no Fs port");
    NumericProcessingGraph result;result.graph_id="accumulator";result.nodes={source,accumulator};result.edges={{1,2,0,0}};return result;
}
template<class T> struct Running {
    NumericContract input;
    std::shared_ptr<const PreparedGraph> plan;
    std::unique_ptr<RuntimeContext> state;
    std::array<T,1024> values{};
    std::size_t count=0,calls=0;
    explicit Running(DType dtype,bool scalar,std::size_t capacity,std::uint32_t rate=0):input(input_contract(dtype,scalar,rate)){
        require(PreparedGraph::prepare_numeric(graph(input,capacity),profile(),plan,state)==Status::ok,"prepare arbitrary N/dtype without Fs");
    }
    static void output(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=2 || event.identity.port!=0 || event.direction!=PortDirection::Output)return;
        auto& self=*static_cast<Running*>(user);require(event.status==Status::ok && event.frame.stamp.available,"available accumulator output");
        const auto& value=std::get<NumericView>(event.frame.payload);self.count=value.actual[0]*value.actual[1];++self.calls;
        require(value.contract->dtype==self.input.dtype && self.count<=self.values.size(),"dtype and bounded output retained");
        for(std::size_t row=0;row<value.actual[0];++row)for(std::size_t column=0;column<value.actual[1];++column)
            require(load_element(value,{0,row,column},self.values[row*value.actual[1]+column])==Status::ok,"load exact retained value");
    }
    void feed(std::span<const T> values){
        const PlaneLayout plane{0,{sizeof(T),0}};FrameStamp stamp;stamp.sequence=calls+1;stamp.producer_epoch=1;stamp.timestamp_ticks=std::int64_t(calls+1);
        std::array<T,1024> storage{};std::copy(values.begin(),values.end(),storage.begin());
        const DataFrameView frame{stamp,NumericView{&input,std::as_bytes(std::span(storage)),std::span(&plane,1),{values.size(),1},{},{}}};
        const auto previous_calls=calls,before=allocations;
        const auto status=plan->process_numeric_ports(1,frame,*state,{},NumericRuntimeCallbacks{output,nullptr,nullptr,this});
        if(status!=Status::ok)std::fprintf(stderr,"dtype=%u input=%zu calls=%zu status=%u\n",unsigned(input.dtype),values.size(),calls,unsigned(status));
        require(status==Status::ok,"feed scalar/vector");
        require(allocations==before && calls==previous_calls+1,"one output per input without allocation or scheduling");
    }
    template<std::size_t N> void expect(const std::array<T,N>& expected){
        require(count==N && std::equal(expected.begin(),expected.end(),values.begin()),"exact retained tail and zero prefix");
    }
};
template<class T> void tail_cases(DType dtype){
    Running<T> run(dtype,false,5);run.feed(std::array<T,2>{1,2});run.expect(std::array<T,5>{0,0,0,1,2});
    run.feed(std::array<T,2>{3,4});run.expect(std::array<T,5>{0,1,2,3,4});
    run.feed(std::array<T,5>{9,10,11,12,13});run.expect(std::array<T,5>{9,10,11,12,13});
    run.feed(std::array<T,8>{20,21,22,23,24,25,26,27});run.expect(std::array<T,5>{23,24,25,26,27});
    run.feed(std::span<const T>{});run.expect(std::array<T,5>{23,24,25,26,27});
}
void scalar_and_precision(){
    Running<std::uint64_t> scalar(DType::UInt64,true,3);constexpr std::uint64_t large=9007199254740993ULL;
    scalar.feed(std::array{large});scalar.expect(std::array<std::uint64_t,3>{0,0,large});
    scalar.feed(std::array{UINT64_MAX});scalar.expect(std::array<std::uint64_t,3>{0,large,UINT64_MAX});
    scalar.feed(std::array<std::uint64_t,1>{7});scalar.expect(std::array<std::uint64_t,3>{large,UINT64_MAX,7});
    scalar.feed(std::array<std::uint64_t,1>{8});scalar.expect(std::array<std::uint64_t,3>{UINT64_MAX,7,8});
}
void arbitrary_sizes_and_rates(){
    std::array<std::int32_t,768> data{};for(std::size_t index=0;index<data.size();++index)data[index]=std::int32_t(index+1);
    for(const auto rate:{0U,1U,44100U,48000U})for(const auto size:{1U,10U,200U,500U}){
        Running<std::int32_t> run(DType::Int32,false,size,rate);run.feed(data);
        require(run.count==size,"arbitrary positive N");
        for(std::size_t index=0;index<size;++index)require(run.values[index]==data[data.size()-size+index],"tail independent of Fs");
    }
}
void no_bypass_and_limits(){
    auto definition=graph(input_contract(DType::UInt32,false,0),10);definition.nodes[1].enabled=false;
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    require(PreparedGraph::prepare_numeric(definition,profile(),plan,state)!=Status::ok && !plan,"accumulator bypass rejected atomically");
    auto& fields=std::get<TransformProcessingParameters>(definition.nodes[1].parameters).values;
    definition.nodes[1].enabled=true;fields={{"size",NumericLiteral{std::uint64_t{0}}}};
    require(!derive_processing_node_contracts(definition.nodes[1],limits),"zero capacity rejected");
    for(const auto* kind:{"Spectrogram","NumberAccumulator"}){
        ConfiguredProcessingNode node;require(bool(make_processing_node(kind,2,limits,node)),"create history primitive");
        node.enabled=false;ResolvedProcessingNode resolved;
        require(!resolve_processing_node(node,limits,resolved),"history primitive has no bypass");
    }
}
void history_without_fs(bool scalar){
    Running<float> run(DType::Float32,scalar,3);if(!scalar)run.input.shape.extents[0]={2,2};
    auto definition=graph(run.input,3);ConfiguredProcessingNode history;
    require(bool(make_processing_node(scalar?"NumberAccumulator":"Spectrogram",2,limits,history,{0,0,0,3})),"create history without audio settings");
    require(bool(configure_processing_input(history,run.input,limits)),"history accepts Fs0");definition.nodes[1]=std::move(history);
    require(PreparedGraph::prepare_numeric(definition,profile(),run.plan,run.state)==Status::ok,"prepare history without Fs");
    if(scalar){
        run.feed(std::array<float,1>{1});run.expect(std::array<float,3>{0,0,1});
        run.feed(std::array<float,1>{2});run.feed(std::array<float,1>{3});run.expect(std::array<float,3>{1,2,3});
    }else{
        run.feed(std::array<float,2>{1,11});run.expect(std::array<float,6>{0,0,0,0,1,11});
        run.feed(std::array<float,2>{2,12});run.feed(std::array<float,2>{3,13});run.expect(std::array<float,6>{1,11,2,12,3,13});
    }
}
}
void* operator new(std::size_t bytes){++allocations;if(void* pointer=std::malloc(bytes))return pointer;throw std::bad_alloc();}
void operator delete(void* pointer) noexcept{std::free(pointer);}
void operator delete(void* pointer,std::size_t) noexcept{std::free(pointer);}
int main(){
    tail_cases<float>(DType::Float32);tail_cases<double>(DType::Float64);
    tail_cases<std::int8_t>(DType::Int8);tail_cases<std::uint8_t>(DType::UInt8);
    tail_cases<std::int16_t>(DType::Int16);tail_cases<std::uint16_t>(DType::UInt16);
    tail_cases<std::int32_t>(DType::Int32);tail_cases<std::uint32_t>(DType::UInt32);
    tail_cases<std::int64_t>(DType::Int64);tail_cases<std::uint64_t>(DType::UInt64);
    scalar_and_precision();arbitrary_sizes_and_rates();no_bypass_and_limits();history_without_fs(true);history_without_fs(false);
    std::puts("generic scalar/vector accumulation: exact tail/replace/shift, ten dtypes, arbitrary N, no Fs/bypass PASS");
}
