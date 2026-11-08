// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/graph/processing_model.h>

namespace rhythm_graph_adapters {
namespace descriptor_ports {
inline constexpr domain::PortId magnitude=0, phase=1;
inline constexpr domain::PortId value=0, histogram_counts=1, histogram_values=2;
}
namespace peak_picker_ports {
inline constexpr domain::PortId smoothed_history=0;
inline constexpr domain::PortId position=0, detected=1, thresholded=2, mean=3, statistic=4;
}
namespace silence_ports {
inline constexpr domain::PortId pcm=0, silent=0;
}
namespace onset_decision_ports {
inline constexpr domain::PortId peak_position=0, silence=1;
inline constexpr domain::PortId accepted_position=0, last_sample=1, detected=2;
}
namespace whitening_ports {
inline constexpr domain::PortId magnitude=0, whitened=0, peaks=1;
inline constexpr domain::PortId floor_control=12, relax_time_control=13;
}
namespace context_sequence_ports {
inline constexpr domain::PortId history_input=0, silence=1, reversed_weighted_history=2, autocorrelation_input=3;
inline constexpr domain::PortId scheduled=0, silent=1, tactus=2, period_samples=3;
inline constexpr domain::PortId bpm=4, confidence=5, beat_sample=6, tatum_sample=7, tatum_event=8;
inline constexpr domain::PortId onset_history=9, autocorrelation=10, comb_response=11, phase_response=12, offsets=13;
inline constexpr domain::PortId delay_control=14, signature_control=15, poll_control=16;
}
namespace context_tracker_ports {
inline constexpr domain::PortId reversed_weighted_history=0, autocorrelation_input=1;
inline constexpr domain::PortId offsets=0, period_frames=1, bpm=2, confidence=3, meter=4;
inline constexpr domain::PortId autocorrelation=5, comb_response=6, phase_response=7;
}
}
