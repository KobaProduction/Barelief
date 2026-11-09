// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/pulse_train_analysis.h>
#include <dsp_kernels/prepared_storage.h>
#include <bit>
#include <cmath>
#include <limits>
#include <cstdint>
namespace rhythm_reference_ports {
using namespace dsp_kernels;
bool PreparedPulseTrainScoring::storage_bytes(std::size_t lag,std::size_t candidates,std::size_t& out) noexcept {
    if(!lag || lag>SIZE_MAX/12 || !candidates)return false;
    std::size_t bytes=0;
    if(!prepared_storage::add_array(bytes,lag,sizeof(float)) || !prepared_storage::add_array(bytes,candidates,3*sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedPulseTrainScoring::workspace_bytes() const noexcept {
    return (phases_.capacity()+magnitudes_.capacity()+variances_.capacity()+scores_.capacity())*sizeof(float);
}
}
