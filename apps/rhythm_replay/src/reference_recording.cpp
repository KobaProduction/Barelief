#include "support.h"
#include <QDir>
#include <QJsonArray>
#include <algorithm>
#include <bit>
#include <cmath>

namespace rhythm_replay::detail {
using namespace signal_contracts;
namespace {
constexpr std::array<domain::NodeId,5> trace_nodes{1313,1312,1300,1301,1303};
constexpr std::array<const char*,5> trace_names{"reference_pcm.f32","reference_fft.f32","reference_power.f32","reference_mel.f32","reference_db.f32"};
constexpr std::array<std::size_t,5> trace_widths{1,2050,1025,128,128};
constexpr std::array<domain::NodeId,3> progression_nodes{1313,1314,1312};
constexpr const char* diagnostic_header="record,stage,guard,node,port,direction,status,dtype,rank,actual0,actual1,sample_rate,tick_num,tick_den,domain_bytes,domain_hex,stream_bytes,stream_hex,sequence,source_sequence,generation,epoch,producer_epoch,graph_revision,timestamp_ticks,has_interval,sample_begin,sample_count,sample_end_exclusive,sample_end_valid,available,discontinuity,latency_num,latency_den,frequency_dependent_latency,events,pcm_begin,pcm_samples,spectral_frames,method_frames,expected_window_begin";
const char* reference_stamp_error(const DataFrameView& frame) noexcept {
    const auto* view=std::get_if<NumericView>(&frame.payload);
    if(!view)return "payload_not_numeric";
    if(view->contract->dtype!=DType::Float32)return "stamp_dtype";
    if(view->contract->clock.sample_rate!=44100)return "stamp_sample_rate";
    if(view->contract->clock.tick_seconds!=Ratio{1,44100})return "stamp_tick_seconds";
    if(!frame.stamp.has_sample_interval)return "stamp_missing_pcm_interval";
    return frame.stamp.sample_count==2048?nullptr:"stamp_window_count";
}
const char* trace_geometry_error(const DataFrameView& frame,std::size_t stage) noexcept {
    const auto& view=std::get<NumericView>(frame.payload);
    if(view.contract->dtype!=DType::Float32)return "trace_dtype";
    if(view.contract->clock.sample_rate!=44100)return "trace_sample_rate";
    if(!view.actual[0])return "trace_empty_actual";
    if(!stage)return view.contract->shape.rank==Rank::Vector?nullptr:"pcm_rank";
    if(const auto* reason=reference_stamp_error(frame))return reason;
    const auto count=stage==4?view.actual[1]:view.actual[0]*(view.contract->shape.rank==Rank::Matrix?view.actual[1]:1);
    return count==trace_widths[stage]?nullptr:"trace_width";
}
void diagnostic_text(std::string_view text,std::array<char,193>& hex) noexcept {
    constexpr char digits[]="0123456789abcdef";
    const auto count=std::min(text.size(),(hex.size()-1)/2);
    for(std::size_t i=0;i<count;++i){const auto byte=static_cast<unsigned char>(text[i]);hex[2*i]=digits[byte>>4];hex[2*i+1]=digits[byte&15];}
    hex[2*count]=0;
}
void capture_diagnostic_value(ReferenceEventDiagnostic& out,const NumericView& view) noexcept {
    out.dtype=view.contract->dtype;out.rank=view.contract->shape.rank;out.actual=view.actual;
    const auto& clock=view.contract->clock;out.tick=clock.tick_seconds;out.sample_rate=clock.sample_rate;
    out.domain_bytes=clock.domain.size();diagnostic_text(clock.domain,out.domain_hex);
}
void capture_diagnostic(ReferenceEventDiagnostic& out,const dataflow_runtime::NumericPortFrameView& event) noexcept {
    out.seen=true;++out.events;out.node=event.identity.node;out.port=event.identity.port;
    out.direction=event.direction;out.status=event.status;out.stamp=event.frame.stamp;
    out.stream_bytes=out.stamp.stream_id.size();diagnostic_text(out.stamp.stream_id,out.stream_hex);out.stamp.stream_id={};
    const auto* view=std::get_if<NumericView>(&event.frame.payload);if(!view)return;
    capture_diagnostic_value(out,*view);
}
void diagnostic_identity(std::FILE* file,const ReferenceEventDiagnostic& event) noexcept {
    std::fprintf(file,"%d,%u,%u,%u,%u,%u,%zu,%zu,%u,%lld,%llu,%zu,%s,%zu,%s,",
        int(event.node),unsigned(event.port),unsigned(event.direction),unsigned(event.status),unsigned(event.dtype),unsigned(event.rank),
        event.actual[0],event.actual[1],unsigned(event.sample_rate),static_cast<long long>(event.tick.numerator),
        static_cast<unsigned long long>(event.tick.denominator),event.domain_bytes,event.domain_hex.data(),event.stream_bytes,event.stream_hex.data());
}
void diagnostic_stamp(std::FILE* file,const FrameStamp& stamp) noexcept {
    const bool end_valid=stamp.has_sample_interval && stamp.sample_count<=std::uint64_t(INT64_MAX) && stamp.sample_begin<=INT64_MAX-std::int64_t(stamp.sample_count);
    const auto end=end_valid?stamp.sample_begin+std::int64_t(stamp.sample_count):0;
    std::fprintf(file,"%llu,%llu,%llu,%llu,%llu,%llu,%lld,%u,%lld,%llu,%lld,%u,%u,%u,%lld,%llu,%u,",
        static_cast<unsigned long long>(stamp.sequence),static_cast<unsigned long long>(stamp.source_sequence),
        static_cast<unsigned long long>(stamp.generation),static_cast<unsigned long long>(stamp.epoch),
        static_cast<unsigned long long>(stamp.producer_epoch),static_cast<unsigned long long>(stamp.graph_revision),
        static_cast<long long>(stamp.timestamp_ticks),unsigned(stamp.has_sample_interval),static_cast<long long>(stamp.sample_begin),
        static_cast<unsigned long long>(stamp.sample_count),static_cast<long long>(end),unsigned(end_valid),unsigned(stamp.available),
        unsigned(stamp.discontinuity),static_cast<long long>(stamp.latency_seconds.numerator),
        static_cast<unsigned long long>(stamp.latency_seconds.denominator),unsigned(stamp.frequency_dependent_latency));
}
const char* method_value_error(const NumericView& view,float& value) noexcept {
    if(!view.actual[0])return "method_empty_actual";
    if(load_element(view,{0,view.actual[0]-1,0},value)!=Status::ok)return "method_load";
    return std::isfinite(value)?nullptr:"method_nonfinite";
}
const char* trace_value_error(const NumericView& view,ElementIndex index,float& value) noexcept {
    if(load_element(view,index,value)!=Status::ok)return "trace_load";
    return std::isfinite(value)?nullptr:"trace_nonfinite";
}
}
void ReferenceRecorder::open(const QString& directory,bool trace) {
    static_assert(std::endian::native==std::endian::little,"PC reference artifacts are Float32 LE");
    static_assert(sizeof(float)==4);
    const QDir dir(directory);tracing_=trace;
    envelopes_.open(dir.filePath("reference_envelopes.csv"),"method,frame,sample_rate,window_begin,window_samples,t_start,t_center,t_end,t_ready,upstream_latency_s,value");
    diagnostic_.open(dir.filePath("reference_diagnostic.csv"),diagnostic_header);
    if(tracing_)for(std::size_t i=0;i<trace_.size();++i)trace_[i].open_binary(dir.filePath(trace_names[i]));
}
bool ReferenceRecorder::write_method(Method& method,const dataflow_runtime::NumericPortFrameView& event,double available) noexcept {
    active_stage_=method.name;active_method_frames_=method.frames;
    if(const auto* reason=reference_stamp_error(event.frame))return reject(reason);
    const auto& view=std::get<NumericView>(event.frame.payload);float value=0;
    if(const auto* reason=method_value_error(view,value))return reject(reason);
    const auto& stamp=event.frame.stamp;
    if(stamp.sample_begin!=pcm_begin_+std::int64_t(method.frames*512))return reject("method_window_begin");
    const double start=double(stamp.sample_begin)/44100.,end=(double(stamp.sample_begin)+2047.)/44100.;
    const double center=(double(stamp.sample_begin)+1023.5)/44100.;
    const double latency=double(stamp.latency_seconds.numerator)/double(stamp.latency_seconds.denominator);
    return std::fprintf(envelopes_.get(),"%s,%llu,44100,%lld,2048,%.17g,%.17g,%.17g,%.17g,%.17g,%.9g\n",
        method.name,static_cast<unsigned long long>(++method.frames),static_cast<long long>(stamp.sample_begin),
        start,center,end,std::max(end,available),latency,double(value))>0 || reject("method_csv_write");
}
bool ReferenceRecorder::write_values(const NumericView& view,std::FILE* file,bool last_row) noexcept {
    const bool matrix=view.contract->shape.rank==Rank::Matrix;
    const auto rows=last_row?1:view.actual[0],columns=matrix?view.actual[1]:1;
    std::size_t filled=0;
    for(std::size_t index=0;index<rows*columns;++index){
        const auto row=last_row?view.actual[0]-1:index/columns;
        if(const auto* reason=trace_value_error(view,{0,row,index%columns},scratch_[filled]))return reject(reason);
        if(++filled==scratch_.size()){
            if(std::fwrite(scratch_.data(),sizeof(float),filled,file)!=filled)return reject("trace_binary_write");
            filled=0;
        }
    }
    return !filled || std::fwrite(scratch_.data(),sizeof(float),filled,file)==filled || reject("trace_binary_write_tail");
}
bool ReferenceRecorder::accept_pcm(const dataflow_runtime::NumericPortFrameView& event) noexcept {
    const auto& stamp=event.frame.stamp;const auto count=std::get<NumericView>(event.frame.payload).actual[0];
    if(!pcm_samples_)pcm_begin_=stamp.sample_begin;
    if(!stamp.has_sample_interval)return reject("pcm_missing_interval");
    if(stamp.sample_count!=count)return reject("pcm_actual_count");
    if(stamp.sample_begin!=pcm_begin_+std::int64_t(pcm_samples_))return reject("pcm_continuity");
    pcm_samples_+=count;return true;
}
bool ReferenceRecorder::write_trace(const dataflow_runtime::NumericPortFrameView& event) noexcept {
    const auto found=std::find(trace_nodes.begin(),trace_nodes.end(),event.identity.node);
    if(found==trace_nodes.end())return true;
    const auto stage=std::size_t(found-trace_nodes.begin());const auto& view=std::get<NumericView>(event.frame.payload);
    active_stage_=trace_names[stage];active_method_frames_=0;
    if(const auto* reason=trace_geometry_error(event.frame,stage))return reject(reason);
    if(!stage && !accept_pcm(event))return false;
    if(stage==1)++spectral_frames_;
    ++stage_frames_[stage];
    return !tracing_ || write_values(view,trace_[stage].get(),stage==4);
}
bool ReferenceRecorder::receive(const dataflow_runtime::NumericPortFrameView& event,double available) noexcept {
    observe_progression(event);
    if(event.identity.port!=0)return true;
    if(!write_trace(event))return diagnose(event);
    for(auto& method:methods_)if(method.node==event.identity.node){
        if(!write_method(method,event,available))return diagnose(event);
        return true;
    }
    return true;
}
void ReferenceRecorder::observe_progression(const dataflow_runtime::NumericPortFrameView& event) noexcept {
    if(event.identity.port!=0)return;
    const auto found=std::find(progression_nodes.begin(),progression_nodes.end(),event.identity.node);
    if(found!=progression_nodes.end())capture_diagnostic(progression_[std::size_t(found-progression_nodes.begin())],event);
}
void ReferenceRecorder::write_diagnostic(std::FILE* file,const char* record,const ReferenceEventDiagnostic& event) const noexcept {
    std::fprintf(file,"%s,%s,%s,",record,active_stage_,reason_);diagnostic_identity(file,event);diagnostic_stamp(file,event.stamp);
    std::fprintf(file,"%llu,%lld,%llu,%llu,%llu,%.0Lf\n",static_cast<unsigned long long>(event.events),
        static_cast<long long>(pcm_begin_),static_cast<unsigned long long>(pcm_samples_),static_cast<unsigned long long>(spectral_frames_),
        static_cast<unsigned long long>(active_method_frames_),static_cast<long double>(pcm_begin_)+static_cast<long double>(active_method_frames_)*512);
}
bool ReferenceRecorder::diagnose(const dataflow_runtime::NumericPortFrameView& event) noexcept {
    if(diagnosed_)return false;
    diagnosed_=true;ReferenceEventDiagnostic rejected;capture_diagnostic(rejected,event);
    std::snprintf(failure_message_.data(),failure_message_.size(),"Invalid reference pipeline record: stage=%s node=%d port=%u guard=%s",
        active_stage_,int(event.identity.node),unsigned(event.identity.port),reason_);
    std::fprintf(stderr,"%s\n%s\n",failure_message_.data(),diagnostic_header);
    for(const auto& prior:progression_)if(prior.seen){write_diagnostic(diagnostic_.get(),"latest_progression",prior);write_diagnostic(stderr,"latest_progression",prior);}
    write_diagnostic(diagnostic_.get(),"first_rejected",rejected);write_diagnostic(stderr,"first_rejected",rejected);
    diagnostic_.flush();return false;
}
QJsonArray ReferenceRecorder::methods_summary() {
    QJsonArray methods;
    for(const auto& method:methods_){
        require(method.frames==spectral_frames_,"Reference descriptor and FFT frame counts differ");
        methods.push_back(QJsonObject{{"method",method.name},{"frames",double(method.frames)}});
    }
    return methods;
}
QJsonArray ReferenceRecorder::trace_summary() {
    QJsonArray stages;
    for(std::size_t i=0;i<trace_.size();++i){
        require(!i || stage_frames_[i]==spectral_frames_,"Reference frontend stages have different frame counts");
        if(tracing_){require(trace_[i].flush(),"Cannot flush reference trace");stages.push_back(QJsonObject{{"file",trace_names[i]},{"width",int(trace_widths[i])},{"frames",double(i?stage_frames_[i]:pcm_samples_)}});}
    }
    return stages;
}
QJsonObject ReferenceRecorder::finish() {
    require(envelopes_.flush(),"Cannot flush reference envelopes");
    const auto expected=pcm_samples_<2048?0:1+(pcm_samples_-2048)/512;
    require(spectral_frames_==expected,"Reference framing differs from complete causal STFT windows");
    return {{"schema","reference-pipeline.v1"},{"sample_rate",44100},{"fft_size",2048},{"hop_samples",512},
        {"center",false},{"periodic_window",true},{"pcm_begin",double(pcm_begin_)},{"pcm_samples",double(pcm_samples_)},{"frames",double(spectral_frames_)},
        {"methods",methods_summary()},{"trace",trace_summary()},{"pcm_source","production resampler output; SRC numerical parity is separate"},
        {"time_convention","window start/center/end are source-sample positions; readiness and nominal SRC delay are separate"}};
}
}
