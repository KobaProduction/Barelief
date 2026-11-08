// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adapter_operation.h"
#include <rhythm_reference_ports/contrast_peak_picker.h>
#include <variant>

namespace rhythm_graph_adapters::detail {
inline constexpr domain::PortId contrast_control_port=65500;
struct ContrastPlan {
    bool accumulator=false;
    std::size_t maximum=0,events=0,kernel_bytes=0,buffer_bytes=0;
    rhythm_reference_ports::ContrastPeakPickerConfig config;
};
bool contrast_operation_kind(std::string_view) noexcept;
bool plan_contrast_operation(const dataflow_runtime::NumericOperationNodeView&,ContrastPlan&) noexcept;
bool contrast_output_contracts(const dataflow_runtime::NumericOperationNodeView&,const ContrastPlan&) noexcept;
std::size_t contrast_output_count(const ContrastPlan&) noexcept;
domain::ProcessingSupport assess_contrast_operation(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
dataflow_runtime::NumericOperationPolicy contrast_operation_policy(const dataflow_runtime::NumericOperationNodeView&);
std::unique_ptr<AdapterOperation> prepare_contrast_operation(const dataflow_runtime::NumericOperationNode&);

class ContrastOperation final:public AdapterOperation {
public:
    bool prepare(const dataflow_runtime::NumericOperationNode&);
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override;
    signal_contracts::Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override;
    void reset() noexcept override;
    signal_contracts::Status process(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept override;
    std::size_t workspace_bytes() const noexcept override;
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
private:
    signal_contracts::Status process_enabled(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept;
    signal_contracts::Status validate_call(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl,bool& has_frame) const noexcept;
    signal_contracts::Status validate_contrast_outputs(std::span<dataflow_runtime::NumericOperationOutput>) const noexcept;
    bool calculate(const dataflow_runtime::NumericOperationInput*,dataflow_runtime::NumericOperationEvent) noexcept;
    signal_contracts::Status publish(std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationEvent) noexcept;
    signal_contracts::Status publish_disabled(std::span<dataflow_runtime::NumericOperationOutput>,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    ContrastPlan plan_;
    std::variant<rhythm_reference_ports::PreparedContrastPeakPicker,rhythm_reference_ports::PreparedContrastPeakAccumulator> kernel_;
    std::vector<float> final_events_;
    std::size_t rows_=0,count_=0;
    dataflow_runtime::NumericOperationOrigin source_stamp_;
};
}
