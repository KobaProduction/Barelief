#pragma once
#include <dataflow_runtime/runtime.h>
#include <dsp_kernels/dsp.h>
#include <dsp_kernels/prepared_resampler.h>
#include <dsp_kernels/numeric_convert.h>
#include <dsp_kernels/numeric_reduce.h>
#include <limits>
namespace dataflow_runtime::detail {
using namespace domain;
using namespace signal_contracts;
inline constexpr std::size_t absent=std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t fixed_plan_reservation=4096;
struct GatherOwner {
    std::vector<std::byte> metadata;
    BufferSlot slot;
    BufferPool pool;
    PreparedNumericGather gather;
    explicit GatherOwner(std::size_t size):metadata(size),pool(metadata,{&slot,1},size){}
};
// Immutable cold-preparation provenance. It never supplies processing values.
// Neutral unary operations share their input token; only local domain
// boundaries and ordered multi-input contributions allocate new tokens.
struct ContentLineage;
using ContentToken=std::shared_ptr<const ContentLineage>;
struct ContentContribution {
    PortId input=0;
    ContentToken token;
    bool operator==(const ContentContribution&) const=default;
};
struct ContentLineage {
    NodeId boundary=-1;
    PortId output=0;
    std::vector<ContentContribution> inputs;
};
struct PlanNode {
    struct ScalarInput {DataContract contract;std::size_t parent=absent,clock=absent;PortId port=0,parent_port=0;bool clock_known=false;};
    struct EndpointPlan {std::vector<ScalarInput> inputs;std::array<std::size_t,3> indices{};std::vector<double> weights;double weight_scale=0;};
    std::unique_ptr<EndpointPlan> endpoint_plan;
    std::size_t parent = absent, clock = absent;
    std::size_t product_frame_input=0;
    struct {NodeId id=-1;NodeKind kind=NodeKind::NetworkInput;bool enabled=true;} node;
    std::uint16_t parent_port=0,selected_input=0;
    // Internal flags only; this object is never a serialized wire layout.
    bool reset:1 = true;
    bool clock_reset:1 = true;
    bool selector_changed:1=false;
    bool internal_conversion:1=false;
    bool identity_bypass:1=false;
    struct SpectrumPeakSettings {double bin_hz=1;std::uint32_t first_bin=0;};
    SpectrumPeakSettings spectrum_peak;

    bool scalar_view() const noexcept {return node.kind==NodeKind::ScalarView && endpoint_plan!=nullptr;}
    std::vector<std::size_t> children;
    struct MixerDestination {std::size_t node=0,input=0;PortId output=0;};
    std::vector<MixerDestination> mixer_destinations;
    // Prepared mono lanes of a grouped selector. Selection changes as one graph
    // revision; each lane forwards synchronously, without a second audio buffer.
    std::vector<MixerDestination> selector_destinations;
    ProcessingOperation operation=ProcessingOperation::Source;
    std::size_t configured=absent;
    std::size_t numeric_clock_parent=absent;
    DataContract numeric_input;
    std::vector<DataContract> numeric_outputs;
    std::vector<ContentToken> output_content;
    std::unique_ptr<dsp_kernels::PreparedConversion> conversion;
    std::unique_ptr<dsp_kernels::PreparedConversion> row_conversion;
    std::unique_ptr<dsp_kernels::PreparedReduction> reduction;
    std::shared_ptr<const GatherOwner> gather;
    std::shared_ptr<const GatherOwner> input_gather;
    std::size_t matrix_row=0;
    unsigned center_method=0;
    double center_offset=0;
    float gain=1;
    float wled_peak_hz=0,wled_magnitude=0;
    std::uint64_t constant_bits=0; // Cold materialized in the declared scalar dtype.
    bool half_last=false;
    unsigned merge_mode=2;
    // Shared numeric scale for FrequencyStep and WindowGain, not a dB reference.
    double spectrum_reference=1;
    dsp_kernels::WindowFunction window_function=dsp_kernels::WindowFunction::Hann;
    std::shared_ptr<const dsp_kernels::PreparedWindows> windows;
    std::size_t hop_base=0;
    std::uint64_t hop_fraction=0,hop_denominator=1;
    double comparison_threshold=1e-12;
    dsp_kernels::ResamplerMode resampler_mode=dsp_kernels::ResamplerMode::ExistingFir;
    unsigned resampler_quality=5;
};
std::size_t index(std::span<const PlanNode>,NodeId) noexcept;
}
