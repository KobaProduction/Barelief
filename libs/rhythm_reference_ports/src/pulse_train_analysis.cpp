// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia PercivalEnhanceHarmonics/PercivalEvaluatePulseTrains at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/pulse_train_analysis.h>
#include <dsp_kernels/sequence_statistics.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool finite(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);});
}
bool overlap(std::span<const float> input,std::span<float> output) noexcept {
    if(input.empty() || input.data()==output.data())return false;
    const std::less<const float*> before;
    return before(input.data(),output.data()+output.size()) && before(output.data(),input.data()+input.size());
}
float sample(std::span<const float> values,std::size_t position) noexcept {
    return position<values.size()?values[position]:0;
}
}
bool enhance_correlation_harmonics(std::span<const float> input,std::span<float> output) noexcept {
    if(input.size()!=output.size() || overlap(input,output) || !finite(input))return false;
    if(input.data()!=output.data())std::copy(input.begin(),input.end(),output.begin());
    for(std::size_t i=0;i<input.size()/4;++i)output[i]+=output[2*i]+output[4*i];
    return finite(output);
}
bool PreparedPulseTrainScoring::prepare(std::size_t maximum_lag,std::size_t maximum_candidates) {
    if(!maximum_lag || maximum_lag>std::numeric_limits<std::size_t>::max()/12 || !maximum_candidates)return false;
    std::size_t bound=0;if(!storage_bytes(maximum_lag,maximum_candidates,bound))return false;
    PreparedPulseTrainScoring next;
    next.phases_.resize(maximum_lag);next.magnitudes_.resize(maximum_candidates);
    next.variances_.resize(maximum_candidates);next.scores_.resize(maximum_candidates);
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedPulseTrainScoring::valid(std::span<const float> onset,std::span<const float> candidates) const noexcept {
    if(phases_.empty() || candidates.size()>scores_.size() || !finite(onset) || !finite(candidates))return false;
    for(const float candidate:candidates){
        const float rounded=std::round(candidate);
        if(rounded<0 || rounded>float(phases_.size()) || std::size_t(rounded)>phases_.size())return false;
    }
    return true;
}
float PreparedPulseTrainScoring::pulse(std::span<const float> onset,std::size_t phase,std::size_t lag) const noexcept {
    float result=0;
    for(std::size_t beat=0;beat<4;++beat){
        result+=sample(onset,phase+beat*lag);
        result+=.5f*sample(onset,phase+beat*lag*2);
        result+=.5f*sample(onset,phase+beat*lag*3/2);
    }
    return result;
}
bool PreparedPulseTrainScoring::evaluate(std::span<const float> onset,std::size_t lag,std::size_t candidate) noexcept {
    magnitudes_[candidate]=0;variances_[candidate]=0;
    if(!lag)return true;
    auto phases=std::span(phases_).first(lag);
    for(std::size_t phase=0;phase<lag;++phase)phases[phase]=pulse(onset,phase,lag);
    magnitudes_[candidate]=*std::max_element(phases.begin(),phases.end());
    return std::isfinite(magnitudes_[candidate]) && dsp_kernels::variance_float32(phases,variances_[candidate]);
}
std::size_t PreparedPulseTrainScoring::select() noexcept {
    const float magnitude_sum=std::accumulate(magnitudes_.begin(),magnitudes_.begin()+std::ptrdiff_t(count_),0.f);
    const float variance_sum=std::accumulate(variances_.begin(),variances_.begin()+std::ptrdiff_t(count_),0.f);
    std::size_t best=0;
    for(std::size_t i=0;i<count_;++i){
        scores_[i]=magnitudes_[i]/magnitude_sum+variances_[i]/variance_sum;
        if(scores_[i]>scores_[best])best=i;
    }
    return best;
}
bool PreparedPulseTrainScoring::process(std::span<const float> onset,std::span<const float> candidates,float& selected) noexcept {
    if(!valid(onset,candidates))return false;
    count_=candidates.size();
    if(!count_){selected=-1;return true;}
    for(std::size_t i=0;i<count_;++i)
        if(!evaluate(onset,std::size_t(std::round(candidates[i])),i))return false;
    selected=std::round(candidates[select()]);return true;
}
}
