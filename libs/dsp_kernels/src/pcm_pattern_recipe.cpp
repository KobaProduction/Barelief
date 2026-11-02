// Source composition from madmom bin/GMMPatternTracker at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/pcm_pattern_recipe.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace dsp_kernels {
bool PreparedPcmPatternRecipe::prepare(std::size_t maximum,std::span<const GmmPatternData> data,
                                      std::span<const PatternTempoParameters> tempo) {
    std::size_t bound=0;if(!storage_bytes(maximum,data,tempo,bound))return false;
    PreparedPcmPatternRecipe next;
    if(!next.features_.prepare(maximum) || !next.decoder_.prepare(next.features_.frames(maximum),data,tempo,50) ||
       next.features_.features()!=next.decoder_.feature_count())return false;
    next.maximum_pcm_=maximum;if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmPatternRecipe::read(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& input=*static_cast<const Input*>(context);
    return input.self->features_.read_frame(input.pcm,frame,output);
}
bool PreparedPcmPatternRecipe::process(std::span<const float> pcm) noexcept {
    features_.reset();
    if(!maximum_pcm_ || pcm.size()>maximum_pcm_ || !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    const Input input{this,pcm};
    return decoder_.process({&input,read},features_.frames(pcm.size()));
}
}

namespace dsp_kernels {
bool PreparedPcmPatternRecipe::storage_bytes(std::size_t maximum,std::span<const GmmPatternData> data,
    std::span<const PatternTempoParameters> tempo,std::size_t& out) noexcept {
    std::size_t frames=0,features=0,bytes=0,nested=0;
    if(!PreparedPatternFeatureRecipe::geometry(maximum,frames,features) || !PreparedPatternFeatureRecipe::storage_bytes(maximum,bytes) ||
       !PreparedPatternStateDecoder::storage_bytes(frames,data,tempo,50,nested) || !prepared_storage::add_bytes(bytes,nested))return false;
    for(const auto& pattern:data)for(const auto& g:pattern.gmms)if(g.features!=features)return false;
    out=bytes;return true;
}
}
