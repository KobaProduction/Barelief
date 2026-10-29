#pragma once
#include <dsp_kernels/frequency_profile.h>
#include <memory>

namespace desktop_analysis::offline {
// Immutable, lightweight analysis inputs: no RMS groups or temporal previews.
struct CurveCorpus {
    dsp_kernels::ProfileCompatibility calibration;
    std::vector<dsp_kernels::CompositionStatistics> tracks;
    // A ready .brprofile stores aggregate measured arrays but not per-track data.
    // These measurements allow re-fitting; aggregation itself cannot be changed.
    std::shared_ptr<const dsp_kernels::FrequencyMeasurements> saved_measurements;
    dsp_kernels::FrequencyCurveOptions saved_options;
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
// Large scan summaries retain frequency-aligned mean track maxima, not individual
// track statistics. Only their original aggregation can be refitted.
std::shared_ptr<const CurveCorpus> curve_corpus_summary(
    const dsp_kernels::FrequencyProfile&,std::span<const double> contributors);
CurveAnalysis analyze_frequency_curves(std::shared_ptr<const CurveCorpus>,
    const dsp_kernels::FrequencyCurveOptions&,std::shared_ptr<const CurveAnalysis> previous={});
}
