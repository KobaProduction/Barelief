// SPDX-License-Identifier: GPL-3.0-or-later
#include <rhythm_reference_ports/detail/predictive_reference_frontend.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
namespace rhythm_reference_ports::detail {
bool PreparedPredictiveReferenceFrontend::complex_storage_bytes(std::size_t frame,std::size_t& output) noexcept {
    output=0;
    return frame>=2 && frame<=INT32_MAX && frame%2==0 &&
        dsp_kernels::prepared_storage::add_array(output,frame,5*sizeof(double));
}
bool PreparedPredictiveReferenceFrontend::prepare_complex(std::size_t frame) {
    std::size_t bytes=0;if(!complex_storage_bytes(frame,bytes))return false;
    PreparedPredictiveReferenceFrontend next;
    next.magnitude_.resize(frame);next.phase_.resize(frame);next.previous_magnitude_.resize(frame);
    next.previous_phase_.resize(frame);next.older_phase_.resize(frame);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedPredictiveReferenceFrontend::process_complex(std::span<const float> complex,float& output) noexcept {
    if(magnitude_.empty() || complex.size()!=2*magnitude_.size() ||
        !std::all_of(complex.begin(),complex.end(),[](float x){return std::isfinite(x);}))return false;
    if(!polar(complex) || !evaluate(output))return false;
    commit_history();return true;
}
double PreparedPredictiveReferenceFrontend::distance(std::size_t i) const noexcept {
    const double current=magnitude_[i],previous=previous_magnitude_[i];
    if(current-previous<=0)return 0;
    // This branch deliberately uses the source's raw, unwrapped deviation.
    const double deviation=phase_[i]-(2*previous_phase_[i])+older_phase_[i];
    return std::sqrt(std::pow(current,2)+std::pow(previous,2)-2*current*previous*std::cos(deviation));
}
std::size_t PreparedPredictiveReferenceFrontend::workspace_bytes() const noexcept {
    return (magnitude_.capacity()+phase_.capacity()+previous_magnitude_.capacity()+previous_phase_.capacity()+older_phase_.capacity())*sizeof(double);
}
void PreparedPredictiveReferenceFrontend::restart() noexcept {
    std::fill(magnitude_.begin(),magnitude_.end(),0.);std::fill(phase_.begin(),phase_.end(),0.);
    std::fill(previous_magnitude_.begin(),previous_magnitude_.end(),0.);
    std::fill(previous_phase_.begin(),previous_phase_.end(),0.);std::fill(older_phase_.begin(),older_phase_.end(),0.);value_=0;
}
bool PreparedPredictiveReferenceFrontend::polar(std::span<const float> complex) noexcept {
    for(std::size_t i=0;i<magnitude_.size();++i){
        const double real=complex[2*i],imaginary=complex[2*i+1];
        phase_[i]=std::atan2(imaginary,real);magnitude_[i]=std::sqrt(std::pow(real,2)+std::pow(imaginary,2));
        if(!std::isfinite(magnitude_[i]) || magnitude_[i]>std::numeric_limits<float>::max() || !std::isfinite(phase_[i]))return false;
    }
    return true;
}
bool PreparedPredictiveReferenceFrontend::evaluate(float& output) noexcept {
    double sum=0;for(std::size_t i=0;i<magnitude_.size();++i)sum=sum+distance(i);
    if(!std::isfinite(sum) || std::abs(sum)>std::numeric_limits<float>::max())return false;
    value_=sum;output=float(sum);return true;
}
void PreparedPredictiveReferenceFrontend::commit_history() noexcept {
    for(std::size_t i=0;i<magnitude_.size();++i){
        older_phase_[i]=previous_phase_[i];previous_phase_[i]=phase_[i];previous_magnitude_[i]=magnitude_[i];
    }
}
}
