// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "batch_operation_plan.h"
#include <rhythm_reference_ports/loop_tempo_recipe.h>
#include <rhythm_reference_ports/beat_loudness_sequence.h>
#include <rhythm_reference_ports/beat_band_salience.h>
#include <rhythm_reference_ports/correlation_meter.h>
#include <rhythm_reference_ports/band_rhythm_transform.h>
#include <rhythm_reference_ports/activity_fluctuation_recipe.h>
namespace rhythm_graph_adapters::detail {
enum class LoudnessKind {LoopFit,LoopTempo,Segment,Ratios,Frequency,Sequence,Salience,Meter,Transform,Activity};
struct LoudnessPlan {
    LoudnessKind kind{};
    std::size_t rows=0,columns=1,secondary=0,bands=0,bins=0,frames=0,scales=0,frame_size=0,window=16,outputs=0;
    float rate=44100,analysis_rate=120,threshold=.95f;
    rhythm_reference_ports::BeatSegmentEnergyConfig segment;
    rhythm_reference_ports::BeatLoudnessSequenceConfig sequence;
    rhythm_reference_ports::BandRhythmTransformConfig transform;
    rhythm_reference_ports::ActivityFluctuationConfig activity;
    std::array<BatchShape,9> shape{};
};
bool loudness_operation_kind(std::string_view) noexcept;
bool loudness_operation_plan(const domain::ResolvedProcessingNode&,LoudnessPlan&) noexcept;
std::size_t loudness_operation_inputs(LoudnessKind) noexcept;
std::span<const float> loudness_borders(const domain::ResolvedProcessingNode&,LoudnessKind) noexcept;
bool loudness_operation_storage(const domain::ResolvedProcessingNode&,const LoudnessPlan&,std::size_t&) noexcept;
dataflow_runtime::NumericOperationPolicy loudness_operation_policy(const domain::ResolvedProcessingNode&,const LoudnessPlan&);
domain::ProcessingSupport assess_loudness_operation(const domain::ResolvedProcessingNode&,const dataflow_runtime::NumericRuntimeProfile&) noexcept;
std::unique_ptr<AdapterOperation> prepare_loudness_operation(const domain::ConfiguredProcessingNode&);
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
std::unique_ptr<AdapterOperation> prepare_loop_kernel(const domain::ResolvedProcessingNode&,const LoudnessPlan&);
std::unique_ptr<AdapterOperation> prepare_energy_kernel(const domain::ResolvedProcessingNode&,const LoudnessPlan&);
std::unique_ptr<AdapterOperation> prepare_rhythm_matrix_kernel(const domain::ResolvedProcessingNode&,const LoudnessPlan&);
std::size_t loop_kernel_size(LoudnessKind) noexcept;
std::size_t energy_kernel_size(LoudnessKind) noexcept;
std::size_t rhythm_matrix_kernel_size(LoudnessKind) noexcept;
}
