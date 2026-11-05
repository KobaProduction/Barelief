// SPDX-License-Identifier: GPL-3.0-or-later
// Port of OnsetDetectionFunction::calculateTukeyWindow at
// adamstark/BTrack@9d6127618a5679e9caa74c594b88f1d74f0e035f.
// Copyright (C) 2008-2014 Queen Mary University of London, Adam Stark.
// Float32 decomposition Copyright (C) 2026 Koba Production.
// GPL version 3 or later; no warranty. See ../COPYING.
#include <rhythm_reference_ports/reference_window.h>
#include <dsp_kernels/window_math.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
namespace rhythm_reference_ports {
namespace {
bool source_hanningz(std::span<float> output) noexcept {
    // aubio's Double TWO_PI/size expression is rounded to the cosf argument;
    // its outer Double literals are rounded once to the Float32 coefficient.
    constexpr double period=2.*std::numbers::pi_v<double>;
    for(std::size_t i=0;i<output.size();++i){
        const float angle=float(period*double(i)/double(output.size()));
        output[i]=float(.5*(1.-double(std::cos(angle))));
    }
    return true;
}
bool cosine_analysis_window(std::span<float> output,AnalysisWindowShape shape) noexcept {
    constexpr std::array<std::array<float,4>,6> weights{{
        {1,0,0,0},{.5f,0,0,0},{.54f,-.46f,0,0},{.5f,-.5f,0,0},
        {.42f,-.5f,.08f,0},{.35875f,-.48829f,.14128f,-.01168f}}};
    const float period=float(output.size()-std::size_t(shape>=AnalysisWindowShape::Blackman));
    return dsp_kernels::cosine_sum_window(output,weights[unsigned(shape)],period);
}
bool taper_analysis_window(std::span<float> output,AnalysisWindowShape shape) noexcept {
    const float size=float(output.size());
    for(std::size_t i=0;i<output.size();++i){
        if(shape==AnalysisWindowShape::Gaussian){
            const float x=(float(i)-.5f*(size-1.f))/(.25f*(size-1.f));output[i]=std::exp(-.5f*x*x);
        }else{
            const float x=(2.f*float(i)-size)/(size+1.f);
            output[i]=1.f-(shape==AnalysisWindowShape::Welch?x*x:std::abs(x));
        }
    }
    return true;
}
}
// Window formula source: aubio mathutils.c, pinned ad5cf975aed08cc4562dd008cf9f83b12b82ffb8.
// Copyright (C) 2003-2014 Paul Brossier, GPL3 or later; same ../COPYING.
bool analysis_window(std::span<float> output,AnalysisWindowShape shape) noexcept {
    if(output.size()<2 || output.size()>std::size_t(INT32_MAX) || unsigned(shape)>unsigned(AnalysisWindowShape::Parzen))return false;
    if(shape==AnalysisWindowShape::PeriodicHann)return source_hanningz(output);
    return shape<=AnalysisWindowShape::BlackmanHarris?cosine_analysis_window(output,shape):taper_analysis_window(output,shape);
}
bool predictive_window(std::span<float> output,PredictiveWindowShape shape) noexcept {
    if(output.size()<2 || output.size()>std::size_t(INT32_MAX) || unsigned(shape)>unsigned(PredictiveWindowShape::ShiftedTaper))return false;
    if(shape==PredictiveWindowShape::ShiftedTaper)return shifted_cosine_taper(output);
    constexpr std::array<std::array<float,3>,4> weights{{
        {1,0,0},{.5f,-.5f,0},{.54f,-.46f,0},{.42f,-.5f,.08f}}};
    return dsp_kernels::cosine_sum_window(output,weights[unsigned(shape)],float(output.size()-1));
}
bool shifted_cosine_taper(std::span<float> output) noexcept {
    if(output.size()<2 || output.size()>std::size_t(PTRDIFF_MAX))return false;
    const float n=float(output.size()-1),plateau=.25f*n;
    for(std::size_t i=0;i<output.size();++i){
        const float position=float(std::ptrdiff_t(i)-std::ptrdiff_t(output.size()/2)+1);
        output[i]=std::abs(position)<=plateau?1.f:.5f*(1.f+std::cos(std::numbers::pi_v<float>*(4.f*position/n-1.f)));
    }
    return true;
}
}
