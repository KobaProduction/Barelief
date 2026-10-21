#pragma once

// Integration oracle: the same eight changing tones are checked at every
// prepared-graph stage. Reference DFT/filter weights do not call DSP kernels.
namespace {
constexpr std::size_t trace_n=4096,trace_bins=trace_n/2,trace_bands=50;
constexpr double trace_pi=3.1415926535897932384626433832795;
float trace_sample(std::size_t sample,std::size_t channel) {
    constexpr std::array<double,8> bins{3,8,31.25,67,113.5,241,503,997};
    double value=0;
    for(std::size_t tone=0;tone<bins.size();++tone) {
        const auto amplitude=(.018+.002*tone)*(1+.1*std::sin(sample*.00013+tone));
        value+=amplitude*std::sin(2*trace_pi*bins[tone]*sample/trace_n+channel*.31*tone);
    }
    return float(value*(channel?.7:1.));
}
struct StageTrace {
    std::array<std::vector<float>,9> stages;
    std::array<bool,9> seen{};
    StageTrace() {
        stages[2].resize(trace_n);stages[3].resize(trace_n);
        stages[4].resize(trace_bins);stages[5].resize(trace_bins);
        stages[6].resize(trace_bins);stages[7].resize(trace_bins);
        stages[8].resize(trace_bands);
    }
};
void trace_close(float actual,double expected) {
    CHECK(std::isfinite(actual));
    CHECK(std::abs(double(actual)-expected)<=5e-6*(1+std::abs(expected)));
}
void trace_copy_stage(const NumericView& view,std::vector<float>& values) {
    const auto width=view.actual[0];
    for(std::size_t channel=0;channel<view.planes.size();++channel)
        for(std::size_t bin=0;bin<width;++bin)
            CHECK(load_element(view,{channel,bin,0},values[channel*width+bin])==Status::ok);
}
void stage_trace_output(void* user,const NumericPortFrameView& frame) {
    const auto stage=frame.identity.node==21?3:frame.identity.node;
    if(stage<2||stage>8||frame.identity.node==3||frame.identity.port!=0||frame.frame.stamp.timestamp_ticks!=4800)return;
    auto& trace=*static_cast<StageTrace*>(user);
    CHECK(frame.status==Status::ok);
    if(frame.status!=Status::ok)return;
    const auto& view=std::get<NumericView>(frame.frame.payload);
    CHECK(view.contract->shape.rank==Rank::Vector&&view.actual[1]==1);
    auto& values=trace.stages[stage];
    CHECK(view.actual[0]*view.planes.size()==values.size());
    if(view.actual[0]*view.planes.size()!=values.size())return;
    trace_copy_stage(view,values);
    trace.seen[stage]=true;
}
NumericContract trace_geometry(NumericContract c) {
    // The ingress owns one 256-sample block; intermediate storage uses N4096.
    if(c.semantic==Semantic::PCM)c.shape.extents[0]={256,256};
    if(c.semantic==Semantic::Window||c.semantic==Semantic::Windowed)c.shape.extents[0]={trace_n,trace_n};
    if(c.semantic==Semantic::SpectrumMagnitude) {
        c.shape.extents[0]={trace_bins,trace_bins};c.coordinates[0].step={48000,trace_n};
    }
    if(c.semantic==Semantic::MelEnergy)c.shape.extents[0]={trace_bands,trace_bands};
    if(c.semantic==Semantic::History)c.shape.extents[1]={trace_bands,trace_bands};
    return c;
}
void trace_setting(ConfiguredProcessingNode& node,const char* key,NumericLiteral value) {
    auto& values=std::get<TransformProcessingParameters>(node.parameters).values;
    const auto found=std::find_if(values.begin(),values.end(),[&](const auto& field){return field.key==key;});
    if(found!=values.end())found->value=value;
    else values.push_back({key,value});
}
void trace_transform_geometry(ConfiguredProcessingNode& node) {
    if(node.kind=="Window") {
        trace_setting(node,"size",std::uint64_t(trace_n));
        trace_setting(node,"input_rate",std::uint64_t{48000});
    }
    if(node.kind=="BarkWeighting"||node.kind=="Mel") {
        trace_setting(node,"sample_rate",std::uint64_t{48000});
        trace_setting(node,"bin_hz",48000./trace_n);
        trace_setting(node,"first_bin",std::uint64_t{0});
    }
    if(node.kind=="Mel")trace_setting(node,"bands",std::uint64_t(trace_bands));
}
void trace_coefficient_geometry(ConfiguredProcessingNode& node) {
    if(node.kind=="WindowFunction") {
        trace_setting(node,"size",std::uint64_t(trace_n));
        const auto input=std::get<NumericContract>(node.inputs[0].contract.view());
        CHECK(domain::array_generator_output_contract(node,input,node.outputs[0].contract)==Status::ok);
    }
    if(node.kind=="ArrayMultiply") {
        auto coefficient=std::get<NumericContract>(node.inputs[1].contract.view());
        coefficient.shape.extents[0]={trace_n,trace_n};
        node.inputs[1].contract=own(coefficient);
    }
}
void trace_retag_node(ConfiguredProcessingNode& node) {
    for(auto* ports:{&node.inputs,&node.outputs})for(auto& port:*ports) {
        auto c=std::get<NumericContract>(port.contract.view());
        c.semantic=Semantic::Numeric;c.history_element=Semantic::Numeric;
        port.contract=own(c);
    }
}
void trace_node_geometry(ConfiguredProcessingNode& node,bool retag) {
    for(auto* ports:{&node.inputs,&node.outputs})for(auto& port:*ports) {
        auto c=trace_geometry(std::get<NumericContract>(port.contract.view()));
        port.contract=own(c);
    }
    trace_transform_geometry(node);
    trace_coefficient_geometry(node);
    if(retag)trace_retag_node(node);
}
NumericProcessingGraph trace_graph(bool retag) {
    auto graph=dsp_graph();graph.graph_id="dsp.independent.stage.trace";
    for(auto& node:graph.nodes)trace_node_geometry(node,retag);
    return graph;
}
FrameStamp trace_stamp(std::size_t block) {
    FrameStamp stamp;stamp.sequence=block+1;stamp.source_sequence=stamp.sequence;
    stamp.producer_epoch=5;stamp.timestamp_ticks=std::int64_t(block*256);
    return stamp;
}
bool trace_process_block(GraphExecutor& executor,const NumericContract& pcm,StageTrace& trace,std::size_t block,std::array<float,512>& samples) {
    constexpr std::array<PlaneLayout,2> planes{{{0,{4,0}},{256*sizeof(float),{4,0}}}};
    NumericView view{&pcm,std::as_bytes(std::span(samples)),std::span(planes).first(1),{256,1},{},{}};
    const auto before=allocations;
    const auto status=executor.process_numeric_ports(1,{trace_stamp(block),view},{},{stage_trace_output,nullptr,nullptr,&trace});
    CHECK(status==Status::ok);
    if(status!=Status::ok) {
        std::printf("DSP trace ingress block=%zu status=%u view=%u\n",block,unsigned(status),unsigned(validate_view({trace_stamp(block),view},profile().admission.frame)));
        return false;
    }
    CHECK(allocations==before);return true;
}
bool trace_ingress(GraphExecutor& executor,const NumericContract& pcm,StageTrace& trace) {
    std::array<float,512> samples{};
    for(std::size_t block=0;block<19;++block) {
        for(std::size_t channel=0;channel<1;++channel)for(std::size_t sample=0;sample<256;++sample)
            samples[channel*256+sample]=trace_sample(block*256+sample,channel);
        if(!trace_process_block(executor,pcm,trace,block,samples))return false;
    }
    return true;
}
using TraceWindow=std::array<std::array<double,trace_n>,1>;
TraceWindow trace_window_reference(const StageTrace& trace) {
    TraceWindow windowed{};
    for(std::size_t channel=0;channel<1;++channel)for(std::size_t sample=0;sample<trace_n;++sample) {
        const auto input=trace_sample(4800-trace_n+sample,channel);
        const auto value=double(input)*(.5*(1-std::cos(2*trace_pi*sample/(trace_n-1))));
        trace_close(trace.stages[2][channel*trace_n+sample],input);
        trace_close(trace.stages[3][channel*trace_n+sample],value);
        windowed[channel][sample]=value;
    }
    return windowed;
}
double trace_dft_magnitude(const std::array<double,trace_n>& windowed,std::size_t bin) {
    double real=0,imaginary=0;
    const auto angle=-2*trace_pi*bin/trace_n;
    const auto step_real=std::cos(angle),step_imaginary=std::sin(angle);
    double phase_real=1,phase_imaginary=0;
    for(std::size_t sample=0;sample<trace_n;++sample) {
        real+=windowed[sample]*phase_real;
        imaginary+=windowed[sample]*phase_imaginary;
        const auto next=phase_real*step_real-phase_imaginary*step_imaginary;
        phase_imaginary=phase_real*step_imaginary+phase_imaginary*step_real;phase_real=next;
    }
    return std::hypot(real,imaginary)/trace_n;
}
std::array<double,trace_bins> trace_spectrum_reference(const StageTrace& trace,const TraceWindow& windowed) {
    std::array<double,trace_bins> merged{};
    for(std::size_t bin=0;bin<trace_bins;++bin) {
        std::array<double,1> weighted{};
        for(std::size_t channel=0;channel<1;++channel) {
            const auto magnitude=trace_dft_magnitude(windowed[channel],bin);
            weighted[channel]=magnitude*7*std::asinh((double(bin)*48000/trace_n)/650);
            trace_close(trace.stages[4][channel*trace_bins+bin],magnitude);
            trace_close(trace.stages[5][channel*trace_bins+bin],weighted[channel]);
        }
        merged[bin]=weighted[0];
        trace_close(trace.stages[6][bin],merged[bin]);trace_close(trace.stages[7][bin],merged[bin]);
    }
    return merged;
}
double trace_mel_hz(std::size_t index) {
    const auto mel_max=2595*std::log10(1+24000./700);
    return 700*(std::pow(10.,mel_max*index/(trace_bands+1)/2595)-1);
}
double trace_mel_reference(const std::array<double,trace_bins>& merged,std::size_t band) {
    const auto left=trace_mel_hz(band),center=trace_mel_hz(band+1),right=trace_mel_hz(band+2);
    double value=0;
    for(std::size_t bin=0;bin<trace_bins;++bin) {
        const auto frequency=double(bin)*48000/trace_n;
        const auto weight=frequency<left||frequency>right?0.:frequency<center?
            (frequency-left)/(center-left):(right-frequency)/(right-center);
        value+=merged[bin]*weight;
    }
    return value;
}
void trace_history_reference(GraphExecutor& executor,const StageTrace& trace,const std::array<double,trace_bins>& merged) {
    DataFrameView history;std::uint64_t filled=0;
    CHECK(executor.plan()->history(9,0,*executor.context(),history,&filled)==Status::ok);
    const auto& history_view=std::get<NumericView>(history.payload);
    CHECK(history_view.actual[0]==history_view.contract->shape.extents[0].maximum&&filled==3);
    for(std::size_t band=0;band<trace_bands;++band) {
        const auto value=trace_mel_reference(merged,band);
        trace_close(trace.stages[8][band],value);float last=0;
        CHECK(load_element(history_view,{0,history_view.actual[0]-1,band},last)==Status::ok);
        trace_close(last,value);
    }
}
bool trace_all_stages_seen(const StageTrace& trace) {
    for(std::size_t id=2;id<=8;++id)CHECK(trace.seen[id]);
    return std::all_of(trace.seen.begin()+2,trace.seen.end(),[](bool seen){return seen;});
}
void trace_reference(GraphExecutor& executor,const StageTrace& trace) {
    const auto windowed=trace_window_reference(trace);
    const auto merged=trace_spectrum_reference(trace,windowed);
    trace_history_reference(executor,trace,merged);
}
void numeric_full_chain_oracle(bool retag=false) {
    auto graph=trace_graph(retag);
    auto p=profile();
    // N4096 coefficients, paired input storage and the Mel50 matrix exceed 1 MiB.
    p.resources.maximum.payload_bytes=2*1024*1024;
    GraphExecutor executor(p);application::PreparedExecution pending;
    CHECK(executor.prepare_numeric(graph,{p.admission,p.resources},pending)==Status::ok);
    if(!pending)return;
    CHECK(executor.apply(std::move(pending))==Status::ok);
    StageTrace trace;
    const auto pcm=std::get<NumericContract>(graph.nodes[0].outputs[0].contract.view());
    if(!trace_ingress(executor,pcm,trace))return;
    if(!trace_all_stages_seen(trace))return;
    trace_reference(executor,trace);
    std::printf("DSP full-chain oracle: eight changing tones, N4096/Hann/FFT/Bark/explicit gain/Mel50/History, no hot C++ allocations\n");
}
}
