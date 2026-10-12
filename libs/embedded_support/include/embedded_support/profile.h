#pragma once
#include <application/graph_lifecycle.h>
#include <dataflow_runtime/runtime.h>
#include <bdsp_protocol/numeric_control_limits.h>
#include <bdsp_protocol/numeric_data_limits.h>
namespace embedded_support {
enum class Link { tcp, uart_8n1 };
// Admission limits, not measurements of the chip's real heap/CPU/network capacity.
struct Profile {
    Link link = Link::tcp;
    std::size_t memory_bytes = 256 * 1024, adapter_bytes = 32 * 1024;
    std::size_t max_frame_bytes = 4096, queue_frames = 2;
    // Must match or conservatively undercut the negotiated shared packet payload.
    std::size_t packet_payload_capacity = 4096;
    std::uint32_t sample_rate = 8000, block_samples = 256, fft_size = 2048, mel_bands = 8;
    std::uint32_t uart_baud = 2000000, tcp_bytes_per_second = 200000;
    std::uint32_t utilization_percent = 80;
    signal_contracts::Ratio execution_interval{1,120};
    bool spectral = false; // raw PCM first; history is always OFF in this profile.
    std::uint32_t channels = 1;
    // Explicit reservation ceilings, not chip measurements. Existing kernel
    // reference stack reservation requires a host-selected task stack budget.
    std::size_t task_stack_bytes = 32768;
    std::size_t control_bytes = 0;
};
struct Admission {
    signal_contracts::Status status = signal_contracts::Status::invalid_argument;
    std::size_t frame_bytes = 0, wire_bytes = 0, reserved_bytes = 0;
    std::uint64_t required_bytes_per_second = 0, allowed_bytes_per_second = 0;
};
Admission admit(const Profile &, std::size_t stream_id_bytes,
                application::MemoryRequirements runtime = {}) noexcept;
// Link admission for the local paced source: exactly one AudioSource (node 0),
// no network ingress. Sum every sink, including typed silence from disabled
// nodes. Window-derived outputs are bounded at max(analysis cadence, source block rate).
// Composition checks this before Prepare and again with negotiated packet size.
Admission graph_link_admission(const Profile&,const domain::NumericProcessingGraph&,
    std::size_t packet_payload_capacity) noexcept;
// Caller owns returned profile for the complete lifetime of capability queries.
dataflow_runtime::NumericRuntimeProfile numeric_runtime_profile(const Profile&) noexcept;
bdsp_protocol::numeric_control::Limits numeric_control_limits(const Profile&) noexcept;
bdsp_protocol::numeric_data::Limits numeric_data_limits(const Profile&) noexcept;
struct DeviceProfile {
    Profile transport;
    dataflow_runtime::NumericRuntimeProfile runtime;
    bdsp_protocol::numeric_data::Limits data;
};
DeviceProfile device_profile(const Profile&) noexcept;
// Adds actual wire endpoint admission to the executor's kernel/resource query.
// DeviceProfile and its immutable nested storage outlive every borrowed query.
domain::NumericDeviceCapabilities numeric_capabilities(const DeviceProfile&) noexcept;
}
