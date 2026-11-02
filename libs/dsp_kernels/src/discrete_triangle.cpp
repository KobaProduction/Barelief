// TriangularFilter and band_bins math adapted from madmom audio/filters.py,
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/discrete_triangle.h>
#include <algorithm>
namespace dsp_kernels {
bool discrete_triangle_filter(std::size_t first,std::size_t center,std::size_t stop,
    bool normalize,std::span<float> weights,DiscreteTriangleGeometry& geometry) noexcept {
    if(first>stop || stop>weights.size() || center<first || center>stop || first>=weights.size())return false;
    if(stop-first<2){center=first;stop=first+1;}
    if(center>=stop)return false;
    std::fill(weights.begin(),weights.end(),0.f);float sum=0;
    for(std::size_t bin=first;bin<stop;++bin){
        const float weight=bin<center?float(bin-first)/float(center-first):1.f-float(bin-center)/float(stop-center);
        weights[bin]=weight;sum+=weight;
    }
    if(normalize && sum>0)for(auto& weight:weights.subspan(first,stop-first))weight/=sum;
    while(first<stop && weights[first]==0)++first;
    while(stop>first && weights[stop-1]==0)--stop;
    geometry={center,{first,stop-first}};return true;
}
}
