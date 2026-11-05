#include "support.h"
#include <cmath>

namespace rhythm_replay::detail {
namespace {
bool matching(const std::array<TempogramProbe,2>& f,const std::array<TempogramProbe,2>& a) noexcept {
    const auto count=f[0].snapshots;
    return count==f[1].snapshots && count==a[0].snapshots && count==a[1].snapshots &&
        f[0].observed==f[1].observed && f[0].observed==a[0].observed && f[0].observed==a[1].observed;
}
bool read_candidate(const signal_contracts::NumericView& view,std::size_t row,dsp_kernels::rhythm::TempoEvidenceCandidate& candidate) noexcept {
    using namespace signal_contracts;
    std::array<float,domain::tempo_candidate_columns> v{};
    for(std::size_t column=0;column<v.size();++column)
        if(load_element(view,{0,row,column},v[column])!=Status::ok || !std::isfinite(v[column]))return false;
    if(v[6]<0 || v[6]>15 || std::floor(v[6])!=v[6])return false;
    candidate={v[0],v[1],v[2],v[3],v[4],v[5],std::uint8_t(v[6])};return true;
}
}
void TempoEvidenceRecorder::open(const QString& path,unsigned frame_rate) {
    require(frame_rate>=10 && frame_rate<=1000,"Invalid recorded analysis rate");
    file_.open(path,"frame,t_obs,filled_short,filled_long,available,rank,bpm,score,fourier_short,fourier_long,acf_short,acf_long,origin_curves,maturity,harmonic_ambiguity");
}
bool TempoEvidenceRecorder::read_matrix(const signal_contracts::NumericView& view) noexcept {
    using namespace signal_contracts;
    if(view.contract->dtype!=DType::Float32 || view.actual!=std::array{std::size_t{8},domain::tempo_candidate_columns})return false;
    for(std::size_t row=0;row<output_.candidates.size();++row)
        if(!read_candidate(view,row,output_.candidates[row]))return false;
    return true;
}
bool TempoEvidenceRecorder::begin_frame(const dataflow_runtime::NumericPortFrameView& event) noexcept {
    const auto& view=std::get<signal_contracts::NumericView>(event.frame.payload);
    ++frames_;sequence_=event.frame.stamp.sequence;
    const auto tick=view.contract->clock.tick_seconds;
    observed_=double(event.frame.stamp.timestamp_ticks)*double(tick.numerator)/double(tick.denominator);
    return read_matrix(view);
}
bool TempoEvidenceRecorder::read_scalar(const dataflow_runtime::NumericPortFrameView& event) noexcept {
    using namespace signal_contracts;
    const auto& view=std::get<NumericView>(event.frame.payload);
    if(event.identity.port==1){
        std::uint32_t count=0;if(load_element(view,{},count)!=Status::ok || count>8)return false;
        output_.count=count;return true;
    }
    if(event.identity.port==2)return load_element(view,{},output_.maturity)==Status::ok &&
        std::isfinite(output_.maturity) && output_.maturity>=0 && output_.maturity<=1;
    std::uint8_t ambiguity=0;if(load_element(view,{},ambiguity)!=Status::ok || ambiguity>1)return false;
    output_.harmonic_ambiguity=ambiguity!=0;return true;
}
bool TempoEvidenceRecorder::receive(const dataflow_runtime::NumericPortFrameView& event,const std::array<TempogramProbe,2>& f,const std::array<TempogramProbe,2>& a) noexcept {
    if(event.identity.node!=1220)return true;
    if(!event.frame.stamp.available)return false;
    if(event.identity.port==0)return begin_frame(event);
    if(event.frame.stamp.sequence!=sequence_ || !read_scalar(event))return false;
    if(event.identity.port!=3 || (frames_!=1 && frames_%15!=0))return true;
    return complete(f,a);
}
bool TempoEvidenceRecorder::complete(const std::array<TempogramProbe,2>& f,const std::array<TempogramProbe,2>& a) noexcept {
    if(f[0].snapshots!=snapshots_+1 || !matching(f,a) || f[0].frames!=frames_ || f[0].observed!=observed_)return false;
    if(f[0].filled!=a[0].filled || f[1].filled!=a[1].filled || !write(frames_,observed_,f))return false;
    snapshots_=f[0].snapshots;return true;
}
bool TempoEvidenceRecorder::write(std::uint64_t frame,double time,const std::array<TempogramProbe,2>& f) noexcept {
    const auto& output=output_;
    if(!output.count)return write_candidate({},0,false,frame,time,f);
    for(std::size_t i=0;i<output.count;++i)
        if(!write_candidate(output.candidates[i],i,true,frame,time,f))return false;
    return true;
}
bool TempoEvidenceRecorder::write_candidate(const dsp_kernels::rhythm::TempoEvidenceCandidate& value,std::size_t rank,bool available,
                                           std::uint64_t frame,double time,const std::array<TempogramProbe,2>& f) noexcept {
    ++rows_;const auto& out=output_;
    return std::fprintf(file_.get(),"%llu,%.17g,%llu,%llu,%u,%zu,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%u,%.17g,%u\n",
        static_cast<unsigned long long>(frame),time,static_cast<unsigned long long>(f[0].filled),static_cast<unsigned long long>(f[1].filled),
        unsigned(available),rank,value.bpm,value.score,value.fourier_short,value.fourier_long,value.acf_short,value.acf_long,
        unsigned(value.origin_curves),out.maturity,unsigned(out.harmonic_ambiguity))>0;
}
QJsonObject TempoEvidenceRecorder::finish(std::uint64_t expected) {
    require(snapshots_==expected && file_.flush(),"Tempo evidence snapshots incomplete or write failed");
    return {{"snapshots",double(snapshots_)},{"rows",double(rows_)},{"frames",double(frames_)},
        {"source","production TempoEvidence node1220; recorder performs no DSP"},
        {"method","bounded refined Fourier/linear-ACF evidence; correlated components, score is not probability"},
        {"confirmed_decision",false}};
}
}
