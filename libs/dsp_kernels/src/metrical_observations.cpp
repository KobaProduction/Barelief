// Adapted from madmom features/beats_hmm.py observation models at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU Linz
// and Austrian Research Institute for Artificial Intelligence (OFAI), Vienna.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/metrical_observations.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels {
bool metrical_observation_pointers(const PreparedMetricalStateSpace& space,float lambda,MetricalObservation mode,
                                   std::span<std::uint32_t> output) noexcept {
    if(!space.states() || output.size()!=space.states() || !std::isfinite(lambda) || lambda<=1 ||
       unsigned(mode)>unsigned(MetricalObservation::BeatAndDownbeat))return false;
    const float border=1.f/lambda;const auto positions=space.positions();
    for(std::size_t state=0;state<output.size();++state){
        const float position=positions[state];output[state]=0;
        if(mode==MetricalObservation::BeatAndDownbeat && position-std::floor(position)<border)output[state]=1;
        if(position<border)output[state]=mode==MetricalObservation::BarStart?1:2;
    }
    return true;
}
float beat_activation_density(float activation,float lambda,bool active,bool logarithmic) noexcept {
    const float probability=active?activation:(1.f-activation)/(lambda-1.f);
    return logarithmic?std::log(probability):probability;
}
bool downbeat_activation_log_densities(std::span<const float> input,float lambda,std::span<float> output) noexcept {
    if(input.size()!=2 || output.size()!=3 || !std::isfinite(lambda) || lambda<=1 ||
       !std::all_of(input.begin(),input.end(),[](float p){return std::isfinite(p) && p>=0 && p<=1;}) ||
       input[0]+input[1]>1)return false;
    output[0]=std::log((1.f-(input[0]+input[1]))/(lambda-1.f));
    output[1]=std::log(input[0]);output[2]=std::log(input[1]);return true;
}
}
