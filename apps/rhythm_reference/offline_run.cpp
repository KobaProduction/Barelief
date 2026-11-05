// SPDX-License-Identifier: AGPL-3.0-or-later
#include "offline_run.h"
#include "fourier_profile.h"
#include "research_profile.h"
#include "wave_input.h"
#include "frontend_capture.h"
#include <core_app/numeric_essentia_reference_graph.h>
#include <core_app/numeric_madmom_reference_graph.h>
#include <desktop/numeric_project_document.h>
#include <offline/audio_decoder.h>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using namespace signal_contracts;
using namespace dataflow_runtime;
constexpr std::size_t capture_budget=134217728;
enum class Recipe {EssentiaDegara,EssentiaMulti,MadmomFixed,MadmomLocal,MadmomCrf};
bool essentia(Recipe r) {return r==Recipe::EssentiaDegara || r==Recipe::EssentiaMulti;}
std::uint32_t rate(Recipe r) {return essentia(r)?44100:48000;}
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
Recipe recipe(const QString& id) {
    if(id=="essentia-finite-degara")return Recipe::EssentiaDegara;
    if(id=="essentia-finite-multifeature")return Recipe::EssentiaMulti;
    if(id=="madmom-whole-superflux-fixed")return Recipe::MadmomFixed;
    if(id=="madmom-whole-superflux-local")return Recipe::MadmomLocal;
    if(id=="madmom-whole-superflux-crf")return Recipe::MadmomCrf;
    throw std::invalid_argument("Unknown whole-input preset");
}
MadmomReferenceMode mode(Recipe r) {
    if(r==Recipe::MadmomFixed)return MadmomReferenceMode::Fixed;
    return r==Recipe::MadmomLocal?MadmomReferenceMode::Local:MadmomReferenceMode::Crf;
}
NumericExampleGraph example(Recipe r,std::size_t count) {
    if(essentia(r))return numeric_essentia_finite_reference_example({count,count,44100},
        r==Recipe::EssentiaMulti?EssentiaReferenceMethod::MultiFeature:EssentiaReferenceMethod::Degara);
    return numeric_madmom_whole_reference_example(count,mode(r));
}
QByteArray sha256(const QByteArray& bytes) {return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();}
QByteArray file_sha256(const QString& path) {
    QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot hash input WAV");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file),"Cannot read input WAV for hash");return hash.result().toHex();
}
struct Input {
    std::vector<float> pcm;QByteArray hash;
    std::uint32_t sample_rate=0;std::size_t declared_samples=0;
    std::unique_ptr<desktop_analysis::offline::AudioDecoder> decoder;
};
void decode_samples(desktop_analysis::offline::AudioDecoder& decoder,std::vector<float>& pcm) {
    std::array<float,4096> packet{};std::size_t offset=0;
    for(;;){
        QString error;std::size_t count=0;
        require(decoder.read(packet,count,error),"WAV decoding failed");
        if(!count)break;
        require(count<=pcm.size()-offset,"Decoded input exceeds declared length");
        require(std::all_of(packet.begin(),packet.begin()+std::ptrdiff_t(count),[](float v){return std::isfinite(v);}),"Nonfinite WAV input");
        std::copy_n(packet.begin(),count,pcm.begin()+std::ptrdiff_t(offset));offset+=count;
    }
    require(offset==pcm.size(),"Decoded input length differs from declared length");
}
Input open_input(const QString& path,Recipe r) {
    const auto declared=inspect_wave_input(path);Input result;
    const auto profile=offline_research_profile({});const auto& limits=profile.admission.frame;
    const auto maximum=std::min(limits.max_elements,limits.max_payload_bytes/sizeof(float));
    require(declared.frames>0 && declared.frames<=maximum,"PCM exceeds declared offline frame budget");
    require(declared.sample_rate==rate(r),"Whole-input preset requires its native sample rate");
    result.decoder=std::make_unique<desktop_analysis::offline::AudioDecoder>();QString error;
    require(result.decoder->open(path,error,dsp_kernels::ChannelPolicy::Mean),"Cannot decode input WAV");
    require(result.decoder->sample_rate()==declared.sample_rate && result.decoder->length()==declared.frames,"WAV decoder metadata differs from declared PCM frames");
    result.sample_rate=result.decoder->sample_rate();result.declared_samples=std::size_t(result.decoder->length());
    result.hash=file_sha256(path);return result;
}
void decode_input(Input& input,const QString& path) {
    input.pcm.resize(input.declared_samples);decode_samples(*input.decoder,input.pcm);
    require(file_sha256(path)==input.hash,"Input WAV changed while decoding");input.decoder.reset();
}
NumericContract contract(const PreparedGraph& plan,domain::NodeId id,domain::PortId port=0) {
    const auto value=plan.port_contract(id,domain::PortDirection::Output,port);
    require(value && std::holds_alternative<NumericContract>(*value),"Missing prepared numeric port");
    return std::get<NumericContract>(*value);
}
struct Endpoint {
    domain::NodeId node;domain::PortId port;DType dtype;Rank rank;QString file;
    bool allow_nan_sentinel=false,allow_negative_infinity=false;
};
std::vector<Endpoint> essentia_endpoints(bool multi) {
    std::vector<Endpoint> result;constexpr std::array<domain::NodeId,5> odfs{40,41,42,45,46};
    for(std::size_t i=0;i<(multi?5u:1u);++i){
        result.push_back({odfs[i],0,DType::Float32,Rank::Vector,QString("curve%1.f32").arg(i)});
        result.push_back({domain::NodeId(50+i),1,DType::Float32,Rank::Vector,QString("candidate%1.f32").arg(i)});
    }
    if(multi){
        result.push_back({60,0,DType::Float32,Rank::Vector,"ticks.f32"});
        result.push_back({60,1,DType::Float32,Rank::Scalar,"agreement_confidence.f32"});
        result.push_back({60,2,DType::UInt64,Rank::Scalar,"selected.u64"});
        result.push_back({60,3,DType::Float32,Rank::Vector,"scores.f32"});
        result.push_back({60,4,DType::Float32,Rank::Matrix,"pairwise.f32"});
    }
    result.push_back({61,0,DType::Float32,Rank::Scalar,"bpm.f32"});
    result.push_back({61,1,DType::Float32,Rank::Scalar,"confidence.f32"});
    result.push_back({61,2,DType::Float32,Rank::Vector,"intervals.f32"});
    result.push_back({61,3,DType::Float32,Rank::Vector,"estimates.f32"});return result;
}
std::vector<Endpoint> madmom_endpoints(bool crf) {
    std::vector<Endpoint> result{
        {35,0,DType::Float32,Rank::Matrix,"tempo.f32",true},
        {35,1,DType::Float32,Rank::Vector,"histogram.f32"},
        {35,2,DType::Float32,Rank::Vector,"coordinates.f32"},
        {35,3,DType::Float32,Rank::Vector,"smoothed.f32"},
        {4,0,DType::Float32,Rank::Vector,"beats.f32"},
        {4,1,DType::UInt64,Rank::Vector,"beat_positions.u64"}};
    if(crf){
        result.push_back({4,2,DType::UInt64,Rank::Vector,"trial_intervals.u64"});
        result.push_back({4,3,DType::Float32,Rank::Vector,"trial_scores.f32",false,true});
        result.push_back({4,4,DType::Float32,Rank::Matrix,"crf_tempo.f32",true});
    }else result.push_back({4,2,DType::Float32,Rank::Vector,"decoder_histogram.f32"});
    return result;
}
struct Capture {
    Endpoint endpoint;NumericContract declared;
    std::vector<std::byte> bytes;
    std::array<std::size_t,2> actual{};
    std::size_t width=0,capacity_bytes=0,used=0,nonfinite=0;WholeOutputProgress publications;
    explicit Capture(Endpoint spec,const PreparedGraph& plan):endpoint(std::move(spec)),declared(contract(plan,endpoint.node,endpoint.port)) {
        require(declared.dtype==endpoint.dtype && declared.shape.rank==endpoint.rank && declared.channel_order.size()==1,"Unexpected final endpoint contract");
        const auto* dtype=dtype_descriptor(declared.dtype);require(dtype,"Unknown final dtype");width=dtype->bytes;
        const auto rows=declared.shape.extents[0].maximum,columns=declared.shape.extents[1].maximum;
        require(columns && rows<=capture_budget/width/columns,"Final capture exceeds bounded budget");
        capacity_bytes=std::size_t(rows*columns)*width;
    }
    bool floating_valid(std::uint64_t bits) noexcept {
        if(endpoint.dtype!=DType::Float32)return true;
        const auto value=std::bit_cast<float>(std::uint32_t(bits));
        if(std::isfinite(value))return true;
        ++nonfinite;
        return (endpoint.allow_nan_sentinel && std::isnan(value)) ||
            (endpoint.allow_negative_infinity && value==-std::numeric_limits<float>::infinity());
    }
    bool sentinel_valid() const noexcept {
        if(!endpoint.allow_nan_sentinel)return true;
        if(actual[1]!=2)return false;
        if(nonfinite)return actual[0]==1 && nonfinite==1 && std::isnan(value<float>(0)) && value<float>(1)==0;
        for(std::size_t i=0;i<actual[0];++i)if(value<float>(i*2)<=0 || value<float>(i*2+1)<0)return false;
        return true;
    }
    template<class T> T value(std::size_t index) const noexcept {
        T result{};std::memcpy(&result,bytes.data()+index*sizeof(T),sizeof(T));return result;
    }
    bool geometry_valid(const NumericView& view) const noexcept {
        if(!view.contract || view.contract->dtype!=endpoint.dtype || view.contract->shape.rank!=endpoint.rank || view.planes.size()!=1)return false;
        for(std::size_t axis=0;axis<2;++axis)
            if(view.actual[axis]<declared.shape.extents[axis].minimum || view.actual[axis]>declared.shape.extents[axis].maximum)return false;
        return view.actual[1] && view.actual[0]<=bytes.size()/width/view.actual[1];
    }
    bool copy_element(const NumericView& view,std::size_t row,std::size_t column) noexcept {
        std::uint64_t bits=0;
        if(load_bits(view,{0,row,column},endpoint.dtype,bits)!=Status::ok || !floating_valid(bits))return false;
        for(std::size_t byte=0;byte<width;++byte)bytes[(row*actual[1]+column)*width+byte]=std::byte(bits>>(8*byte));
        return true;
    }
    bool copy(const NumericView& view) noexcept {
        if(!geometry_valid(view))return false;
        actual=view.actual;used=actual[0]*actual[1]*width;
        for(std::size_t row=0;row<actual[0];++row)for(std::size_t column=0;column<actual[1];++column)
            if(!copy_element(view,row,column))return false;
        return sentinel_valid();
    }
    void observe(const NumericPortFrameView& event,bool final) noexcept {
        if(event.identity.node!=endpoint.node || event.identity.port!=endpoint.port)return;
        if(!publications.observe(event.status,event.frame.stamp.available,final))return;
        const auto* view=std::get_if<NumericView>(&event.frame.payload);
        if(!view || !copy(*view))++publications.errors;
    }
    bool complete() const noexcept {return publications.complete();}
};
class Session {
    Recipe recipe_;std::size_t expected_,collected_=0,batch_attempts_=0,source_samples_=0,source_publications_=0;
    WholeInputProgress progress_;
    std::uint64_t sequence_=0;bool collector_error_=false;
    std::shared_ptr<const PreparedGraph> plan_;std::unique_ptr<RuntimeContext> state_;
    NumericContract source_,batch_;std::vector<float> packet_,whole_;
    FrameStamp collected_stamp_;std::vector<Capture> captures_;
    std::unique_ptr<MadmomFrontendCapture> frontend_;
    static void output(void* user,const NumericPortFrameView& event) noexcept {
        auto& self=*static_cast<Session*>(user);
        if(event.direction!=domain::PortDirection::Output)return;
        if(self.frontend_)self.frontend_->observe(event);
        if(event.identity.node==1 && !event.identity.port)self.observe_source(event);
        if(event.identity.node==(essentia(self.recipe_)?2:3) && !event.identity.port)self.collect(event);
        for(auto& capture:self.captures_)capture.observe(event,self.progress_.finalized());
    }
    NumericRuntimeCallbacks callbacks() noexcept {return {output,nullptr,nullptr,this};}
    void observe_source(const NumericPortFrameView& event) noexcept {
        if(event.status==Status::unavailable && !event.frame.stamp.available && !source_samples_)return;
        const auto* view=std::get_if<NumericView>(&event.frame.payload);const auto& stamp=event.frame.stamp;
        if(event.status!=Status::ok || !stamp.available || !view || !view->contract ||
            view->actual[1]!=1 || view->actual[0]>expected_-source_samples_){collector_error_=true;return;}
        if(!stamp.has_sample_interval || stamp.sample_begin!=std::int64_t(source_samples_) || stamp.sample_count!=view->actual[0] ||
            stamp.timestamp_ticks!=std::int64_t(source_samples_) || stamp.discontinuity!=(source_samples_==0)){
            collector_error_=true;return;
        }
        source_samples_+=view->actual[0];++source_publications_;
    }
    bool valid_stamp(const FrameStamp& stamp,std::size_t count) const noexcept {
        if(stamp.discontinuity!=(collected_==0))return false;
        if(essentia(recipe_))return stamp.has_sample_interval && stamp.sample_begin==std::int64_t(collected_) &&
            stamp.sample_count==count && stamp.timestamp_ticks==std::int64_t(collected_);
        return !stamp.has_sample_interval && !stamp.sample_begin && !stamp.sample_count &&
            stamp.timestamp_ticks==std::int64_t(collected_*240);
    }
    bool valid_collector(const NumericView& view) const noexcept {
        if(!view.contract || view.contract->dtype!=DType::Float32 || view.planes.size()!=1 || view.actual[1]!=1)return false;
        const auto& clock=view.contract->clock;
        if(clock.domain!="local" || clock.sample_rate!=rate(recipe_) || clock.tick_seconds!=Ratio{1,rate(recipe_)})return false;
        return essentia(recipe_)?view.contract->shape.rank==Rank::Vector:
            view.contract->shape.rank==Rank::Scalar && view.actual[0]==1;
    }
    bool same_producer(const FrameStamp& stamp) const noexcept {
        if(!collected_)return true;
        return stamp.epoch==collected_stamp_.epoch && stamp.generation==collected_stamp_.generation &&
            stamp.producer_epoch==collected_stamp_.producer_epoch && stamp.source_sequence>=collected_stamp_.source_sequence &&
            stamp.latency_seconds==collected_stamp_.latency_seconds && stamp.latency_samples==collected_stamp_.latency_samples &&
            stamp.frequency_dependent_latency==collected_stamp_.frequency_dependent_latency;
    }
    void collect(const NumericPortFrameView& event) noexcept {
        if(event.status==Status::unavailable && !event.frame.stamp.available && !collected_)return;
        const auto* view=std::get_if<NumericView>(&event.frame.payload);
        if(progress_.finalized() || event.status!=Status::ok || !event.frame.stamp.available || !view || !valid_collector(*view)){
            collector_error_=true;return;
        }
        const auto count=view->actual[0];
        if(count>whole_.size()-collected_ || !valid_stamp(event.frame.stamp,count)){collector_error_=true;return;}
        if(!same_producer(event.frame.stamp)){collector_error_=true;return;}
        copy_collector(*view,count);collected_stamp_=event.frame.stamp;collected_stamp_.stream_id="whole-native-pcm";
    }
    void copy_collector(const NumericView& view,std::size_t count) noexcept {
        for(std::size_t i=0;i<count;++i){
            float value=0;
            if(load_element(view,{0,i,0},value)!=Status::ok || !std::isfinite(value) || (!essentia(recipe_) && value<0)){
                collector_error_=true;return;
            }
            whole_[collected_++]=value;
        }
    }
    Status packet(std::span<const float> input) noexcept {
        std::copy(input.begin(),input.end(),packet_.begin());
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence_;stamp.epoch=stamp.producer_epoch=stamp.generation=1;
        stamp.discontinuity=progress_.processed()==0;stamp.has_sample_interval=true;stamp.sample_count=input.size();
        stamp.timestamp_ticks=stamp.sample_begin=std::int64_t(progress_.processed());stamp.stream_id="whole-native-pcm";
        const PlaneLayout plane{0,{sizeof(float),0}};
        NumericView view{&source_,std::as_bytes(std::span(packet_)),{&plane,1},{input.size(),1}};
        const auto result=plan_->process_numeric_ports(1,{stamp,view},*state_,{},callbacks());
        if(!progress_.accept(input.size()))return Status::invalid_argument;return result;
    }
    void prepare(const NumericExampleGraph& graph) {
        const auto profile=offline_research_profile(research_fourier_backend(graph.graph));
        require(PreparedGraph::prepare_numeric(graph.graph,profile,plan_,state_)==Status::ok,"Whole-input graph preparation failed");
        source_=contract(*plan_,1);batch_=contract(*plan_,essentia(recipe_)?3:31);
        require(source_.dtype==DType::Float32 && source_.shape.rank==Rank::Vector && source_.channel_order.size()==1,"Unexpected PCM ingress");
        require(source_.shape.extents[0].maximum<=8192 && source_.shape.extents[0].minimum<=1,"Unbounded PCM ingress");
        packet_.resize(std::size_t(source_.shape.extents[0].maximum));
        whole_.resize(essentia(recipe_)?expected_:1+(expected_-2048)/240);
        prepare_captures();
    }
    void prepare_captures() {
        const auto endpoints=essentia(recipe_)?essentia_endpoints(recipe_==Recipe::EssentiaMulti):madmom_endpoints(recipe_==Recipe::MadmomCrf);
        std::size_t bytes=0;captures_.reserve(endpoints.size());
        for(const auto& endpoint:endpoints){
            captures_.emplace_back(endpoint,*plan_);
            require(captures_.back().capacity_bytes<=capture_budget-bytes,"Total final capture exceeds bounded budget");
            bytes+=captures_.back().capacity_bytes;
        }
        for(auto& capture:captures_)capture.bytes.resize(capture.capacity_bytes);
    }
    FrameStamp final_stamp() const noexcept {
        FrameStamp stamp;
        if(essentia(recipe_))stamp=collected_stamp_;
        else {stamp.sequence=stamp.source_sequence=1;stamp.epoch=stamp.producer_epoch=stamp.generation=1;stamp.stream_id="madmom-whole-odf";}
        stamp.discontinuity=true;stamp.available=true;stamp.timestamp_ticks=0;
        stamp.has_sample_interval=essentia(recipe_);stamp.sample_begin=0;stamp.sample_count=essentia(recipe_)?collected_:0;
        return stamp;
    }
    bool paired_events() const noexcept {
        if(essentia(recipe_))return true;
        const auto* seconds=find(4,0);const auto* indices=find(4,1);
        if(!seconds || !indices || !seconds->complete() || !indices->complete() || seconds->actual!=indices->actual)return false;
        for(std::size_t i=0;i<seconds->actual[0];++i){
            const auto position=indices->value<std::uint64_t>(i);
            if(position>=collected_ || seconds->value<float>(i)!=float(position)/200.f)return false;
            if(i && position<=indices->value<std::uint64_t>(i-1))return false;
        }
        return valid_trials(seconds->actual[0]);
    }
    const Capture* find(domain::NodeId node,domain::PortId port) const noexcept {
        const auto found=std::find_if(captures_.begin(),captures_.end(),[=](const Capture& c){return c.endpoint.node==node && c.endpoint.port==port;});
        return found==captures_.end()?nullptr:&*found;
    }
    bool valid_trials(std::size_t events) const noexcept {
        if(recipe_!=Recipe::MadmomCrf)return true;
        const auto* intervals=find(4,2);const auto* scores=find(4,3);
        if(!intervals || !scores || !intervals->complete() || !scores->complete() || intervals->actual!=scores->actual)return false;
        std::size_t finite=0;
        for(std::size_t i=0;i<scores->actual[0];++i)if(std::isfinite(scores->value<float>(i)))++finite;
        return events?finite>0:finite==0;
    }
public:
    Session(Recipe r,std::size_t expected,const NumericExampleGraph& graph):recipe_(r),expected_(expected),progress_(expected) {prepare(graph);}
    Status feed(std::span<const float> input,std::size_t packet_size) noexcept {
        if(progress_.finalized() || !packet_size || packet_size>packet_.size() || input.size()>progress_.remaining())return Status::invalid_argument;
        for(std::size_t offset=0;offset<input.size();){
            const auto count=std::min(packet_size,input.size()-offset);
            const auto result=packet(input.subspan(offset,count));if(result!=Status::ok || collector_error_)return Status::invalid_argument;
            offset+=count;
        }
        return Status::ok;
    }
    Status finish() noexcept {
        if(!progress_.finish(!collector_error_ && collected_==whole_.size()))return Status::invalid_argument;
        const auto stamp=final_stamp();
        if(recipe_==Recipe::EssentiaDegara && plan_->process_clock(59,stamp,*state_,{},callbacks())!=Status::ok)return Status::invalid_argument;
        const PlaneLayout plane{0,{sizeof(float),0}};
        NumericView view{&batch_,std::as_bytes(std::span(whole_)),{&plane,1},{collected_,1}};
        ++batch_attempts_;
        return plan_->process_numeric_ports(essentia(recipe_)?3:31,{stamp,view},*state_,{},callbacks());
    }
    bool complete() const noexcept {
        return progress_.finalized() && source_samples_==expected_ && !collector_error_ && paired_events() &&
            std::all_of(captures_.begin(),captures_.end(),[](const Capture& c){return c.complete();});
    }
    bool empty_result() const noexcept {
        const auto node=essentia(recipe_)?(recipe_==Recipe::EssentiaMulti?60:50):4;
        const auto port=recipe_==Recipe::EssentiaDegara?1:0;
        const auto found=std::find_if(captures_.begin(),captures_.end(),[=](const Capture& c){return c.endpoint.node==node && c.endpoint.port==port;});
        return found!=captures_.end() && found->complete() && !found->actual[0];
    }
    const std::vector<Capture>& captures() const noexcept {return captures_;}
    std::span<const float> collected() const noexcept {return std::span(whole_).first(collected_);}
    std::size_t processed() const noexcept {return progress_.processed();}
    std::size_t batch_attempts() const noexcept {return batch_attempts_;}
    MadmomFrontendCapture& enable_frontend_capture() {
        std::size_t used=0;for(const auto& capture:captures_)used+=capture.capacity_bytes;
        require(MadmomFrontendCapture::maximum_bytes<=capture_budget-used,"Combined diagnostic capture exceeds budget");
        frontend_=std::make_unique<MadmomFrontendCapture>(*plan_);return *frontend_;
    }
    QJsonObject telemetry() const {
        return {{"source_available_samples",qint64(source_samples_)},{"source_available_publications",qint64(source_publications_)},
            {"collector_error",collector_error_},{"collector_last_timestamp_ticks",qint64(collected_stamp_.timestamp_ticks)},
            {"collector_clock_rate",int(rate(recipe_))},{"collector_latency_numerator",qint64(collected_stamp_.latency_seconds.numerator)},
            {"collector_latency_denominator",qint64(collected_stamp_.latency_seconds.denominator)},
            {"collector_frequency_dependent_latency",collected_stamp_.frequency_dependent_latency}};
    }
};
void write(const QDir& directory,const QString& name,const QByteArray& bytes) {
    QFile file(directory.filePath(name));require(file.open(QIODevice::WriteOnly|QIODevice::NewOnly),"Cannot create fresh result file");
    require(file.write(bytes)==bytes.size(),"Cannot write result file");require(file.flush(),"Cannot flush result file");
}
QByteArray bytes(std::span<const float> values) {
    static_assert(std::endian::native==std::endian::little);
    return QByteArray(reinterpret_cast<const char*>(values.data()),qsizetype(values.size_bytes()));
}
QJsonObject capture_record(const Capture& c) {
    const auto data=QByteArray(reinterpret_cast<const char*>(c.bytes.data()),qsizetype(c.used));
    return {{"node",c.endpoint.node},{"port",c.endpoint.port},{"file",c.endpoint.file},
        {"dtype",c.endpoint.dtype==DType::UInt64?"UInt64":"Float32"},{"rank",int(c.endpoint.rank)},
        {"shape",QJsonArray{qint64(c.actual[0]),qint64(c.actual[1])}},{"byte_order","little"},
        {"sha256",QString::fromLatin1(sha256(data))},{"available_publications",qint64(c.publications.ok)},
        {"streaming_publications",qint64(c.publications.early)},{"unavailable_boundaries",qint64(c.publications.unavailable)},
        {"errors",qint64(c.publications.errors)},{"nonfinite_values",qint64(c.nonfinite)},{"complete",c.complete()}};
}
QJsonObject recipe_record(Recipe r,std::size_t count) {
    if(essentia(r))return {{"revision","b9fa6cb674ca43dfb94d28d293aeda441c6745db"},{"sample_rate",44100},
        {"frame",2048},{"primary_hop",1024},{"global_hop",512},{"seed",17},{"session_samples",qint64(count)},
        {"frame_tail","CoverEnd"},{"time_origin","session-relative seconds"},{"method",r==Recipe::EssentiaMulti?"multifeature":"degara"}};
    return {{"revision","27f032e8947204902c675e5e341a3faf5dc86dae"},{"sample_rate",48000},
        {"frame",2048},{"hop",240},{"frame_rate",200},{"pcm_samples",qint64(count)},
        {"odf_frames",qint64(1+(count-2048)/240)},{"mode",int(mode(r))},{"whole_input",true},
        {"frame_tail","CompleteOnly"},{"time_origin","complete-window left edge k/200 seconds"}};
}
void save_graph(const QDir& directory,const NumericExampleGraph& graph) {
    desktop::NumericProjectDocument document;
    auto status=desktop::create_numeric_project(graph.graph,{},document);
    if(status)status=desktop::save_numeric_project(document,directory.filePath("graph.json"));
    require(bool(status),"Cannot save finite graph project");
}
QJsonArray save_captures(const QDir& directory,const Session& session) {
    QJsonArray records;
    for(const auto& c:session.captures()){
        if(c.complete())write(directory,c.endpoint.file,QByteArray(reinterpret_cast<const char*>(c.bytes.data()),qsizetype(c.used)));
        records.append(capture_record(c));
    }
    return records;
}
QJsonObject result_record(const WholeInputArguments& args,const Input& input,const Session& session,
    Status processing,Status finalization,const QJsonArray& records) {
    const bool complete=processing==Status::ok && finalization==Status::ok && session.complete();
    return {{"preset",args.preset},{"input",QFileInfo(args.input).absoluteFilePath()},
        {"input_sha256",QString::fromLatin1(input.hash)},{"sample_rate",int(input.sample_rate)},
        {"declared_samples",qint64(input.pcm.size())},{"processed_samples",qint64(session.processed())},
        {"collected_values",qint64(session.collected().size())},{"packet_samples",qint64(args.packet_samples)},
        {"batch_attempts",qint64(session.batch_attempts())},{"execution_complete",complete},
        {"processing_status",int(processing)},{"finalization_status",int(finalization)},
        {"result",complete?(session.empty_result()?"no_result":"available"):"error"},
        {"numeric_parity","not_evaluated"},{"endpoints",records}};
}
void save_manifest(const QDir& directory,QJsonObject report,const Input& input,const Session& session) {
    report["telemetry"]=session.telemetry();report["channel_policy"]="Mean";
    report["pcm_sha256"]=QString::fromLatin1(sha256(bytes(input.pcm)));
    report["graph_sha256"]=QString::fromLatin1(file_sha256(directory.filePath("graph.json")));
    report["executable_sha256"]=QString::fromLatin1(file_sha256(QCoreApplication::applicationFilePath()));
    write(directory,"capture.json",QJsonDocument(report).toJson());
}
void save_result(const WholeInputArguments& args,Recipe r,const Input& input,const NumericExampleGraph& graph,
    const Session& session,Status processing,Status finalization) {
    require(!QFileInfo::exists(args.output),"Output directory already exists");
    require(QDir().mkpath(args.output),"Cannot create output directory");QDir directory(args.output);
    write(directory,"pcm.f32",bytes(input.pcm));
    if(!essentia(r))write(directory,"envelope.f32",bytes(session.collected()));
    const auto records=save_captures(directory,session);
    if(r==Recipe::EssentiaDegara && session.complete()){
        QFile source(directory.filePath("candidate0.f32"));require(source.open(QIODevice::ReadOnly),"Missing Degara ticks");
        write(directory,"ticks.f32",source.readAll());
    }
    save_graph(directory,graph);write(directory,"recipe.json",QJsonDocument(recipe_record(r,input.pcm.size())).toJson());
    save_manifest(directory,result_record(args,input,session,processing,finalization,records),input,session);
}
void validate_arguments(const WholeInputArguments& args) {
    require(!args.input.isEmpty() && !args.output.isEmpty(),"Whole input requires input and fresh output paths");
    require(args.packet_samples==97 || args.packet_samples==4096,"Whole input packet size must be 97 or 4096");
    require(!QFileInfo::exists(args.output),"Output directory must be new");
}
void frontend_provenance(QJsonObject& report,const Input& input,const Session& session,Status status) {
    report["diagnostic_prefix_only"]=true;report["whole_result"]=false;report["eof_batches"]=0;
    report["prepared_pcm_samples"]=qint64(input.pcm.size());report["processed_pcm_samples"]=qint64(session.processed());
    report["collected_odf_frames"]=qint64(session.collected().size());report["processing_status"]=int(status);
    report["input_wav_sha256"]=QString::fromLatin1(input.hash);report["pcm_sha256"]=QString::fromLatin1(sha256(bytes(input.pcm)));
    report["executable_sha256"]=QString::fromLatin1(file_sha256(QCoreApplication::applicationFilePath()));
}
int frontend_diagnostic(const WholeInputArguments& args,Recipe r,const Input& input,const NumericExampleGraph& graph,Session& session) {
    constexpr std::size_t prefix=49328;
    require(r==Recipe::MadmomCrf && input.pcm.size()==514761 && args.packet_samples==4096,"Diagnostic requires the declared full madmom CRF fixture");
    auto& capture=session.enable_frontend_capture();
    const auto status=session.feed(std::span(input.pcm).first(prefix),args.packet_samples);
    require(session.batch_attempts()==0,"Frontend diagnostic must not finalize a whole input");
    require(!QFileInfo::exists(args.output) && QDir().mkpath(args.output),"Cannot create fresh diagnostic directory");
    QDir directory(args.output);auto report=capture.write(directory);save_graph(directory,graph);
    frontend_provenance(report,input,session,status);
    write(directory,"frontend.json",QJsonDocument(report).toJson());
    return status==Status::ok && capture.complete() && session.processed()==prefix && session.collected().size()==198?0:3;
}
}

void add_whole_input_options(QCommandLineParser& parser) {
    parser.addOption({"whole-input","Run one complete finite WAV through a whole-input preset"});
    parser.addOption({"input","Native-rate WAV input","path"});
    parser.addOption({"output","Fresh whole-input evidence directory","directory"});
    parser.addOption({"packet-samples","Actual PCM ingress packet samples: 97 or 4096","count","4096"});
    parser.addOption({"frontend-diagnostic","Capture selected madmom frontend frames without finalizing the whole input"});
}
WholeInputArguments whole_input_arguments(const QCommandLineParser& parser) {
    for(const auto* incompatible:{"export","project","duration","port","synthetic","observe"})
        require(!parser.isSet(QLatin1String(incompatible)),"Whole-input mode has incompatible live/export options");
    bool valid=false;const auto count=parser.value("packet-samples").toULongLong(&valid);
    require(valid && (count==97 || count==4096),"Invalid whole-input packet size");
    WholeInputArguments args{parser.value("preset"),parser.value("input"),parser.value("output"),std::size_t(count)};
    args.frontend_diagnostic=parser.isSet("frontend-diagnostic");
    validate_arguments(args);return args;
}
int run_whole_input(const WholeInputArguments& args) {
    validate_arguments(args);const auto r=recipe(args.preset);auto input=open_input(args.input,r);
    const auto graph=example(r,input.declared_samples);Session session(r,input.declared_samples,graph);
    decode_input(input,args.input);
    if(args.frontend_diagnostic)return frontend_diagnostic(args,r,input,graph,session);
    std::srand(17);const auto processing=session.feed(input.pcm,args.packet_samples);
    const auto finalization=processing==Status::ok?session.finish():Status::unavailable;
    save_result(args,r,input,graph,session,processing,finalization);
    qInfo()<<"Whole input"<<args.preset<<"samples"<<qulonglong(session.processed())<<"complete"<<session.complete();
    return processing==Status::ok && finalization==Status::ok && session.complete()?0:3;
}

bool WholeInputProgress::accept(std::size_t count) noexcept {
    if(finalized_ || !count || count>remaining())return false;
    processed_+=count;return true;
}
bool WholeInputProgress::finish(bool collector_complete) noexcept {
    if(finalized_ || !expected_ || remaining() || !collector_complete)return false;
    finalized_=true;return true;
}
bool WholeOutputProgress::observe(signal_contracts::Status status,bool available,bool final) noexcept {
    if(status==Status::unavailable && !available){++unavailable;if(ok || early)++errors;return false;}
    if(status!=Status::ok || !available){++errors;return false;}
    if(!final){++early;return false;}
    ++ok;return true;
}
