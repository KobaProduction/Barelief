#pragma once
#include "operation_support.h"
#include "common_operations.h"

namespace dsp_graph_adapters::detail {
// Each concrete common block owns its prepared primitive and bounded scratch.
// Only ownership, validation and publication are shared here.
class CommonOperation:public dataflow_runtime::PreparedNumericOperation {
public:
    void bind(std::shared_ptr<const dataflow_runtime::NumericOperationDescriptor> d,domain::ProcessingAdmissionLimits limits) noexcept {descriptor_=std::move(d);limits_=limits;}
    Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override {return Status::incompatible_contract;}
    void reset() noexcept override {reset_kernel();}
    bool owned_resources(domain::ProcessingResources&) const noexcept override;
    Status process(std::span<const NumericOperationInput>,std::span<NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept override;
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
    bool publish(std::span<NumericOperationOutput>,const NumericOperationInput&) const noexcept;
};
template<class Derived> class CommonKernel:public CommonOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Derived>(static_cast<const Derived&>(*this));}
    std::size_t object_bytes() const noexcept override {return sizeof(Derived);}
};
using PreparedCommon=std::unique_ptr<CommonOperation>;
bool common_resources(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&,
    std::size_t object,std::size_t workspace,domain::ProcessingSupport&) noexcept;
bool common_vector(const dataflow_runtime::NumericOperationNodeView&,domain::PortId,std::size_t&) noexcept;
bool common_output(const dataflow_runtime::NumericOperationNodeView&,domain::PortId,signal_contracts::Rank,std::size_t,std::size_t=1,
    signal_contracts::DType=signal_contracts::DType::Float32) noexcept;
bool common_values(const NumericOperationInput&,std::span<float>,std::span<const float>&) noexcept;
PreparedCommon prepare_absolute_value(const dataflow_runtime::NumericOperationNodeView&);
domain::ProcessingSupport assess_absolute_value(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
PreparedCommon prepare_common_values(const dataflow_runtime::NumericOperationNodeView&);
bool temporal_frame_sequence_rows(const dataflow_runtime::NumericOperationNodeView&,std::size_t,std::size_t&) noexcept;
PreparedCommon prepare_temporal_frame_sequence(const dataflow_runtime::NumericOperationNodeView&);
domain::ProcessingSupport assess_temporal_frame_sequence(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
bool linear_expansion_actual(const dataflow_runtime::NumericOperationNodeView&,std::size_t,std::size_t&) noexcept;
PreparedCommon prepare_feature_aggregate(const dataflow_runtime::NumericOperationNodeView&);
domain::ProcessingSupport assess_feature_aggregate(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
PreparedCommon prepare_common_filters(const dataflow_runtime::NumericOperationNodeView&);
PreparedCommon prepare_common_sequences(const dataflow_runtime::NumericOperationNodeView&);
domain::ProcessingSupport assess_common_values(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
domain::ProcessingSupport assess_common_filters(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
domain::ProcessingSupport assess_common_sequences(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
PreparedCommon prepare_common_axis(const dataflow_runtime::NumericOperationNodeView&);
domain::ProcessingSupport assess_common_axis(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;

}
