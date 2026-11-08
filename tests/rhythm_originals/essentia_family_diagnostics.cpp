#include "source_fourier_profile.h"
#include "essentia_family_export.h"
#include <rhythm_reference_ports/pulse_train_analysis.h>
#include <array>
#include <limits>
namespace r=rhythm_reference_ports;
namespace essentia_family_export {
namespace {
void pulse(const Case& c,native_export::Output& out) {
    r::PreparedPulseTrainScoring operation;float lag=0;
    native_export::require(operation.prepare(c.input.size(),std::max(std::size_t{1},c.aux.size())),"pulse train prepare");
    native_export::require(operation.process(c.input,c.aux,lag),"pulse train process");out.values<float>("lag.f32",{lag});
    out.row<float>("magnitudes.f32",operation.magnitudes());out.row<float>("variances.f32",operation.variances());out.row<float>("scores.f32",operation.scores());
}
}
void diagnostics(const Case& c,const std::string& method) {
    native_export::Output out(c.prefix);
    if(method=="pulse_train")pulse(c,out);else throw std::runtime_error("unknown Essentia diagnostic primitive");
}
}
