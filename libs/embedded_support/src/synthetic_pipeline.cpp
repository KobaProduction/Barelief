#include <embedded_support/synthetic_pipeline.h>
#include <cmath>
#include <numbers>
#include <limits>
namespace embedded_support {
using namespace signal_contracts;
namespace nd=bdsp_protocol::numeric_data;
namespace {
NumericContract source_contract(const Profile& p) noexcept {
    static constexpr std::array<std::uint32_t,2> channels{0,1};
    NumericContract pcm;pcm.dtype=DType::Float32;pcm.semantic=Semantic::PCM;pcm.quantity=Quantity::Amplitude;
    pcm.unit=NumericUnit::NormalizedAmplitude;pcm.channel_order=std::span(channels).first(p.channels);
    pcm.shape.rank=Rank::Vector;pcm.shape.axes={Axis::Sample,Axis::None};
    pcm.shape.extents={ExtentBounds{1,p.block_samples},ExtentBounds{1,1}};
    pcm.clock={"embedded-source",{1,p.sample_rate},p.sample_rate};return pcm;
}
}
Status default_processing_graph(const Profile& p,std::string_view id,domain::NumericProcessingGraph& graph_out) {
    auto admitted=admit(p,id.size());if(admitted.status!=Status::ok)return admitted.status;
    auto runtime=numeric_runtime_profile(p);
    static constexpr std::array<std::uint32_t,2> channels{0,1};
    auto pcm=source_contract(p);
    domain::NumericProcessingGraph graph;graph.graph_id="embedded-synthetic";graph.execution_interval=p.execution_interval;
    auto own=[&](const NumericContract& c,domain::OwnedProcessingContract& result) {
        return domain::own_processing_contract(c,runtime.admission.frame,runtime.admission.max_owned_metadata_bytes,result);
    };
    auto status=Status::ok;
    auto node=[&](std::string kind,const NumericContract* in,const NumericContract* out,domain::ProcessingParameters params) {
        domain::ConfiguredProcessingNode n;n.id=graph.nodes.size();n.kind=std::move(kind);n.parameters=std::move(params);
        for(bool is_input:{true,false}) {
            const auto* c=is_input?in:out;if(!c)continue;domain::OwnedProcessingContract owned;
            auto s=own(*c,owned);if(s!=Status::ok)return s;
            (is_input?n.inputs:n.outputs).push_back({0,is_input?domain::PortDirection::Input:domain::PortDirection::Output,std::move(owned)});
        }
        if(!graph.nodes.empty())graph.edges.push_back({n.id-1,n.id,0,0});
        graph.nodes.push_back(std::move(n));return Status::ok;
    };
    status=node("AudioSource",nullptr,&pcm,domain::EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{p.sample_rate}}}}});if(status!=Status::ok)return status;
    auto result=pcm;
    if(p.spectral) {
        auto window=pcm;window.semantic=Semantic::Window;window.shape.extents[0]={p.fft_size,p.fft_size};
        status=node("Window",&pcm,&window,domain::TransformProcessingParameters{{{"size",NumericLiteral{std::uint64_t{p.fft_size}}}}});if(status!=Status::ok)return status;
        auto windowed=window;windowed.semantic=Semantic::Windowed;
        status=node("WindowFunction",&window,&windowed,domain::TransformProcessingParameters{{{"function",std::string("hann")}}});if(status!=Status::ok)return status;
        auto spectrum=window;spectrum.semantic=Semantic::SpectrumMagnitude;spectrum.quantity=Quantity::SpectrumMagnitude;
        spectrum.unit=NumericUnit::Values;spectrum.shape.axes[0]=Axis::Bin;spectrum.shape.extents[0]={p.fft_size/2,p.fft_size/2};
        spectrum.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{p.sample_rate,p.fft_size}};
        status=node("StereoFFT",&windowed,&spectrum,domain::TransformProcessingParameters{});if(status!=Status::ok)return status;
        auto mono=spectrum;mono.channel_order=std::span(channels).first(1);

        result=mono;result.semantic=Semantic::MelEnergy;result.quantity=Quantity::MelEnergy;
        result.shape.axes[0]=Axis::Band;result.shape.extents[0]={p.mel_bands,p.mel_bands};result.coordinates[0]={};
        status=node("Mel",&mono,&result,domain::TransformProcessingParameters{{{"bands",NumericLiteral{std::uint64_t{p.mel_bands}}}}});if(status!=Status::ok)return status;
    }
    // Explicit format conversion belongs to this graph. DATA never relabels Native bytes.
    auto wire=result;wire.representation.encoding=StorageEncoding::FixedLittleEndian;
    status=node("Converter",&result,&wire,domain::ConverterProcessingParameters{});if(status!=Status::ok)return status;
    status=node("NetworkOutput",&wire,nullptr,domain::EndpointProcessingParameters{});if(status!=Status::ok)return status;
    graph_out=std::move(graph);return Status::ok;
}
domain::NumericProcessingGraph default_processing_graph(const Profile& p,std::string_view id) {
    domain::NumericProcessingGraph graph;
    (void)default_processing_graph(p,id,graph);return graph;
}
Status SyntheticPipeline::prepare(const Profile& p,std::string_view id) {
    domain::NumericProcessingGraph graph;
    auto status=default_processing_graph(p,id,graph);if(status!=Status::ok)return status;
    auto runtime=numeric_runtime_profile(p);
    const auto source_bytes=std::size_t(p.channels)*p.block_samples*sizeof(float);
    const auto old=memory_requirements();
    if(!old.fits(runtime.resources.max_peak_bytes) || source_bytes>runtime.resources.max_peak_bytes)return Status::exhausted;
    runtime.resources.retained_working_bytes=old.state_bytes+old.buffer_bytes+old.scratch_bytes;
    runtime.resources.max_peak_bytes-=source_bytes;
    auto input=graph.nodes.front().outputs.front().contract;
    auto output_contract=graph.nodes.back().inputs.front().contract;
    const auto source_metadata=input.owned_bytes()+output_contract.owned_bytes()+id.size();
    if(source_metadata>runtime.resources.max_peak_bytes)return Status::exhausted;
    runtime.resources.max_peak_bytes-=source_metadata;
    auto device=device_profile(p);device.runtime=runtime;
    std::array<std::size_t,16> workspace{};domain::ProcessingGraphAdmission accepted;
    const auto admission=domain::admit_processing_graph(graph,runtime.admission,numeric_capabilities(device),workspace,accepted);
    if(!admission)return admission.error==domain::ProcessingError::ResourceLimit?Status::exhausted:Status::incompatible_contract;
    std::shared_ptr<const dataflow_runtime::PreparedGraph> candidate;std::unique_ptr<dataflow_runtime::RuntimeContext> context;
    status=dataflow_runtime::PreparedGraph::prepare_numeric(graph,runtime,candidate,context);if(status!=Status::ok)return status;
    // Stage source storage and ID before publishing the candidate bundle.
    std::vector<float> pcm_storage(std::size_t(p.channels)*p.block_samples);std::string stream(id);
    profile_=p;id_=std::move(stream);plan_=std::move(candidate);context_=std::move(context);
    input_=std::move(input);output_contract_=std::move(output_contract);pcm_=std::move(pcm_storage);
    for(std::size_t channel=0;channel<p.channels;++channel)planes_[channel]={channel*p.block_samples*sizeof(float),{sizeof(float),0}};
    output_=graph.nodes.back().id;phase_.fill(0);samples_=sequence_=0;return Status::ok;
}
Status SyntheticPipeline::prepare_source(const Profile& p,std::string_view id) {
    const auto admitted=admit(p,id.size());if(admitted.status!=Status::ok)return admitted.status;
    const auto runtime=numeric_runtime_profile(p);
    domain::OwnedProcessingContract input;
    auto status=domain::own_processing_contract(source_contract(p),runtime.admission.frame,
        runtime.admission.max_owned_metadata_bytes,input);
    if(status!=Status::ok)return status;
    // Account for the retained bundle while cold preparation stages replacements.
    const auto source_bytes=std::size_t(p.channels)*p.block_samples*sizeof(float);
    const auto metadata_bytes=input.owned_bytes()+id.size();
    const auto old=memory_requirements();
    if(source_bytes>runtime.resources.max_peak_bytes || metadata_bytes>runtime.resources.max_peak_bytes-source_bytes ||
       !old.fits(runtime.resources.max_peak_bytes-source_bytes-metadata_bytes))return Status::exhausted;
    std::vector<float> storage(std::size_t(p.channels)*p.block_samples);std::string stream(id);
    plan_.reset();context_.reset();output_contract_={};output_=0;
    profile_=p;id_=std::move(stream);input_=std::move(input);pcm_=std::move(storage);
    for(std::size_t channel=0;channel<p.channels;++channel)planes_[channel]={channel*p.block_samples*sizeof(float),{sizeof(float),0}};
    phase_.fill(0);samples_=sequence_=0;return Status::ok;
}
application::MemoryRequirements SyntheticPipeline::memory_requirements() const noexcept {
    auto memory=plan_?plan_->memory_requirements():application::MemoryRequirements{};
    memory.buffer_bytes+=pcm_.capacity()*sizeof(float)+input_.owned_bytes()+output_contract_.owned_bytes()+id_.capacity();return memory;
}
nd::Binding SyntheticPipeline::output_binding() const noexcept {
    return {{1,1},{"embedded-synthetic",output_,nd::Direction::Input,0},id_,output_contract_.view(),{},ValueDomain::FiniteOnly};
}
Status SyntheticPipeline::tick(NumericFrameHandler handler,void* user) noexcept {
    if(!plan_)return Status::unavailable;
    struct Delivery {SyntheticPipeline* pipeline;NumericFrameHandler handler;void* user;Status status=Status::ok;}
        delivery{this,handler,user};
    const auto status=tick_source([](void* ctx,const nd::Message& message) {
        auto& d=*static_cast<Delivery*>(ctx);
        const auto& numeric=std::get<NumericContract>(message.contract);
        DataFrameView frame{message.stamp,NumericView{&numeric,message.numeric.storage,message.numeric.planes,
            message.numeric.actual,message.numeric.ring,message.numeric.coordinates,message.numeric.semantic_silence}};
        d.status=d.pipeline->process_pcm(frame,d.handler,d.user);
    },&delivery);
    return status==Status::ok?delivery.status:status;
}
Status SyntheticPipeline::tick_source(NumericFrameHandler handler,void* user) noexcept {
    if(pcm_.empty())return Status::unavailable;
    if(sequence_==UINT64_MAX || samples_>std::uint64_t(INT64_MAX)-profile_.block_samples)return Status::exhausted;
    constexpr double tau=2*std::numbers::pi;
    for(std::size_t i=0;i<profile_.block_samples;++i) {
        double left=0,right=0,time=double(samples_+i)/profile_.sample_rate;
        for(std::size_t tone=0;tone<phase_.size();++tone) {
            const double frequency=(.02+.04*tone)*profile_.sample_rate*(1+.12*std::sin(time*.7+tone));
            phase_[tone]=std::fmod(phase_[tone]+tau*frequency/profile_.sample_rate,tau);
            left+=std::sin(phase_[tone]);right+=std::sin(phase_[tone]+.25*tone);
        }
        pcm_[i]=float(left*.05);if(profile_.channels==2)pcm_[profile_.block_samples+i]=float(right*.05);
    }
    const auto contract=input_.view();
    FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence_;stamp.generation=stamp.producer_epoch=stamp.epoch=1;
    stamp.graph_revision=1;stamp.timestamp_ticks=samples_;stamp.stream_id=id_;stamp.sample_begin=std::int64_t(samples_);stamp.sample_count=profile_.block_samples;stamp.has_sample_interval=true;
    nd::Message message;message.endpoint={"embedded-synthetic",0,nd::Direction::Output,0};
    message.contract=contract;message.stamp=stamp;message.kind=nd::Kind::Frame;
    message.numeric={std::as_bytes(std::span(pcm_)),std::span(planes_).first(profile_.channels),{profile_.block_samples,1},{},{},false};
    if(handler)handler(user,message);
    samples_+=profile_.block_samples;return Status::ok;
}
Status SyntheticPipeline::process_pcm(const DataFrameView& input,NumericFrameHandler handler,void* user) noexcept {
    if(!plan_)return Status::unavailable;
    struct Delivery {domain::NodeId node;std::string_view id;NumericFrameHandler handler;void* user;} delivery{output_,id_,handler,user};
    return plan_->process_numeric_ports(0,input,*context_,{1,1},{nullptr,[](void* ctx,const dataflow_runtime::NumericPortFrameView& out) {
        auto& d=*static_cast<Delivery*>(ctx);if(out.identity.node!=d.node || !d.handler)return;
        nd::Message message;message.endpoint={out.graph_id,out.identity.node,nd::Direction::Input,out.identity.port};
        message.contract=out.contract;message.stamp=out.frame.stamp;message.stamp.stream_id=d.id;
        message.kind=message.stamp.available?nd::Kind::Frame:nd::Kind::Unavailable;
        message.envelope.delivery_generation=message.stamp.generation;
        if(message.stamp.available) {const auto& view=std::get<NumericView>(out.frame.payload);
            message.numeric={view.storage,view.planes,view.actual,view.ring,view.coordinates,view.semantic_silence};}
        d.handler(d.user,message);
    },nullptr,&delivery});
}
}
