#pragma once
#include <dsp_kernels/frequency_profile.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels {
// 101 inclusive RMS boundaries: -200, -198, ... 0 dB; band 101 is overflow.
inline constexpr unsigned rms_overflow_band=101;
double rms_band_upper(unsigned index) noexcept;
void accumulate_rms_band(CompositionStatistics&,double rms,std::span<const float> spectrum);
bool valid_rms_bands(const CompositionStatistics&);
bool classify_quiet_frames(CompositionStatistics&,int upper_db);
inline double measurement_rate(const CompositionStatistics& t,double fallback) noexcept {
    return t.sample_rate>0?t.sample_rate:fallback;
}
inline bool valid_measurement_grid(const CompositionStatistics& t,double fallback) noexcept {
    const auto rate=measurement_rate(t,fallback);
    return std::isfinite(t.sample_rate) && t.sample_rate>=0 && std::isfinite(rate) && rate>0 && rate<=768000 &&
        std::isfinite(t.measured_max_hz) && t.measured_max_hz>=0 && t.measured_max_hz<=rate/2;
}
inline bool measured_frequency(const CompositionStatistics& t,double fallback,double hz) noexcept {
    const auto limit=t.measured_max_hz>0?t.measured_max_hz:measurement_rate(t,fallback)/2;
    return hz>=0 && hz<=limit && hz<=measurement_rate(t,fallback)/2;
}
inline double sample_measured_spectrum(std::span<const double> values,double rate,double hz) noexcept {
    if(values.size()<2 || rate<=0 || hz<0 || hz>rate/2)return 0;
    const auto position=hz*2*double(values.size()-1)/rate;
    const auto i=std::min(std::size_t(position),values.size()-1),j=std::min(i+1,values.size()-1);
    return values[i]+(position-double(i))*(values[j]-values[i]);
}
}
