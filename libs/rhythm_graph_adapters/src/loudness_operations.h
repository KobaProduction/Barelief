// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "batch_operation_plan.h"
#include <rhythm_reference_ports/loop_length_confidence.h>
#include <rhythm_reference_ports/spectral_band_ratios.h>
#include <rhythm_reference_ports/beat_band_salience.h>
namespace rhythm_graph_adapters::detail {
enum class LoudnessKind {LoopFit,Ratios,Frequency,Salience};
struct LoudnessPlan {
    LoudnessKind kind{};
    std::size_t rows=0,columns=1,secondary=0,bands=0,bins=0,frames=0,scales=0,frame_size=0,window=16,outputs=0;
    float rate=44100,analysis_rate=120,threshold=.95f;
    std::array<BatchShape,9> shape{};
};
bool loudness_operation_kind(std::string_view) noexcept;
bool loudness_operation_plan(const dataflow_runtime::NumericOperationNodeView&,LoudnessPlan&) noexcept;
std::size_t loudness_operation_inputs(LoudnessKind) noexcept;
std::span<const float> loudness_borders(const dataflow_runtime::NumericOperationNodeView&,LoudnessKind) noexcept;
bool loudness_operation_storage(const dataflow_runtime::NumericOperationNodeView&,const LoudnessPlan&,std::size_t&) noexcept;
dataflow_runtime::NumericOperationPolicy loudness_operation_policy(const dataflow_runtime::NumericOperationNodeView&,const LoudnessPlan&);
domain::ProcessingSupport assess_loudness_operation(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
std::unique_ptr<AdapterOperation> prepare_loudness_operation(const dataflow_runtime::NumericOperationNode&);
std::size_t loudness_object_size(LoudnessKind) noexcept;
class LoudnessOperation:public AdapterOperation {
public:
    signal_contracts::Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override {return signal_contracts::Status::incompatible_contract;}
    void reset() noexcept override {reset_kernel();}
    signal_contracts::Status process(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept override;
protected:
    LoudnessPlan plan_;
    virtual bool run(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>) noexcept=0;
    virtual void reset_kernel() noexcept {}
};
template<class T> class LoudnessKernel:public LoudnessOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<T>(static_cast<const T&>(*this));}
    std::size_t object_bytes() const noexcept override {return sizeof(T);}
};
bool loudness_shape(dataflow_runtime::NumericOperationOutput&,std::size_t,std::size_t=1) noexcept;
bool loudness_matrix_row(void*,std::size_t,std::span<const float>) noexcept;
bool loudness_indices(dataflow_runtime::NumericOperationOutput&,std::span<const std::size_t>) noexcept;
bool loudness_complex(dataflow_runtime::NumericOperationOutput&,std::span<const float>) noexcept;
std::unique_ptr<AdapterOperation> prepare_loop_kernel(const dataflow_runtime::NumericOperationNodeView&,const LoudnessPlan&);
std::unique_ptr<AdapterOperation> prepare_energy_kernel(const dataflow_runtime::NumericOperationNodeView&,const LoudnessPlan&);
std::unique_ptr<AdapterOperation> prepare_rhythm_matrix_kernel(const dataflow_runtime::NumericOperationNodeView&,const LoudnessPlan&);
std::size_t loop_kernel_size(LoudnessKind) noexcept;
std::size_t energy_kernel_size(LoudnessKind) noexcept;
std::size_t rhythm_matrix_kernel_size(LoudnessKind) noexcept;
}
