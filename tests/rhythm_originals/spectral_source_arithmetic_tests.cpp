#include <rhythm_reference_ports/spectral_window.h>
#include <rhythm_reference_ports/erb_filter_bank.h>
#include <rhythm_reference_ports/spectral_onset.h>
#include <rhythm_reference_ports/reference_window.h>
#include <rhythm_reference_ports/onset_descriptor.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <dsp_kernels/window_math.h>
#include "erb_source_coefficients.h"
#include "phase_distance_source_golden.h"
#include "complex_odf_source_golden.h"
#include "aubio_window_source_golden.h"
#include "aubio_divergence_source_golden.h"
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <limits>
namespace {
void check(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"source spectral arithmetic: %s\n",message);std::exit(1);}
}
void hann(std::size_t size,const std::array<std::uint32_t,9>& expected) {
    rhythm_reference_ports::SpectralWindowConfig config;config.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    std::vector<float> window(size);check(rhythm_reference_ports::spectral_window(window,config),"prepare source Hann");
    const std::array<std::size_t,9> indices{0,1,2,7,63,size/2-1,size/2,size-2,size-1};
    for(std::size_t i=0;i<indices.size();++i)check(std::bit_cast<std::uint32_t>(window[indices[i]])==expected[i],"actual pinned Windowing Float32 coefficient");
}
void portable_default() {
    using namespace rhythm_reference_ports;
    std::array<float,512> implicit{},portable{},reference{};SpectralWindowConfig config;
    check(spectral_window(implicit),"default Float32 window");
    config.precision=dsp_kernels::NumericalPrecision::PortableFloat32;
    check(spectral_window(portable,config) && implicit==portable,"default is explicit PortableFloat32");
    config.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    check(spectral_window(reference,config) && implicit!=reference,"Double evaluation requires explicit opt in");
    config.precision=static_cast<dsp_kernels::NumericalPrecision>(2);std::size_t bytes=0;
    check(!spectral_window_storage_bytes(implicit.size(),config,bytes) && !spectral_window(implicit,config),"invalid precision rejected cold");
}
void aubio_hanningz(std::size_t samples) {
    std::vector<float> window(samples);
    check(rhythm_reference_ports::analysis_window(window),"source aubio HanningZ prepare");
    for(const auto& coefficient:aubio_window_source::coefficients)if(coefficient.samples==samples)
        check(std::bit_cast<std::uint32_t>(window[coefficient.index])==coefficient.bits,"actual original aubio mixed-cold HanningZ coefficient bits");
}
void portable_divergence(std::span<const float> magnitude,std::size_t method) {
    dsp_kernels::SpectralDescriptorConfig config;
    config.method=method?dsp_kernels::SpectralDescriptor::ModifiedKullbackLeibler:dsp_kernels::SpectralDescriptor::KullbackLeibler;
    config.epsilon=.1f;config.stable_log1p=false;config.compensated_sum=false;
    dsp_kernels::PreparedSpectralDescriptor operation;float value=0;
    check(operation.prepare(magnitude.size(),config) && operation.process_values(magnitude,value),"default Float32 divergence process");
    check(std::bit_cast<std::uint32_t>(value)==aubio_divergence_source::portable[method],"prior PortableFloat32 argument arithmetic remains bitwise");
}
void source_divergence() {
    std::array<float,513> magnitude{},zeros{};
    for(std::size_t i=0;i<magnitude.size();++i)magnitude[i]=std::bit_cast<float>(aubio_divergence_source::magnitude[i]);
    constexpr std::array methods{rhythm_reference_ports::OnsetRecipe::KullbackLeibler,rhythm_reference_ports::OnsetRecipe::ModifiedKullbackLeibler};
    for(std::size_t method=0;method<methods.size();++method){
        rhythm_reference_ports::PreparedOnsetDescriptor operation;float value=0;
        check(operation.prepare(magnitude.size(),methods[method]) && operation.process(zeros,zeros,value) && value==0,"source divergence zero history");
        check(operation.process(magnitude,zeros,value),"source divergence captured final input");
        check(std::bit_cast<std::uint32_t>(value)==aubio_divergence_source::source[method],"actual genuine KL/MKL Float32 LOG argument bits");
        portable_divergence(magnitude,method);
    }
}
void divergence_policy_guards() {
    using namespace dsp_kernels;SpectralDescriptorConfig config;config.method=SpectralDescriptor::ModifiedKullbackLeibler;
    config.stable_log1p=false;config.divergence_arithmetic=SpectralDivergenceArithmetic(99);
    PreparedSpectralDescriptor operation;check(!operation.prepare(1,config),"unknown divergence policy rejects cold");
    config.divergence_arithmetic=SpectralDivergenceArithmetic::Float64RatioFloat32Log;
    check(!operation.prepare(1,config),"explicit Double ratio needs positive epsilon");
    config.ratio_epsilon=-1;check(!operation.prepare(1,config),"negative reference epsilon rejects");
    config.ratio_epsilon=std::numeric_limits<double>::infinity();check(!operation.prepare(1,config),"nonfinite reference epsilon rejects");
    config.ratio_epsilon=std::numeric_limits<double>::denorm_min();
    check(operation.prepare(1,config),"finite positive Double epsilon admits cold");
    float value=71;const std::array input{std::numeric_limits<float>::max()};
    check(!operation.process_values(input,value) && value==71,"out-of-Float32 log argument rejects before cast/output");
}
void double_window_overflow() {
    std::array<float,1> output{123.f};
    const std::array<double,1> coefficients{2.*double(std::numeric_limits<float>::max())};
    check(!dsp_kernels::cosine_sum_window(output,coefficients,1.),"Double coefficient outside Float32 range rejected");
    check(output[0]==123.f,"out-of-range coefficient is rejected before Float32 store");
}
void erb_coefficients(std::size_t samples) {
    rhythm_reference_ports::PreparedErbFilterBank bank;std::size_t bound=0;
    check(bank.storage_bytes(samples/2+1,{},bound) && bank.prepare(samples/2+1),"prepare source ERB geometry");
    check(bank.workspace_bytes()<=bound,"ERB resource query includes unchanged Float32 payload");
    for(std::size_t i=0;i<erb_source_fixture::centers.size();++i)
        check(std::bit_cast<std::uint32_t>(bank.centers_hz()[i])==erb_source_fixture::centers[i],"actual original ERB center bits");
    for(const auto& coefficient:erb_source_fixture::coefficients)if(coefficient.samples==samples)
        check(std::bit_cast<std::uint32_t>(bank.weights()[coefficient.band*(samples/2+1)+coefficient.bin])==coefficient.bits,"actual original mixed-cold ERB coefficient bits");
}
void source_beat_phase_distance() {
    for(const auto& row:phase_distance_source::rows){
        rhythm_reference_ports::PreparedPolarNovelty operation;
        check(operation.prepare(1,rhythm_reference_ports::PolarNovelty::BeatEmphasis),"source distance prepare");
        for(std::size_t frame=0;frame<row.phase.size();++frame){
            const std::array magnitude{std::bit_cast<float>(row.magnitude[frame])},phase{std::bit_cast<float>(row.phase[frame])};
            std::array<float,1> distance{};
            check(operation.process(magnitude,phase,distance),"source phase distance process");
            check(std::bit_cast<std::uint32_t>(distance[0])==row.squared_distance[frame],"actual original phase-boundary/near-zero distance bits");
        }
    }
}
void source_complex_phase_distance() {
    rhythm_reference_ports::PreparedSpectralOnset operation;
    check(operation.prepare(33,{rhythm_reference_ports::SpectralOnsetMethod::Complex,44100}),"source Complex ODF prepare");
    for(std::size_t frame=0;frame<complex_odf_source::expected.size();++frame){
        std::array<float,33> magnitude{},phase{};
        for(std::size_t bin=0;bin<magnitude.size();++bin){
            magnitude[bin]=std::bit_cast<float>(complex_odf_source::magnitude[frame*magnitude.size()+bin]);
            phase[bin]=std::bit_cast<float>(complex_odf_source::phase[frame*phase.size()+bin]);
        }
        float value=0;check(operation.process(magnitude,phase,value),"source Complex ODF process");
        check(std::bit_cast<std::uint32_t>(value)==complex_odf_source::expected[frame],"actual original complete Complex ODF bits");
    }
}
}
int main() {
    portable_default();
    double_window_overflow();
    source_beat_phase_distance();source_complex_phase_distance();
    source_divergence();divergence_policy_guards();
    for(const std::size_t samples:std::array<std::size_t,5>{64,256,512,1024,4096}){erb_coefficients(samples);aubio_hanningz(samples);}
    // Actual original Essentia b9fa6cb6 Windowing(ones), symmetric normalized
    // Hann. Full fixtures/hash provenance: native/window-source-fixtures.json
    // in the immutable external pinned-original evidence cache.
    hann(512,{0x00000000,0x349ed72e,0x359ed5a5,0x377313da,0x3a925f83,0x3c003fcf,0x3c003fcf,0x349ed72e,0x00000000});
    hann(4096,{0x00000000,0x301e0780,0x311e077a,0x32f1fae8,0x361902a3,0x3a8007fd,0x3a8007fd,0x301e0780,0x00000000});
    std::puts("Original window/ERB/phase/divergence source arithmetic PASS");
}
