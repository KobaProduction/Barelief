#include "support.h"
#include <cmath>

namespace rhythm_replay::detail {
using namespace signal_contracts;
using dataflow_runtime::NumericPortFrameView;
namespace {
double coordinate(const AxisCoordinates& axis,std::size_t index) noexcept {
    return double(axis.origin.numerator)/double(axis.origin.denominator)+
        double(index)*double(axis.step.numerator)/double(axis.step.denominator);
}
double observed_time(const NumericPortFrameView& event) noexcept {
    const auto& clock=std::get<NumericView>(event.frame.payload).contract->clock;
    return double(event.frame.stamp.timestamp_ticks)*double(clock.tick_seconds.numerator)/double(clock.tick_seconds.denominator);
}
bool write_bin(std::FILE* file,const TempogramProbe& probe,std::size_t bin,bool phase=false) noexcept {
    return std::fprintf(file,"%llu,%llu,%.17g,%llu,%zu,%.17g,%.9g\n",
        static_cast<unsigned long long>(probe.capacity),static_cast<unsigned long long>(probe.frames),probe.observed,
        static_cast<unsigned long long>(probe.filled),bin,probe.hz[bin],double(phase?probe.phase[bin]:probe.magnitude[bin]))>0;
}
bool write_snapshot(std::FILE* file,std::FILE* phases,const TempogramProbe& probe) noexcept {
    for(std::size_t bin=0;bin<(probe.acf?121:probe.capacity/2+1);++bin){
        if(!write_bin(file,probe,bin))return false;
        if(!probe.acf && !write_bin(phases,probe,bin,true))return false;
    }
    return true;
}
}
bool TempogramProbe::receive(const NumericPortFrameView& event,std::FILE* file,std::FILE* phases) noexcept {
    if(event.identity.node==history && event.identity.port==1)return accept_count(event,file,phases);
    if(!acf && event.identity.node==history+50 && event.identity.port==1)return accept_phase(event,file,phases);
    if(event.identity.node!=history+(acf?8:5) || event.identity.port!=0)return true;
    return accept_magnitude(event,file,phases);
}
bool TempogramProbe::accept_magnitude(const NumericPortFrameView& event,std::FILE* file,std::FILE* phases) noexcept {
    if(pending_sequence)return false;
    ++frames;
    if(frames!=1 && frames%15!=0)return true;
    if(!capture(event))return false;
    pending_sequence=event.frame.stamp.sequence;return flush(file,phases);
}
bool TempogramProbe::accept_count(const NumericPortFrameView& event,std::FILE* file,std::FILE* phases) noexcept {
    if(load_element(std::get<NumericView>(event.frame.payload),{0,0,0},filled)!=Status::ok || filled>capacity)return false;
    // Downstream row delivery may precede this output. Join matching stamps
    // using bounded owned samples, never a retained borrowed NumericView.
    count_sequence=event.frame.stamp.sequence;return flush(file,phases);
}
bool TempogramProbe::accept_phase(const NumericPortFrameView& event,std::FILE* file,std::FILE* phases) noexcept {
    ++phase_frames;
    if(phase_frames!=1 && phase_frames%15!=0)return true;
    const auto& view=std::get<NumericView>(event.frame.payload);
    if(view.contract->dtype!=DType::Float32 || view.actual!=std::array{std::size_t{1},std::size_t(capacity/2+1)} || view.actual[1]>phase.size() ||
       view.contract->quantity!=Quantity::SpectrumPhase || view.contract->unit!=NumericUnit::Radians)return false;
    for(std::size_t bin=0;bin<view.actual[1];++bin)
        if(load_element(view,{0,0,bin},phase[bin])!=Status::ok || !std::isfinite(phase[bin]) || std::abs(phase[bin])>3.141593f)return false;
    phase_sequence=event.frame.stamp.sequence;phase_observed=observed_time(event);
    return flush(file,phases);
}
bool TempogramProbe::accepts(const NumericView& view) const noexcept {
    if(view.contract->dtype!=DType::Float32 || view.actual[0]!=(acf?121:capacity/2+1) || view.actual[1]!=1 || view.actual[0]>magnitude.size())return false;
    const auto& axis=view.contract->coordinates[0];
    if(axis.unit!=(acf?NumericUnit::Seconds:NumericUnit::Hertz))return false;
    return axis.mode==CoordinateMode::Regular;
}
bool TempogramProbe::capture(const NumericPortFrameView& event) noexcept {
    const auto& view=std::get<NumericView>(event.frame.payload);
    if(!accepts(view))return false;
    for(std::size_t bin=0;bin<view.actual[0];++bin)if(!capture_value(view,bin))return false;
    observed=observed_time(event);
    return true;
}
bool TempogramProbe::capture_value(const NumericView& view,std::size_t bin) noexcept {
    const auto& axis=view.contract->coordinates[0];
    // The same owned coordinate buffer holds Hz for Fourier, lag seconds for ACF.
    hz[bin]=coordinate(axis,bin);
    if(!std::isfinite(hz[bin]) || load_element(view,{0,bin,0},magnitude[bin])!=Status::ok || !std::isfinite(magnitude[bin]))return false;
    return acf?std::abs(magnitude[bin])<=1:magnitude[bin]>=0;
}
bool TempogramProbe::flush(std::FILE* file,std::FILE* phases) noexcept {
    if(!pending_sequence || pending_sequence!=count_sequence)return true;
    if(!acf && pending_sequence!=phase_sequence)return true;
    if(!acf && (observed!=phase_observed || !phases))return false;
    if(!write_snapshot(file,phases,*this))return false;
    pending_sequence=0;++snapshots;
    return true;
}
QJsonObject TempogramProbe::summary() const {
    return {{"window",double(capacity)},{"frames",double(frames)},{"filled",double(filled)},
        {"snapshots",double(snapshots)},{"export_stride_frames",15},
        {"source",acf?"production MatrixRow after History/Transpose/Center/MatrixAutocorrelation":"production MatrixRow after History/Transpose/Center/Hann/MatrixFFT/ComplexPolar"},
        {"magnitude",acf?"signed overlap-energy-normalized linear ACF; lag zero included":"raw abs(DFT); DC and Nyquist included; no window gain correction"}};
}
bool Observations::write_tempograms(const NumericPortFrameView& event) noexcept {
    for(auto& probe:tempograms_)if(!probe.receive(event,tempogram_file_.get(),phase_file_.get()))return false;
    for(auto& probe:correlations_)if(!probe.receive(event,correlation_file_.get()))return false;
    return tempo_evidence_.receive(event,tempograms_,correlations_);
}
}
