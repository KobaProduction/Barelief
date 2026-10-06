#include <embedded_support/synthetic_pipeline.h>
#include <cmath>
#include <numbers>
namespace embedded_support {
Status SyntheticPipeline::prepare(const Profile &profile, std::string_view id) {
    auto admission=admit(profile,id.size());if(admission.status!=Status::ok)return admission.status;
    domain::GraphDefinition graph;graph.graph_id="embedded-synthetic";
    graph.nodes.push_back({0,domain::NodeKind::AudioSource,domain::AudioSourceParameters{},true});
    auto add=[&](domain::NodeKind kind,domain::NodeParameters params){
        auto node=static_cast<domain::NodeId>(graph.nodes.size());
        graph.nodes.push_back({node,kind,std::move(params),true});graph.edges.push_back({node-1,node});
    };
    if(profile.spectral) {
        add(domain::NodeKind::Window,domain::WindowParameters{profile.fft_size});
        add(domain::NodeKind::WindowFunction,domain::WindowFunctionParameters{});
        add(domain::NodeKind::StereoFFT,domain::NoParameters{});
        add(domain::NodeKind::SpectrumMerge,domain::MergeParameters{});
        add(domain::NodeKind::Mel,domain::MelParameters{profile.mel_bands});
    }
    add(domain::NodeKind::NetworkOutput,domain::NetworkParameters{profile.spectral?signal_contracts::FrameType::Mel:signal_contracts::FrameType::PCM,"",std::string(id)});
    const std::array<dataflow_runtime::IngressSpec,1> ingress{{{0,{signal_contracts::FrameType::PCM,profile.sample_rate,0,0,0,profile.block_samples},profile.block_samples}}};
    std::shared_ptr<const dataflow_runtime::PreparedGraph> candidate;
    std::unique_ptr<dataflow_runtime::RuntimeContext> context;
    const auto s=dataflow_runtime::PreparedGraph::prepare(graph,ingress,runtime_limits(profile),candidate,context);
    if(s!=Status::ok)return s;
    admission=admit(profile,id.size(),candidate->memory_requirements());
    // PCM vectors are owned by this source, beyond the executor's workspaces.
    if(admission.status!=Status::ok || 2*profile.block_samples*sizeof(float)>profile.memory_bytes-admission.reserved_bytes)return Status::exhausted;
    profile_=profile;id_=id;plan_=std::move(candidate);context_=std::move(context);
    output_=static_cast<domain::NodeId>(graph.nodes.size()-1);
    left_.resize(profile.block_samples);right_.resize(profile.block_samples);
    phase_.fill(0);samples_=sequence_=0;return Status::ok;
}
application::MemoryRequirements SyntheticPipeline::memory_requirements() const noexcept {
    return plan_?plan_->memory_requirements():application::MemoryRequirements{};
}
Status SyntheticPipeline::tick(FrameHandler handler, void *user) noexcept {
    if(!plan_)return Status::unavailable;
    constexpr double tau=2*std::numbers::pi;
    for(std::size_t i=0;i<left_.size();++i) {
        double l=0,r=0,time=double(samples_+i)/profile_.sample_rate;
        for(std::size_t tone=0;tone<phase_.size();++tone) {
            const double frequency=(.02+.04*tone)*profile_.sample_rate*(1+.12*std::sin(time*.7+tone));
            phase_[tone]=std::fmod(phase_[tone]+tau*frequency/profile_.sample_rate,tau);
            l+=std::sin(phase_[tone]);r+=std::sin(phase_[tone]+.25*tone);
        }
        left_[i]=float(l*.05);right_[i]=float(r*.05);
    }
    signal_contracts::FrameView pcm;pcm.type=signal_contracts::FrameType::PCM;
    pcm.metadata.sequence=++sequence_;pcm.metadata.source_sequence=sequence_;
    pcm.metadata.generation=pcm.metadata.producer_epoch=pcm.metadata.epoch=1;
    pcm.metadata.graph_revision=1;pcm.metadata.sample_rate=profile_.sample_rate;
    pcm.metadata.timestamp_ns=std::int64_t(samples_*1000000000ULL/profile_.sample_rate);
    pcm.metadata.stream_id=id_;pcm.left=left_;pcm.right=right_;
    const auto status=process_pcm(pcm,handler,user);
    samples_+=left_.size();return status;
}
Status SyntheticPipeline::process_pcm(const signal_contracts::FrameView &pcm, FrameHandler handler, void *user) noexcept {
    if(!plan_)return Status::unavailable;
    struct Delivery {domain::NodeId node;std::string_view id;FrameHandler handler;void *user;} delivery{output_,id_,handler,user};
    const auto status=plan_->process(0,pcm,*context_,[](void *ctx,const dataflow_runtime::OutputView &out){
        auto &d=*static_cast<Delivery *>(ctx);
        if(out.node==d.node && d.handler) {auto frame=out.frame;frame.metadata.stream_id=d.id;d.handler(d.user,frame);}
    },&delivery);
    return status;
}
}
