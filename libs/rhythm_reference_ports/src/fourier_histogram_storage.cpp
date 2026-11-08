// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/weighted_tempo_histogram.h>
#include <rhythm_reference_ports/frame_tempo_path.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>
namespace rhythm_reference_ports {
using namespace dsp_kernels;
bool PreparedWeightedTempoHistogram::storage_bytes(std::size_t observations,std::size_t frames,WeightedTempoHistogramConfig c,std::size_t& out) noexcept {
    if(!observations || !frames || !std::isfinite(c.minimum_bpm) || !std::isfinite(c.maximum_bpm) || c.minimum_bpm<0 || c.minimum_bpm>=c.maximum_bpm ||
       !std::isfinite(c.tolerance_percent) || c.tolerance_percent<0 || double(c.maximum_bpm)>=double(SIZE_MAX/sizeof(InterpolatedPeak))-2)return false;
    std::size_t bytes=0;
    if(!prepared_storage::add_array(bytes,std::size_t(std::ceil(c.maximum_bpm))+1,2*sizeof(float)+sizeof(InterpolatedPeak)))return false;
    out=bytes;return true;
}
std::size_t PreparedWeightedTempoHistogram::workspace_bytes() const noexcept {return (histogram_.capacity()+frame_.capacity())*sizeof(float)+result_.capacity()*sizeof(InterpolatedPeak);}
bool PreparedFrameTempoPath::storage_bytes(std::size_t frames,FrameTempoPathConfig c,std::size_t& out) noexcept {
    if(!frames || !c.hop || !std::isfinite(c.frequency_step) || c.frequency_step<std::numeric_limits<float>::min() || !std::isfinite(c.frame_rate) || c.frame_rate<1 ||
       !std::isfinite(c.minimum_bpm) || c.minimum_bpm<0 || !std::isfinite(c.maximum_bpm) || c.maximum_bpm<std::numeric_limits<float>::min() || c.minimum_bpm>c.maximum_bpm ||
       !std::isfinite(c.tolerance_percent) || c.tolerance_percent<0 || !std::isfinite(c.minimum_change_seconds) || c.minimum_change_seconds<0 || !std::isfinite(c.induced_bpm) || c.induced_bpm<0)return false;
    const float stable=c.minimum_change_seconds*c.frame_rate/float(c.hop);
    if(!std::isfinite(stable) || stable>std::nextafter(float(INT_MAX),0.f) || !std::isfinite(c.frequency_step*60.f))return false;
    return prepared_storage::mul_bytes(frames,sizeof(float),out);
}
std::size_t PreparedFrameTempoPath::workspace_bytes() const noexcept {return bpms_.capacity()*sizeof(float);}
}
