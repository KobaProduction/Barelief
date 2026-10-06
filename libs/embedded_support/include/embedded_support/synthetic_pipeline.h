#pragma once
#include <embedded_support/profile.h>
#include <embedded_support/session.h>
#include <dataflow_runtime/runtime.h>
#include <array>
namespace embedded_support {
// Example composition of the public executor, not another DSP implementation.
// prepare allocates; tick and its callback must be serialized and allocation-free.
class SyntheticPipeline {
public:
    Status prepare(const Profile &, std::string_view stream_id);
    Status tick(FrameHandler, void *) noexcept;
    // Device extension: host owns borrowed planar PCM until this call returns.
    // Explicitly chosen by firmware; unavailable capture never falls back to tones.
    Status process_pcm(const signal_contracts::FrameView &, FrameHandler, void *) noexcept;
    application::MemoryRequirements memory_requirements() const noexcept;
private:
    Profile profile_;
    std::string id_;
    std::shared_ptr<const dataflow_runtime::PreparedGraph> plan_;
    std::unique_ptr<dataflow_runtime::RuntimeContext> context_;
    std::vector<float> left_,right_;
    std::array<double,8> phase_{};
    std::uint64_t samples_=0,sequence_=0;
    domain::NodeId output_=1;
};
}
