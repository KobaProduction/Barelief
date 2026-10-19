#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
namespace dsp_kernels {
enum class WeightedMixOperation { Sum, Mean, Rms, MaximumAbsolute };
// Preparation validates runtime populations. Scaling preserves finite weights
// without overflowing their total; signed sum modes keep their declared order.
inline double maximum_mix_weight(std::span<const double> weights) noexcept {
    double maximum=0;for(const auto weight:weights)maximum=std::max(maximum,weight);return maximum;
}
inline double scaled_mix_weight(double weight,double scale) noexcept {return scale?weight/scale:0;}
inline std::optional<double> normalized_weight_fraction(std::span<const double> weights,std::size_t index) noexcept {
    if(index>=weights.size())return {};
    if(!std::all_of(weights.begin(),weights.end(),[](double weight){return std::isfinite(weight) && weight>=0;}))return {};
    const auto scale=maximum_mix_weight(weights);double total=0;
    for(const auto weight:weights)total+=scaled_mix_weight(weight,scale);
    return total?scaled_mix_weight(weights[index],scale)/total:0;
}
// The caller supplies weights in its chosen summation order. For Mean/Rms,
// normalize weights before accumulation; Rms squares use amplitude_scale().
// Validity and population limits belong to the caller's prepared contract.
class WeightedMixAccumulator {
    double sum_=0,total_=0,scale_=0,largest_=0;
public:
    void add_weighted(double sample,double weight) noexcept {
        sum_+=weight*sample;total_+=weight;
        if(std::fabs(weight*sample)>std::fabs(largest_))largest_=weight*sample;
        if(weight>0)scale_=std::max(scale_,std::fabs(sample));
    }
    // Recompute weighted sums independently of amplitude scale and extrema;
    // the caller retains its original sample and tie ordering.
    void reset_sum() noexcept {sum_=total_=0;}
    void add_normalized(double sample,double weight) noexcept {sum_+=weight*sample;total_+=weight;}
    double amplitude_scale() const noexcept {return scale_;}
    double value(WeightedMixOperation operation,double scaled_squares=0) const noexcept {
        switch(operation){
        case WeightedMixOperation::Sum:return sum_;
        case WeightedMixOperation::Mean:return total_?sum_/total_:0;
        case WeightedMixOperation::MaximumAbsolute:return largest_;
        case WeightedMixOperation::Rms:return total_ && scale_?scale_*std::sqrt(scaled_squares/total_):0;
        }
        return 0;
    }
};
}
