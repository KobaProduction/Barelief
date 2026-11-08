#include <dataflow_runtime/runtime.h>
#include <dsp_graph_adapters/profile.h>
#include <domain/graph/node_factory.h>
#include "numeric_test_builders.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <cstring>

namespace {
using namespace signal_contracts;using namespace domain;using namespace dataflow_runtime;
std::size_t allocations=0;
void check(bool condition,const char* why){if(!condition){std::fprintf(stderr,"reference geometry: %s\n",why);std::exit(1);}}
NumericRuntimeProfile profile() {
    NumericRuntimeProfile p{{32,64,8,32,1048576,16777216,{8,800000,3200000,65536,4096,65536}},
        {{16777216,67108864,33554432,1048576},0,268435456},{800000,8,3200000,3200000,16777216},{true,true,true}};
    return dsp_graph_adapters::with_dsp_operations(std::move(p));
}
OwnedProcessingContract own(const NumericContract& input) {
    OwnedProcessingContract result;check(own_processing_contract(input,profile().admission.frame,16777216,result)==Status::ok,"own");return result;
}
void retired_pcm_onset_factory_refused() {
    const auto factory=dsp_graph_adapters::make_dsp_operation_factory();
    NumericOperationNodeView node;node.id=2;node.kind="PcmOnsetEnvelope";
    std::shared_ptr<const NumericOperationDescriptor> descriptor;
    check(factory->describe(node,descriptor)==Status::unavailable && !descriptor,"retired PCM onset has no operation descriptor");
    check(!factory->assess(node,NumericOperationProfile{}).supported,"retired PCM onset cannot be prepared");
}
NumericContract input_contract(bool matrix) {
    static const std::uint32_t mono=0;NumericContract result;result.channel_order={&mono,1};result.dtype=DType::Float32;
    result.clock={"local",{1,44100},44100};result.shape.rank=matrix?Rank::Matrix:Rank::Vector;
    result.shape.axes={Axis::Declared,matrix?Axis::Declared:Axis::None};
    result.shape.extents={ExtentBounds{0,matrix?8u:4096u},ExtentBounds{matrix?4u:1u,matrix?4u:1u}};
    if(matrix)result.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds};
    return result;
}
NumericProcessingGraph graph(const NumericContract& input,std::string_view kind,TransformProcessingParameters params,bool enabled) {
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";
    source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{44100}}}}};
    source.outputs={{0,PortDirection::Output,own(input)}};
    ConfiguredProcessingNode operation;check(bool(make_processing_node(kind,2,profile().admission,operation)),"create operation");operation.enabled=enabled;operation.parameters=std::move(params);
    operation.inputs={{0,PortDirection::Input,source.outputs[0].contract}};
    check(bool(derive_processing_node_contracts(operation,profile().admission)),"derive");
    NumericProcessingGraph result;result.graph_id="reference-geometry";result.nodes={source,operation};result.edges={{1,2,0,0}};return result;
}
struct Seen {
    std::size_t rows=0,columns=0,calls=0;bool coordinates=false;
    static void output(void* context,const NumericPortFrameView& event) {
        if(event.identity.node!=2)return;auto& self=*static_cast<Seen*>(context);
        check(event.status==Status::ok,"disabled output is available");const auto& value=std::get<NumericView>(event.frame.payload);
        check(value.actual==std::array<std::size_t,2>{self.rows,self.columns},"actual extents, without fabricated rows");
        if(self.coordinates)check(value.coordinates.axes[0].size()==2*sizeof(double),"actual row coordinates retained");
        for(std::size_t r=0;r<self.rows;++r)for(std::size_t c=0;c<self.columns;++c){float v=1;check(load_element(value,{0,r,c},v)==Status::ok && v==0,"disabled zeros");}
        ++self.calls;
    }
};
void execute(const NumericProcessingGraph& graph,const NumericContract& input,std::span<const float> values,std::array<std::size_t,2> shape,Seen& seen) {
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"prepare");
    const PlaneLayout plane{0,{input.shape.rank==Rank::Matrix?shape[1]*4:4,input.shape.rank==Rank::Matrix?4u:0u}};
    FrameStamp stamp;stamp.producer_epoch=1;stamp.sequence=stamp.source_sequence=1;
    auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(values),std::span(&plane,1),shape);
    const std::array<double,2> times{0,.25};
    if(seen.coordinates)std::get<NumericView>(frame.payload).coordinates.axes[0]=std::as_bytes(std::span(times));
    check(plan->process_numeric_ports(1,0,frame,*state,{},NumericRuntimeCallbacks{Seen::output,nullptr,nullptr,&seen})==Status::ok,"process");
    check(seen.calls==1,"one output");
}
void matrix_disabled() {
    auto input=input_contract(true);input.unit=NumericUnit::Radians;input.quantity=Quantity::SpectrumPhase;
    const auto g=graph(input,"IndexExtract",TransformProcessingParameters{{{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",NumericArray{{1,0}}}}},false);
    const auto output=std::get<NumericContract>(g.nodes[1].outputs[0].contract.view());
    check(output.unit==input.unit && output.quantity==input.quantity,"permutation retains value units");
    const std::array<float,32> values{1,2,3,4,5,6,7,8};Seen seen{2,2,0,true};execute(g,input,values,{2,4},seen);
}
void framing_disabled() {
    const auto input=input_contract(false);
    const auto g=graph(input,"TemporalFrameSequence",TransformProcessingParameters{{{"window_length",NumericLiteral{std::uint64_t{2048}}},
        {"hop",NumericLiteral{std::uint64_t{1024}}},{"center",false},{"tail",NumericLiteral{std::uint64_t{1}}}}},false);
    const std::array<float,4096> values{};Seen seen{1,2048};execute(g,input,values,{2048,1},seen);
}
NumericProcessingGraph fft_graph(bool separate_producer) {
    auto input=input_contract(false);input.shape.extents[0]={4,4};
    auto result=graph(input,"IndexExtract",TransformProcessingParameters{{{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",NumericArray{{0,1,2,3}}}}},true);
    result.nodes[1].kind="ArrayMultiply";result.nodes[1].parameters=TransformProcessingParameters{};
    auto& multiply=result.nodes[1];multiply.inputs.push_back({1,PortDirection::Input,own(input)});
    check(bool(derive_processing_node_contracts(multiply,profile().admission)),"derive product");
    auto second=result.nodes[0];second.id=3;result.nodes.push_back(std::move(second));
    result.edges.push_back({separate_producer?3:1,2,0,1});
    ConfiguredProcessingNode fft;check(bool(make_processing_node("Fourier",4,profile().admission,fft)),"create FFT");
    fft.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{4}}}}};
    fft.inputs={{0,PortDirection::Input,result.nodes[1].outputs[0].contract},{1,PortDirection::Input,result.nodes[1].outputs[0].contract}};
    check(bool(derive_processing_node_contracts(fft,profile().admission)),"derive FFT");
    result.nodes.push_back(std::move(fft));result.edges.push_back({2,4,0,0});return result;
}
void product_clock_provenance() {
    std::array<std::size_t,8> workspace{};ProcessingGraphAdmission admitted;
    const auto p=profile();const auto capabilities=PreparedGraph::numeric_capabilities(p);
    check(bool(admit_processing_graph(fft_graph(false),profile().admission,capabilities,workspace,admitted)),"FFT accepts product of same producer");
    const auto rejected=admit_processing_graph(fft_graph(true),profile().admission,capabilities,workspace,admitted);
    check(rejected.error==ProcessingError::IncompatiblePorts && rejected.node==4,"FFT rejects product of independent producers");
}

void append_processing(NumericProcessingGraph& g,NodeId id,std::string_view kind,ProcessingParameters parameters) {
    ConfiguredProcessingNode node;check(bool(make_processing_node(kind,id,profile().admission,node)),"create snapshot stage");
    node.parameters=std::move(parameters);
    check(bool(configure_processing_input(node,g.nodes.back().outputs[0].contract.view(),profile().admission)),"configure snapshot stage");
    g.edges.push_back({g.nodes.back().id,id,0,0});g.nodes.push_back(std::move(node));
}
NumericProcessingGraph snapshot_graph() {
    auto input=input_contract(false);input.shape={};input.semantic=Semantic::Scalar;
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";
    source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{44100}}}}};
    source.outputs={{0,PortDirection::Output,own(input)}};
    NumericProcessingGraph g;g.graph_id="stride-snapshot";g.nodes.push_back(std::move(source));
    append_processing(g,2,"NumberAccumulator",HistoryProcessingParameters{3,true});
    append_processing(g,3,"FrameStride",TransformProcessingParameters{{{"every_n",NumericLiteral{std::uint64_t{2}}},{"materialize",true}}});
    append_processing(g,4,"VectorNormalize",TransformProcessingParameters{{{"method",NumericLiteral{std::uint64_t{0}}}}});return g;
}
struct SnapshotSeen {
    std::size_t snapshots=0,sums=0;
    void snapshot(const NumericView& view) {
        const std::size_t count=snapshots?3:1;const float first=snapshots==2?3.f:1.f;
        check(!view.ring.capacity && view.actual==std::array<std::size_t,2>{count,1},"snapshot owns chronological populated values");
        for(std::size_t k=0;k<count;++k){float value=0;check(load_element(view,{0,k,0},value)==Status::ok && value==first+float(k),"wrapped history copied in logical order");}
        check(view.coordinates.axes[0].size()==count*sizeof(double),"snapshot retains populated logical time coordinates");
        for(std::size_t k=0;k<count;++k){double time=0;std::memcpy(&time,view.coordinates.axes[0].data()+k*sizeof(double),sizeof(double));
            check(time==(double(first)+double(k))/44100.,"wrapped time coordinates remain chronological");}
        ++snapshots;
    }
    static void output(void* context,const NumericPortFrameView& event) {
        if(event.identity.node!=3 && event.identity.node!=4)return;auto& self=*static_cast<SnapshotSeen*>(context);
        if(event.status==Status::unavailable)return;
        if(event.status!=Status::ok)std::fprintf(stderr,"snapshot node=%d port=%u status=%u\n",int(event.identity.node),unsigned(event.identity.port),unsigned(event.status));
        check(event.status==Status::ok && event.frame.stamp.available,"snapshot and owning extension actually publish");
        const auto& view=std::get<NumericView>(event.frame.payload);
        if(event.identity.node==3){self.snapshot(view);return;}
        constexpr std::array expected{1.f,1.f,3.f};float value=0;
        check(self.sums<expected.size() && load_element(view,{0,0,0},value)==Status::ok && value==expected[self.sums],"extension reads first logical value of owning snapshot");++self.sums;
    }
};
void sparse_history_snapshot() {
    const auto g=snapshot_graph();std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,profile(),plan,state)==Status::ok,"prepare sparse history snapshot");
    const auto input=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());const PlaneLayout plane{0,{0,0}};
    SnapshotSeen seen;const auto before=allocations;
    for(std::size_t i=1;i<=6;++i){
        const float value=float(i);FrameStamp stamp;stamp.producer_epoch=1;stamp.sequence=stamp.source_sequence=i;stamp.timestamp_ticks=std::int64_t(i);
        const auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(&value,1)),std::span(&plane,1),{1,1});
        check(plan->process_numeric_ports(1,0,frame,*state,{},NumericRuntimeCallbacks{SnapshotSeen::output,nullptr,nullptr,&seen})==Status::ok,"process sparse snapshot");
    }
    check(seen.snapshots==3 && seen.sums==3,"skip unselected frames without extra publications");
    check(allocations==before,"selected snapshot and extension make no hot C++ allocation");
}
struct WindowSeen {
    std::size_t count=0;std::int64_t origin=0,scale=1;
    static void output(void* context,const NumericPortFrameView& event) {
        if(event.identity.node!=2 || event.status==Status::unavailable)return;
        auto& self=*static_cast<WindowSeen*>(context);const auto first=std::int64_t(self.count*5);
        check(event.status==Status::ok && event.frame.stamp.timestamp_ticks==self.origin+first*self.scale,"window timestamp anchors first retained sample independently of packets");
        check(event.frame.stamp.sample_begin==first && event.frame.stamp.sample_count==16,"window sample interval");
        const auto& view=std::get<NumericView>(event.frame.payload);
        for(std::size_t k=0;k<16;++k){float value=0;check(load_element(view,{0,k,0},value)==Status::ok && value==float(first+std::int64_t(k)),"window payload matches timestamp");}
        ++self.count;
    }
};
void window_packet_timestamps(std::size_t packet,std::int64_t origin,std::int64_t scale=1) {
    auto input=input_contract(false);input.semantic=Semantic::PCM;input.shape.axes[0]=Axis::Sample;
    input.clock.tick_seconds.denominator=44100*std::uint64_t(scale);
    const auto g=graph(input,"FrameBuffer",TransformProcessingParameters{{{"size",NumericLiteral{std::uint64_t{16}}},{"hop_samples",NumericLiteral{std::uint64_t{5}}}}},true);
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,profile(),plan,state)==Status::ok,"prepare packet timestamp regression");
    const PlaneLayout plane{0,{sizeof(float),0}};std::array<float,4096> storage{};WindowSeen seen{0,origin,scale};const auto before=allocations;
    for(std::size_t start=0;start<320;start+=packet){
        const auto size=std::min(packet,320-start);for(std::size_t k=0;k<size;++k)storage[k]=float(start+k);
        FrameStamp stamp;stamp.producer_epoch=7;stamp.sequence=stamp.source_sequence=start/packet+1;
        stamp.timestamp_ticks=origin+std::int64_t(start)*scale;stamp.sample_begin=std::int64_t(start);stamp.sample_count=size;stamp.has_sample_interval=true;
        const auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(storage)),std::span(&plane,1),{size,1});
        check(plan->process_numeric_ports(1,0,frame,*state,{},NumericRuntimeCallbacks{WindowSeen::output,nullptr,nullptr,&seen})==Status::ok,"process packet timestamp regression");
    }
    check(seen.count==61 && allocations==before,"all complete windows and no hot allocation");
}
struct OverflowSeen {
    unsigned available=0,overflow=0;
    static void output(void* context,const NumericPortFrameView& event) {
        if(event.identity.node!=2)return;auto& seen=*static_cast<OverflowSeen*>(context);
        if(event.status==Status::ok){check(event.frame.stamp.timestamp_ticks==INT64_MAX-1,"first safe timestamp retained");++seen.available;}
        else if(event.status==Status::overflow)++seen.overflow;
    }
};
void window_timestamp_overflow() {
    auto input=input_contract(false);input.semantic=Semantic::PCM;input.shape.axes[0]=Axis::Sample;
    const auto g=graph(input,"FrameBuffer",TransformProcessingParameters{{{"size",NumericLiteral{std::uint64_t{16}}},{"hop_samples",NumericLiteral{std::uint64_t{5}}}}},true);
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,profile(),plan,state)==Status::ok,"prepare timestamp overflow regression");
    const PlaneLayout plane{0,{sizeof(float),0}};const std::array<float,4096> storage{};
    FrameStamp stamp;stamp.producer_epoch=stamp.sequence=stamp.source_sequence=1;stamp.timestamp_ticks=INT64_MAX-1;
    stamp.has_sample_interval=true;stamp.sample_count=21;
    const auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(storage)),std::span(&plane,1),{21,1});
    OverflowSeen seen;const auto status=plan->process_numeric_ports(1,0,frame,*state,{},NumericRuntimeCallbacks{OverflowSeen::output,nullptr,nullptr,&seen});
    check(status==Status::overflow && seen.available==1 && seen.overflow==1,"overflow rejects next window without wrapped successful timestamp");
}

}
void* operator new(std::size_t bytes){++allocations;if(auto* value=std::malloc(bytes?bytes:1))return value;throw std::bad_alloc();}
void operator delete(void* value) noexcept{std::free(value);}
void operator delete(void* value,std::size_t) noexcept{std::free(value);}
int main(int argc,char** argv){
    if(argc>1 && std::strcmp(argv[1],"--frame-stamps")==0){
        window_packet_timestamps(3,170000);window_packet_timestamps(97,-170000,2);
        window_packet_timestamps(4096,170000);window_timestamp_overflow();
        std::puts("reference frame timestamps PASS");return 0;
    }
    retired_pcm_onset_factory_refused();
    matrix_disabled();framing_disabled();product_clock_provenance();sparse_history_snapshot();
    window_packet_timestamps(3,170000);window_packet_timestamps(97,-170000,2);window_timestamp_overflow();
    std::puts("reference disabled geometry PASS");
}
