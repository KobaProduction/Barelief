#pragma once
#include <dataflow_runtime/runtime.h>
#include <dataflow_runtime/numeric_operation_binding.h>
#include <domain/numeric/common_operations.h>
#include <rhythm_graph_adapters/composition.h>
#include <rhythm_graph_adapters/contrast_composition.h>
#include <rhythm_graph_adapters/profile.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace operation_lifecycle {
using namespace signal_contracts;
using namespace domain;
using namespace dataflow_runtime;
inline void require(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}
}
inline NumericRuntimeProfile profile() {
    constexpr FrameLimits frames{1,131072,524288,65536,256,65536};
    NumericRuntimeProfile result{{32,64,8,16,256,131072,frames},
        {{4194304,4194304,4194304,131072},0,16777216},
        {131072,1,524288,524288,524288},{true,true,true}};
    result.music_enabled=true;
    return rhythm_graph_adapters::with_reference_operations(std::move(result));
}
inline OwnedProcessingContract contract(DType dtype,std::size_t maximum=1,bool variable=false) {
    static constexpr std::uint32_t mono[]{0};
    NumericContract value;value.dtype=dtype;value.channel_order=mono;
    value.unit=NumericUnit::Values;value.representation.alignment=dtype_descriptor(dtype)->alignment;
    if(maximum!=1 || variable){
        value.semantic=Semantic::Numeric;value.shape={Rank::Vector,{Axis::Declared,Axis::None},{{{variable?0:maximum,maximum},{1,1}}}};
        value.clock={"analysis",{1,100},0};
    }
    OwnedProcessingContract owned;
    require(own_processing_contract(value,profile().admission.frame,65536,owned)==Status::ok,"own explicit numeric contract");
    return owned;
}
inline ProcessingParameter scalar(std::string key,float value) {
    return {std::move(key),NumericLiteral{value}};
}
inline ConfiguredProcessingNode spectral_node(std::string_view kind,std::size_t bins=4) {
    std::vector<ProcessingPort> inputs{{0,PortDirection::Input,contract(DType::Float32,bins)}};
    TransformProcessingParameters parameters;
    ConfiguredProcessingNode node;
    require(rhythm_graph_adapters::make_spectral_operation_node(10,kind,inputs,std::move(parameters),profile().admission.frame,65536,node)==Status::ok,"compose spectral operation");
    return node;
}
inline ConfiguredProcessingNode contrast_node() {
    const std::array inputs{ProcessingPort{0,PortDirection::Input,contract(DType::Float32,32,true)},
        ProcessingPort{65500,PortDirection::Input,contract(DType::UInt8)}};
    TransformProcessingParameters parameters;
    parameters.values={scalar("frame_rate",100),scalar("threshold",.05f),scalar("ratio_threshold",0),
        scalar("combine_ms",10),scalar("pre_average_ms",40),scalar("pre_maximum_ms",20),
        {"maximum_onsets",NumericLiteral{std::uint64_t{32}}}};
    ConfiguredProcessingNode node;
    require(rhythm_graph_adapters::make_contrast_operation_node(10,"ContrastPeakAccumulator",inputs,std::move(parameters),profile().admission.frame,65536,node)==Status::ok,"compose explicit EOF accumulator");
    return node;
}
inline ConfiguredProcessingNode recursive_node() {
    ConfiguredProcessingNode node;node.id=10;node.kind="RecursiveFilter";
    node.inputs={{0,PortDirection::Input,contract(DType::Float32,4)}};
    node.parameters=TransformProcessingParameters{{{"numerator",NumericArray{{1}}},{"denominator",NumericArray{{1,-.5f}}}}};
    std::vector<OwnedProcessingContract> outputs;
    require(common_operation_output_contracts(node,std::get<NumericContract>(node.inputs[0].contract.view()),outputs)==Status::ok,"derive common operation outputs");
    for(std::size_t i=0;i<outputs.size();++i)node.outputs.push_back({PortId(i),PortDirection::Output,std::move(outputs[i])});
    return node;
}
inline ResolvedProcessingNode resolved(const ConfiguredProcessingNode& node) {
    return {node.id,node.kind,NodeRole::Processor,ProcessingOperation::NumericExtension,node.inputs,node.outputs,&node.parameters,node.enabled};
}
inline FrameStamp stamp(std::uint64_t sequence) {
    FrameStamp result;result.sequence=result.source_sequence=sequence;
    result.producer_epoch=result.generation=result.epoch=1;return result;
}
struct Operation {
    struct alignas(8) Buffer {std::array<std::byte,512> bytes{};};
    ConfiguredProcessingNode node;
    std::unique_ptr<PreparedNumericOperation> processor;
    std::array<NumericBufferContract,4> input_contracts{},output_contracts{};
    std::array<std::array<PlaneLayout,1>,4> input_planes{},output_planes{};
    std::array<Buffer,4> buffers;
    std::array<NumericOperationOutput,4> output_storage{};
    explicit Operation(ConfiguredProcessingNode value):node(std::move(value)) {
        auto p=profile();std::shared_ptr<const NumericOperationDescriptor> descriptor;
        require(p.operation_factory->describe(numeric_operation_node_view(resolved(node)),descriptor)==Status::ok,"describe owned operation");
        require(p.operation_factory->prepare(std::move(descriptor),numeric_operation_profile(p),processor)==Status::ok,"prepare owned operation");
        const auto physical=numeric_operation_node(node);
        for(std::size_t i=0;i<physical.inputs.size();++i){input_contracts[i]=physical.inputs[i].contract;input_planes[i][0]={0,{dtype_descriptor(input_contracts[i].dtype)->bytes,0}};}
        for(std::size_t i=0;i<physical.outputs.size();++i)prepare_output(i,physical.outputs[i].contract);
    }
    void prepare_output(std::size_t i,const NumericBufferContract& contract) {
        output_contracts[i]=contract;
        output_planes[i][0]={0,{dtype_descriptor(output_contracts[i].dtype)->bytes,0}};
        const auto rows=std::size_t(output_contracts[i].shape.extents[0].maximum);
        output_storage[i]={node.outputs[i].id,{&output_contracts[i],buffers[i].bytes,output_planes[i],{rows,1}}};
    }
    std::span<NumericOperationOutput> outputs() {return std::span(output_storage).first(node.outputs.size());}
    template<class T> NumericOperationInput input(std::size_t slot,std::span<const T> values,bool available=true) {
        NumericBufferView view{&input_contracts[slot],std::as_bytes(values),input_planes[slot],{values.size(),1}};
        NumericOperationInput result{node.inputs[slot].id,view,{},available?Status::ok:Status::unavailable};
        if constexpr(std::is_same_v<T,float>)result.contiguous_float32=values;
        return result;
    }
    float value(std::size_t port,std::size_t index=0) const {
        float result=0;
        require(load_element(output_storage[port].value.view(),{0,index,0},result)==Status::ok,"read operation Float32 output");
        return result;
    }
    void matches(std::size_t port,std::span<const float> expected) const {
        require(output_storage[port].status==Status::ok,"operation output available");
        require(output_storage[port].value.actual[0]==expected.size(),"operation actual extent");
        for(std::size_t i=0;i<expected.size();++i)require(value(port,i)==expected[i],"operation equals shared prepared kernel");
    }
};
struct Capture {
    std::array<float,64> values{};std::size_t rows=0,calls=0;
    Status status=Status::unavailable;FrameStamp last;std::uint64_t count=0;
    void receive_output(const NumericPortFrameView& event,const NumericView& view) {
        status=event.status;last=event.frame.stamp;last.stream_id={};++calls;
        if(event.status!=Status::ok || !event.frame.stamp.available){rows=0;return;}
        rows=view.actual[0];require(rows<=values.size(),"bounded callback copy");
        for(std::size_t i=0;i<rows;++i)require(load_element(view,{0,i,0},values[i])==Status::ok,"copy callback value");
    }

    static void output(void* user,const NumericPortFrameView& event) {
        if(event.identity.node!=10)return;
        auto& self=*static_cast<Capture*>(user);const auto& view=std::get<NumericView>(event.frame.payload);
        if(event.identity.port==1 && event.status==Status::ok){require(load_element(view,{0,0,0},self.count)==Status::ok,"read event count");return;}
        if(event.identity.port!=0)return;
        self.receive_output(event,view);
    }
    void matches(std::span<const float> expected) const {
        require(status==Status::ok && last.available,"runtime output available");require(rows==expected.size(),"runtime output extent");
        require(std::equal(expected.begin(),expected.end(),values.begin()),"runtime equals shared prepared kernel");
    }
};
inline NumericProcessingGraph graph_for(const ConfiguredProcessingNode& node) {
    NumericProcessingGraph graph;graph.graph_id="operation-lifecycle";graph.execution_interval={1,100};
    for(std::size_t i=0;i<node.inputs.size();++i){
        ConfiguredProcessingNode source;source.id=NodeId(i+1);source.kind="NetworkInput";
        source.outputs={{0,PortDirection::Output,node.inputs[i].contract}};graph.nodes.push_back(std::move(source));
        graph.edges.push_back({NodeId(i+1),node.id,0,node.inputs[i].id});
    }
    graph.nodes.push_back(node);return graph;
}
struct Running {
    NumericProcessingGraph graph;GraphExecutor executor{profile()};Capture capture;std::uint64_t sequence=0;
    explicit Running(ConfiguredProcessingNode node):graph(graph_for(node)) {auto prepared=prepare(graph);apply(std::move(prepared));}
    application::PreparedExecution prepare(const NumericProcessingGraph& next) {
        auto p=profile();application::PreparedExecution result;
        require(executor.prepare_numeric(next,{p.admission,p.resources},result)==Status::ok,"prepare numeric lifecycle graph");return result;
    }
    void apply(application::PreparedExecution&& next) {require(executor.apply(std::move(next))==Status::ok,"apply prepared lifecycle graph");}
    template<class T> void push(NodeId source,std::span<const T> values) {
        const auto value=std::get<NumericContract>(graph.nodes[std::size_t(source-1)].outputs[0].contract.view());
        std::array<T,64> storage{};const auto maximum=std::size_t(value.shape.extents[0].maximum);
        require(values.size()<=maximum && maximum<=storage.size(),"bounded declared ingress storage");
        std::copy(values.begin(),values.end(),storage.begin());
        const std::array<PlaneLayout,1> planes{{{0,{sizeof(T),0}}}};
        const DataFrameView frame{stamp(++sequence),NumericView{&value,std::as_bytes(std::span(storage).first(maximum)),planes,{values.size(),1}}};
        const auto status=executor.process_numeric_ports(source,frame,{}, {Capture::output,nullptr,nullptr,&capture});
        if(status!=Status::ok)std::fprintf(stderr,"lifecycle source=%d sequence=%llu status=%u\n",int(source),static_cast<unsigned long long>(sequence),unsigned(status));
        require(status==Status::ok,"accept numeric lifecycle input");
    }
    void control(std::uint8_t value) {push(2,std::span<const std::uint8_t>(&value,1));}
};
}
