#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <dsp_kernels/mel_filter_bank.h>
#include <dsp_kernels/power_to_db.h>
#include <type_traits>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <numeric>
#include <string>

namespace {

using namespace signal_contracts;using namespace domain;using namespace dataflow_runtime;
using dsp_kernels::MelFilterBank;
struct MelFixture {
    double sample_rate=22050,fmin_hz=0,fmax_hz=0,norm_p=2;
    std::size_t n_fft=2048,bands=128;
    bool htk=false;
    dsp_kernels::MelNormalization normalization=dsp_kernels::MelNormalization::Slaney;
};
dsp_kernels::MelFilterBankConfig kernel_config(const MelFixture& p) {
    dsp_kernels::MelFilterBankConfig c;c.sample_rate=p.sample_rate;c.bin_hz=p.sample_rate/double(p.n_fft);c.first_bin=0;c.bands=p.bands;
    c.fmin_hz=p.fmin_hz;c.fmax_hz=p.fmax_hz;c.norm_p=p.norm_p;c.normalization=p.normalization;
    c.scale=p.htk?dsp_kernels::MelScale::Htk:dsp_kernels::MelScale::Slaney;return c;
}
using dsp_kernels::PreparedPowerToDb;using dsp_kernels::PowerToDbConfig;
std::size_t allocations=0;
void check(bool value,const char* why){if(!value){std::fprintf(stderr,"FAIL native frontend: %s\n",why);std::exit(1);}}
void check_status(Status status,const char* why){if(status!=Status::ok)std::fprintf(stderr,"status=%u\n",unsigned(status));check(status==Status::ok,why);}
NumericRuntimeProfile profile() {
    constexpr FrameLimits frames{1,131072,524288,65536,256,65536};
    NumericRuntimeProfile p{{32,64,8,16,256,131072,frames},{{4194304,4194304,4194304,131072},0,16777216},
        {131072,1,524288,524288,524288},{true,true,true}};p.music_enabled=true;return p;
}
struct DenseRows {
    std::span<const float> input;std::span<float> output;std::size_t columns=0;
    static bool read(const void* user,std::size_t row,std::span<float> values) noexcept {
        const auto& self=*static_cast<const DenseRows*>(user);const auto offset=row*self.columns;
        if(offset>self.input.size() || values.size()>self.input.size()-offset)return false;
        std::copy_n(self.input.begin()+std::ptrdiff_t(offset),values.size(),values.begin());return true;
    }
    static bool write(void* user,std::size_t row,std::span<const float> values) noexcept {
        auto& self=*static_cast<DenseRows*>(user);const auto offset=row*self.columns;
        if(offset>self.output.size() || values.size()>self.output.size()-offset)return false;
        std::copy(values.begin(),values.end(),self.output.begin()+std::ptrdiff_t(offset));return true;
    }
};
bool transform(PreparedPowerToDb& kernel,std::span<const float> input,std::size_t rows,std::size_t columns,std::span<float> output) {
    DenseRows view{input,output,columns};return kernel.process({&view,DenseRows::read},rows,columns,{&view,DenseRows::write});
}
void numerical_contracts() {
    static_assert(std::is_same_v<decltype(dsp_kernels::PowerToDbConfig{}.reference),float> &&
        std::is_same_v<decltype(dsp_kernels::PowerToDbConfig{}.amin),float> &&
        std::is_same_v<decltype(dsp_kernels::PowerToDbConfig{}.top_db),float>);
    MelFixture config;config.n_fft=512;config.bands=128;
    MelFilterBank mel;check(mel.prepare(config.n_fft/2+1,kernel_config(config)),"128 native Slaney filters, independent of legacy 50-band cap");
    std::array<float,257> power{};power[12]=4;std::array<float,128> values{};
    const auto before=allocations;check(mel.process(power,values),"full power spectrum to Mel");
    check(allocations==before && *std::max_element(values.begin(),values.end())>0,"Mel processes without allocations");
    const auto saved=values;power[3]=std::numeric_limits<float>::quiet_NaN();check(!mel.process(power,values) && saved==values,"nonfinite input cannot partially overwrite Mel result");
    config.bands=0;check(!mel.prepare(config.n_fft/2+1,kernel_config(config)),"invalid Mel preparation");power[3]=0;check(mel.process(power,values),"old Mel plan survives invalid preparation");
    PreparedPowerToDb db;PowerToDbConfig c;check(db.prepare(3,3,c),"prepare native log power");
    const std::array<float,9> input{1,10,100,0,-1,1e-12f,1e-5f,1e-3f,1e-1f};std::array<float,9> out{};
    const auto start=allocations;check(transform(db,input,3,3,out),"global native top_db");
    check(allocations==start && out[0]==0 && out[1]==10 && out[2]==20 && out[3]==-60 && out[4]==-60,"global peak minus 80 dB, negative real input floors at amin");
    c.reference_mode=dsp_kernels::DbReference::Maximum;check(db.prepare(3,3,c) && transform(db,input,3,3,out),"maximum reference");
    check(out[2]==0 && out[3]==-80,"maximum reference is separate from top_db");
    auto invalid=input;invalid[8]=std::numeric_limits<float>::infinity();const auto old=out;
    check(!transform(db,invalid,3,3,out) && old==out,"entire input validated before writer gets a row");
    c.amin=0;check(!db.prepare(3,3,c) && transform(db,input,3,3,out),"invalid dB preparation preserves old plan");
}
void set(ConfiguredProcessingNode& node,std::string_view key,ProcessingParameterValue value) {
    auto& fields=std::get<TransformProcessingParameters>(node.parameters).values;
    for(auto& field:fields)if(field.key==key){field.value=std::move(value);return;}
    fields.push_back({std::string(key),std::move(value)});
}
ConfiguredProcessingNode make(std::string_view kind,NodeId id,const MelFixture& config) {
    NodeFactorySettings settings;settings.sample_rate=std::uint32_t(config.sample_rate);settings.fft_size=std::uint32_t(config.n_fft);settings.history_capacity=32;
    ConfiguredProcessingNode result;check(bool(make_processing_node(kind,id,profile().admission,result,settings)),"create standalone native block");return result;
}
std::string_view norm_name(dsp_kernels::MelNormalization mode) {
    constexpr std::array names{"slaney","none","l0","lp","maximum","minimum"};return names.at(std::size_t(mode));
}
void configure_mel(ConfiguredProcessingNode& mel,const MelFixture& c) {
    set(mel,"bands",NumericLiteral{std::uint64_t(c.bands)});set(mel,"fmin_hz",NumericLiteral{c.fmin_hz});set(mel,"fmax_hz",NumericLiteral{c.fmax_hz});
    set(mel,"scale",std::string(c.htk?"htk":"slaney"));
    set(mel,"first_bin",NumericLiteral{std::uint64_t{0}});set(mel,"bin_hz",NumericLiteral{c.sample_rate/double(c.n_fft)});set(mel,"norm",std::string(norm_name(c.normalization)));set(mel,"norm_p",NumericLiteral{c.norm_p});
}
NumericProcessingGraph graph(const MelFixture& c,std::size_t capacity,bool enabled=true) {
    auto mel=make("Mel",2,c);configure_mel(mel,c);mel.enabled=enabled;
    auto input=std::get<NumericContract>(mel.inputs[0].contract.view());input.shape.rank=Rank::Matrix;input.quantity=Quantity::SpectrumPower;input.unit=NumericUnit::SquaredAmplitude;
    input.shape.axes={Axis::Time,Axis::Bin};input.shape.extents={ExtentBounds{0,capacity},ExtentBounds{c.n_fft/2+1,c.n_fft/2+1}};
    input.coordinates[1]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{std::int64_t(c.sample_rate),c.n_fft}};input.coordinates[0]={CoordinateMode::Regular,NumericUnit::Seconds,{0,1},{257,std::uint64_t(c.sample_rate)}};
    check(bool(configure_processing_input(mel,input,profile().admission)),"native Mel accepts explicit power history");
    auto db=make("PowerToDb",3,c);check(bool(configure_processing_input(db,mel.outputs[0].contract.view(),profile().admission)),"Mel power to native dB");
    auto onset=make("OnsetStrength",4,c);set(onset,"n_fft",NumericLiteral{std::uint64_t(c.n_fft)});set(onset,"hop_length",NumericLiteral{std::uint64_t{257}});
    check(bool(configure_processing_input(onset,db.outputs[0].contract.view(),profile().admission)),"log Mel to native onset");
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs={{0,PortDirection::Output,mel.inputs[0].contract}};
    NumericProcessingGraph g;g.graph_id="native-mel-db-onset";g.nodes={source,mel,db,onset};g.edges={{1,2,0,0},{2,3,0,0},{3,4,0,0}};return g;
}
struct Capture {
    std::array<float,16384> mel{},db{};std::array<float,128> onset{};
    std::array<std::size_t,2> shape{};std::size_t count=0;bool available=false;FrameStamp stamp{};
    static void receive(void* user,const NumericPortFrameView& event) {
        if(event.direction!=PortDirection::Output || event.identity.node<2 || event.identity.node>4)return;
        auto& self=*static_cast<Capture*>(user);check(validate_view(event.frame,profile().admission.frame)==event.status,"published shape/coordinates/availability validate");
        if(event.identity.node==4){self.available=event.frame.stamp.available;self.stamp=event.frame.stamp;}
        if(!event.frame.stamp.available)return;
        const auto& view=std::get<NumericView>(event.frame.payload);
        if(event.identity.node==4){self.count=view.actual[0];check(self.count<=self.onset.size(),"bounded envelope capture");for(std::size_t i=0;i<self.count;++i)check(load_element(view,{0,i,0},self.onset[i])==Status::ok,"read native onset");return;}
        self.shape=view.actual;auto& target=event.identity.node==2?self.mel:self.db;
        check(view.actual[0]*view.actual[1]<=target.size(),"bounded matrix capture");
        for(std::size_t row=0;row<view.actual[0];++row)for(std::size_t column=0;column<view.actual[1];++column)
            check(load_element(view,{0,row,column},target[row*view.actual[1]+column])==Status::ok,"read observed intermediate result");
    }
};
Capture run(const MelFixture& c,std::span<const float> input,std::size_t rows,bool enabled=true) {
    const auto capacity=std::max(rows,std::size_t{32});const auto g=graph(c,capacity,enabled);
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check_status(PreparedGraph::prepare_numeric(g,profile(),plan,state),"prepare independent native chain");
    const auto contract=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());const auto columns=c.n_fft/2+1;
    std::vector<float> storage(capacity*columns);std::copy(input.begin(),input.end(),storage.begin());
    const std::array<PlaneLayout,1> planes{{{0,{columns*4,4}}}};FrameStamp stamp;stamp.sequence=1;stamp.timestamp_ticks=std::int64_t(rows*257);
    Capture capture;const DataFrameView frame{stamp,NumericView{&contract,std::as_bytes(std::span(storage)),planes,{rows,columns}}};
    const auto before=allocations;check_status(plan->process_numeric_ports(1,frame,*state,{}, {Capture::receive,nullptr,nullptr,&capture}),"execute full float native chain");
    check(allocations==before,"Mel/dB/onset chain never allocates while processing");return capture;
}
void runtime_contracts() {
    MelFixture c;c.n_fft=512;c.bands=16;std::vector<float> input(7*257,1.f);
    input[3*257+10]=100;const auto result=run(c,input,7);
    check(result.available && result.count==7 && result.shape==std::array<std::size_t,2>{7,16},"actual history length passes through separate graph stages");
    check(result.onset[3]>0,"attack reaches native envelope through unquantized Mel/dB");
    const auto disabled=run(c,input,7,false);check(std::all_of(disabled.mel.begin(),disabled.mel.begin()+112,[](float v){return v==0;}),"disabled Mel produces zero power");
    const auto g=graph(c,32);auto p=profile();p.music_enabled=false;
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,p,plan,state)!=Status::ok,"native methods honor music admission");
    auto plain=g;plain.nodes.pop_back();plain.edges.pop_back();
    check_status(PreparedGraph::prepare_numeric(plain,p,plan,state),"Mel and dB remain available without musical methods");
    p=profile();p.resources.maximum.scratch_bytes=16;
    check(PreparedGraph::prepare_numeric(g,p,plan,state)!=Status::ok,"complete filter bank and dB workspaces are admitted");
    auto wrong=g.nodes[1];set(wrong,"sample_rate",NumericLiteral{48000.});check(!derive_processing_node_contracts(wrong,profile().admission),"Fs is an explicit validated setting");
}
void ring_history() {
    MelFixture c;c.n_fft=512;c.bands=16;auto g=graph(c,4);
    auto row=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    row.shape.rank=Rank::Vector;row.shape.axes={Axis::Bin,Axis::None};row.shape.extents={row.shape.extents[1],ExtentBounds{}};
    row.coordinates={row.coordinates[1],AxisCoordinates{}};
    auto history=make("Spectrogram",5,c);std::get<HistoryProcessingParameters>(history.parameters).capacity=4;
    check(bool(configure_processing_input(history,row,profile().admission)),"prepare explicit spectrum history");
    check(bool(configure_processing_input(g.nodes[1],history.outputs[0].contract.view(),profile().admission)),"Mel accepts real history ring");
    check(bool(configure_processing_input(g.nodes[2],g.nodes[1].outputs[0].contract.view(),profile().admission)),"dB follows the actual history contract");
    check(bool(configure_processing_input(g.nodes[3],g.nodes[2].outputs[0].contract.view(),profile().admission)),"onset follows the actual history contract");
    g.nodes[0].outputs[0].contract=history.inputs[0].contract;g.nodes.push_back(history);g.edges[0]={5,2,0,0};g.edges.push_back({1,5,0,0});
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check_status(PreparedGraph::prepare_numeric(g,profile(),plan,state),"prepare wrapped-history frontend");
    const auto contract=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    std::array<float,257> input{};const std::array<PlaneLayout,1> planes{{{0,{4,0}}}};Capture capture;
    for(std::size_t tick=0;tick<7;++tick){
        input.fill(float(tick+1));input[10]=float(10+tick*tick);FrameStamp stamp;stamp.sequence=tick+1;stamp.timestamp_ticks=std::int64_t(tick*257);
        const DataFrameView frame{stamp,NumericView{&contract,std::as_bytes(std::span(input)),planes,{257,1}}};const auto before=allocations;
        check_status(plan->process_numeric_ports(1,frame,*state,{}, {Capture::receive,nullptr,nullptr,&capture}),"stream real ring through independent stages");
        check(allocations==before,"wrapped history frontend allocates nothing");
    }
    MelFilterBank kernel;check(kernel.prepare(257,kernel_config(c)),"independent linear filter oracle");std::array<float,16> expected{};
    for(std::size_t row_index=0;row_index<4;++row_index){
        const auto tick=row_index+3;input.fill(float(tick+1));input[10]=float(10+tick*tick);
        check(kernel.process(input,expected),"evaluate chronological row");
        check(std::equal(expected.begin(),expected.end(),capture.mel.begin()+std::ptrdiff_t(row_index*16)),"Mel follows logical ring order after wrap");
    }
}
struct ScalarCapture {
    float value=0;bool available=false;
    static void receive(void* user,const NumericPortFrameView& event) {
        if(event.identity.node!=3 || event.direction!=PortDirection::Output)return;
        auto& self=*static_cast<ScalarCapture*>(user);self.available=event.frame.stamp.available;
        if(self.available)check(load_element(std::get<NumericView>(event.frame.payload),{0,0,0},self.value)==Status::ok,"read scalar dB");
    }
};
void scalar_db() {
    MelFixture c;auto db=make("PowerToDb",3,c);auto input=std::get<NumericContract>(db.inputs[0].contract.view());input.shape={};input.coordinates={};
    check(bool(configure_processing_input(db,input,profile().admission)),"scalar power is a valid dB input");
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs={{0,PortDirection::Output,db.inputs[0].contract}};
    NumericProcessingGraph g;g.graph_id="scalar-db";g.nodes={source,db};g.edges={{1,3,0,0}};
    auto p=profile();p.music_enabled=false;std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check_status(PreparedGraph::prepare_numeric(g,p,plan,state),"prepare general scalar transform without music opt-in");
    const auto contract=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    const std::array<float,1> value{100};const std::array<PlaneLayout,1> planes{{{0,{0,0}}}};
    const DataFrameView frame{FrameStamp{},NumericView{&contract,std::as_bytes(std::span(value)),planes,{1,1}}};ScalarCapture capture;const auto before=allocations;
    check_status(plan->process_numeric_ports(1,frame,*state,{}, {ScalarCapture::receive,nullptr,nullptr,&capture}),"execute scalar dB transform");
    check(allocations==before && capture.available && capture.value==20,"scalar dB is correct and allocates nothing");
}
template<class T> void write(std::ofstream& file,std::span<const T> values) {
    file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size_bytes()));check(bool(file),"write native fixture result");
}
std::vector<float> read(const char* path,std::size_t count) {
    std::vector<float> values(count);std::ifstream file(path,std::ios::binary);file.read(reinterpret_cast<char*>(values.data()),std::streamsize(count*sizeof(float)));
    check(bool(file),"read native fixture input");return values;
}
MelFixture mel_arguments(char** argv) {
    MelFixture c;c.sample_rate=std::stod(argv[4]);c.n_fft=std::stoull(argv[5]);c.bands=std::stoull(argv[6]);
    c.fmin_hz=std::stod(argv[7]);c.fmax_hz=std::stod(argv[8]);c.htk=bool(std::stoul(argv[9]));
    c.normalization=dsp_kernels::MelNormalization(std::stoul(argv[10]));c.norm_p=std::stod(argv[11]);return c;
}
int mel_fixture(int argc,char** argv,bool use_graph) {
    check(argc==13,"Mel fixture arguments");const auto c=mel_arguments(argv);const auto rows=std::stoull(argv[12]);
    const auto input=read(argv[2],rows*(c.n_fft/2+1));std::ofstream file(argv[3],std::ios::binary);
    if(use_graph){const auto result=run(c,input,rows);check(result.available,"native graph result available");
        write(file,std::span<const float>(result.mel).first(rows*c.bands));write(file,std::span<const float>(result.db).first(rows*c.bands));
        write(file,std::span<const float>(result.onset).first(rows));return 0;}
    MelFilterBank kernel;check(kernel.prepare(c.n_fft/2+1,kernel_config(c)),"prepare Mel fixture");write(file,kernel.weights());std::vector<float> out(c.bands);
    for(std::size_t row=0;row<rows;++row){check(kernel.process(std::span(input).subspan(row*(c.n_fft/2+1),c.n_fft/2+1),out),"project Mel fixture");write(file,std::span<const float>(out));}
    return 0;
}
int db_fixture(int argc,char** argv) {
    check(argc==12,"dB fixture arguments");const auto rows=std::stoull(argv[4]),columns=std::stoull(argv[5]);
    PowerToDbConfig c;c.reference_mode=dsp_kernels::DbReference(std::stoul(argv[6]));c.reference=std::stod(argv[7]);c.amin=std::stod(argv[8]);
    c.top_db=std::stod(argv[9]);c.top_db_enabled=bool(std::stoul(argv[10]));c.axes=dsp_kernels::DbReductionAxes(std::stoul(argv[11]));
    const auto input=read(argv[2],rows*columns);std::vector<float> out(rows*columns);PreparedPowerToDb kernel;
    check(kernel.prepare(std::max(rows,1ULL),columns,c) && transform(kernel,input,rows,columns,out),"native dB fixture");
    std::ofstream file(argv[3],std::ios::binary);write(file,std::span<const float>(out));return 0;
}
}
void* operator new(std::size_t n){++allocations;auto* p=std::malloc(n?n:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
int main(int argc,char** argv) {
    if(argc>1)return std::string_view(argv[1])=="db"?db_fixture(argc,argv):mel_fixture(argc,argv,std::string_view(argv[1])=="graph");
    numerical_contracts();runtime_contracts();ring_history();scalar_db();std::puts("native Mel/dB kernels, explicit graph stages, coordinates, geometry, ring, scalar, profile, no-allocation PASS");
}
