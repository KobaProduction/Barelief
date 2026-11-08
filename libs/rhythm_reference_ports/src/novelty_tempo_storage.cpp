// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/fourier_tempo_candidates.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <climits>
namespace rhythm_reference_ports {
using namespace dsp_kernels;
bool PreparedFourierTempoCandidates::storage_bytes(std::size_t bins,FourierTempoCandidateConfig c,std::size_t& out) noexcept {
    if(bins<2 || !c.maximum_peaks || c.maximum_peaks==SIZE_MAX || !std::isfinite(c.frequency_step) || c.frequency_step<=0 || !std::isfinite(60.f*c.frequency_step) ||
       !std::isfinite(c.minimum_bpm) || !std::isfinite(c.maximum_bpm) || c.minimum_bpm<0 || c.maximum_bpm<=c.minimum_bpm || double(c.maximum_bpm)>double(SIZE_MAX/sizeof(float))-2)return false;
    c.minimum_bpm=std::floor(c.minimum_bpm);c.maximum_bpm=std::ceil(c.maximum_bpm);
    InterpolatedPeakConfig peak;peak.range=float(bins-1);peak.minimum=c.minimum_bpm/(60.f*c.frequency_step);peak.maximum=c.maximum_bpm/(60.f*c.frequency_step);
    peak.threshold=0;peak.maximum_peaks=c.maximum_peaks;peak.ordering=PeakOrdering::Amplitude;std::size_t bytes=0;
    if(!PreparedInterpolatedPeaks::storage_bytes(bins,peak,bytes) || !prepared_storage::add_array(bytes,bins,sizeof(float)) ||
       !prepared_storage::add_array(bytes,std::size_t(c.maximum_bpm)+1,sizeof(float)) || !prepared_storage::add_array(bytes,c.maximum_peaks+1,sizeof(InterpolatedPeak)))return false;
    out=bytes;return true;
}
std::size_t PreparedFourierTempoCandidates::workspace_bytes() const noexcept {
    return detector_.workspace_bytes()+(scratch_.capacity()+tempo_.capacity())*sizeof(float)+observations_.capacity()*sizeof(InterpolatedPeak);
}
}
