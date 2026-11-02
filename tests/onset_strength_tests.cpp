#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <dsp_kernels/onset_strength.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <string>

namespace {

using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
using dsp_kernels::PreparedOnsetStrength;using dsp_kernels::OnsetStrengthConfig;
std::size_t allocations=0;
void check(bool value,const char* why){if(!value){std::fprintf(stderr,"FAIL librosa onset: %s\n",why);std::exit(1);}}
void check_status(Status status,const char* why){if(status!=Status::ok)std::fprintf(stderr,"status=%u\n",unsigned(status));check(status==Status::ok,why);}
NumericRuntimeProfile profile() {
    constexpr FrameLimits frames{1,131072,524288,65536,256,65536};
    NumericRuntimeProfile p{{32,64,8,16,256,131072,frames},{{4194304,4194304,4194304,131072},0,16777216},
        {131072,1,524288,524288,524288},{true,true,true}};
    p.music_enabled=true;return p;
}
void kernel_contract() {
    PreparedOnsetStrength kernel;OnsetStrengthConfig c;c.center=false;
    check(kernel.prepare(2,4,c),"prepare native S variant");
    const std::array<float,8> input{-30,-20,-20,-20,-25,-10,-25,-10};std::array<float,4> out{};
    const auto before=allocations;check(kernel.process(input,4,out),"process dB input without normalization");
    check(allocations==before && out==std::array<float,4>{0,5,5,0},"raw native mean flux, no allocations or 0..1 clamp");
    c.center=true;c.n_fft=4;c.hop_length=2;check(kernel.prepare(2,4,c) && kernel.process(input,4,out),"centered variant");
    check(out==std::array<float,4>{0,0,5,5},"native left padding and end trim");
    c.detrend=true;check(kernel.prepare(2,4,c) && kernel.process(input,4,out),"native DC filter");
    check(std::abs(out[3]-4.95f)<1e-6f,"detrend starts after padded zeros");
    auto invalid=input;invalid[6]=std::numeric_limits<float>::quiet_NaN();const auto saved=out;
    check(!kernel.process(invalid,4,out) && out==saved,"invalid input leaves caller output unchanged");
    c.lag=0;check(!kernel.prepare(2,4,c) && kernel.process(input,4,out),"invalid preparation preserves usable kernel");
}
void short_and_reflected_filter() {
    PreparedOnsetStrength kernel;OnsetStrengthConfig c;c.center=false;c.lag=7;c.max_size=4;
    std::array<float,7> out{};check(kernel.prepare(1,4,c) && kernel.process(std::array<float,2>{-20,-10},2,out),"lag longer than native noncentered input");
    check(std::all_of(out.begin(),out.end(),[](float v){return v==0;}),"native noncentered padding length can exceed input");
    c.lag=1;check(kernel.prepare(4,2,c),"even max-filter window");
    const std::array<float,8> input{1,5,2,3,6,6,6,6};std::array<float,2> filtered{};
    check(kernel.process(input,2,filtered) && filtered[1]==1,"reflect boundary and asymmetric even filter origin");
    c.max_size=999;check(kernel.prepare(4,2,c) && kernel.process(input,2,filtered) && filtered[1]==1,"filter wider than repeated reflection period");
}
NumericProcessingGraph graph(bool enabled=true) {
    ConfiguredProcessingNode onset;check(bool(make_processing_node("OnsetStrength",2,profile().admission,onset)),"catalog factory creates native method block");
    auto c=std::get<NumericContract>(onset.inputs[0].contract.view());c.shape.extents={ExtentBounds{0,8},ExtentBounds{2,2}};
    check(own_processing_contract(c,profile().admission.frame,131072,onset.inputs[0].contract)==Status::ok,"explicit S shape");
    check(bool(derive_processing_node_contracts(onset,profile().admission)),"derive native onset output");onset.enabled=enabled;
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};
    source.outputs={{0,PortDirection::Output,onset.inputs[0].contract}};
    NumericProcessingGraph g;g.graph_id="native-librosa-onset";g.nodes={source,onset};g.edges={{1,2,0,0}};return g;
}
struct Capture {
    std::array<float,8> values{};std::size_t count=0;bool available=false;
    static void receive(void* user,const NumericPortFrameView& event) {
        if(event.identity.node!=2)return;
        auto& self=*static_cast<Capture*>(user);self.available=event.frame.stamp.available;
        if(!self.available)return;
        const auto& v=std::get<NumericView>(event.frame.payload);self.count=v.actual[0];
        check(v.contract->dtype==DType::Float32,"native envelope remains Float32 through graph publication");
        for(std::size_t i=0;i<self.count;++i)check(load_element(v,{0,i,0},self.values[i])==Status::ok,"read native graph result");
    }
};
NumericProcessingGraph history_graph(bool enabled,bool filled=false) {
    auto g=graph(enabled);auto row=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    row.shape={Rank::Vector,{Axis::Band,Axis::None},{ExtentBounds{2,2},ExtentBounds{}}};row.semantic=Semantic::Numeric;
    ConfiguredProcessingNode history;NodeFactorySettings settings;settings.history_capacity=8;
    check(bool(make_processing_node("Spectrogram",3,profile().admission,history,settings)),"reuse prepared History block");
    std::get<HistoryProcessingParameters>(history.parameters).filled_only=filled;
    check(bool(configure_processing_input(history,row,profile().admission)),"explicit full-precision row into History");
    check(bool(configure_processing_input(g.nodes.back(),history.outputs[0].contract.view(),profile().admission)),"explicit History into native method");
    g.nodes.front().outputs[0].contract=history.inputs[0].contract;g.nodes.push_back(std::move(history));
    g.edges={{1,3,0,0},{3,2,0,0}};return g;
}
void graph_history(bool enabled,bool filled=false) {
    const auto g=history_graph(enabled,filled);std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    check(PreparedGraph::prepare_numeric(g,profile(),plan,context)==Status::ok,"prepare standalone native block");
    const auto contract=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    const std::array<PlaneLayout,1> planes{{{0,{4,0}}}};Capture capture;std::array<float,16> chronological{};
    for(unsigned time=0;time<15;++time){
        const std::array<float,2> row{float(-30+int(time%3)*7),float(-20+int(time%4)*3)};
        FrameStamp stamp;stamp.sequence=time+1;stamp.timestamp_ticks=time*400;
        const DataFrameView frame{stamp,NumericView{&contract,std::as_bytes(std::span(row)),planes,{2,1}}};
        const auto before=allocations;
        check_status(plan->process_numeric_ports(1,frame,*context,{}, {Capture::receive,nullptr,nullptr,&capture}),"internal History ring is consumed directly");
        check(allocations==before && capture.available && capture.count==(filled?std::min(time+1,8u):8),"selected warmup extent and wrapped History process without allocations");
        if(time>=7)std::copy(row.begin(),row.end(),chronological.begin()+std::ptrdiff_t((time-7)*2));
    }
    PreparedOnsetStrength kernel;std::array<float,8> expected{};
    check(kernel.prepare(2,8) && kernel.process(chronological,8,expected),"shared method on final chronological history");
    check(capture.values==(enabled?expected:std::array<float,8>{}),"wrapped graph history exactly matches native kernel; disabled output is typed silence");
}
void unavailable() {
    const auto g=graph();std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    check(PreparedGraph::prepare_numeric(g,profile(),plan,context)==Status::ok,"prepare unavailable boundary scenario");
    const auto contract=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    const std::array<float,16> input{-30,-20,-20,-20,-25,-10,-25,-10,-20,-5};
    const std::array<PlaneLayout,1> planes{{{0,{8,4}}}};
    const NumericView view{&contract,std::as_bytes(std::span(input)),planes,{5,2}};
    FrameStamp stamp;stamp.sequence=1;DataFrameView frame{stamp,view};Capture capture;
    check_status(plan->process_numeric_ports(1,frame,*context,{}, {Capture::receive,nullptr,nullptr,&capture}),"borrowed contiguous matrix input");
    check(capture.available && capture.values[3]==5,"native result exists before unavailable boundary");
    frame.stamp.sequence=2;frame.stamp.available=false;frame.payload=view;std::get<NumericView>(frame.payload).storage={};
    const auto status=plan->process_numeric_ports(1,frame,*context,{}, {Capture::receive,nullptr,nullptr,&capture});
    check((status==Status::ok || status==Status::unavailable) && !capture.available,"unavailable input cannot leave an available onset");
}
void admission() {
    auto g=graph();auto p=profile();p.music_enabled=false;
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    check(PreparedGraph::prepare_numeric(g,p,plan,context)!=Status::ok,"native method honors music profile");
    p=profile();p.resources.maximum.payload_bytes=8;
    check(PreparedGraph::prepare_numeric(g,p,plan,context)!=Status::ok,"explicit history workspace charged to admission");
    auto c=std::get<NumericContract>(g.nodes.back().inputs[0].contract.view());c.dtype=DType::UInt8;c.representation.alignment=1;
    check(own_processing_contract(c,profile().admission.frame,131072,g.nodes.back().inputs[0].contract)==Status::ok,"declare quantized test input");
    check(!derive_processing_node_contracts(g.nodes.back(),profile().admission),"no hidden uint8 conversion inside native method");
}
int reference(int argc,char** argv) {
    check(argc==12,"reference arguments");const auto bins=std::stoull(argv[3]),frames=std::stoull(argv[4]);
    OnsetStrengthConfig c{std::stoull(argv[5]),std::stoull(argv[6]),std::stoull(argv[7]),std::stoull(argv[8]),
        bool(std::stoul(argv[9])),bool(std::stoul(argv[10])),std::stoul(argv[11])?dsp_kernels::OnsetAggregate::Median:dsp_kernels::OnsetAggregate::Mean};
    PreparedOnsetStrength kernel;check(kernel.prepare(bins,frames,c),"reference preparation");
    std::vector<float> input(bins*frames);std::vector<float> out(kernel.output_frames(frames));
    std::ifstream file(argv[1],std::ios::binary);file.read(reinterpret_cast<char*>(input.data()),std::streamsize(input.size()*sizeof(float)));
    check(bool(file) && kernel.process(input,frames,out),"native reference input");
    std::ofstream result(argv[2],std::ios::binary);result.write(reinterpret_cast<const char*>(out.data()),std::streamsize(out.size()*sizeof(float)));
    check(bool(result),"native reference output");return 0;
}
}
void* operator new(std::size_t n){++allocations;auto* p=std::malloc(n?n:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
int main(int argc,char** argv){if(argc>1)return reference(argc,argv);kernel_contract();short_and_reflected_filter();graph_history(true);graph_history(false);graph_history(true,true);graph_history(false,true);unavailable();admission();std::puts("native librosa S onset kernel/graph/ring/profile/no-allocation PASS");}
