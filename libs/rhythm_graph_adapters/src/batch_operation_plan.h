// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adapter_operation.h"
#include "parameter_values.h"
#include <rhythm_reference_ports/pulse_tempo_recipe.h>
#include <rhythm_reference_ports/band_novelty.h>
#include <rhythm_reference_ports/fixed_tempo_candidates.h>
#include <rhythm_reference_ports/novelty_tempo_histogram.h>
#include <array>
namespace rhythm_graph_adapters::detail {
enum class BatchKind {PulseTempo,PulseOnset,PulseCorrelation,PulseTrain,LagConsensus,BandNovelty,FixedCandidates,FourierCandidates,WeightedHistogram,FramePath,PhasePulse,FourierHistogram,NoveltyHistogram};
struct BatchShape {signal_contracts::Rank rank=signal_contracts::Rank::Vector;std::size_t rows=1,columns=1;signal_contracts::DType type=signal_contracts::DType::Float32;bool variable=false;signal_contracts::ValueDomain values=signal_contracts::ValueDomain::FiniteOnly;};
struct BatchPlan {
    BatchKind kind{};std::size_t rows=0,columns=1,secondary=0,frames=0,bins=0,outputs=0;std::array<BatchShape,32> shape{};
    rhythm_reference_ports::PulseTempoRecipeConfig pulse;
    rhythm_reference_ports::TempoOctaveClassifier classifier;
    rhythm_reference_ports::BandNoveltyConfig band;
    rhythm_reference_ports::FixedTempoCandidateConfig fixed;
    rhythm_reference_ports::FourierTempoHistogramConfig histogram;
    rhythm_reference_ports::NoveltyTempoHistogramConfig novelty;
    float rate=44100.f/128.f;
};
bool batch_operation_kind(std::string_view) noexcept;
std::size_t batch_operation_input_count(BatchKind) noexcept;
bool batch_operation_plan(const domain::ResolvedProcessingNode&,BatchPlan&) noexcept;
bool batch_operation_shapes(const domain::ResolvedProcessingNode&,BatchPlan&) noexcept;
bool batch_operation_storage(const BatchPlan&,const domain::ResolvedProcessingNode&,std::size_t&) noexcept;
std::size_t batch_operation_object_size(BatchKind) noexcept;
dataflow_runtime::NumericOperationPolicy batch_operation_policy(const domain::ResolvedProcessingNode&,const BatchPlan&);
domain::ProcessingSupport assess_batch_operation(const domain::ResolvedProcessingNode&,const dataflow_runtime::NumericRuntimeProfile&) noexcept;
std::unique_ptr<AdapterOperation> prepare_batch_operation(const domain::ConfiguredProcessingNode&);
class BatchOperation:public AdapterOperation {
public:
    signal_contracts::Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override {return signal_contracts::Status::incompatible_contract;}
    void reset() noexcept override {reset_kernel();}
    signal_contracts::Status process(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept override;
    BatchPlan plan_;
protected:
    virtual bool run(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>) noexcept=0;
    virtual void reset_kernel() noexcept {}
private:
    signal_contracts::Status preflight(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>) const noexcept;
};
template<class T> class BatchKernel:public BatchOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<T>(static_cast<const T&>(*this));}
    std::size_t object_bytes() const noexcept override {return sizeof(T);}
};
bool batch_bool(const domain::ResolvedProcessingNode&,std::string_view,bool,bool&) noexcept;
dsp_kernels::FloatMatrixSource batch_rows(const dataflow_runtime::NumericOperationInput&) noexcept;
bool batch_scalar(dataflow_runtime::NumericOperationOutput&,float) noexcept;
bool batch_integer(dataflow_runtime::NumericOperationOutput&,std::uint64_t) noexcept;
bool batch_status(dataflow_runtime::NumericOperationOutput&,std::uint8_t) noexcept;
bool batch_vector(dataflow_runtime::NumericOperationOutput&,std::span<const float>) noexcept;
bool batch_peaks(dataflow_runtime::NumericOperationOutput&,std::span<const rhythm_reference_ports::InterpolatedPeak>) noexcept;
std::unique_ptr<AdapterOperation> prepare_pulse_batch(const domain::ResolvedProcessingNode&,const BatchPlan&);
std::unique_ptr<AdapterOperation> prepare_novelty_batch(const domain::ResolvedProcessingNode&,const BatchPlan&);
std::unique_ptr<AdapterOperation> prepare_histogram_batch(const domain::ResolvedProcessingNode&,const BatchPlan&);
std::size_t pulse_batch_object_size(BatchKind) noexcept;
std::size_t novelty_batch_object_size(BatchKind) noexcept;
std::size_t histogram_batch_object_size(BatchKind) noexcept;
}
