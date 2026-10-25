#include <dsp_kernels/frequency_measurements.h>
#include <array>

namespace dsp_kernels {
double rms_band_upper(unsigned index) noexcept {
    return std::pow(10.,(-200.+2.*index)/20.);
}
namespace {
unsigned band_index(double rms) {
    static const auto boundaries=[]{
        std::array<double,rms_overflow_band> values{};
        for(unsigned i=0;i<values.size();++i)values[i]=rms_band_upper(i);
        return values;
    }();
    return unsigned(std::lower_bound(boundaries.begin(),boundaries.end(),rms)-boundaries.begin());
}
CompositionStatistics::RmsBand& find_band(CompositionStatistics& t,unsigned index) {
    auto at=std::lower_bound(t.rms_bands.begin(),t.rms_bands.end(),index,[](const auto& b,unsigned i){return b.index<i;});
    if(at!=t.rms_bands.end() && at->index==index)return *at;
    CompositionStatistics::RmsBand band;band.index=index;
    return *t.rms_bands.insert(at,std::move(band));
}
void add_quiet_band(CompositionStatistics& t,const CompositionStatistics::RmsBand& b) {
    t.quiet_frames+=b.frames;t.quiet_rms+=b.square_sum;
    for(std::size_t k=0;k<b.mean_spectrum.size();++k){
        t.quiet_mean_spectrum[k]+=double(b.mean_spectrum[k])*double(b.frames);
        t.quiet_peak_spectrum[k]=std::max(t.quiet_peak_spectrum[k],double(b.peak_spectrum[k]));
    }
}
void finish_quiet_statistics(CompositionStatistics& t) {
    if(!t.quiet_frames)return;
    t.quiet_rms=std::sqrt(t.quiet_rms/double(t.quiet_frames));
    for(auto& value:t.quiet_mean_spectrum)value/=double(t.quiet_frames);
}
bool valid_band_spectra(const CompositionStatistics::RmsBand& b,std::size_t bins) {
    if(b.mean_spectrum.size()!=bins || b.peak_spectrum.size()!=bins)return false;
    for(std::size_t k=0;k<bins;++k){
        if(!std::isfinite(b.mean_spectrum[k]) || !std::isfinite(b.peak_spectrum[k]) ||
            b.mean_spectrum[k]<0 || b.peak_spectrum[k]<b.mean_spectrum[k])return false;
    }
    return true;
}
}
void accumulate_rms_band(CompositionStatistics& t,double rms,std::span<const float> spectrum) {
    auto& band=find_band(t,band_index(rms));
    ++band.frames;band.square_sum+=rms*rms;
    band.mean_spectrum.resize(spectrum.size());band.peak_spectrum.resize(spectrum.size());
    for(std::size_t k=0;k<spectrum.size();++k){
        band.mean_spectrum[k]=float(double(band.mean_spectrum[k])+(spectrum[k]-band.mean_spectrum[k])/double(band.frames));
        band.peak_spectrum[k]=std::max(band.peak_spectrum[k],float(spectrum[k]));
    }
}
bool valid_rms_bands(const CompositionStatistics& t) {
    if(!t.rms_statistics_version)return t.rms_bands.empty();
    if(t.rms_statistics_version!=1 || t.rms_bands.size()>rms_overflow_band+1)return false;
    std::uint64_t frames=0;unsigned previous=0;bool first=true;
    for(const auto& b:t.rms_bands){
        if(b.index>rms_overflow_band || (!first && b.index<=previous) || !b.frames ||
            b.frames>t.active_frames-frames || !std::isfinite(b.square_sum) || b.square_sum<0 || !valid_band_spectra(b,t.mean_spectrum.size()))return false;
        first=false;previous=b.index;frames+=b.frames;
    }
    return frames==t.active_frames;
}
bool classify_quiet_frames(CompositionStatistics& t,int upper_db) {
    if(upper_db<-200 || upper_db>0 || upper_db%2 || !t.rms_statistics_version || !valid_rms_bands(t))return false;
    const auto boundary=unsigned((upper_db+200)/2);
    t.quiet_frames=0;t.quiet_rms=0;
    t.quiet_mean_spectrum.assign(t.mean_spectrum.size(),0);t.quiet_peak_spectrum.assign(t.mean_spectrum.size(),0);
    for(const auto& b:t.rms_bands)if(b.index<=boundary)add_quiet_band(t,b);
    finish_quiet_statistics(t);
    return true;
}
}
