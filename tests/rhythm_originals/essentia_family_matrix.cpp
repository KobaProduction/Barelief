#include "source_fourier_profile.h"
#include "essentia_family_export.h"
#include <rhythm_reference_ports/beat_band_salience.h>
namespace r=rhythm_reference_ports;
namespace essentia_family_export {
namespace {
void beatogram(const Case& c,native_export::Output& out) {
    Matrix input{c.input,c.columns};r::PreparedBeatBandSalience operation;
    native_export::require(operation.prepare(c.rows,c.columns),"beatogram prepare");
    native_export::require(operation.process(input.source(),c.aux,c.columns),"beatogram process");
    for(std::size_t b=0;b<operation.bands();++b)out.row<float>("beatogram.f32",operation.band_values(b));
    out.values<std::uint64_t>("shape.u64",{operation.bands(),operation.beats()});
}
}
void matrix(const Case& c,const std::string& method) {
    native_export::Output out(c.prefix);
    if(method=="beatogram")beatogram(c,out);
    else throw std::runtime_error("unknown Essentia matrix primitive");
}
}
