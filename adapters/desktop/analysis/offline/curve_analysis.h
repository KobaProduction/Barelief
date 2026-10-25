#pragma once
#include <dsp_kernels/frequency_profile.h>
#include <memory>

namespace desktop_analysis::offline {
// Immutable, lightweight analysis inputs: no RMS groups or temporal previews.
struct CurveCorpus {
    dsp_kernels::ProfileCompatibility calibration;
    std::vector<dsp_kernels::CompositionStatistics> tracks;
};
struct CurveAnalysis {
    std::shared_ptr<const CurveCorpus> corpus;
    std::shared_ptr<const dsp_kernels::FrequencyMeasurements> measurements;
    dsp_kernels::FrequencyCurveOptions settings;
    dsp_kernels::FrequencyProfile polynomial,piecewise;
    std::string polynomial_error,piecewise_error,error;
    const dsp_kernels::FrequencyProfile& selected()const;
};
std::shared_ptr<const CurveCorpus> curve_corpus(const dsp_kernels::FrequencyProfile&,
    std::span<const std::uint8_t> included={});
CurveAnalysis analyze_frequency_curves(std::shared_ptr<const CurveCorpus>,
    const dsp_kernels::FrequencyCurveOptions&,std::shared_ptr<const CurveAnalysis> previous={});
}
