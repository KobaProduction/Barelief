#include <embedded_support/synthetic_pipeline.h>
#include <domain/numeric/contracts.h>
#include <domain/signal/contracts.h>
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
Status append_window_length(domain::NumericProcessingGraph& graph,domain::OwnedProcessingContract& scalar) {
    domain::ConfiguredProcessingNode length;length.id=7;length.kind="ArrayLength";
    length.parameters=domain::TransformProcessingParameters{};
    length.inputs={{0,domain::PortDirection::Input,graph.nodes[1].outputs[0].contract}};
    auto status=domain::numeric_metadata_output_contract(std::get<NumericContract>(length.inputs[0].contract.view()),false,scalar);
    if(status!=Status::ok)return status;
    length.outputs={{0,domain::PortDirection::Output,scalar}};
    graph.nodes.push_back(std::move(length));graph.edges.push_back({1,7,0,0});
    return Status::ok;
}
void connect_window_product(domain::NumericProcessingGraph& graph,domain::OwnedProcessingContract values) {
    graph.nodes[2].inputs.push_back({1,domain::PortDirection::Input,std::move(values)});
    graph.edges.push_back({8,2,0,1});
}
Status append_window_coefficients(domain::NumericProcessingGraph& graph,const Profile& profile) {
    domain::OwnedProcessingContract scalar;
    auto status=append_window_length(graph,scalar);if(status!=Status::ok)return status;
    domain::ConfiguredProcessingNode coefficients;coefficients.id=8;coefficients.kind="WindowFunction";
    coefficients.parameters=domain::TransformProcessingParameters{{{"size",NumericLiteral{std::uint64_t{profile.fft_size}}},{"function",std::string{"hann"}}}};
    coefficients.inputs={{0,domain::PortDirection::Input,scalar}};
    domain::OwnedProcessingContract values;
    status=domain::array_generator_output_contract(coefficients,std::get<NumericContract>(scalar.view()),values);
    if(status!=Status::ok)return status;
    coefficients.outputs={{0,domain::PortDirection::Output,values}};
    graph.nodes.push_back(std::move(coefficients));graph.edges.push_back({7,8,0,0});
    connect_window_product(graph,std::move(values));return Status::ok;
}
struct GraphComposition {
    const Profile& profile;
    domain::ProcessingAdmissionLimits limits;
    domain::NumericProcessingGraph graph;
    Status port(domain::ConfiguredProcessingNode& node,domain::PortDirection direction,const NumericContract* contract) {
        if(!contract)return Status::ok;
        domain::OwnedProcessingContract owned;
        const auto status=domain::own_processing_contract(*contract,limits.frame,limits.max_owned_metadata_bytes,owned);
        if(status!=Status::ok)return status;
        auto& ports=direction==domain::PortDirection::Input?node.inputs:node.outputs;
        ports.push_back({0,direction,std::move(owned)});return Status::ok;
    }
    Status node(std::string kind,const NumericContract* input,const NumericContract* output,domain::ProcessingParameters parameters) {
        domain::ConfiguredProcessingNode value;
        value.id=graph.nodes.size();value.kind=std::move(kind);value.parameters=std::move(parameters);
        auto status=port(value,domain::PortDirection::Input,input);if(status!=Status::ok)return status;
        status=port(value,domain::PortDirection::Output,output);if(status!=Status::ok)return status;
        if(!graph.nodes.empty())graph.edges.push_back({value.id-1,value.id,0,0});
        graph.nodes.push_back(std::move(value));return Status::ok;
    }
    Status window(const NumericContract& pcm,NumericContract& output) {
        output=pcm;output.semantic=Semantic::Window;output.shape.extents[0]={profile.fft_size,profile.fft_size};
        auto status=node("Window",&pcm,&output,domain::TransformProcessingParameters{{
            {"size",NumericLiteral{std::uint64_t{profile.fft_size}}},
            {"input_rate",NumericLiteral{std::uint64_t{profile.sample_rate}}}}});
        if(status!=Status::ok)return status;
        return node("ArrayMultiply",&output,&output,domain::TransformProcessingParameters{});
    }
    Status spectrum(const NumericContract& window,NumericContract& output) {
        output=window;output.semantic=Semantic::SpectrumMagnitude;output.quantity=Quantity::SpectrumMagnitude;
        output.unit=NumericUnit::Values;output.shape.axes[0]=Axis::Bin;output.shape.extents[0]={profile.fft_size/2,profile.fft_size/2};
        output.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{profile.sample_rate,profile.fft_size}};
        return node("StereoFFT",&window,&output,domain::TransformProcessingParameters{});
    }
    Status mel(const NumericContract& spectrum,NumericContract& output) {
        output=spectrum;output.semantic=Semantic::MelEnergy;output.quantity=Quantity::MelEnergy;
        output.shape.axes[0]=Axis::Band;output.shape.extents[0]={profile.mel_bands,profile.mel_bands};output.coordinates[0]={};
        return node("Mel",&spectrum,&output,domain::TransformProcessingParameters{{
            {"bands",NumericLiteral{std::uint64_t{profile.mel_bands}}},
            {"sample_rate",NumericLiteral{std::uint64_t{profile.sample_rate}}},
            {"bin_hz",NumericLiteral{double(profile.sample_rate)/profile.fft_size}},
            {"first_bin",NumericLiteral{std::uint64_t{0}}},
            {"frequencies_hz",domain::NumericArray{}}}});
    }
    Status analysis(const NumericContract& pcm,NumericContract& output) {
        if(!profile.spectral){output=pcm;return Status::ok;}
        NumericContract windowed,frequency;
        auto status=window(pcm,windowed);if(status!=Status::ok)return status;
        status=spectrum(windowed,frequency);if(status!=Status::ok)return status;
        return mel(frequency,output);
    }
    Status output(const NumericContract& input) {
        auto wire=input;wire.representation.encoding=StorageEncoding::FixedLittleEndian;
        const auto status=node("Converter",&input,&wire,domain::ConverterProcessingParameters{});
        if(status!=Status::ok)return status;
        return node("NetworkOutput",&wire,nullptr,domain::EndpointProcessingParameters{});
    }
    Status compose() {
        graph.graph_id="embedded-synthetic";graph.execution_interval=profile.execution_interval;
        const auto pcm=source_contract(profile);
        auto status=source(pcm);
        if(status!=Status::ok)return status;
        NumericContract result;status=analysis(pcm,result);if(status!=Status::ok)return status;
        status=output(result);if(status!=Status::ok)return status;
        return profile.spectral?append_window_coefficients(graph,profile):Status::ok;
    }
    Status source(const NumericContract& pcm) {
        return node("AudioSource",nullptr,&pcm,domain::EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{profile.sample_rate}}}}});
    }
};
struct StagedSyntheticPlan {
    const Profile& profile;
    domain::NumericProcessingGraph graph{};
    dataflow_runtime::NumericRuntimeProfile runtime{};
    domain::OwnedProcessingContract input{},output{};
    std::shared_ptr<const dataflow_runtime::PreparedGraph> plan{};
    std::unique_ptr<dataflow_runtime::RuntimeContext> context{};
    domain::NodeId sink() const noexcept {return profile.spectral?6:2;}
    Status budget(const application::MemoryRequirements& old,std::size_t id_bytes) {
        const auto source_bytes=std::size_t(profile.channels)*profile.block_samples*sizeof(float);
        if(!old.fits(runtime.resources.max_peak_bytes) || source_bytes>runtime.resources.max_peak_bytes)return Status::exhausted;
        runtime.resources.retained_working_bytes=old.state_bytes+old.buffer_bytes+old.scratch_bytes;
        runtime.resources.max_peak_bytes-=source_bytes;
        const auto metadata=input.owned_bytes()+output.owned_bytes()+id_bytes;
        if(metadata>runtime.resources.max_peak_bytes)return Status::exhausted;
        runtime.resources.max_peak_bytes-=metadata;return Status::ok;
    }
    Status admission() {
        auto device=device_profile(profile);device.runtime=runtime;
        std::array<std::size_t,2*bdsp_protocol::numeric_control::device_limits().nodes> workspace{};
        domain::ProcessingGraphAdmission accepted;
        const auto status=domain::admit_processing_graph(graph,runtime.admission,numeric_capabilities(device),workspace,accepted);
        if(status)return Status::ok;
        return status.error==domain::ProcessingError::ResourceLimit?Status::exhausted:Status::incompatible_contract;
    }
    Status prepare(std::string_view id,const application::MemoryRequirements& old) {
        auto status=default_processing_graph(profile,id,graph);if(status!=Status::ok)return status;
        runtime=numeric_runtime_profile(profile);
        input=graph.nodes.front().outputs.front().contract;output=graph.nodes[sink()].inputs.front().contract;
        status=budget(old,id.size());if(status!=Status::ok)return status;
        status=admission();if(status!=Status::ok)return status;
        return dataflow_runtime::PreparedGraph::prepare_numeric(graph,runtime,plan,context);
    }
};
Status source_reservation(const Profile& profile,std::size_t id_bytes,const domain::OwnedProcessingContract& input,const application::MemoryRequirements& old) {
    const auto runtime=numeric_runtime_profile(profile);
    const auto source_bytes=std::size_t(profile.channels)*profile.block_samples*sizeof(float);
    const auto metadata=input.owned_bytes()+id_bytes;
    if(source_bytes>runtime.resources.max_peak_bytes || metadata>runtime.resources.max_peak_bytes-source_bytes || !old.fits(runtime.resources.max_peak_bytes-source_bytes-metadata))return Status::exhausted;
    return Status::ok;
}
std::array<double,2> source_sample(const Profile& profile,double time,std::span<double> phases) noexcept {
    constexpr double tau=2*std::numbers::pi;
    std::array<double,2> result{};
    for(std::size_t tone=0;tone<phases.size();++tone) {
        const double frequency=(.02+.04*tone)*profile.sample_rate*(1+.12*std::sin(time*.7+tone));
        phases[tone]=std::fmod(phases[tone]+tau*frequency/profile.sample_rate,tau);
        result[0]+=std::sin(phases[tone]);result[1]+=std::sin(phases[tone]+.25*tone);
    }
    return result;
}
struct OutputDelivery {
    domain::NodeId node;std::string_view id;NumericFrameHandler handler;void* user;
    nd::Message message(const dataflow_runtime::NumericPortFrameView& output) const noexcept {
        nd::Message result;result.endpoint={output.graph_id,output.identity.node,nd::Direction::Input,output.identity.port};
        result.contract=output.contract;result.stamp=output.frame.stamp;result.stamp.stream_id=id;
        result.kind=result.stamp.available?nd::Kind::Frame:nd::Kind::Unavailable;
        result.envelope.delivery_generation=result.stamp.generation;
        if(result.stamp.available) {
            const auto& view=std::get<NumericView>(output.frame.payload);
            result.numeric={view.storage,view.planes,view.actual,view.ring,view.coordinates,view.semantic_silence};
        }
        return result;
    }
    void emit(const dataflow_runtime::NumericPortFrameView& output) const noexcept {
        if(output.identity.node!=node || !handler)return;
        handler(user,message(output));
    }
};
void deliver_output(void* context,const dataflow_runtime::NumericPortFrameView& output) {
    static_cast<OutputDelivery*>(context)->emit(output);
}
}
Status default_processing_graph(const Profile& profile,std::string_view id,domain::NumericProcessingGraph& out) {
    const auto admitted=admit(profile,id.size());if(admitted.status!=Status::ok)return admitted.status;
    GraphComposition composition{profile,numeric_runtime_profile(profile).admission,{}};
    const auto status=composition.compose();if(status!=Status::ok)return status;
    out=std::move(composition.graph);return Status::ok;
}
domain::NumericProcessingGraph default_processing_graph(const Profile& p,std::string_view id) {
    domain::NumericProcessingGraph graph;
    (void)default_processing_graph(p,id,graph);return graph;
}
Status SyntheticPipeline::prepare(const Profile& p,std::string_view id) {
    StagedSyntheticPlan candidate{p};
    const auto status=candidate.prepare(id,memory_requirements());if(status!=Status::ok)return status;
    // Stage source storage and ID before publishing the candidate bundle.
    std::vector<float> pcm_storage(std::size_t(p.channels)*p.block_samples);std::string stream(id);
    publish_source(p,std::move(stream),std::move(pcm_storage),std::move(candidate.input));
    plan_=std::move(candidate.plan);context_=std::move(candidate.context);
    output_contract_=std::move(candidate.output);output_=candidate.sink();return Status::ok;
}
void SyntheticPipeline::publish_source(const Profile& profile,std::string id,std::vector<float> storage,domain::OwnedProcessingContract input) {
    profile_=profile;id_=std::move(id);input_=std::move(input);pcm_=std::move(storage);
    for(std::size_t channel=0;channel<profile.channels;++channel)planes_[channel]={channel*profile.block_samples*sizeof(float),{sizeof(float),0}};
    phase_.fill(0);samples_=sequence_=0;
}
Status SyntheticPipeline::prepare_source(const Profile& p,std::string_view id) {
    const auto admitted=admit(p,id.size());if(admitted.status!=Status::ok)return admitted.status;
    const auto runtime=numeric_runtime_profile(p);
    domain::OwnedProcessingContract input;
    auto status=domain::own_processing_contract(source_contract(p),runtime.admission.frame,
        runtime.admission.max_owned_metadata_bytes,input);
    if(status!=Status::ok)return status;
    // Account for the retained bundle while cold preparation stages replacements.
    status=source_reservation(p,id.size(),input,memory_requirements());if(status!=Status::ok)return status;
    std::vector<float> storage(std::size_t(p.channels)*p.block_samples);std::string stream(id);
    clear_processing();
    publish_source(p,std::move(stream),std::move(storage),std::move(input));return Status::ok;
}
void SyntheticPipeline::clear_processing() noexcept {
    plan_.reset();context_.reset();output_contract_={};output_=0;
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
    fill_source_samples();
    const auto contract=input_.view();
    nd::Message message;message.endpoint={"embedded-synthetic",0,nd::Direction::Output,0};
    message.contract=contract;message.stamp=source_stamp();message.kind=nd::Kind::Frame;
    message.numeric={std::as_bytes(std::span(pcm_)),std::span(planes_).first(profile_.channels),{profile_.block_samples,1},{},{},false};
    if(handler)handler(user,message);
    samples_+=profile_.block_samples;return Status::ok;
}
void SyntheticPipeline::fill_source_samples() noexcept {
    for(std::size_t sample=0;sample<profile_.block_samples;++sample) {
        const auto value=source_sample(profile_,double(samples_+sample)/profile_.sample_rate,phase_);
        pcm_[sample]=float(value[0]*.05);
        if(profile_.channels==2)pcm_[profile_.block_samples+sample]=float(value[1]*.05);
    }
}
FrameStamp SyntheticPipeline::source_stamp() noexcept {
    FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence_;
    stamp.generation=stamp.producer_epoch=stamp.epoch=1;
    stamp.graph_revision=1;stamp.timestamp_ticks=samples_;stamp.stream_id=id_;
    stamp.sample_begin=std::int64_t(samples_);stamp.sample_count=profile_.block_samples;stamp.has_sample_interval=true;
    return stamp;
}
Status SyntheticPipeline::process_pcm(const DataFrameView& input,NumericFrameHandler handler,void* user) noexcept {
    if(!plan_)return Status::unavailable;
    OutputDelivery delivery{output_,id_,handler,user};
    return plan_->process_numeric_ports(0,input,*context_,{1,1},{nullptr,deliver_output,nullptr,&delivery});
}
}
