#include "source_fourier_profile.h"
#include "essentia_family_export.h"
#include <rhythm_reference_ports/loop_length_confidence.h>
namespace r=rhythm_reference_ports;
namespace essentia_family_export {
namespace {
void loop_confidence(const Case& c,native_export::Output& out) {
    r::PreparedLoopLengthConfidence operation;r::LoopLengthConfidenceResult result;
    native_export::require(c.aux.size()==1 && operation.prepare(float(c.rate)),"loop confidence prepare");
    native_export::require(operation.process(c.input,c.aux[0],result),"loop confidence process");out.values<float>("confidence.f32",{result.confidence});
}
}

void pcm(const Case& c,const std::string& method) {
    native_export::Output out(c.prefix);
    if(method=="loop_confidence")return loop_confidence(c,out);
    throw std::runtime_error("unknown Essentia PCM primitive");
}
}
