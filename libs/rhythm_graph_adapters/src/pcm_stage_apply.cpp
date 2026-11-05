// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
namespace {
bool onset_controls(PreparedPcmOnsetSequence& source,const PcmStageControls& controls) noexcept {
    return (!controls.profile || source.apply_default_parameters(OnsetRecipe(*controls.profile))) &&
        (!controls.threshold || source.set_threshold(*controls.threshold)) && (!controls.silence || source.set_silence(*controls.silence)) &&
        (!controls.compression || source.set_compression(*controls.compression)) && (!controls.whiten || source.set_whitening(*controls.whiten)) &&
        (!controls.delay_unsigned || source.set_delay_samples(*controls.delay_unsigned)) &&
        (!controls.minimum || source.set_minimum_interval_samples(*controls.minimum));
}
bool tempo_controls(PreparedPcmContextTempo& source,const PcmStageControls& controls) noexcept {
    return (!controls.threshold || source.set_threshold(*controls.threshold)) && (!controls.silence || source.set_silence(*controls.silence)) &&
        (!controls.delay_signed || source.set_delay_samples(*controls.delay_signed)) && (!controls.signature || source.set_tatum_signature(*controls.signature));
}
bool sequence_controls(PreparedContextBeatSequence& source,const PcmStageControls& controls) noexcept {
    return (!controls.delay_signed || source.set_delay_samples(*controls.delay_signed)) &&
        (!controls.signature || source.set_tatum_signature(*controls.signature));
}
}
bool PcmStageOperation::apply_controls(const PcmStageControls& controls) noexcept {
    switch(plan_.kind){
    case PcmStageKind::Onset:return onset_controls(std::get<PreparedPcmOnsetSequence>(kernel_),controls);
    case PcmStageKind::Tempo:return tempo_controls(std::get<PreparedPcmContextTempo>(kernel_),controls);
    case PcmStageKind::Sequence:return sequence_controls(std::get<PreparedContextBeatSequence>(kernel_),controls);
    case PcmStageKind::Tracker:return true;
    case PcmStageKind::Spectrum:{
        auto& source=std::get<PreparedHopSpectrum>(kernel_);
        return (!controls.window || source.set_window(AnalysisWindowShape(*controls.window))) &&
            (controls.coefficients.empty() || source.set_window_coefficients(controls.coefficients));
    }
    }
    return false;
}
}
