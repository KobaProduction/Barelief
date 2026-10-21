#include <dataflow_runtime/runtime.h>
#include <dsp_kernels/dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <limits>
#include <chrono>
using namespace domain;
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
std::size_t allocations=0;
void require(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
constexpr FrameLimits limits{1,131072,524288,32768,256,32768};
constexpr std::array<std::uint32_t,1> channels{7};
constexpr std::size_t bands=8;
constexpr double tau=6.2831853071795864769;
NumericRuntimeProfile profile(){return {{32,64,8,16,256,131072,limits},{{4194304,4194304,4194304,131072},0,16777216},{131072,1,524288,524288,524288},{true,true,true}};}
OwnedProcessingContract owned(const NumericContract& c){OwnedProcessingContract out;require(own_processing_contract(c,limits,131072,out)==Status::ok,"own contract");return out;}
ProcessingPort port(PortDirection direction,const NumericContract& c){return {0,direction,owned(c)};}
NumericContract input_contract(){NumericContract c;c.dtype=DType::Float32;c.semantic=Semantic::MelEnergy;c.channel_order=channels;
    c.shape={Rank::Vector,{Axis::Band,Axis::None},{ExtentBounds{bands,bands},ExtentBounds{1,1}}};
    c.clock={"audio-origin",{1,48000},48000};c.coordinates[0]={CoordinateMode::Regular,NumericUnit::Values,{0,1},{1,1}};return c;}
NumericContract history_count_contract(const NumericContract& input){
    auto count=input;count.dtype=DType::UInt64;count.semantic=Semantic::Scalar;count.shape={};count.coordinates={};count.unit=NumericUnit::Frames;count.quantity=Quantity::FilledFrameCount;
    return count;
}
NumericProcessingGraph graph(std::size_t samples=512,bool window=false,std::string function="hann",bool seconds=true,std::size_t capacity=400){
    const auto input=input_contract();auto history=input;history.semantic=Semantic::History;history.history_element=input.semantic;
    history.shape={Rank::Matrix,{Axis::Time,Axis::Band},{ExtentBounds{0,capacity},ExtentBounds{bands,bands}}};
    history.coordinates={seconds?AxisCoordinates{CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{0,1}}:AxisCoordinates{},input.coordinates[0]};
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={port(PortDirection::Output,input)};
    ConfiguredProcessingNode retained;retained.id=2;retained.kind="Spectrogram";retained.parameters=HistoryProcessingParameters{capacity};
    retained.inputs={port(PortDirection::Input,input)};retained.outputs={port(PortDirection::Output,history)};
    const auto count=history_count_contract(input);
    retained.outputs.push_back({1,PortDirection::Output,owned(count)});
    ConfiguredProcessingNode transpose;transpose.id=5;transpose.kind="MatrixTranspose";transpose.parameters=TransformProcessingParameters{};
    transpose.inputs={port(PortDirection::Input,history)};OwnedProcessingContract transposed,windowed;
    require(matrix_transpose_output_contract(history,transposed)==Status::ok,"derive transposed matrix");
    transpose.outputs={{0,PortDirection::Output,transposed}};
    ConfiguredProcessingNode taper;taper.id=6;taper.kind="MatrixWindowFunction";taper.enabled=window;
    taper.parameters=TransformProcessingParameters{{{"function",std::move(function)}}};taper.inputs={{0,PortDirection::Input,transposed}};
    require(matrix_window_output_contract(std::get<NumericContract>(transposed.view()),windowed)==Status::ok,"derive matrix window");
    taper.outputs={{0,PortDirection::Output,windowed}};
    ConfiguredProcessingNode fft;fft.id=3;fft.kind="MatrixFFT";fft.inputs={{0,PortDirection::Input,windowed}};
    fft.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t(samples)}}}};
    OwnedProcessingContract transformed;require(matrix_fft_output_contract(std::get<NumericContract>(windowed.view()),samples,transformed)==Status::ok,"derive row FFT matrix");
    fft.outputs={{0,PortDirection::Output,transformed}};
    ConfiguredProcessingNode row;row.id=4;row.kind="MatrixRow";row.parameters=TransformProcessingParameters{{{"row",NumericLiteral{std::uint64_t{3}}}}};
    row.inputs={{0,PortDirection::Input,transformed}};OwnedProcessingContract vector;
    require(matrix_row_output_contract(std::get<NumericContract>(transformed.view()),vector)==Status::ok,"derive row vector");row.outputs={{0,PortDirection::Output,vector}};
    NumericProcessingGraph g;g.graph_id="matrix-fft-oracle";g.nodes={source,retained,fft,row,transpose,taper};g.edges={{1,2},{2,5},{5,6},{6,3},{3,4}};return g;
}
struct Observation {
    std::array<float,bands*256> matrix{};std::array<float,256> row{};std::array<double,256> hz{};
    std::size_t bins=0,available=0,unavailable=0;bool last_available=false,row_available=false;
    Status last_status=Status::ok,row_status=Status::ok;bool typed_unavailable=false;
    static void output(void* user,const NumericPortFrameView& frame){auto& o=*static_cast<Observation*>(user);
        if(frame.identity.node!=3 && frame.identity.node!=4)return;
        if(frame.identity.node==4){o.row_available=frame.frame.stamp.available;o.row_status=frame.status;if(!o.row_available)return;
            const auto& value=std::get<NumericView>(frame.frame.payload);for(std::size_t i=0;i<value.actual[0];++i)require(load_element(value,{0,i,0},o.row[i])==Status::ok,"read selected row");return;}
        o.last_available=frame.frame.stamp.available;o.last_status=frame.status;if(!o.last_available){++o.unavailable;
            const auto* value=std::get_if<NumericView>(&frame.frame.payload);o.typed_unavailable=value && value->contract && value->contract->shape.rank==Rank::Matrix && value->storage.empty();return;}++o.available;
        const auto& value=std::get<NumericView>(frame.frame.payload);o.bins=value.actual[1];
        require(value.actual[0]==bands && !value.ring.capacity && value.contract->semantic!=Semantic::History,"ordinary [band][bin] matrix");
        require(o.bins<=256,"bounded test observation");
        for(std::size_t band=0;band<bands;++band)for(std::size_t bin=0;bin<o.bins;++bin)
            require(load_element(value,{0,band,bin},o.matrix[band*256+bin])==Status::ok && std::isfinite(o.matrix[band*256+bin]),"read finite modulation bin");
        if(value.contract->coordinates[1].mode==CoordinateMode::ExplicitFloat64LE){require(value.coordinates.axes[1].size()==o.bins*8,"explicit Hz payload");
            for(std::size_t i=0;i<o.bins;++i){std::uint64_t bits=0;for(std::size_t b=0;b<8;++b)bits|=std::uint64_t(std::to_integer<unsigned char>(value.coordinates.axes[1][i*8+b]))<<(8*b);std::memcpy(&o.hz[i],&bits,8);}}
    }
    NumericRuntimeCallbacks callbacks(){return {output,nullptr,nullptr,this};}
};
struct Running {std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    explicit Running(const NumericProcessingGraph& g){require(PreparedGraph::prepare_numeric(g,profile(),plan,state)==Status::ok,"prepare temporal graph");}
    void replace(const NumericProcessingGraph& g,const char* scenario){std::shared_ptr<const PreparedGraph> next;std::unique_ptr<RuntimeContext> context;
        const auto status=PreparedGraph::prepare_numeric(g,profile(),next,context,plan.get(),state.get());
        if(status!=Status::ok){std::fprintf(stderr,"replacement %s status=%u\n",scenario,unsigned(status));
            for(const auto& node:g.nodes){ResolvedProcessingNode resolved;const auto result=resolve_processing_node(node,profile().admission,resolved);
                if(!result)std::fprintf(stderr,"node=%d kind=%s error=%u\n",node.id,node.kind.c_str(),unsigned(result.error));}}
        require(status==Status::ok,"prepare temporal replacement");plan=std::move(next);state=std::move(context);}
};
Status feed(Running& running,const NumericProcessingGraph& g,std::size_t ordinal,Observation& o,std::int64_t timestamp=-1,bool nonfinite=false,bool silence=false){
    const auto input=std::get<NumericContract>(g.nodes.front().outputs.front().contract.view());std::array<float,bands> values{};
    // Eight independently changing amplitudes; no device or speaker involvement.
    if(!silence)for(std::size_t band=0;band<bands;++band)values[band]=float(.8+.3*std::cos(tau*double(band+3)*double(ordinal)/512.));
    if(nonfinite)values[3]=std::numeric_limits<float>::infinity();
    const std::array<PlaneLayout,1> planes{{{0,{4,0}}}};FrameStamp stamp;stamp.sequence=ordinal+1;stamp.source_sequence=stamp.sequence;
    stamp.producer_epoch=3;stamp.generation=1;stamp.timestamp_ticks=timestamp<0?std::int64_t(ordinal*400):timestamp;
    const DataFrameView frame{stamp,NumericView{&input,std::as_bytes(std::span(values)),planes,{bands,1},{},{}}};
    const auto before=allocations;const auto result=running.plan->process_numeric_ports(1,frame,*running.state,{},o.callbacks());
    require(result==Status::ok || result==Status::unavailable || result==Status::incompatible_contract || (nonfinite && result==Status::invalid_argument),"bounded processing status");
    require(allocations==before,"processing does not allocate");
    return result;
}
void near(double actual,double expected,const char* why,double tolerance=2e-5){require(std::abs(actual-expected)<tolerance,why);}
void prepared_window_tests(){
    constexpr std::size_t maximum=512;
    dsp_kernels::PreparedWindows bank(maximum);
    std::array<float,maximum> values{},expected{};
    for(std::size_t index=0;index<dsp_kernels::window_function_names.size();++index){const auto function=dsp_kernels::WindowFunction(index);
        for(std::size_t length=0;length<=maximum;++length){
            const auto before=allocations;
            require(bank.coefficients(function,std::span(values).first(length)),"prepared variable window supports every admitted length");
            require(before==allocations,"window lookup never allocates");
            dsp_kernels::window(std::span(expected).first(length),function);
            for(std::size_t i=0;i<length;++i)near(values[i],expected[i],"prepared rotations match direct window",1e-6);
        }
        const auto direct=bank.coefficients(function);require(direct.size()==maximum,"all full windows are preloaded");
    }
    const std::array<std::array<double,5>,3> oracle{{{{0,.5,1,.5,0}},{{.00006,.21747,1,.21747,.00006}},{{-.000421051,-.05473684,1.000000003,-.05473684,-.000421051}}}};
    for(unsigned index=0;index<3;++index){const auto function=dsp_kernels::WindowFunction(index+4);double cg=0,pg=0;
        for(unsigned i=0;i<5;++i){near(dsp_kernels::window_coefficient(i,5,function),oracle[index][i],"new window coefficient agrees with independent five-point oracle",2e-7);cg+=oracle[index][i];pg+=oracle[index][i]*oracle[index][i];}
        near(dsp_kernels::window_coherent_gain(5,function),cg/5,"new coherent gain uses exact applied window",1e-7);
        near(dsp_kernels::window_power_gain(5,function),pg/5,"new power gain uses exact applied window",1e-7);
    }
    std::size_t bytes=0;require(dsp_kernels::PreparedWindows::storage_bytes(maximum,bytes) && bytes>=maximum*dsp_kernels::window_function_names.size()*sizeof(float),"window admission includes all preloaded coefficients");
    require(!dsp_kernels::PreparedWindows::storage_bytes(SIZE_MAX,bytes),"window admission rejects overflowing geometry");
    const auto before=allocations;
    dsp_kernels::PreparedWindows invalid(SIZE_MAX);
    require(!invalid.prepared() && allocations==before && invalid.coefficients(dsp_kernels::WindowFunction::Hann).empty(),"invalid public bank geometry refuses without allocation");
    require(!invalid.coefficients(dsp_kernels::WindowFunction::Hann,std::span(values).first(1)),"unprepared bank cannot process");
    const auto retained=bank.coefficients(dsp_kernels::WindowFunction::Hann).data();
    require(!bank.prepare(SIZE_MAX) && bank.prepared() && bank.coefficients(dsp_kernels::WindowFunction::Hann).data()==retained,"invalid cold reprepare preserves accepted window bank");
}
    struct WindowSwitchCapture {
        std::array<float,bands*8> values{};std::size_t columns=0;
        static void receive(void* user,const NumericPortFrameView& frame){
            if(frame.identity.node!=6)return;
            require(frame.frame.stamp.available,"window switch remains available");
            auto& out=*static_cast<WindowSwitchCapture*>(user);const auto& view=std::get<NumericView>(frame.frame.payload);out.columns=view.actual[1];
            for(std::size_t row=0;row<bands;++row)for(std::size_t col=0;col<out.columns;++col)
                require(load_element(view,{0,row,col},out.values[row*8+col])==Status::ok,"read switched window");
        }
    };
void preloaded_window_switches(){
    auto retained=graph();Running history(retained);Observation o;
    for(std::size_t i=0;i<10;++i)feed(history,retained,i,o);
    const auto start=std::chrono::steady_clock::now();
    for(std::size_t i=0;i<32;++i){
        const auto name=std::array{"hann","hamming","blackman","rectangular"}[i%4];
        retained.nodes[5].enabled=true;retained.nodes[5].parameters=TransformProcessingParameters{{{"function",std::string{name}}}};
        history.replace(retained,"preloaded function switch");feed(history,retained,10+i,o);
        DataFrameView snapshot;std::uint64_t ordinal=0;
        require(history.plan->history(2,0,*history.state,snapshot,nullptr,&ordinal)==Status::ok && ordinal==11+i,"window switches retain compatible History ordinal");
    }
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::printf("32 window Prepare/Apply/process switches: %.2f ms; History uninterrupted\n",elapsed);
}
void window_switch_tests(){
    auto input=input_contract();input.semantic=Semantic::Numeric;
    input.shape={Rank::Matrix,{Axis::Band,Axis::Time},{ExtentBounds{bands,bands},ExtentBounds{0,8}}};
    input.coordinates={};
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={port(PortDirection::Output,input)};
    ConfiguredProcessingNode taper;taper.id=6;taper.kind="MatrixWindowFunction";
    taper.parameters=TransformProcessingParameters{};
    taper.inputs={port(PortDirection::Input,input)};taper.outputs={port(PortDirection::Output,input)};
    NumericProcessingGraph g;g.graph_id="window-switch";g.nodes={source,taper};g.edges={{1,6}};
    Running running(g);
    WindowSwitchCapture capture;

    std::array<float,bands*8> values{};values.fill(1.f);
    const std::array<PlaneLayout,1> planes{{{0,{8*4,4}}}};
    std::uint64_t sequence=0;
    for(auto function:{dsp_kernels::WindowFunction::Hann,dsp_kernels::WindowFunction::Blackman,dsp_kernels::WindowFunction::Hamming,dsp_kernels::WindowFunction::Rectangular}){
        const auto name=function==dsp_kernels::WindowFunction::Hann?"hann":function==dsp_kernels::WindowFunction::Hamming?"hamming":function==dsp_kernels::WindowFunction::Blackman?"blackman":"rectangular";
        g.nodes[1].parameters=TransformProcessingParameters{{{"function",std::string{name}}}};
        running.replace(g,"variable window function");
        for(std::size_t length:{5u,8u,0u,1u,7u,5u}){
            FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence;stamp.producer_epoch=3;stamp.generation=1;
            const DataFrameView frame{stamp,NumericView{&input,std::as_bytes(std::span(values)),planes,{bands,length},{},{}}};
            const auto before=allocations;
            require(running.plan->process_numeric_ports(1,frame,*running.state,{}, {WindowSwitchCapture::receive,nullptr,nullptr,&capture})==Status::ok,"window switching processes variable matrix");
            require(before==allocations && capture.columns==length,"variable window switch has no allocations or stale extent");
            std::array<float,8> expected{};dsp_kernels::window(std::span(expected).first(length),function);
            for(std::size_t row=0;row<bands;++row)for(std::size_t col=0;col<length;++col)
                near(capture.values[row*8+col],expected[col],"changed function uses new coefficients at the same variable length",1e-6);
        }
    }
    preloaded_window_switches();
}
void oracle(Running& running,const Observation& o,std::size_t samples,dsp_kernels::WindowFunction function,bool enabled){
    DataFrameView frame;std::uint64_t filled=0;require(running.plan->history(2,0,*running.state,frame,&filled)==Status::ok,"retained snapshot");
    const auto& history=std::get<NumericView>(frame.payload);const auto used=std::min(history.actual[0],samples);
    std::vector<float> coefficients(history.actual[0]);dsp_kernels::window(coefficients,function);
    for(std::size_t band=0;band<bands;++band)for(std::size_t bin=0;bin<samples/2;++bin){double re=0,im=0;
        for(std::size_t row=0;row<used;++row){float value=0;require(load_element(history,{0,history.actual[0]-used+row,band},value)==Status::ok,"read wrapped history oracle");
            const auto v=double(value)*(enabled?coefficients[history.actual[0]-used+row]:1.);const auto phase=tau*double(bin)*double(row)/double(samples);re+=v*std::cos(phase);im-=v*std::sin(phase);}
        near(o.matrix[band*256+bin],std::hypot(re,im)/double(samples),"direct DFT independently matches every band/bin");}
}
void numerical_window_case(std::size_t samples,dsp_kernels::WindowFunction function){
        const auto name=function==dsp_kernels::WindowFunction::Hann?"hann":function==dsp_kernels::WindowFunction::Hamming?"hamming":function==dsp_kernels::WindowFunction::Blackman?"blackman":"rectangular";
        auto g=graph(samples,true,name);Running running(g);Observation o;
        feed(running,g,0,o);require(o.last_available && o.row_available,"first finite zero-padded matrix does not wait for timestamps");
        for(std::size_t i=1;i<413;++i)feed(running,g,i,o);
        require(o.last_available && o.row_available && o.bins==samples/2,"all temporal outputs available after ring wrap");
        oracle(running,o,samples,function,true);near(o.hz[1],120./double(samples),"temporal cadence uses 120Hz observed times, not 48000Hz audio Fs",1e-10);
        for(std::size_t bin=0;bin<o.bins;++bin)near(o.row[bin],o.matrix[3*256+bin],"MatrixRow matches selected band exactly",1e-10);
}
void timestamp_origin_cases(){
    auto large_origin=graph();Running large_run(large_origin);Observation large_observation;
    constexpr std::int64_t origin_ticks=48000000000000000LL;
    feed(large_run,large_origin,0,large_observation,origin_ticks);
    feed(large_run,large_origin,1,large_observation,origin_ticks);
    feed(large_run,large_origin,2,large_observation,origin_ticks+12);
    require(large_observation.last_available && large_observation.row_available,"duplicate timestamps do not stall row FFT");
    auto backward_origin=graph();Running backward_run(backward_origin);Observation backward_observation;
    feed(backward_run,backward_origin,0,backward_observation,origin_ticks);
    feed(backward_run,backward_origin,1,backward_observation,origin_ticks-12);
    feed(backward_run,backward_origin,2,backward_observation,origin_ticks+12);
    require(backward_observation.last_available && backward_observation.row_available,"backward timestamps do not stall row FFT");
}
void numerical_tests(){
    for(std::size_t samples:{256,512})for(auto function:{dsp_kernels::WindowFunction::Hann,dsp_kernels::WindowFunction::Hamming,dsp_kernels::WindowFunction::Blackman,dsp_kernels::WindowFunction::Rectangular}){
        numerical_window_case(samples,function);
    }
    auto g=graph();Running running(g);Observation o;for(std::size_t i=0;i<413;++i)feed(running,g,i,o);
    oracle(running,o,512,dsp_kernels::WindowFunction::Hann,false);
    DataFrameView ordinary;require(running.plan->history(3,0,*running.state,ordinary)==Status::incompatible_contract,"ordinary temporal matrix is never an ancestor History snapshot");
    std::get<TransformProcessingParameters>(g.nodes[3].parameters).values[0].value=NumericLiteral{std::uint64_t{7}};
    running.replace(g,"row selection");DataFrameView before;std::uint64_t filled=0;require(running.plan->history(2,0,*running.state,before,&filled)==Status::ok && filled==400,"row selection preserves History");
    feed(running,g,413,o);for(std::size_t bin=0;bin<o.bins;++bin)near(o.row[bin],o.matrix[7*256+bin],"last row selectable",1e-10);
    feed(running,g,414,o,std::int64_t(414*400+30));require(o.last_available && o.row_available,"irregular timestamps do not gate the declared analysis cadence");
    timestamp_origin_cases();
    auto indices=graph(512,false,"hann",false);Running index_run(indices);Observation index_observation;
    for(std::size_t i=0;i<25;++i)feed(index_run,indices,i,index_observation,std::int64_t(i*400+i*i));
    require(index_observation.last_available,"unknown cadence remains useful as bin indices");oracle(index_run,index_observation,512,dsp_kernels::WindowFunction::Hann,false);
    feed(index_run,indices,25,index_observation,-1,true);require(!index_observation.last_available,"nonfinite values do not publish stale spectrum");
    auto disabled=graph(512,false,"hann",false);disabled.nodes[2].enabled=false;Running disabled_run(disabled);Observation zero;
    feed(disabled_run,disabled,0,zero);require(zero.last_available,"disabled known matrix emits typed zeros");for(float value:zero.matrix)require(value==0,"disabled temporal FFT is zero");
}
void disconnected_history(Running& running,const NumericProcessingGraph& enlarged,Observation& observed,DataFrameView& snapshot,std::uint64_t& filled){
    // A removed data edge retains the known clock. Its next events append typed
    // zero rows, while compatible older real History remains until aged out.
    auto disconnected=enlarged;disconnected.edges.erase(disconnected.edges.begin());running.replace(disconnected,"source disconnect");
    feed(running,disconnected,431,observed);
    require(running.plan->history(2,0,*running.state,snapshot,&filled)==Status::ok && filled==402,"disconnected known route continues History cadence");
    const auto& advancing=std::get<NumericView>(snapshot.payload);
    for(std::size_t band=0;band<bands;++band){float value=1;
        require(load_element(advancing,{0,advancing.actual[0]-1,band},value)==Status::ok && value==0,"first disconnected frame is typed zero");}
    require(observed.last_available && observed.row_available,"typed zero ingress keeps temporal result available");
    oracle(running,observed,512,dsp_kernels::WindowFunction::Hann,false);
    for(std::size_t ordinal=432;ordinal<943;++ordinal)feed(running,disconnected,ordinal,observed);
    require(observed.last_available && observed.row_available,"fully aged-out disconnected result remains available");
    for(std::size_t band=0;band<bands;++band)for(std::size_t bin=0;bin<observed.bins;++bin)
        require(observed.matrix[band*256+bin]==0,"disconnected temporal output contains no stale spectrum");
    for(std::size_t bin=0;bin<observed.bins;++bin)require(observed.row[bin]==0,"selected disconnected spectrum contains no stale values");
    near(observed.hz[1],120./512.,"disconnected zeros retain real signal-clock cadence",1e-10);
}
void resize_and_disconnect_tests(){
    auto original=graph();Running running(original);Observation observed;
    for(std::size_t ordinal=0;ordinal<430;++ordinal)feed(running,original,ordinal,observed);
    DataFrameView snapshot;std::uint64_t filled=0;
    require(running.plan->history(2,0,*running.state,snapshot,&filled)==Status::ok && filled==400,"full original History before resize");
    const auto& previous=std::get<NumericView>(snapshot.payload);std::array<float,400*bands> preserved{};
    for(std::size_t row=0;row<400;++row)for(std::size_t band=0;band<bands;++band)
        require(load_element(previous,{0,row,band},preserved[row*bands+band])==Status::ok,"copy resize oracle outside processing");
    auto enlarged=graph(512,false,"hann",true,512);running.replace(enlarged,"History resize");
    require(running.plan->history(2,0,*running.state,snapshot,&filled)==Status::ok && filled==400,"400 observed rows survive 400-to-512 resize");
    const auto& retained=std::get<NumericView>(snapshot.payload);require(retained.actual[0]==512,"resized History exposes full capacity");
    for(std::size_t row=0;row<512;++row)for(std::size_t band=0;band<bands;++band){float value=0;
        require(load_element(retained,{0,row,band},value)==Status::ok,"read resized History");
        near(value,row<112?0:preserved[(row-112)*bands+band],"resize preserves newest compatible rows and typed older padding",1e-10);}
    feed(running,enlarged,430,observed);require(observed.last_available && observed.row_available,"resized temporal chain remains available");
    oracle(running,observed,512,dsp_kernels::WindowFunction::Hann,false);
    near(observed.hz[1],120./512.,"resized History uses observed cadence and ignores artificial padding",1e-10);
    disconnected_history(running,enlarged,observed,snapshot,filled);
}
void rolling_cadence_recovery_tests(){
    auto g=graph(512,false,"hann",true,8);Running running(g);Observation observed;
    for(std::size_t ordinal=0;ordinal<8;++ordinal)
        require(feed(running,g,ordinal,observed)==Status::ok,"initial temporal cadence is nonfatal");
    require(observed.last_available && observed.row_available,"initial rolling cadence available");
    DataFrameView snapshot;std::uint64_t filled=0,accepted=0;
    require(running.plan->history(2,0,*running.state,snapshot,&filled,&accepted)==Status::ok && filled==8 && accepted==8,"History is full before source timeline changes");
    std::array<float,bands> newest{};
    for(std::size_t band=0;band<bands;++band)
        require(load_element(std::get<NumericView>(snapshot.payload),{0,7,band},newest[band])==Status::ok,"retain source boundary oracle");
    std::size_t ordinal=8;
    // A new source can restart its timeline or jump forward while the older
    // compatible rows remain. Both cases must recover through ordinary aging.
    for(std::int64_t origin:{std::int64_t{0},std::int64_t{48000}}){
        for(std::size_t row=0;row<8;++row,++ordinal){
            require(feed(running,g,ordinal,observed,origin+std::int64_t(row*400))==Status::ok,"mixed source cadence never fails processing");
            require(running.plan->history(2,0,*running.state,snapshot,&filled,&accepted)==Status::ok && filled==8 && accepted==ordinal+1,"mixed cadence keeps History occupancy and advances ordinal");
            const auto stats=running.plan->node_stats(3,*running.state);require(stats && stats->errors==0,"mixed source cadence is not a node error");
            require(observed.last_available && observed.row_available,"mixed rolling timestamps never gate row FFT");
            near(observed.hz[1],120./512.,"Hz follows the declared analysis cadence",1e-10);
            if(row==7)oracle(running,observed,512,dsp_kernels::WindowFunction::Hann,false);
            if(origin==0 && row==0)for(std::size_t band=0;band<bands;++band){float value=0;
                require(load_element(std::get<NumericView>(snapshot.payload),{0,6,band},value)==Status::ok && value==newest[band],"source boundary retains compatible older History instead of resetting");}
        }
    }
    require(observed.unavailable==0 && observed.available==24,"all finite frames produce row FFT from the first observation");
}
NumericProcessingGraph clock_warmup_graph(){
    // Exercise the same process_clock return contract consumed by the desktop.
    auto g=graph(512,false,"hann",true,8);
    auto input=input_contract();input.semantic=Semantic::PCM;input.shape.axes[0]=Axis::Sample;input.coordinates={};
    input.clock={"audio-origin",{1,8000},8000};g.execution_interval={1,1000};
    auto history=input;history.semantic=Semantic::History;history.history_element=input.semantic;
    history.shape={Rank::Matrix,{Axis::Time,Axis::Sample},{ExtentBounds{0,8},ExtentBounds{bands,bands}}};
    history.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{0,1}};
    g.nodes[0].kind="SignalGenerator";g.nodes[0].parameters=SignalGeneratorProcessingParameters{};
    g.nodes[0].clock_node=7;g.nodes[0].outputs={port(PortDirection::Output,input)};
    g.nodes[1].inputs={port(PortDirection::Input,input)};g.nodes[1].outputs[0]=port(PortDirection::Output,history);
    auto count=std::get<NumericContract>(g.nodes[1].outputs[1].contract.view());count.clock=input.clock;g.nodes[1].outputs[1].contract=owned(count);
    OwnedProcessingContract transposed,windowed,matrix,vector;
    require(matrix_transpose_output_contract(history,transposed)==Status::ok,"clock transpose contract");
    require(matrix_window_output_contract(std::get<NumericContract>(transposed.view()),windowed)==Status::ok,"clock window contract");
    require(matrix_fft_output_contract(std::get<NumericContract>(windowed.view()),512,matrix)==Status::ok,"clock matrix FFT contract");
    g.nodes[4].inputs={port(PortDirection::Input,history)};g.nodes[4].outputs={{0,PortDirection::Output,transposed}};
    g.nodes[5].inputs={{0,PortDirection::Input,transposed}};g.nodes[5].outputs={{0,PortDirection::Output,windowed}};
    g.nodes[2].inputs={{0,PortDirection::Input,windowed}};
    require(matrix_row_output_contract(std::get<NumericContract>(matrix.view()),vector)==Status::ok,"clock row contract");
    g.nodes[2].outputs={{0,PortDirection::Output,matrix}};
    g.nodes[3].inputs={{0,PortDirection::Input,matrix}};g.nodes[3].outputs={{0,PortDirection::Output,vector}};
    ConfiguredProcessingNode clock;clock.id=7;clock.kind="SampleClock";
    clock.parameters=SampleClockProcessingParameters{8000,"audio-origin"};g.nodes.push_back(std::move(clock));
    g.configuration_bindings.push_back({7,1});
    return g;
}
void clock_warmup_tests(){
    auto g=clock_warmup_graph();
    Running running(g);Observation observed;
    auto event=[&](std::uint64_t ordinal,std::int64_t ticks=-1){FrameStamp stamp;stamp.sequence=stamp.source_sequence=ordinal+1;
        stamp.producer_epoch=3;stamp.generation=1;stamp.timestamp_ticks=stamp.sample_begin=ticks<0?std::int64_t(ordinal*bands):ticks;
        stamp.sample_count=bands;stamp.has_sample_interval=true;stamp.discontinuity=ordinal==0;
        const auto before=allocations;
        require(running.plan->process_clock(7,stamp,*running.state,{},observed.callbacks())==Status::ok,"matrix FFT never fails SampleClock on valid data");
        require(allocations==before,"clock warmup does not allocate");
        const auto stats=running.plan->node_stats(3,*running.state);require(stats && stats->errors==0,"warmup is not a node error");};
    auto cold=[&](){DataFrameView snapshot;std::uint64_t filled=99;
        require(running.plan->history(2,0,*running.state,snapshot,&filled)==Status::ok && filled==0,"cold History has zero observations");};
    auto warmup=[&](){require(observed.last_available && observed.row_available &&
        observed.last_status==Status::ok && observed.row_status==Status::ok,"first clock and timestamp transitions publish available row FFT");};
    cold();event(0);warmup();event(1);
    require(observed.last_available && observed.row_available,"second consecutive clock establishes temporal cadence");
    near(observed.hz[1],1000./512.,"clock cadence derives from the prepared analysis interval",1e-10);
    event(2);require(observed.last_available && observed.row_available,"clock continues after warmup");
    running.plan->reset_history(2,*running.state);cold();event(3);warmup();event(4);
    require(observed.last_available && observed.row_available,"History reset warms up again without stopping clock");
    require(observed.unavailable==0 && observed.available==5,"cold and reset cycles remain available");
    for(std::uint64_t ordinal=5;ordinal<11;++ordinal)event(ordinal);
    std::uint64_t ordinal=11;
    for(std::int64_t origin:{std::int64_t{0},std::int64_t{48000}})
        for(std::size_t row=0;row<8;++row,++ordinal){event(ordinal,origin+std::int64_t(row*bands));
            if(row<7)warmup();
            else {require(observed.last_available && observed.row_available,"SampleClock survives mixed cadence and resumes uniform spectra");
                near(observed.hz[1],1000./512.,"resumed clock has honest temporal Hz",1e-10);}}
    event(ordinal,48000+std::int64_t(8*bands));
    require(observed.last_available && observed.row_available,"generator continues after source timeline recovery");
}
    struct VariableRowObservation {
        std::array<float,256> values{};std::size_t deliveries=0;bool available=false,typed_unavailable=false;Status status=Status::ok;
        static void receive(void* user,const NumericPortFrameView& frame){if(frame.identity.node!=4)return;
            auto& observed=*static_cast<VariableRowObservation*>(user);++observed.deliveries;observed.available=frame.frame.stamp.available;observed.status=frame.status;
            const auto& value=std::get<NumericView>(frame.frame.payload);
            observed.typed_unavailable=!observed.available && value.contract && value.contract->shape.rank==Rank::Vector && value.storage.empty();
            if(observed.available)for(std::size_t bin=0;bin<256;++bin)
                require(load_element(value,{0,bin,0},observed.values[bin])==Status::ok,"read recovered variable matrix row");
        }
    };
NumericProcessingGraph variable_row_graph(){
    auto fixture=graph();auto input=std::get<NumericContract>(fixture.nodes[2].outputs[0].contract.view());
    input.shape.extents[0]={0,bands};
    input.coordinates[1]={CoordinateMode::Regular,NumericUnit::Values,{0,1},{1,1}};
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={port(PortDirection::Output,input)};
    ConfiguredProcessingNode row;row.id=4;row.kind="MatrixRow";
    row.parameters=TransformProcessingParameters{{{"row",NumericLiteral{std::uint64_t{bands-1}}}}};
    row.inputs={port(PortDirection::Input,input)};OwnedProcessingContract output;
    require(matrix_row_output_contract(input,output)==Status::ok,"variable matrix row contract");
    row.outputs={{0,PortDirection::Output,output}};
    NumericProcessingGraph g;g.graph_id="variable-matrix-row";g.nodes={source,row};g.edges={{1,4}};
    return g;
}
void variable_matrix_row_tests(){
    auto g=variable_row_graph();const auto input=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    VariableRowObservation observed;

    Running running(g);std::array<float,bands*256> values{};const std::array<PlaneLayout,1> planes{{{0,{256*4,4}}}};
    std::size_t ordinal=0;
    for(std::size_t rows:{1u,8u,3u,8u}){
        for(std::size_t index=0;index<values.size();++index)values[index]=float(ordinal*10000+index);
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=++ordinal;stamp.producer_epoch=3;stamp.generation=1;stamp.timestamp_ticks=std::int64_t(ordinal*400);
        // Borrowed views retain the admitted maximum layout storage; actual
        // extents select the currently populated rows within that allocation.
        const DataFrameView frame{stamp,NumericView{&input,std::as_bytes(std::span(values)),planes,{rows,256},{},{}}};
        const auto view_status=validate_view(frame,limits);float first_value=0;
        const auto load_status=load_element(std::get<NumericView>(frame.payload),{0,0,0},first_value);
        const auto before=allocations;
        const auto processing_status=running.plan->process_numeric_ports(1,frame,*running.state,{}, {VariableRowObservation::receive,nullptr,nullptr,&observed});
        if(view_status!=Status::ok || load_status!=Status::ok || processing_status!=Status::ok)
            std::fprintf(stderr,"variable row rows=%zu process=%u validate_view=%u load_first=%u bytes=%zu\n",rows,unsigned(processing_status),unsigned(view_status),unsigned(load_status),std::get<NumericView>(frame.payload).storage.size());
        require(view_status==Status::ok && load_status==Status::ok && first_value==values[0],"variable matrix fixture has valid admitted storage and data");
        require(processing_status==Status::ok,"missing current row is a nonfatal publication");
        require(allocations==before,"variable row processing does not allocate");
        require(observed.deliveries==ordinal,"variable row publishes every event");
        const auto stats=running.plan->node_stats(4,*running.state);require(stats && stats->errors==0,"missing current row is not a node error");
        if(rows<bands)require(!observed.available && observed.status==Status::unavailable && observed.typed_unavailable,"short matrix emits typed unavailable without stale row");
        else {require(observed.available && observed.status==Status::ok,"selected row recovers when matrix grows");
            for(std::size_t bin=0;bin<256;++bin)require(observed.values[bin]==values[(bands-1)*256+bin],"recovered row contains current values");}
    }
}
void continuous_silence_tests(){
    auto g=graph();g.execution_interval={92,11025};Running running(g);Observation observed;
    for(std::size_t ordinal=0;ordinal<41;++ordinal)require(feed(running,g,ordinal,observed)==Status::ok,"tones process before pause");
    for(std::size_t ordinal=41;ordinal<542;++ordinal){
        // A paused/zero provider may retain or restart its timestamps. Neither
        // sample values nor duplicate/backward coordinates are availability gates.
        const auto ticks=ordinal%3==0?std::int64_t{48000}:std::int64_t{0};
        require(feed(running,g,ordinal,observed,ticks,false,true)==Status::ok,"every paused zero frame processes successfully");
        require(observed.last_available && observed.row_available,"every paused zero frame publishes FFT and selected row");
        near(observed.hz[1],(11025./92.)/512.,"explicit timestamps use actual declared 44.1k analysis interval",1e-10);
        if(ordinal>=440){for(float value:observed.matrix)require(value==0,"fully zero History yields exactly zero FFT");
            for(float value:observed.row)require(value==0,"fully zero History yields exactly zero selected row");}
    }
    require(feed(running,g,542,observed,0)==Status::ok && observed.last_available && observed.row_available,"tone resumes immediately after pause");
    double energy=0;for(float value:observed.matrix)energy+=double(value)*value;require(energy>0,"resumed spectrum contains new tone data");
    const auto stats=running.plan->node_stats(3,*running.state);require(stats && !stats->errors,"silence and timestamp jumps are not FFT errors");
}
    struct TransposeProbe {
        std::array<std::uint64_t,bands*3> values{};std::array<double,3> seconds{};
        static void output(void* user,const NumericPortFrameView& frame){if(frame.identity.node!=5)return;
            auto& probe=*static_cast<TransposeProbe*>(user);require(frame.frame.stamp.available,"transpose output available");
            const auto& value=std::get<NumericView>(frame.frame.payload);
            require(value.actual==std::array<std::size_t,2>{bands,3} && !value.ring.capacity,"transpose materializes swapped ordinary matrix");
            require(value.contract->shape.axes==std::array{Axis::Band,Axis::Time},"transpose swaps physical axes");
            for(std::size_t band=0;band<bands;++band)for(std::size_t time=0;time<3;++time)
                require(load_element(value,{0,band,time},probe.values[band*3+time])==Status::ok,"transpose preserves integer bits");
            require(value.coordinates.axes[1].size()==3*8,"transpose moves Time coordinates to columns");
            for(std::size_t time=0;time<3;++time){std::uint64_t bits=0;
                for(std::size_t byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned char>(value.coordinates.axes[1][time*8+byte]))<<(byte*8);
                std::memcpy(&probe.seconds[time],&bits,8);}
        }
    };
void transpose_event(Running& running,const NumericContract& input,const std::array<PlaneLayout,1>& planes,TransposeProbe& probe,std::size_t ordinal){std::array<std::uint64_t,bands> values{};
        for(std::size_t band=0;band<bands;++band)values[band]=(std::uint64_t{1}<<63)+ordinal*bands+band;
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=ordinal+1;stamp.producer_epoch=3;stamp.generation=1;stamp.timestamp_ticks=std::int64_t(ordinal*400);
        const DataFrameView frame{stamp,NumericView{&input,std::as_bytes(std::span(values)),planes,{bands,1},{},{}}};const auto before=allocations;
        require(running.plan->process_numeric_ports(1,frame,*running.state,{}, {TransposeProbe::output,nullptr,nullptr,&probe})==Status::ok,"UInt64 ring transpose processes");
        require(allocations==before,"transpose does not allocate");}
void transpose_bits_and_ring_tests(){
    auto g=graph(512,false,"hann",true,3);auto input=input_contract();input.dtype=DType::UInt64;
    const auto history_owner=g.nodes[1].outputs[0].contract;
    auto history=std::get<NumericContract>(history_owner.view());history.dtype=input.dtype;
    g.nodes[0].outputs={port(PortDirection::Output,input)};g.nodes[1].inputs={port(PortDirection::Input,input)};
    g.nodes[1].outputs[0]=port(PortDirection::Output,history);
    OwnedProcessingContract transposed;require(matrix_transpose_output_contract(history,transposed)==Status::ok,"UInt64 transpose contract");
    g.nodes[4].inputs={port(PortDirection::Input,history)};g.nodes[4].outputs={{0,PortDirection::Output,transposed}};
    g.nodes={g.nodes[0],g.nodes[1],g.nodes[4]};g.edges={{1,2},{2,5}};
    TransposeProbe probe;

    Running running(g);const std::array<PlaneLayout,1> planes{{{0,{8,0}}}};
    auto event=[&](std::size_t ordinal){transpose_event(running,input,planes,probe,ordinal);};
    for(std::size_t ordinal=0;ordinal<5;++ordinal){event(ordinal);if(ordinal>=2)
        for(std::size_t band=0;band<bands;++band)for(std::size_t time=0;time<3;++time){
            require(probe.values[band*3+time]==(std::uint64_t{1}<<63)+(ordinal-2+time)*bands+band,"wrapped ring is transposed in chronological order without precision loss");
            near(probe.seconds[time],double(ordinal-2+time)/120.,"wrapped Time coordinates remain chronological",1e-12);}}
    ConfiguredProcessingNode gate;gate.id=7;gate.kind="Gate";gate.parameters=RouteProcessingParameters{1,0,false,1};
    gate.inputs={port(PortDirection::Input,history)};gate.outputs={port(PortDirection::Output,history)};
    g.nodes.push_back(std::move(gate));g.edges={{1,2},{2,7},{7,5}};running.replace(g,"UInt64 closed Gate");event(5);
    for(auto value:probe.values)require(value==0,"transpose honors ring silence masks");
}
void contracts_tests(){
    auto g=graph();auto input=std::get<NumericContract>(g.nodes[2].inputs[0].contract.view());OwnedProcessingContract output;
    require(matrix_fft_output_contract(input,300,output)!=Status::ok,"non-power-of-two refused");
    input.coordinates[1]={CoordinateMode::Regular,NumericUnit::Seconds,{0,1},{1,60}};
    require(matrix_fft_output_contract(input,512,output)==Status::ok,"derive regular seconds");
    auto result=std::get<NumericContract>(output.view());require(result.coordinates[1].step==Ratio{15,128} && result.clock.sample_rate==48000,"exact temporal reciprocal and audio provenance");
    input.coordinates[1]={};require(matrix_fft_output_contract(input,512,output)==Status::ok,"derive unknown cadence");
    result=std::get<NumericContract>(output.view());require(result.coordinates[1].unit==NumericUnit::Values,"unknown cadence uses indices");
    auto defaults=graph();defaults.nodes[2].parameters=TransformProcessingParameters{};
    ResolvedProcessingNode default_node;require(bool(resolve_processing_node(defaults.nodes[2],profile().admission,default_node)),"registered temporal defaults resolve");
    require(!defaults.nodes[5].enabled,"separate matrix window starts bypassed");
    require(!node_kind("TemporalFFT"),"removed monolithic type has no legacy alias");
    auto invalid=graph();std::get<TransformProcessingParameters>(invalid.nodes[3].parameters).values[0].value=NumericLiteral{std::uint64_t{8}};
    ResolvedProcessingNode resolved;require(!resolve_processing_node(invalid.nodes[3],profile().admission,resolved),"out-of-range row refused atomically");
    auto constrained=profile();constrained.resources.maximum.scratch_bytes=1;std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    require(PreparedGraph::prepare_numeric(g,constrained,plan,state)!=Status::ok && !plan && !state,"resource budget accounts FFT scratch");
}
}
void* operator new(std::size_t bytes){++allocations;auto* p=std::malloc(bytes?bytes:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
int main(){prepared_window_tests();window_switch_tests();contracts_tests();numerical_tests();resize_and_disconnect_tests();rolling_cadence_recovery_tests();clock_warmup_tests();variable_matrix_row_tests();continuous_silence_tests();transpose_bits_and_ring_tests();std::puts("separate matrix transpose/window/FFT, DFT/padding/ring/bits/coordinates/cadence/silence/resume/no-allocation PASS");}
