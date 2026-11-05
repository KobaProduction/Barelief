// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia src/algorithms/rhythm/tempotapmaxagreement.cpp at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// AGPL version3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/beat_agreement.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <limits>
#include <numbers>
#include <utility>
namespace rhythm_reference_ports {
void PreparedBeatAgreement::boundaries() {
    std::vector<float> centers;centers.reserve(41);centers.push_back(-.5f);
    constexpr float delta=1.f/39.f;
    for(float center=-.5f+1.5f*delta;center<=.5f-1.5f*delta;center+=delta)centers.push_back(center);
    centers.push_back(.5f);boundaries_.resize(centers.size()-1);
    for(std::size_t i=0;i<boundaries_.size();++i)boundaries_[i]=.5f*(centers[i]+centers[i+1]);
    histogram_.resize(boundaries_.size()+1);
}
bool PreparedBeatAgreement::prepare(std::size_t methods,std::size_t beats,float minimum) {
    std::size_t admitted=0;if(!storage_bytes(methods,beats,minimum,admitted))return false;
    if(!methods || methods>SIZE_MAX/methods || !beats || !std::isfinite(minimum) || minimum<0)return false;
    PreparedBeatAgreement next;next.maximum_methods_=methods;next.maximum_beats_=beats;next.minimum_time_=minimum;
    next.scores_.resize(methods);next.pairwise_.resize(methods*methods);next.boundaries();if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedBeatAgreement::validate(std::span<const BeatSequenceView> methods) const noexcept {
    if(!maximum_methods_ || methods.size()>maximum_methods_)return false;
    for(const auto& method:methods){
        if(method.seconds.size()>maximum_beats_)return false;
        for(std::size_t i=0;i<method.seconds.size();++i)
            if(!std::isfinite(method.seconds[i]) || method.seconds[i]<0 || (i && method.seconds[i]<=method.seconds[i-1]))return false;
    }
    return true;
}
std::span<const float> PreparedBeatAgreement::trim(std::span<const float> values) const noexcept {
    const auto first=std::lower_bound(values.begin(),values.end(),minimum_time_);return values.subspan(std::size_t(first-values.begin()));
}
std::size_t PreparedBeatAgreement::closest(std::span<const float> values,float time) const noexcept {
    const auto right=std::lower_bound(values.begin(),values.end(),time);const auto index=std::size_t(right-values.begin());
    if(!index)return 0;
    if(index==values.size())return index-1;
    return time-values[index-1]<=values[index]-time?index-1:index;
}
float PreparedBeatAgreement::error(std::span<const float> reference,float time) const noexcept {
    const auto at=closest(reference,time);const float delta=time-reference[at];
    float interval=0;
    if(at==0)interval=.5f*(reference[1]-reference[0]);
    else if(at+1==reference.size() || delta<0)interval=.5f*(reference[at]-reference[at-1]);
    else interval=.5f*(reference[at+1]-reference[at]);
    return .5f*delta/interval;
}
bool PreparedBeatAgreement::fill_histogram(std::span<const float> reference,std::span<const float> candidate) noexcept {
    std::fill(histogram_.begin(),histogram_.end(),0.f);
    for(const float time:candidate){
        // Essentia's fmod is a positive floor modulo, including early ticks
        // whose relative error is below -.5. std::fmod retains their sign.
        const float shifted=error(reference,time)+.5f;
        const float phase=shifted-std::floor(shifted)-.5f;
        if(!std::isfinite(phase))return false;
        const auto bin=std::size_t(std::upper_bound(boundaries_.begin(),boundaries_.end(),phase)-boundaries_.begin());
        ++histogram_[bin];
    }
    return true;
}
float PreparedBeatAgreement::entropy(std::span<const float> reference,std::span<const float> candidate) noexcept {
    if(!fill_histogram(reference,candidate))return std::numeric_limits<float>::quiet_NaN();
    histogram_[0]+=histogram_.back();const float total=std::accumulate(histogram_.begin(),histogram_.end()-1,0.f);
    float result=0;
    for(std::size_t bin=0;bin+1<histogram_.size();++bin){
        const float probability=total>0?histogram_[bin]/total:0;
        if(probability>0)result-=probability*std::log2(probability);
    }
    return result;
}
float PreparedBeatAgreement::information_gain(std::span<const float> first,std::span<const float> second) noexcept {
    if(first.size()<2 || second.size()<2)return 0;
    // The pinned source calls essentia::log2(_numberBins), whose template
    // returns the argument's int type. Preserve that source maximum (5),
    // rather than substituting the mathematical floating log2(40).
    const auto source_maximum=float(int(std::log(40.)/std::numbers::ln2));
    return source_maximum-std::max(entropy(first,second),entropy(second,first));
}
bool PreparedBeatAgreement::pairs(std::span<const BeatSequenceView> methods) noexcept {
    for(std::size_t first=0;first<methods.size();++first)
        for(std::size_t second=first+1;second<methods.size();++second){
            const float gain=information_gain(trim(methods[first].seconds),trim(methods[second].seconds));
            if(!std::isfinite(gain))return false;
            pairwise_[first*methods_+second]=gain;pairwise_[second*methods_+first]=gain;
        }
    return true;
}
float PreparedBeatAgreement::method_score(std::size_t method) const noexcept {
    float sum=0;
    for(std::size_t other=method+1;other<methods_;++other)sum+=pairwise_[method*methods_+other];
    for(std::size_t other=0;other<method;++other)sum+=pairwise_[method*methods_+other];
    return methods_>1?sum/float(methods_-1):0;
}
bool PreparedBeatAgreement::process(std::span<const BeatSequenceView> methods,BeatAgreementSelection& output) noexcept {
    if(!validate(methods))return false;
    methods_=methods.size();std::fill(scores_.begin(),scores_.end(),0.f);std::fill(pairwise_.begin(),pairwise_.end(),0.f);
    if(methods.empty()){output={};return true;}
    if(!pairs(methods))return false;
    output=select_method();return true;
}
BeatAgreementSelection PreparedBeatAgreement::select_method() noexcept {
    for(std::size_t i=0;i<methods_;++i)scores_[i]=method_score(i);
    const auto scores=std::span(scores_).first(methods_);
    return {std::size_t(std::max_element(scores.begin(),scores.end())-scores.begin()),std::accumulate(scores.begin(),scores.end(),0.f)/float(methods_)};
}
}
