// SPDX-License-Identifier: AGPL-3.0-or-later
// Mathematical stages of Essentia TempoTap/TempoTapDegara at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/periodic_comb.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace rhythm_reference_ports {
namespace {
bool finite(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);});
}
bool overlaps(std::span<const float> left,std::span<const float> right) noexcept {
    if(left.empty() || right.empty())return false;
    const std::less<const float*> before;
    return before(left.data(),right.data()+right.size()) && before(right.data(),left.data()+left.size());
}
bool comb_geometry(std::size_t input,std::size_t first,std::size_t last,std::size_t harmonics) noexcept {
    const auto maximum=std::size_t(std::numeric_limits<std::ptrdiff_t>::max());
    if(first>last || !harmonics || harmonics>maximum/2 || last>=maximum/harmonics)return false;
    return first==last || harmonics*(last+1)-2<input;
}
float comb_at(std::span<const float> input,float weight,std::size_t period,std::size_t harmonics) noexcept {
    float value=0;
    for(std::size_t a=1;a<=harmonics;++a)
        for(auto b=1-std::ptrdiff_t(a);b<std::ptrdiff_t(a);++b){
            const auto lag=std::size_t(std::ptrdiff_t(a*(period+1))-1+b);
            value+=input[lag]*weight/float(2*a-1);
        }
    return value;
}
float phase_at(std::span<const float> input,float period,std::size_t pulses,std::size_t position) noexcept {
    float value=0;
    for(std::size_t a=0;a<pulses;++a){
        const float at=std::round(float(a)*period+float(position));
        if(std::isfinite(at) && at>=0 && at<float(input.size()))value+=input[std::size_t(at)];
    }
    return value;
}
}
bool harmonic_comb_geometry(std::size_t input,std::size_t output,std::size_t first,std::size_t last,std::size_t harmonics) noexcept {
    return last<=output && comb_geometry(input,first,last,harmonics);
}
bool harmonic_comb_response(std::span<const float> input,std::span<const float> weights,
                            std::size_t first,std::size_t last,std::size_t harmonics,
                            std::span<float> output) noexcept {
    if(weights.size()!=output.size() || last>output.size() || !comb_geometry(input.size(),first,last,harmonics))return false;
    if(overlaps(input,output) || overlaps(weights,output) || !finite(input) || !finite(weights))return false;
    std::fill(output.begin(),output.end(),0.f);
    for(std::size_t period=first;period<last;++period){
        output[period]=comb_at(input,weights[period],period,harmonics);
        if(!std::isfinite(output[period]))return false;
    }
    return true;
}
bool periodic_phase_response(std::span<const float> input,float period,std::size_t pulses,std::span<float> output) noexcept {
    if(input.empty() || !std::isfinite(period) || period<=0 || !pulses || overlaps(input,output) || !finite(input))return false;
    for(std::size_t i=0;i<output.size();++i){
        output[i]=phase_at(input,period,pulses,i);
        if(!std::isfinite(output[i]))return false;
    }
    return true;
}
}
