#pragma once
#include <dsp_kernels/dsp.h>
#include <QStringList>

namespace presentation {
inline constexpr const char* spectrum_compensation_caption="FFT and window compensation";
inline const char* spectrum_projection_caption(dsp_kernels::SpectrumProjection value) {
    using enum dsp_kernels::SpectrumProjection;
    switch(value){case Real:return "Real";case Imaginary:return "Imaginary";case Phase:return "Phase, radians";
    case Magnitude:return "FFT magnitude: raw or window-scaled";case PeakAmplitude:return "One-sided peak amplitude";
    case Power:return "Power spectrum (spectrum scaling)";case Psd:return "Power spectral density (PSD)";
    case RmsAmplitude:return "One-sided RMS amplitude";
    case BinPower:return "Bin power (PSD x frequency step)";
    case WindowScaledMagnitude:return "Window-scaled magnitude";}return "";
}
inline void amplitude_spectrum_choices(QStringList& keys,QStringList& captions) {
    for(const auto value:dsp_kernels::amplitude_spectrum_projections()){
        keys.push_back(QString::fromUtf8(dsp_kernels::spectrum_projection_name(value)));
        captions.push_back(QString::fromUtf8(spectrum_projection_caption(value)));
    }
}
}
