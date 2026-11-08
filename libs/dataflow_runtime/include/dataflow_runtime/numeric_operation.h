#pragma once
#include <domain/processing_contracts.h>
#include <signal_contracts/numeric_buffer.h>
#include <dsp_kernels/kernel_limits.h>
#include <iterator>
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace dataflow_runtime {
struct NumericOperationProfile;
namespace detail {struct NumericOperationBindingAccess;struct NumericOperationInvocation;struct NumericOperationClock;struct NumericOperationAuthority;struct NumericOperationWriterLease;}
class NumericOperationFactory;
// Explicit operation protocol, independent of packet length and transport clock.
enum class NumericOperationMode { IndependentBatch, ContinuingStream };
enum class NumericOperationEvent { Continue, BatchComplete, EndOfStream };
enum class NumericOperationFailure { PreserveState, ResetState };
enum class NumericInputUse { Frame, HeldValue, Control };
enum class NumericInputMatch { IndependentBatch, SessionSequence, ExactInterval };
enum class NumericInputSession { Unspecified, SharedProvenance };
struct NumericOperationInputRule {
    domain::PortId port=0;
    NumericInputUse use=NumericInputUse::Frame;
    bool required=true;
    signal_contracts::ValueDomain values=signal_contracts::ValueDomain::FiniteOnly;
    bool operator==(const NumericOperationInputRule&) const=default;
};
enum class NumericOperationClockKind { Unclocked, SampleRate, MatchInput };
struct NumericOperationClockRule {
    domain::PortDirection direction=domain::PortDirection::Input;
    domain::PortId port=0,source_port=0;
    NumericOperationClockKind kind=NumericOperationClockKind::SampleRate;
    double sample_rate=0;
    bool allow_unknown=true;
    bool operator==(const NumericOperationClockRule&) const=default;
};
enum class NumericOperationAxisKind { Index, Uniform, Declaration };
struct NumericOperationAxisRule {
    domain::PortDirection direction=domain::PortDirection::Input;
    domain::PortId port=0;
    std::size_t axis=0;
    NumericOperationAxisKind kind=NumericOperationAxisKind::Index;
    signal_contracts::NumericUnit unit=signal_contracts::NumericUnit::Values;
    double origin=0,step=1,tolerance=0;
    std::optional<signal_contracts::Ratio> exact_origin;
    std::optional<signal_contracts::Ratio> exact_step;
    bool allow_absent=true,allow_index=false,any_origin=false;
    bool allow_explicit=true,ignore_other_units=false,default_absent=true;
    bool require_increasing=false;
    std::optional<signal_contracts::Axis> tag;
    std::optional<signal_contracts::CoordinateMode> mode;
    bool operator==(const NumericOperationAxisRule&) const=default;
};
struct NumericOperationMeaningRule {
    domain::PortDirection direction=domain::PortDirection::Input;
    domain::PortId port=0;
    std::vector<signal_contracts::Quantity> quantities;
    std::vector<signal_contracts::NumericUnit> units;
    std::optional<domain::PortId> same_unit_as_input;
    bool operator==(const NumericOperationMeaningRule&) const=default;
};
struct NumericOperationCoordinateRoute {
    domain::PortId input=0,output=0;
    std::size_t input_axis=0,output_axis=0;
    bool operator==(const NumericOperationCoordinateRoute&) const=default;
};
struct NumericOperationAxisMatch {
    domain::PortDirection direction=domain::PortDirection::Input;
    domain::PortId port=0,source_port=0;
    std::size_t axis=0,source_axis=0;
    bool compare_values=true,compare_tags=true;
    bool operator==(const NumericOperationAxisMatch&) const=default;
};
struct NumericOperationPolicy {
    NumericOperationMode mode=NumericOperationMode::IndependentBatch;
    NumericInputMatch match=NumericInputMatch::IndependentBatch;
    NumericOperationFailure arithmetic_failure=NumericOperationFailure::ResetState;
    domain::PortId trigger=0;
    // Optional UInt8 scalar: 0=continue,1=batch complete,2=EOF,3=reset.
    // Reset delivers unavailable/discontinuous outputs even without a batch;
    // ordinary incomplete joins do not deliver a new output marker.
    // IndependentBatch declares a complete batch on every accepted trigger.
    std::optional<domain::PortId> control;
    // Explicit delivery-only stream label route, never exposed to the kernel.
    std::optional<domain::PortId> stream_input;
    std::vector<NumericOperationInputRule> inputs;
    // Empty means FiniteOnly for every output; otherwise one entry per port.
    // AnyIeee must be an explicit supported binding policy. Axes stay finite.
    std::vector<signal_contracts::ValueDomain> output_values;
    // Matched Frame ports explicitly share the session identity tuple
    // {producer_epoch,generation,epoch,graph_revision}. Stream IDs may differ.
    // ExactInterval additionally requires compatible declared clock domains,
    // tick durations and sample rates; transport clocks never supply DSP Fs.
    NumericInputSession session=NumericInputSession::Unspecified;
    // Runtime validates explicit requirements without revealing metadata values.
    std::vector<NumericOperationClockRule> clocks;
    std::vector<NumericOperationAxisRule> axes;
    std::vector<NumericOperationMeaningRule> meanings;
    std::vector<NumericOperationCoordinateRoute> coordinate_routes;
    std::vector<NumericOperationAxisMatch> axis_matches;
    bool operator==(const NumericOperationPolicy&) const=default;
};
// Cold factories receive only explicit settings and physical storage geometry.
struct NumericOperationPort {
    domain::PortId id=0;
    domain::PortDirection direction=domain::PortDirection::Input;
    signal_contracts::NumericBufferContract contract;
};
class NumericOperationPorts {
public:
    NumericOperationPorts()=default;
    NumericOperationPorts(std::span<const NumericOperationPort>) noexcept;
    NumericOperationPorts(const std::vector<NumericOperationPort>& ports) noexcept:NumericOperationPorts(std::span(ports)){}
    std::size_t size() const noexcept {return size_;}
    bool empty() const noexcept {return !size_;}
    NumericOperationPort operator[](std::size_t index) const noexcept {return read_(data_,index);}
    NumericOperationPort front() const noexcept {return (*this)[0];}
    struct iterator {
        using value_type=NumericOperationPort;
        using difference_type=std::ptrdiff_t;
        using iterator_category=std::input_iterator_tag;
        const NumericOperationPorts* owner=nullptr;std::size_t index=0;
        value_type operator*() const noexcept {return (*owner)[index];}
        iterator& operator++() noexcept {++index;return *this;}
        iterator operator++(int) noexcept {auto old=*this;++*this;return old;}
        bool operator==(const iterator&) const=default;
    };
    iterator begin() const noexcept {return {this,0};}
    iterator end() const noexcept {return {this,size_};}
private:
    const void* data_=nullptr;
    std::size_t size_=0;
    NumericOperationPort (*read_)(const void*,std::size_t) noexcept=nullptr;
    friend struct detail::NumericOperationBindingAccess;
};
struct NumericOperationNode;
struct NumericOperationNodeView {
    domain::NodeId id=-1;
    std::string_view kind;
    NumericOperationPorts inputs,outputs;
    const domain::ProcessingParameters* parameters=nullptr;
    bool enabled=true;
    NumericOperationNodeView()=default;
    NumericOperationNodeView(const NumericOperationNode&) noexcept;
};
struct NumericOperationNode {
    domain::NodeId id=-1;
    std::string kind;
    std::vector<NumericOperationPort> inputs,outputs;
    domain::ProcessingParameters parameters;
    bool enabled=true;
};
struct NumericOperationProfile {
    domain::ProcessingAdmissionLimits admission;
    domain::ProcessingResourceProfile resources;
    dsp_kernels::KernelLimits kernels;
    dsp_kernels::KernelCapabilities capabilities;
};
signal_contracts::Status resolve_numeric_operation_parameter(const NumericOperationNodeView&,
    std::string_view,domain::ProcessingParameterView&) noexcept;
class NumericOperationResources {
public:
    virtual ~NumericOperationResources()=default;
    virtual bool owned_bytes(std::size_t&) const noexcept=0;
};
struct NumericOperationDescriptor {
    NumericOperationNode node;
    NumericOperationPolicy policy;
    std::shared_ptr<const NumericOperationResources> immutable_resources;
    const NumericOperationFactory* factory_identity=nullptr;
};
signal_contracts::Status own_numeric_operation_descriptor(const NumericOperationNodeView&,
    NumericOperationPolicy,std::shared_ptr<const NumericOperationDescriptor>&,const NumericOperationFactory* =nullptr);
bool numeric_operation_descriptor_required_bytes(const NumericOperationNodeView&,
    const domain::ProcessingAdmissionLimits&,std::size_t&) noexcept;
bool numeric_operation_descriptor_owned_bytes(const NumericOperationDescriptor&,
    const domain::ProcessingAdmissionLimits&,std::size_t&) noexcept;
bool numeric_operation_add_bytes(std::size_t&,std::size_t) noexcept;
bool numeric_operation_mul_bytes(std::size_t,std::size_t,std::size_t&) noexcept;
enum class NumericOperationPublication { Frame, Point, Batch, DisabledFrame };
// Opaque owning provenance. It can be retained for a buffered result, but has
// no frame/clock/coordinate/numeric getter. No payload or graph is retained.
class NumericOperationOrigin {
public:
    NumericOperationOrigin()=default;
private:
    struct Identity {
        std::uint64_t sequence=0,source_sequence=0,generation=0,epoch=0,graph_revision=0,producer_epoch=0;
        std::int64_t timestamp=0,sample_begin=0,latency_numerator=0;
        std::uint64_t sample_count=0,latency_samples=0,latency_denominator=1;
        bool interval=false,discontinuity=false,frequency_dependent_latency=false;
    } identity_;
    std::shared_ptr<const detail::NumericOperationClock> clock_;
    std::shared_ptr<const detail::NumericOperationAuthority> authority_;
    std::uint64_t invocation_=0;
    std::size_t coordinate_group_=0;
    NumericOperationPublication publication_=NumericOperationPublication::Frame;
    friend struct detail::NumericOperationBindingAccess;
};
NumericOperationOrigin numeric_operation_origin(NumericOperationOrigin,NumericOperationPublication) noexcept;
bool numeric_operation_same_session(const NumericOperationOrigin&,const NumericOperationOrigin&) noexcept;
bool numeric_operation_same_step(const NumericOperationOrigin&,const NumericOperationOrigin&) noexcept;
bool numeric_operation_same_coordinates(const NumericOperationOrigin&,const NumericOperationOrigin&) noexcept;
class NumericOperationCoordinateWriter {
public:
    NumericOperationCoordinateWriter()=default;
    bool available(std::size_t axis) const noexcept {return axis<2 && (axes_&(1u<<axis));}
private:
    std::shared_ptr<const detail::NumericOperationWriterLease> lease_;
    std::uint64_t generation_=0;
    std::size_t output_=0;
    unsigned axes_=0;
    friend struct detail::NumericOperationBindingAccess;
};
struct NumericOperationInput {
    domain::PortId port=0;
    signal_contracts::NumericBufferView value;
    NumericOperationOrigin origin;
    signal_contracts::Status status=signal_contracts::Status::unavailable;
    std::span<const float> contiguous_float32;
};
struct NumericOperationOutput {
    domain::PortId port=0;
    signal_contracts::MutableNumericBufferView value;
    NumericOperationCoordinateWriter coordinates;
    NumericOperationOrigin origin;
    signal_contracts::Status status=signal_contracts::Status::unavailable;
};
struct NumericOperationControl {
    NumericOperationEvent event=NumericOperationEvent::Continue;
    bool reset=false,enabled=true;
};
signal_contracts::Status numeric_operation_coordinate(NumericOperationOutput&,std::size_t,std::size_t,double) noexcept;
signal_contracts::Status numeric_operation_axis(NumericOperationOutput&,std::size_t,std::size_t) noexcept;
class PreparedNumericOperation {
public:
    virtual ~PreparedNumericOperation()=default;
    virtual std::unique_ptr<PreparedNumericOperation> clone() const=0;
    virtual signal_contracts::Status copy_state_from(const PreparedNumericOperation&) noexcept=0;
    virtual void reset() noexcept=0;
    virtual bool owned_resources(domain::ProcessingResources&) const noexcept=0;
    // No allocation or retention of these borrowed buffers and capabilities.
    virtual signal_contracts::Status process(std::span<const NumericOperationInput>,
        std::span<NumericOperationOutput>,NumericOperationControl) noexcept=0;
};
class NumericOperationFactory {
public:
    virtual ~NumericOperationFactory()=default;
    virtual signal_contracts::Status describe(const NumericOperationNodeView&,
        std::shared_ptr<const NumericOperationDescriptor>&) const=0;
    virtual domain::ProcessingSupport assess(const NumericOperationNodeView&,
        const NumericOperationProfile&) const noexcept=0;
    virtual signal_contracts::Status prepare(std::shared_ptr<const NumericOperationDescriptor>,
        const NumericOperationProfile&,std::unique_ptr<PreparedNumericOperation>&) const=0;
};
} // namespace dataflow_runtime
