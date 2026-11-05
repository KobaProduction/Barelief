// SPDX-License-Identifier: AGPL-3.0-or-later
#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <dsp_kernels/dsp.h>
#include <kiss_backend.h>
#include <ooura_backend.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
using namespace signal_contracts;
using namespace domain;
using namespace dataflow_runtime;
constexpr std::size_t n=16,capacity_rows=3;
std::size_t allocations=0,backend_calls=0,backend_clones=0;
void check(bool ok,const char* message){if(!ok){std::fprintf(stderr,"RealFourier provider: %s\n",message);std::abort();}}
NumericRuntimeProfile profile() {
    NumericRuntimeProfile p{{16,16,8,16,65536,1048576,{8,4096,65536,4096,256,4096}},
        {{1048576,1048576,1048576,65536},0,8388608},{4096,8,65536,65536,262144},{true,true,true}};
    p.music_enabled=true;return p;
}
NumericContract input_contract(bool matrix) {
    static const std::uint32_t mono=0;NumericContract c;c.channel_order={&mono,1};c.dtype=DType::Float32;
    c.clock={"real-provider",{1,44100},44100};c.shape.rank=matrix?Rank::Matrix:Rank::Vector;
    c.shape.axes={matrix?Axis::Time:Axis::Sample,matrix?Axis::Sample:Axis::None};
    c.shape.extents={matrix?ExtentBounds{1,capacity_rows}:ExtentBounds{n,n},matrix?ExtentBounds{n,n}:ExtentBounds{1,1}};
    if(matrix)c.coordinates[0]={CoordinateMode::Regular,NumericUnit::Seconds,{5,1},{1,10}};
    return c;
}
ConfiguredProcessingNode source_node(bool matrix) {
    const auto c=input_contract(matrix);OwnedProcessingContract owned;
    check(own_processing_contract(c,profile().admission.frame,65536,owned)==Status::ok,"own source contract");
    ConfiguredProcessingNode result;result.id=1;result.kind="NetworkInput";
    result.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{44100}}}}};
    result.outputs={{0,PortDirection::Output,std::move(owned)}};return result;
}
NumericProcessingGraph graph(bool matrix,bool endpoints,std::string_view id,bool enabled=true) {
    NumericProcessingGraph g;g.graph_id="real-forward-provider";g.nodes.push_back(source_node(matrix));
    ConfiguredProcessingNode op;check(bool(make_processing_node("RealFourier",2,profile().admission,op)),"make RealFourier");
    op.enabled=enabled;op.inputs={{0,PortDirection::Input,g.nodes[0].outputs[0].contract}};
    op.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{n}}},
        {"backend_id",std::string(id)},{"include_endpoints",endpoints}}};
    check(bool(derive_processing_node_contracts(op,profile().admission)),"derive selected geometry");
    g.nodes.push_back(std::move(op));g.edges={{1,2,0,0}};return g;
}
class EndpointBackend final:public dsp_kernels::FourierBackend {
public:
    bool process(std::span<const float> input,std::span<float> output) noexcept override {
        if(!dsp_kernels::fourier_backend_call_valid({n,dsp_kernels::FourierBackendTransform::RealForward},input,output))return false;
        ++backend_calls;std::fill(output.begin(),output.end(),0.f);
        output[0]=-0.f;output[1]=-0.f;output[2]=1.25f;output[3]=-2.5f;output[n]=-3.f;output[n+1]=-0.f;return true;
    }
    dsp_kernels::FourierBackendResources resources() const noexcept override {return {sizeof(*this),sizeof(*this),0,0};}
    std::unique_ptr<dsp_kernels::FourierBackend> clone() const override {++backend_clones;return std::make_unique<EndpointBackend>();}
};
class EndpointFactory final:public dsp_kernels::FourierBackendFactory {
public:
    const char* id() const noexcept override {return "test-real-endpoint-bits";}
    bool query(dsp_kernels::FourierBackendRequest r,dsp_kernels::FourierBackendResources& result) const noexcept override {
        if(r!=dsp_kernels::FourierBackendRequest{n,dsp_kernels::FourierBackendTransform::RealForward})return false;
        result={sizeof(EndpointBackend),sizeof(EndpointBackend),0,0};return true;
    }
    std::unique_ptr<dsp_kernels::FourierBackend> prepare(dsp_kernels::FourierBackendRequest r) const override {
        dsp_kernels::FourierBackendResources bounds;if(!query(r,bounds))return {};
        return std::make_unique<EndpointBackend>();
    }
};
struct Seen {
    std::span<const float> expected;
    bool matrix=false,endpoints=false;
    std::size_t rows=1,total=0;
    std::array<std::size_t,3> calls{};
    std::size_t width(PortId port) const {return port==2?1:n/2+std::size_t(endpoints)-std::size_t(port==1 && !endpoints);}
    void value(const NumericView& v,PortId port,std::size_t row,std::size_t column) const {
        const auto target=port==2?ElementIndex{0,row,0}:matrix?ElementIndex{0,row,column}:ElementIndex{0,column,0};
        float actual=0;check(load_element(v,target,actual)==Status::ok,"read typed output");
        const auto index=row*(n+2)+(port==2?0:2*(column+std::size_t(!endpoints))+port);
        check(std::bit_cast<std::uint32_t>(actual)==std::bit_cast<std::uint32_t>(expected[index]),"raw split bits including signed endpoint zeros");
    }
    void output(const NumericPortFrameView& event) {
        if(event.identity.node!=2)return;
        if(event.status==Status::unavailable && !total)return;
        check(event.status==Status::ok && event.frame.stamp.available,"successful real FFT frame");
        const auto port=event.identity.port;check(port<3 && !calls[port],"exactly one publication per port");
        const auto& v=std::get<NumericView>(event.frame.payload);const auto columns=width(port);
        const std::array<std::size_t,2> actual=matrix && port<2?std::array{rows,columns}:std::array{port==2?rows:columns,std::size_t{1}};
        check(v.actual==actual,"default/full endpoints and populated matrix rows");
        if(port<2){const auto& axis=v.contract->coordinates[matrix?1:0];
            check(axis.mode==CoordinateMode::Regular && axis.origin==Ratio{endpoints?0:1,1} && axis.step==Ratio{1,1},"explicit bin origin and spacing");}
        for(std::size_t row=0;row<rows;++row)for(std::size_t column=0;column<columns;++column)value(v,port,row,column);
        ++calls[port];++total;
    }
    static void callback(void* context,const NumericPortFrameView& event){static_cast<Seen*>(context)->output(event);}
};
void expected_values(std::span<const float> input,std::span<float> output,std::size_t rows,
    const dsp_kernels::FourierBackendFactory* factory) {
    dsp_kernels::PreparedFourierBackend backend;dsp_kernels::PreparedRfft builtin;
    check(factory?backend.prepare({n,dsp_kernels::FourierBackendTransform::RealForward},*factory):builtin.prepare(n),"independent real transform");
    for(std::size_t row=0;row<rows;++row){
        const auto in=input.subspan(row*n,n);auto out=output.subspan(row*(n+2),n+2);
        check(factory?backend.process(in,out):builtin.process(in,out),"independent packed real output");
    }
}
void feed(const PreparedGraph& plan,RuntimeContext& state,const NumericContract& contract,
    std::span<const float> input,Seen& seen) {
    const PlaneLayout plane{0,{seen.matrix?n*sizeof(float):sizeof(float),seen.matrix?sizeof(float):0}};
    FrameStamp stamp;stamp.available=true;stamp.sequence=stamp.source_sequence=1;stamp.producer_epoch=1;
    const DataFrameView frame{stamp,NumericView{&contract,std::as_bytes(input),std::span(&plane,1),
        {seen.matrix?seen.rows:n,seen.matrix?n:1}}};
    check(plan.process_numeric_ports(1,0,frame,state,{}, {Seen::callback,nullptr,nullptr,&seen})==Status::ok,"real transform processing");
    check(seen.total==3,"complete three real FFT outputs");
}
void run_case(bool matrix,bool endpoints,std::shared_ptr<const dsp_kernels::FourierBackendFactory> factory,bool enabled=true) {
    const auto id=factory?factory->id():"";const bool fake=std::string_view(id)=="test-real-endpoint-bits";
    const auto g=graph(matrix,endpoints,id,enabled);auto p=profile();p.fourier_backend=factory;
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,p,plan,state)==Status::ok,"prepare real provider graph");
    const auto clones_before=backend_clones;RuntimeContext cloned(*state);
    check(!fake || backend_clones>clones_before,"cold context copy clones the provider plan");
    std::array<float,capacity_rows*n> input{};std::array<float,capacity_rows*(n+2)> expected{};
    for(std::size_t i=0;i<input.size();++i)input[i]=float(int(i%9)-4)*.125f;
    const auto rows=matrix?2u:1u;expected_values(input,expected,rows,factory.get());if(!enabled)expected.fill(0);
    std::weak_ptr<const dsp_kernels::FourierBackendFactory> lifetime=factory;
    const bool named=bool(factory);factory.reset();p.fourier_backend.reset();
    check(!named || !lifetime.expired(),"prepared graph pins selected factory lifetime");
    const auto contract=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    Seen first{expected,matrix,endpoints,rows},second{expected,matrix,endpoints,rows};
    const auto before=allocations,calls_before=backend_calls;
    feed(*plan,*state,contract,input,first);feed(*plan,cloned,contract,input,second);
    check(allocations==before,"provider/builtin/split/context paths have no hot C++ allocations");
    check(!fake || backend_calls-calls_before==(enabled?2*rows:0),"real provider called exactly once per enabled row");
}
void missing_and_mismatch() {
    const auto g=graph(false,true,"kiss_fft130_float32");auto p=profile();
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,p,plan,state)!=Status::ok && !plan && !state,"missing named real provider atomically rejected");
    p.fourier_backend={&desktop_fourier::ooura_backend_factory(),[](const auto*){}};
    check(PreparedGraph::prepare_numeric(g,p,plan,state)!=Status::ok && !plan && !state,"wrong real provider ID cannot fallback");
}
}
void* operator new(std::size_t bytes){++allocations;if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
int main() {
    missing_and_mismatch();
    for(const bool matrix:{false,true})for(const bool endpoints:{false,true}){
        run_case(matrix,endpoints,{});
        run_case(matrix,endpoints,{&desktop_fourier::kiss_backend_factory(),[](const auto*){}});
        run_case(matrix,endpoints,std::make_shared<EndpointFactory>());
    }
    run_case(true,true,std::make_shared<EndpointFactory>(),false);
    std::puts("RealFourier provider/default/endpoints/clone/noalloc PASS");
}
