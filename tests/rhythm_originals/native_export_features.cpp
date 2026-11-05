#include "native_export_io.h"
#include <rhythm_reference_ports/onset_descriptor.h>
#include <array>
namespace native_export {
namespace {
namespace r=rhythm_reference_ports;
constexpr std::array methods={r::OnsetRecipe::Energy,r::OnsetRecipe::FrequencyContent,r::OnsetRecipe::Complex,
    r::OnsetRecipe::Phase,r::OnsetRecipe::WeightedPhase,r::OnsetRecipe::SpectralDifference,
    r::OnsetRecipe::KullbackLeibler,r::OnsetRecipe::ModifiedKullbackLeibler,r::OnsetRecipe::SpectralFlux};
}
void aubio_features(const Case& c) {
    const std::size_t bins=c.frame/2+1;
    require(c.pcm.size()%(2*bins)==0,"polar input rows [frame][magnitude then phase]");
    std::array<r::PreparedOnsetDescriptor,9> descriptor;
    for(std::size_t i=0;i<methods.size();++i)require(descriptor[i].prepare(bins,methods[i]),"shared-polar descriptor prepare");
    Output output(c.prefix);const auto input=std::span(c.pcm);
    for(std::size_t position=0;position<input.size();position+=2*bins){
        const auto magnitude=input.subspan(position,bins),phase=input.subspan(position+bins,bins);
        for(auto& stage:descriptor){
            float value=0;require(stage.process(magnitude,phase,value),"shared-polar descriptor process");
            output.values<float>("odf.f32",{value});
        }
    }
}
}
