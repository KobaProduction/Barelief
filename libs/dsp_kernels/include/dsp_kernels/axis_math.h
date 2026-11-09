#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dsp_kernels {
enum class AxisSumAccumulation {Sequential=0,Pairwise=1};
// Pure bounded array operations shared by prepared graphs and offline clients.
// Empty maximum has no index. Strict comparison retains the first tied index.
template<typename Scalar> bool first_maximum_index(std::span<const Scalar> input,std::size_t& index) noexcept {
    if(input.empty() || !std::all_of(input.begin(),input.end(),[](Scalar value){return std::isfinite(value);}))return false;
    index=std::size_t(std::max_element(input.begin(),input.end())-input.begin());return true;
}
namespace detail {
// NumPy v1.26.4 loops_utils.h.src pairwise reduction order, shared by
// Float32 and Double. NumPy Developers, BSD-3-Clause: packaging/licenses/numpy.txt.
template<typename Scalar> Scalar pairwise_sum_tail(std::span<const Scalar> values,std::size_t first,Scalar sum) noexcept {
    for(std::size_t i=first;i<values.size();++i)sum+=values[i];
    return sum;
}
template<typename Scalar> Scalar pairwise_sum_block(std::span<const Scalar> values) noexcept {
    std::array<Scalar,8> lanes{};std::copy_n(values.begin(),lanes.size(),lanes.begin());
    const auto end=values.size()-values.size()%lanes.size();
    for(std::size_t i=lanes.size();i<end;i+=lanes.size())
        for(std::size_t lane=0;lane<lanes.size();++lane)lanes[lane]+=values[i+lane];
    const Scalar sum=((lanes[0]+lanes[1])+(lanes[2]+lanes[3]))+((lanes[4]+lanes[5])+(lanes[6]+lanes[7]));
    return pairwise_sum_tail(values,end,sum);
}
template<typename Scalar> Scalar pairwise_sum(std::span<const Scalar> values) noexcept {
    if(values.size()<8)return pairwise_sum_tail(values,0,-Scalar{0});
    if(values.size()<=128)return pairwise_sum_block(values);
    auto split=values.size()/2;split-=split%8;
    return pairwise_sum(values.first(split))+pairwise_sum(values.subspan(split));
}
template<typename Scalar> bool axis_sum_step(Scalar value,Scalar& sum) noexcept {
    if(!std::isfinite(value))return false;
    sum+=value;return std::isfinite(sum);
}
}
template<typename Scalar> bool sequential_axis_sum(std::span<const Scalar> input,std::span<Scalar> output,bool cumulative) noexcept {
    if(output.size()!=(cumulative?input.size():1))return false;
    Scalar sum=0;
    for(std::size_t i=0;i<input.size();++i){
        if(!detail::axis_sum_step(input[i],sum))return false;
        if(cumulative)output[i]=sum;
    }
    if (!cumulative) { output[0] = sum; }
    return true;
}
// Pairwise is an explicit reduction policy, independent of storage layout.
// Cumulative sums retain their sequential definition. Recursion depth is
// bounded by the bit width of size_t; only fixed-size lane arrays use stack.
template<typename Scalar> bool axis_sum(std::span<const Scalar> input,std::span<Scalar> output,bool cumulative,
    AxisSumAccumulation accumulation=AxisSumAccumulation::Sequential) noexcept {
    if(accumulation==AxisSumAccumulation::Sequential)return sequential_axis_sum(input,output,cumulative);
    if(accumulation!=AxisSumAccumulation::Pairwise || cumulative || output.size()!=1 ||
       !std::all_of(input.begin(),input.end(),[](Scalar value){return std::isfinite(value);}))return false;
    const Scalar sum=Scalar{0}+detail::pairwise_sum(input);
    if(!std::isfinite(sum))return false;
    output[0]=sum;return true;
}
// Both endpoints compare against the opposite endpoint. Equal peak heights
// are ordered by descending index. Caller supplies one index slot per sample.
template<typename Scalar> bool ordered_wrapped_peaks(std::span<const Scalar> input,std::span<std::uint64_t> indices,std::size_t& count) noexcept {
    count=0;
    if(indices.size()<input.size() || !std::all_of(input.begin(),input.end(),[](Scalar value){return std::isfinite(value);}))return false;
    for(std::size_t i=0;i<input.size();++i)
        if(input[i]>input[(i+input.size()-1)%input.size()] && input[i]>input[(i+1)%input.size()])indices[count++]=i;
    std::sort(indices.begin(),indices.begin()+std::ptrdiff_t(count),[input](auto a,auto b){return input[a]==input[b]?a>b:input[a]>input[b];});
    return true;
}
}
