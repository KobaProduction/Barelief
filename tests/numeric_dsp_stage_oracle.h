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
        stages[2].resize(trace_n*2);stages[3].resize(trace_n*2);
        stages[4].resize(trace_bins*2);stages[5].resize(trace_bins*2);
        stages[6].resize(trace_bins);stages[7].resize(trace_bins);
        stages[8].resize(trace_bands);
    }
};
void stage_trace_output(void* user,const NumericPortFrameView& frame) {
    if(frame.identity.node<2||frame.identity.node>8||frame.frame.stamp.timestamp_ticks!=4800)return;
    auto& trace=*static_cast<StageTrace*>(user);
    CHECK(frame.status==Status::ok);
    if(frame.status!=Status::ok)return;
    const auto& view=std::get<NumericView>(frame.frame.payload);
    CHECK(view.contract->shape.rank==Rank::Vector&&view.actual[1]==1);
    const auto width=view.actual[0];auto& values=trace.stages[frame.identity.node];
    CHECK(width*view.planes.size()==values.size());
    if(width*view.planes.size()!=values.size())return;
    for(std::size_t channel=0;channel<view.planes.size();++channel)
        for(std::size_t bin=0;bin<width;++bin)
            CHECK(load_element(view,{channel,bin,0},values[channel*width+bin])==Status::ok);
    trace.seen[frame.identity.node]=true;
}
void numeric_full_chain_oracle(bool retag=false) {
    auto graph=dsp_graph();graph.graph_id="dsp.independent.stage.trace";
    for(auto& n:graph.nodes)for(auto* ports:{&n.inputs,&n.outputs})for(auto& port:*ports) {
        auto c=std::get<NumericContract>(port.contract.view());
        // Plane storage is admitted against maximum contract geometry, not
        // only actual length. The ingress owns exactly one 256-sample block.
        if(c.semantic==Semantic::PCM)c.shape.extents[0]={256,256};
        if(c.semantic==Semantic::Window||c.semantic==Semantic::Windowed)c.shape.extents[0]={trace_n,trace_n};
        if(c.semantic==Semantic::SpectrumMagnitude) {
            c.shape.extents[0]={trace_bins,trace_bins};c.coordinates[0].step={48000,trace_n};
        }
        if(c.semantic==Semantic::MelEnergy)c.shape.extents[0]={trace_bands,trace_bands};
        if(c.semantic==Semantic::History)c.shape.extents[1]={trace_bands,trace_bands};
        port.contract=own(c);
    }
    std::get<TransformProcessingParameters>(graph.nodes[1].parameters).values[0].value=NumericLiteral{std::uint64_t(trace_n)};
    std::get<TransformProcessingParameters>(graph.nodes[7].parameters).values[0].value=NumericLiteral{std::uint64_t(trace_bands)};
    if(retag)for(auto& node:graph.nodes)for(auto* ports:{&node.inputs,&node.outputs})for(auto& port:*ports){
        auto contract=std::get<NumericContract>(port.contract.view());
        contract.semantic=Semantic::Numeric;contract.history_element=Semantic::Numeric;
        port.contract=own(contract);
    }
    auto p=profile();GraphExecutor executor(p);application::PreparedExecution pending;
    CHECK(executor.prepare_numeric(graph,{p.admission,p.resources},pending)==Status::ok);
    if(!pending)return;
    CHECK(executor.apply(std::move(pending))==Status::ok);
    StageTrace trace;std::array<float,512> samples{};
    constexpr std::array<PlaneLayout,2> planes{{{0,{4,0}},{256*sizeof(float),{4,0}}}};
    const auto pcm=std::get<NumericContract>(graph.nodes[0].outputs[0].contract.view());
    for(std::size_t block=0;block<19;++block) {
        for(std::size_t channel=0;channel<2;++channel)for(std::size_t sample=0;sample<256;++sample)
            samples[channel*256+sample]=trace_sample(block*256+sample,channel);
        FrameStamp stamp;stamp.sequence=block+1;stamp.source_sequence=stamp.sequence;
        stamp.producer_epoch=5;stamp.timestamp_ticks=std::int64_t(block*256);
        NumericView view{&pcm,std::as_bytes(std::span(samples)),planes,{256,1},{},{}};
        const auto before=allocations;
        const auto status=executor.process_numeric_ports(1,{stamp,view},{},{stage_trace_output,nullptr,nullptr,&trace});
        CHECK(status==Status::ok);
        if(status!=Status::ok) {
            std::printf("DSP trace ingress block=%zu status=%u view=%u\n",block,unsigned(status),unsigned(validate_view({stamp,view},p.admission.frame)));
            return;
        }
        CHECK(allocations==before);
    }
    for(std::size_t id=2;id<=8;++id)CHECK(trace.seen[id]);
    if(!std::all_of(trace.seen.begin()+2,trace.seen.end(),[](bool seen){return seen;}))return;
    const auto close=[](float actual,double expected) {
        CHECK(std::isfinite(actual));CHECK(std::abs(double(actual)-expected)<=5e-6*(1+std::abs(expected)));
    };
    std::array<std::array<double,trace_n>,2> windowed{};
    for(std::size_t channel=0;channel<2;++channel)for(std::size_t sample=0;sample<trace_n;++sample) {
        const auto input=trace_sample(4800-trace_n+sample,channel);
        const auto value=double(input)*(.5*(1-std::cos(2*trace_pi*sample/(trace_n-1))));
        close(trace.stages[2][channel*trace_n+sample],input);
        close(trace.stages[3][channel*trace_n+sample],value);windowed[channel][sample]=value;
    }
    std::array<double,trace_bins> merged{};
    for(std::size_t bin=0;bin<trace_bins;++bin) {
        std::array<double,2> weighted{};
        for(std::size_t channel=0;channel<2;++channel) {
            double real=0,imaginary=0;
            const auto angle=-2*trace_pi*bin/trace_n;
            const auto step_real=std::cos(angle),step_imaginary=std::sin(angle);
            double phase_real=1,phase_imaginary=0;
            for(std::size_t sample=0;sample<trace_n;++sample) {
                real+=windowed[channel][sample]*phase_real;
                imaginary+=windowed[channel][sample]*phase_imaginary;
                const auto next=phase_real*step_real-phase_imaginary*step_imaginary;
                phase_imaginary=phase_real*step_imaginary+phase_imaginary*step_real;phase_real=next;
            }
            const auto magnitude=std::hypot(real,imaginary)/trace_n;
            weighted[channel]=magnitude*7*std::asinh((double(bin)*48000/trace_n)/650);
            close(trace.stages[4][channel*trace_bins+bin],magnitude);
            close(trace.stages[5][channel*trace_bins+bin],weighted[channel]);
        }
        merged[bin]=std::hypot(weighted[0],weighted[1])/std::sqrt(2.);
        close(trace.stages[6][bin],merged[bin]);close(trace.stages[7][bin],merged[bin]);
    }
    DataFrameView history;std::uint64_t filled=0;
    CHECK(executor.plan()->history(9,0,*executor.context(),history,&filled)==Status::ok);
    const auto& history_view=std::get<NumericView>(history.payload);
    CHECK(history_view.actual[0]==history_view.contract->shape.extents[0].maximum && filled==3);
    const auto mel_max=2595*std::log10(1+24000./700);
    for(std::size_t band=0;band<trace_bands;++band) {
        const auto hz=[&](std::size_t index){return 700*(std::pow(10.,mel_max*index/(trace_bands+1)/2595)-1);};
        const auto left=hz(band),center=hz(band+1),right=hz(band+2);double value=0;
        for(std::size_t bin=0;bin<trace_bins;++bin) {
            const auto frequency=double(bin)*48000/trace_n;
            const auto weight=frequency<left||frequency>right?0.:frequency<center?
                (frequency-left)/(center-left):(right-frequency)/(right-center);
            value+=merged[bin]*weight;
        }
        close(trace.stages[8][band],value);float last=0;
        CHECK(load_element(history_view,{0,history_view.actual[0]-1,band},last)==Status::ok);close(last,value);
    }
    std::printf("DSP full-chain oracle: eight changing tones, N4096/Hann/FFT/Bark/RMS/Mel50/History, no hot C++ allocations\n");
}
}
