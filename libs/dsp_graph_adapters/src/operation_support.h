#pragma once
#include <dataflow_runtime/numeric_operation.h>
#include <dsp_graph_adapters/model_provider.h>
#include <domain/numeric/sequence_operations.h>
#include <dsp_kernels/numeric_rows.h>
#include <dsp_kernels/prepared_storage.h>
#include <string_view>

namespace dsp_graph_adapters::detail {
using dataflow_runtime::NumericOperationInput;
using dataflow_runtime::NumericOperationOutput;
using signal_contracts::Status;
dataflow_runtime::NumericOperationNodeView definition(const dataflow_runtime::NumericOperationNode&) noexcept;
const domain::ProcessingParameterValue* parameter(const dataflow_runtime::NumericOperationNodeView&,std::string_view) noexcept;
bool number(const dataflow_runtime::NumericOperationNodeView&,std::string_view,float,float&) noexcept;
bool integer(const dataflow_runtime::NumericOperationNodeView&,std::string_view,std::size_t,std::size_t&) noexcept;
bool flag(const dataflow_runtime::NumericOperationNodeView&,std::string_view,bool,bool&) noexcept;
std::span<const float> array(const dataflow_runtime::NumericOperationNodeView&,std::string_view) noexcept;
std::string_view text(const dataflow_runtime::NumericOperationNodeView&,std::string_view) noexcept;
std::optional<signal_contracts::NumericBufferContract> contract(const dataflow_runtime::NumericOperationNodeView&,domain::PortDirection,domain::PortId) noexcept;
bool input_geometry(const dataflow_runtime::NumericOperationNodeView&,domain::PortId,signal_contracts::Rank,std::size_t&,std::size_t&) noexcept;
bool optional_vector(const dataflow_runtime::NumericOperationNodeView&,domain::PortId,std::size_t maximum,bool scalar=false) noexcept;
bool output_geometry(const dataflow_runtime::NumericOperationNodeView&,domain::PortId,signal_contracts::Rank,
    std::size_t,std::size_t=1,signal_contracts::DType=signal_contracts::DType::Float32) noexcept;
const NumericOperationInput* input_at(std::span<const NumericOperationInput>,domain::PortId) noexcept;
// Availability is separate from presence; an available empty value stays present.
const NumericOperationInput* available_input_at(std::span<const NumericOperationInput>,domain::PortId) noexcept;
NumericOperationOutput* output_at(std::span<NumericOperationOutput>,domain::PortId) noexcept;
bool read_row(const void*,std::size_t,std::span<float>) noexcept;
bool write_row(void*,std::size_t,std::span<const float>) noexcept;
dsp_kernels::FloatMatrixSource rows(const NumericOperationInput&) noexcept;
dsp_kernels::FloatMatrixDestination rows(NumericOperationOutput&) noexcept;
bool set_shape(NumericOperationOutput&,std::size_t,std::size_t=1) noexcept;
bool write_values(NumericOperationOutput&,std::span<const float>) noexcept;
bool write_indices(NumericOperationOutput&,std::span<const std::size_t>) noexcept;
bool write_u32(NumericOperationOutput&,std::span<const std::uint32_t>) noexcept;
bool write_mask(NumericOperationOutput&,std::span<const std::uint8_t>) noexcept;
bool write_samples(NumericOperationOutput&,std::span<const std::uint64_t>) noexcept;
bool write_seconds(NumericOperationOutput&,std::span<const double>) noexcept;
bool write_scalar(NumericOperationOutput&,float) noexcept;
bool feature_rate(const dataflow_runtime::NumericOperationNodeView&,float,float&) noexcept;
bool within(const domain::ProcessingResources&,const domain::ProcessingResources&) noexcept;
bool descriptor_resources(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&,
    std::size_t object_bytes,std::size_t kernel_bytes,domain::ProcessingSupport&) noexcept;

// Common ownership and preflight, without musical policy or source formulas.
class SequenceOperation:public dataflow_runtime::PreparedNumericOperation {
public:
    void bind(std::shared_ptr<const dataflow_runtime::NumericOperationDescriptor>,domain::ProcessingAdmissionLimits) noexcept;
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override;
    Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override;
    void reset() noexcept override;
    bool owned_resources(domain::ProcessingResources&) const noexcept override;
    Status process(std::span<const NumericOperationInput>,std::span<NumericOperationOutput>,
        dataflow_runtime::NumericOperationControl) noexcept override;
    virtual std::size_t object_bytes() const noexcept=0;
    virtual std::size_t workspace_bytes() const noexcept=0;
protected:
    virtual bool run(std::span<const NumericOperationInput>,std::span<NumericOperationOutput>) noexcept=0;
    virtual void reset_kernel() noexcept {}
    std::shared_ptr<const dataflow_runtime::NumericOperationDescriptor> descriptor_;
    domain::ProcessingAdmissionLimits limits_{};
private:
    Status preflight_inputs(std::span<const NumericOperationInput>) const noexcept;
    Status preflight_outputs(std::span<const NumericOperationInput>,std::span<NumericOperationOutput>) const noexcept;
    Status preflight(std::span<const NumericOperationInput>,std::span<NumericOperationOutput>) const noexcept;
    bool publish(std::span<NumericOperationOutput>,dataflow_runtime::NumericOperationOrigin) noexcept;
};
using PreparedSequence=std::unique_ptr<SequenceOperation>;
PreparedSequence prepare_onset(const dataflow_runtime::NumericOperationNode&);
PreparedSequence prepare_tempo(const dataflow_runtime::NumericOperationNode&);
PreparedSequence prepare_beat_pulse(const dataflow_runtime::NumericOperationNode&);
PreparedSequence prepare_histogram(const dataflow_runtime::NumericOperationNode&);
PreparedSequence prepare_state(const dataflow_runtime::NumericOperationNode&,std::shared_ptr<const ClassicalModel>);
PreparedSequence prepare_model(const dataflow_runtime::NumericOperationNode&,std::shared_ptr<const ClassicalModel>);
domain::ProcessingSupport assess_onset(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
domain::ProcessingSupport assess_tempo(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
domain::ProcessingSupport assess_beat_pulse(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
domain::ProcessingSupport assess_histogram(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
domain::ProcessingSupport assess_state(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&,const ClassicalModel*) noexcept;
domain::ProcessingSupport assess_model(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&,const ClassicalModel*) noexcept;
PreparedSequence prepare_sequence(const dataflow_runtime::NumericOperationNode&,std::shared_ptr<const ClassicalModel> = {});
PreparedSequence prepare_spectral_sequence(const dataflow_runtime::NumericOperationNode&);
domain::ProcessingSupport assess_spectral_sequence(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
}
