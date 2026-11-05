#include "support.h"
#include <offline/audio_decoder.h>
#include <desktop/numeric_source_format.h>
#include <desktop/numeric_project_document.h>
#include <core_app/numeric_default_graph.h>
#include <domain/graph/node_factory.h>
#include <speex_backend.h>
#include <dsp_kernels/prepared_resampler.h>
#include <dsp_kernels/sample_cadence.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <chrono>
#include <vector>

namespace rhythm_replay {
namespace {
using namespace signal_contracts;
using namespace dataflow_runtime;
using detail::require;
std::uint64_t monotonic_time(void*) noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
NumericRuntimeProfile replay_profile() {
    constexpr FrameLimits frame{64,131072,524288,65536,4096,1048576};
    constexpr domain::ProcessingAdmissionLimits admission{512,2048,128,128,1048576,16777216,frame};
    NumericRuntimeProfile profile{admission,{{16777216,67108864,67108864,1048576},0,268435456},
        {131072,64,524288,524288,16777216},{true,true,true}};
    profile.music_enabled=true;profile.experimental_resampling_enabled=true;
    profile.resampler_backend=&desktop_resampling::speex_backend_factory();
    profile.monotonic_nanoseconds=monotonic_time;return profile;
}
void require_status(Status status,const char* operation) {
    if(status!=Status::ok)throw std::runtime_error(std::string(operation)+": status "+std::to_string(unsigned(status)));
}
void save_bytes(const QString& path,const QByteArray& content) {
    QSaveFile file(path);require(file.open(QIODevice::WriteOnly),"Cannot open replay artifact");
    require(file.write(content)==content.size() && file.commit(),"Cannot commit replay artifact");
}
QByteArray hash_file(const QString& path) {
    QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot hash replay input");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file),"Cannot read replay input hash");return hash.result().toHex();
}
void configure_rate(domain::NumericProcessingGraph& graph,unsigned rate) {
    graph.execution_interval={1,rate};
    for(auto& node:graph.nodes)if(node.id==1102 || node.kind=="MatrixAutocorrelation" || node.kind=="MatrixFFT" || node.kind=="TempoEvidence"){
        auto& fields=std::get<domain::TransformProcessingParameters>(node.parameters).values;
        for(auto& field:fields)if(field.key=="feature_rate_fps" || field.key=="frame_rate_fps" ||
            (node.kind=="MatrixFFT" && field.key=="sample_rate"))field.value=NumericLiteral{std::uint64_t(rate)};
    }
}
void declare_file_packets(domain::NumericProcessingGraph& graph,std::size_t maximum,const FrameLimits& limits,std::size_t metadata) {
    for(const auto id:{13,200}){
        const auto source=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& node){return node.id==id;});
        require(source!=graph.nodes.end(),"File ingress source/selector missing");
        auto& port=source->outputs.front();auto value=std::get<NumericContract>(port.contract.view());
        value.shape.extents[0]={1,id==13?maximum:std::max(value.shape.extents[0].maximum,std::uint64_t(maximum))};
        domain::OwnedProcessingContract owned;
        require_status(domain::own_processing_contract(value,limits,metadata,owned),"Cannot declare bounded file route");
        port.contract=std::move(owned);
    }
}
void prepare_file_route(domain::NumericProcessingGraph& graph,const domain::ProcessingAdmissionLimits& limits) {
    const auto source=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.id==13;});
    const auto selector=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.id==200;});
    require(source!=graph.nodes.end() && selector!=graph.nodes.end(),"File route endpoints missing");
    const auto lane=std::find_if(selector->inputs.begin(),selector->inputs.end(),[](const auto& input){return input.id==2;});
    require(lane!=selector->inputs.end(),"File selector lane missing");lane->contract=source->outputs.front().contract;
    domain::NumericProcessingGraph evaluated,executable;
    const auto result=domain::resolve_numeric_parameter_bindings(graph,limits,evaluated,executable,200);
    if(!result)throw std::runtime_error("File packet route: node "+std::to_string(result.node)+", port "+std::to_string(result.port)+", error "+std::to_string(unsigned(result.error)));
    graph=std::move(evaluated);
}
void bind_analyzer_history(domain::NumericProcessingGraph& graph,const domain::ProcessingAdmissionLimits& limits) {
    const auto history=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.id==7;});
    const auto analyzer=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.id==1102;});
    require(history!=graph.nodes.end() && analyzer!=graph.nodes.end(),"Replay history/analyzer missing");
    require(bool(domain::configure_processing_input(*analyzer,history->outputs[0].contract.view(),limits)),"Cannot bind explicit analyzer history");
}
void validate_options(const Options& options) {
    require(!options.input.isEmpty() && !options.output_directory.isEmpty(),"Replay input and output are required");
    require(options.packet_samples>0 && options.packet_samples<=4096,"Replay packet must have 1..4096 samples");
    require(options.analysis_rate==30 || options.analysis_rate==45 || options.analysis_rate==60,"Replay analysis rate must be 30, 45 or 60 Hz");
}
std::size_t maximum_cadence_hop(unsigned sample_rate,unsigned analysis_rate) {
    dsp_kernels::SampleCadence cadence;
    require(cadence.prepare(sample_rate,1,analysis_rate),"Cannot declare replay cadence capacity");
    return cadence.base+std::size_t(cadence.fraction!=0);
}
std::size_t minimum_cadence_fifo(std::size_t packet,unsigned native_rate,unsigned analysis_rate) {
    const auto batch=dsp_kernels::PreparedResampler::maximum_output(packet,native_rate,rhythm_replay_graph::pcm_rate);
    const auto hop=maximum_cadence_hop(rhythm_replay_graph::pcm_rate,analysis_rate);
    // One extra sample covers a rounded SRC endpoint before the native ceil deadline.
    require(batch && hop<SIZE_MAX && batch<=SIZE_MAX-hop-1,"Replay cadence FIFO bound exhausted");
    return batch+hop+1;
}
void configure_cadence_fifo(domain::NumericProcessingGraph& graph,std::size_t samples) {
    require(samples<=UINT64_MAX/1000,"Replay cadence FIFO duration exhausted");
    const auto duration=std::uint64_t(samples)*1000,rate=std::uint64_t(rhythm_replay_graph::pcm_rate);
    const auto milliseconds=duration/rate+std::uint64_t(duration%rate!=0);
    require(milliseconds>=domain::min_mixer_fifo_ms && milliseconds<=domain::max_mixer_fifo_ms,"Replay cadence FIFO exceeds its bounded profile");
    const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.id==rhythm_replay_graph::pcm_mixer;});
    require(found!=graph.nodes.end(),"Replay cadence mixer missing");
    auto* parameters=std::get_if<domain::MixerProcessingParameters>(&found->parameters);
    require(parameters && parameters->consumption==domain::MixerConsumption::ClockedFifo,"Replay cadence requires an explicit FIFO mixer");
    parameters->fifo_milliseconds=std::max(parameters->fifo_milliseconds,std::uint32_t(milliseconds));
}
void validate_cadence_contract(const PreparedGraph& plan,std::size_t maximum_hop) {
    const auto declared=plan.port_contract(rhythm_replay_graph::pcm_mixer,domain::PortDirection::Output,0);
    require(declared.has_value(),"Replay PCM cadence contract missing");
    const auto* pcm=std::get_if<NumericContract>(&*declared);
    require(pcm && pcm->dtype==DType::Float32 && pcm->shape.rank==Rank::Vector && pcm->channel_order.size()==1,
        "Replay PCM cadence requires a mono Float32 vector");
    require(pcm->clock.domain=="local" && pcm->clock.sample_rate==rhythm_replay_graph::pcm_rate &&
        pcm->clock.tick_seconds==Ratio{1,rhythm_replay_graph::pcm_rate},"Replay PCM cadence clock differs");
    require(pcm->shape.extents[0].minimum<=1 && pcm->shape.extents[0].maximum>=maximum_hop,
        "Replay PCM cadence contract cannot hold its rational hop");
}
class ReplayCadence {
    dsp_kernels::SampleCadence native_,pcm_;
    std::uint64_t native_floor_=0,native_deadline_=0,pcm_begin_=0,pcm_end_=0,sequence_=0;
    std::size_t hop_=0;
    void advance_deadline() {
        const auto native_hop=native_.next();hop_=pcm_.next();
        require(native_hop && hop_ && native_floor_<=UINT64_MAX-native_hop,"Replay cadence source timeline exhausted");
        native_floor_+=native_hop;
        require(!native_.remainder || native_floor_<UINT64_MAX,"Replay cadence deadline exhausted");
        native_deadline_=native_floor_+std::uint64_t(native_.remainder!=0);
        require(pcm_begin_<=std::uint64_t(INT64_MAX)-hop_,"Replay cadence PCM timeline exhausted");
        pcm_end_=pcm_begin_+hop_;
    }
public:
    MixerInputStats queue(const PreparedGraph& plan,const RuntimeContext& state) const {
        const auto stats=plan.mixer_input_stats(rhythm_replay_graph::pcm_mixer,0,state);
        require(stats.has_value(),"Replay PCM cadence FIFO missing");
        require(!stats->dropped_samples && !stats->underrun_samples,"Replay PCM cadence FIFO dropped or invented samples");
        require(stats->depth<=stats->capacity,"Replay PCM cadence FIFO depth exceeds capacity");
        return *stats;
    }
    void prepare(unsigned native_rate,unsigned analysis_rate,std::size_t packet,const PreparedGraph& plan,const RuntimeContext& state) {
        require(native_.prepare(native_rate,1,analysis_rate) && pcm_.prepare(rhythm_replay_graph::pcm_rate,1,analysis_rate),
            "Cannot prepare exact replay cadence");
        const auto maximum_hop=pcm_.base+std::size_t(pcm_.fraction!=0);
        validate_cadence_contract(plan,maximum_hop);
        require(queue(plan,state).capacity>=minimum_cadence_fifo(packet,native_rate,analysis_rate),
            "Replay PCM cadence FIFO cannot hold one source packet and pending hop");
        advance_deadline();
    }
    bool due(std::uint64_t source_samples) const noexcept {return source_samples>=native_deadline_;}
    std::size_t hop() const noexcept {return hop_;}
    FrameStamp tick() {
        require(sequence_<UINT64_MAX,"Replay cadence sequence exhausted");
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence_;stamp.epoch=stamp.producer_epoch=1;
        stamp.stream_id="file-replay";stamp.has_sample_interval=true;stamp.discontinuity=pcm_begin_==0;
        stamp.sample_begin=stamp.timestamp_ticks=std::int64_t(pcm_begin_);stamp.sample_count=hop_;
        return stamp;
    }
    void commit() {pcm_begin_=pcm_end_;advance_deadline();}
    void finish(std::uint64_t source_samples,const MixerInputStats& queued) const {
        if(due(source_samples))throw std::runtime_error("Replay cadence EOF lacks real PCM: input="+std::to_string(source_samples)+
            " deadline="+std::to_string(native_deadline_)+" queued="+std::to_string(queued.depth)+" hop="+std::to_string(hop_));
    }
};
class Session {
    const Options& options_;
    Result result_;
    desktop_analysis::offline::AudioDecoder decoder_;
    NumericRuntimeProfile profile_=replay_profile();
    std::shared_ptr<const PreparedGraph> plan_;
    std::unique_ptr<RuntimeContext> state_;
    domain::OwnedProcessingContract source_;
    detail::Observations observations_;
    ReplayCadence cadence_;
    std::vector<float> buffer_;
    std::size_t source_capacity_=0;
    std::uint64_t sequence_=0;
    QByteArray graph_json_,input_sha_;
    void prepare_graph() {
        auto graph=numeric_rhythm_replay_graph();configure_rate(graph,options_.analysis_rate);QString error;
        declare_file_packets(graph,source_capacity_,profile_.admission.frame,profile_.admission.max_owned_metadata_bytes);
        prepare_file_route(graph,profile_.admission);
        if(!desktop::resolve_numeric_source_format(graph,13,result_.source_rate,1,profile_.admission,&error))throw std::runtime_error(error.toStdString());
        bind_analyzer_history(graph,profile_.admission);
        configure_cadence_fifo(graph,minimum_cadence_fifo(options_.packet_samples,result_.source_rate,options_.analysis_rate));
        require_status(PreparedGraph::prepare_numeric(graph,profile_,plan_,state_),"Cannot prepare replay graph");
        cadence_.prepare(result_.source_rate,options_.analysis_rate,options_.packet_samples,*plan_,*state_);
        const auto contract=plan_->port_contract(13,domain::PortDirection::Output,0);
        require(contract.has_value(),"Replay source contract missing");
        require_status(domain::own_processing_contract(*contract,profile_.admission.frame,profile_.admission.max_owned_metadata_bytes,source_),"Cannot own replay source");
        const auto numeric=std::get<NumericContract>(source_.view());
        require(numeric.shape.extents[0].minimum<=options_.packet_samples && options_.packet_samples<=numeric.shape.extents[0].maximum,"Replay packet exceeds source contract");
        require(bool(desktop::encode_numeric_graph(graph,graph_json_)),"Cannot serialize replay graph");
    }
    void prepare_source_storage() {
        source_capacity_=std::max(options_.packet_samples,maximum_cadence_hop(result_.source_rate,options_.analysis_rate));
        require(source_capacity_<=profile_.admission.frame.max_elements &&
            source_capacity_<=profile_.admission.frame.max_payload_bytes/sizeof(float),"Replay source capacity exceeds its bounded profile");
        buffer_.resize(source_capacity_);
    }
    void prepare() {
        validate_options(options_);QString error;
        if(!decoder_.open(options_.input,error,dsp_kernels::ChannelPolicy::Mean))throw std::runtime_error(error.toStdString());
        result_.source_rate=decoder_.sample_rate();input_sha_=hash_file(options_.input);
        prepare_source_storage();
        prepare_graph();observations_.open(options_.output_directory,options_.analysis_rate,options_.reference_trace);
        save_bytes(QDir(options_.output_directory).filePath("graph.json"),graph_json_);
    }
    FrameStamp stamp(std::size_t count) {
        FrameStamp stamp;stamp.sequence=sequence_++;stamp.source_sequence=stamp.sequence;stamp.epoch=1;stamp.producer_epoch=1;
        stamp.available=true;stamp.stream_id="file-replay";
        stamp.timestamp_ticks=stamp.sample_begin=std::int64_t(result_.input_samples);stamp.sample_count=count;stamp.has_sample_interval=true;
        return stamp;
    }
    void process(std::size_t count) {
        require(result_.input_samples<=std::uint64_t(INT64_MAX)-count,"Replay source timeline exhausted");
        const auto contract=std::get<NumericContract>(source_.view());
        const PlaneLayout plane{0,{sizeof(float),0}};
        // Storage covers the prepared envelope; actual/sample_count contain
        // only decoded samples, including the short final packet.
        NumericView view{&contract,std::as_bytes(std::span{buffer_}),{&plane,1},{count,1}};
        observations_.available_at(double(result_.input_samples+count)/result_.source_rate);
        const auto status=plan_->process_numeric_ports(13,{stamp(count),view},*state_,{}, {detail::Observations::receive,nullptr,nullptr,&observations_});
        observations_.check();
        if(status!=Status::ok)throw std::runtime_error("Replay ingress sample "+std::to_string(result_.input_samples)+" count "+std::to_string(count)+" status "+std::to_string(unsigned(status)));
        result_.input_samples+=count;
        drain_cadence();
    }
    void emit_cadence(const MixerInputStats& queued) {
        const auto hop=cadence_.hop();
        require_status(plan_->process_clock(rhythm_replay_graph::pcm_clock,cadence_.tick(),*state_,{},
            {detail::Observations::receive,nullptr,nullptr,&observations_}),"Replay PCM cadence tick");
        observations_.check();
        const auto after=cadence_.queue(*plan_,*state_);
        require(queued.depth>=hop && after.depth==queued.depth-hop,"Replay PCM cadence consumption differs");
        cadence_.commit();
    }
    void drain_cadence() {
        auto queued=cadence_.queue(*plan_,*state_);
        while(cadence_.due(result_.input_samples) && queued.depth>=cadence_.hop()){
            emit_cadence(queued);queued=cadence_.queue(*plan_,*state_);
        }
    }
    void read_stream() {
        const auto packet=std::span{buffer_}.first(options_.packet_samples);
        for(;;){
            std::size_t count=0;QString error;
            if(!decoder_.read(packet,count,error))throw std::runtime_error(error.toStdString());
            if(!count)break;
            process(count);
        }
    }
    void finish() {
        cadence_.finish(result_.input_samples,cadence_.queue(*plan_,*state_));
        auto summary=observations_.finish();result_.analysis_frames=observations_.frames();
        summary["schema"]="rhythm-replay.v1";summary["input"]=QFileInfo(options_.input).absoluteFilePath();
        summary["input_sha256"]=QString::fromLatin1(input_sha_);summary["input_samples"]=double(result_.input_samples);
        summary["source_rate"]=int(result_.source_rate);summary["analysis_rate"]=int(options_.analysis_rate);
        summary["packet_samples"]=double(options_.packet_samples);summary["channel_policy"]="mean";
        summary["analyzer_input"]="float32-linear";
        summary["graph_sha256"]=QString::fromLatin1(QCryptographicHash::hash(graph_json_,QCryptographicHash::Sha256).toHex());
        summary["labels_supplied_to_analyzer"]=false;
        save_bytes(QDir(options_.output_directory).filePath("summary.json"),QJsonDocument(summary).toJson());
        result_.success=true;
    }
public:
    explicit Session(const Options& options):options_(options){}
    Result run(){prepare();read_stream();finish();return result_;}
};
}
Result run(const Options& options) {
    try{return Session(options).run();}
    catch(const std::exception& error){Result result;result.error=QString::fromUtf8(error.what());return result;}
}
}
