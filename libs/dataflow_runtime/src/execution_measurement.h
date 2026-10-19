#pragma once
#include "node_state.h"
#include <algorithm>
namespace dataflow_runtime::detail {
inline std::uint64_t measurement_now(const NumericRuntimeProfile& profile) noexcept {
    return profile.monotonic_nanoseconds?profile.monotonic_nanoseconds(profile.measurement_clock):std::uint64_t{0};
}
class ExecutionMeasurement {
    NodeStats& stats;
    const NumericRuntimeProfile& profile;
    const std::uint64_t started;
    std::uint64_t& children;
    const double deadline;
    const std::uint64_t extra;
    void record(std::uint64_t self) noexcept {
        stats.self_nanoseconds+=self;stats.max_self_nanoseconds=std::max(stats.max_self_nanoseconds,self);
        if(self>deadline)++stats.deadline_misses;
        std::size_t bin=0;
        for(auto value=self/1000;value>1 && bin<15;value>>=1)++bin;
        ++stats.self_histogram[bin];
    }
public:
    ExecutionMeasurement(NodeStats& counters,const NumericRuntimeProfile& runtime,Ratio interval,std::uint64_t& child_time,std::uint64_t prior) noexcept
        :stats(counters),profile(runtime),started(measurement_now(profile)),children(child_time),deadline(1e9*double(interval.numerator)/double(interval.denominator)),extra(prior){}
    ~ExecutionMeasurement(){
        if(!profile.monotonic_nanoseconds)return;
        const auto end=measurement_now(profile);const auto elapsed=end>=started?end-started:0;
        record((elapsed>=children?elapsed-children:0)+extra);
    }
};
}
