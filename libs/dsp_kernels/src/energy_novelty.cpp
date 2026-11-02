#include <dsp_kernels/energy_novelty.h>
#include <algorithm>
#include <cmath>
namespace dsp_kernels {
bool PreparedEnergyNovelty::prepare(EnergyNoveltyConfig config) noexcept {
    if(config.value<EnergyNoveltyValue::SumSquares || config.value>EnergyNoveltyValue::RootMeanSquare)return false;
    config_=config;previous_=0;prepared_=true;return true;
}
bool PreparedEnergyNovelty::process(std::span<const float> values,float& output) noexcept {
    if(!prepared_ || values.empty())return false;
    float sum=0,error=0;
    for(const auto value:values){
        if(!std::isfinite(value))return false;
        const float term=value*value-(config_.compensated_sum?error:0.f),next=sum+term;
        error=(next-sum)-term;sum=next;
    }
    const float energy=config_.value==EnergyNoveltyValue::RootMeanSquare?std::sqrt(sum/float(values.size())):sum;
    const float novelty=config_.positive_difference?std::max(0.f,energy-previous_):energy;
    if(!std::isfinite(energy) || !std::isfinite(novelty))return false;
    previous_=energy;output=novelty;return true;
}
}
