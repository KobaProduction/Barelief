#pragma once
#include <rhythm_reference_ports/pcm_predictive_onset.h>
#include <kiss_backend.h>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
namespace source_fourier_profile {
inline bool btrack_reference() {
    const char* value=std::getenv("BARELIEF_BTRACK_SOURCE_FRONTEND");
    if(!value || !*value)return false;
    if(std::string_view(value)!="desktop-reference")throw std::runtime_error("unknown explicit BTrack frontend profile");
    return true;
}
inline const dsp_kernels::FourierBackendFactory* btrack_factory() {
    return btrack_reference()?&desktop_fourier::kiss_backend_factory():nullptr;
}
inline rhythm_reference_ports::PcmPredictiveOnsetConfig btrack_config(rhythm_reference_ports::PcmPredictiveOnsetConfig config) {
    if(btrack_reference())config.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    return config;
}
}
