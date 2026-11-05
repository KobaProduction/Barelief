#pragma once
#include "python_export.h"
#include <cmath>

namespace originals {
struct ReplayPartition {std::size_t first=0,count=0;bool reset=false;};
inline std::vector<ReplayPartition> replay_partitions(std::span<const float> chunks,std::span<const float> resets,std::size_t frames) {
    require(chunks.size()==resets.size() && !chunks.empty() && resets.front()!=0,"reset-aware explicit replay partitions");
    std::vector<ReplayPartition> output;std::size_t first=0;
    for(std::size_t i=0;i<chunks.size();++i){
        require(std::isfinite(chunks[i]) && chunks[i]>=1 && double(chunks[i])<=double(frames),"finite replay partition before integer conversion");
        const auto count=std::size_t(chunks[i]);require(count && chunks[i]==float(count) && first<=frames && count<=frames-first,"explicit replay partition range");
        output.push_back({first,count,resets[i]!=0});first+=count;
    }
    require(first==frames,"complete replay input");return output;
}
template<typename Process> bool replay(std::span<const float> input,std::span<const ReplayPartition> partitions,Process&& process) {
    for(const auto& part:partitions)if(!process(input.subspan(part.first,part.count),part.reset))return false;
    return true;
}
}
