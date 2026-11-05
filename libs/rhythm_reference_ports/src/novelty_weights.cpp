// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia NoveltyCurve::weightCurve at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/novelty_weights.h>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace rhythm_reference_ports {
namespace {
float symmetric_weight(std::size_t i,std::size_t size,NoveltyWeight kind) noexcept {
    const auto half=size/2,position=std::min(i,size-1-i),distance=half-position;
    if((size&1) && i==half)
        return kind==NoveltyWeight::Triangle || kind==NoveltyWeight::InverseParabola?float(half):0;
    switch(kind){
    case NoveltyWeight::Triangle:return float(position+1);
    case NoveltyWeight::InverseTriangle:return float(distance);
    case NoveltyWeight::Parabola:return float(distance*distance);
    case NoveltyWeight::InverseParabola:return float(half*half-distance*distance+1);
    default:return 0;
    }
}
float weight(std::size_t i,std::size_t size,NoveltyWeight kind) noexcept {
    switch(kind){
    case NoveltyWeight::Flat:return 1;
    case NoveltyWeight::Linear:return float(i+1);
    case NoveltyWeight::Quadratic:return float(i*i+1);
    case NoveltyWeight::InverseQuadratic:return float(size*size-i*i);
    default:return symmetric_weight(i,size,kind);
    }
}
}
bool novelty_band_weights(std::span<float> output,NoveltyWeight kind,std::span<const float> supplied) noexcept {
    if(output.empty() || output.size()>46340 || kind<NoveltyWeight::Flat || kind>=NoveltyWeight::Hybrid)return false;
    if(kind==NoveltyWeight::Supplied){
        if(supplied.size()!=output.size() || !std::all_of(supplied.begin(),supplied.end(),[](float x){return std::isfinite(x);}))return false;
        std::copy(supplied.begin(),supplied.end(),output.begin());return true;
    }
    for(std::size_t i=0;i<output.size();++i)output[i]=weight(i,output.size(),kind);
    return true;
}
}
