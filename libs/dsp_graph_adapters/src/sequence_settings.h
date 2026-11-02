#pragma once
#include "operation_support.h"
#include <dsp_kernels/pcm_onset_envelope.h>
#include <dsp_kernels/envelope_tempo_ratios.h>
#include <dsp_kernels/pcm_beat_tracking.h>
#include <dsp_kernels/crf_beat_sequence.h>

namespace dsp_graph_adapters::detail {
bool read_tempo_settings(const domain::ResolvedProcessingNode&,dsp_kernels::TempoEstimatorConfig&) noexcept;
bool read_envelope_settings(const domain::ResolvedProcessingNode&,dsp_kernels::EnvelopeTempoConfig&) noexcept;
bool read_pcm_settings(const domain::ResolvedProcessingNode&,dsp_kernels::PcmOnsetStrengthConfig&,bool median=false) noexcept;
bool read_ratio_settings(const domain::ResolvedProcessingNode&,dsp_kernels::TempoRatioSequenceConfig&,
    dsp_kernels::FeatureReduction default_reduction=dsp_kernels::FeatureReduction::None) noexcept;
bool read_envelope_ratio_settings(const domain::ResolvedProcessingNode&,dsp_kernels::EnvelopeTempoRatiosConfig&) noexcept;
bool read_tracking_settings(const domain::ResolvedProcessingNode&,dsp_kernels::EnvelopeBeatTrackingConfig&) noexcept;
bool read_pulse_settings(const domain::ResolvedProcessingNode&,dsp_kernels::EnvelopeLocalPulseConfig&) noexcept;
bool read_histogram_settings(const domain::ResolvedProcessingNode&,dsp_kernels::TempoHistogramSequenceConfig&) noexcept;
bool read_analysis_settings(const domain::ResolvedProcessingNode&,dsp_kernels::TempoAnalysisSequenceConfig&) noexcept;
PreparedSequence prepare_tempo_ratio(const domain::ConfiguredProcessingNode&);
domain::ProcessingSupport assess_tempo_ratio(const domain::ResolvedProcessingNode&,const dataflow_runtime::NumericRuntimeProfile&) noexcept;
PreparedSequence prepare_pcm(const domain::ConfiguredProcessingNode&);
domain::ProcessingSupport assess_pcm(const domain::ResolvedProcessingNode&,const dataflow_runtime::NumericRuntimeProfile&) noexcept;
}
