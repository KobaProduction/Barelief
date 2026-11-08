// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"

namespace rhythm_graph_adapters::detail {
bool PcmStageOperation::apply_controls(const PcmStageControls& controls) noexcept {
    if(plan_.kind==PcmStageKind::Tracker)return true;
    auto& source=std::get<rhythm_reference_ports::PreparedContextBeatSequence>(kernel_);
    return (!controls.delay_signed || source.set_delay_samples(*controls.delay_signed)) &&
        (!controls.signature || source.set_tatum_signature(*controls.signature));
}
}
