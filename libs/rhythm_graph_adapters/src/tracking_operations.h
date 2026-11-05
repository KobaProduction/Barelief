// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "batch_operation_plan.h"
#include <rhythm_reference_ports/pcm_beat_recipe.h>
#include <rhythm_reference_ports/erb_filter_bank.h>
#include <rhythm_reference_ports/legacy_rhythm_recipe.h>
namespace rhythm_graph_adapters::detail {
inline constexpr domain::PortId tracking_control_port=65500;
enum class TrackingKind {InformationGain,BandEmphasis,Erb,Threshold,PeriodPath,Renewal,PeriodBeat,Agreement,MultiBeat,PcmBeat,BandFeatures,CombPeriod,FeaturePeriod,PhaseTicks,HarmonicComb,PhaseResponse,Silence,LagPool,TickRefinement,LagVote,Legacy};
struct TrackingPlan {
    TrackingKind kind{};std::size_t rows=0,columns=1,inputs=1,outputs=0,first=0,stop=0,half=8,harmonics=4,maximum_rows=4096,maximum_matches=65536,maximum_ticks=65536,evaluation_hop=1024,leading=0,acquired=0,total_frames=0,response_length=0;
    std::array<std::size_t,5> counts{};
    std::array<BatchShape,32> shape{};
    float period=1,minimum_time=5,start_silence=0,stop_silence=0,frame_time=512;
    bool frame=false,control=false;
    rhythm_reference_ports::BandBeatEmphasisConfig emphasis;
    rhythm_reference_ports::ErbFilterBankConfig erb;
    rhythm_reference_ports::GlobalOnsetRecipeConfig global;
    rhythm_reference_ports::PeriodPathConfig path;
    rhythm_reference_ports::RenewalBeatConfig renewal;
    rhythm_reference_ports::PeriodBeatTrackingConfig beat;
    rhythm_reference_ports::MultiFeaturePeriodConfig multi;
    rhythm_reference_ports::PcmBeatRecipeConfig pcm;
    rhythm_reference_ports::CombPeriodPhaseConfig comb;
    rhythm_reference_ports::PeriodPhaseTickConfig ticks;
    rhythm_reference_ports::FrameSilenceConfig silence;
    rhythm_reference_ports::TickSequenceRefinementConfig refinement;
    rhythm_reference_ports::LagTempoVoteConfig vote;
    rhythm_reference_ports::LegacyRhythmRecipeConfig legacy;
    bool global_recipe=false;
};
bool tracking_operation_kind(std::string_view) noexcept;
bool tracking_operation_plan(const domain::ResolvedProcessingNode&,TrackingPlan&) noexcept;
bool tracking_operation_shapes(const domain::ResolvedProcessingNode&,TrackingPlan&) noexcept;
bool tracking_operation_storage(const domain::ResolvedProcessingNode&,const TrackingPlan&,std::size_t&) noexcept;
dataflow_runtime::NumericOperationPolicy tracking_operation_policy(const domain::ResolvedProcessingNode&);
domain::ProcessingSupport assess_tracking_operation(const domain::ResolvedProcessingNode&,const dataflow_runtime::NumericRuntimeProfile&) noexcept;
std::unique_ptr<AdapterOperation> prepare_tracking_operation(const domain::ConfiguredProcessingNode&);
std::size_t tracking_operation_object_bytes(TrackingKind,bool global_recipe=false) noexcept;
class TrackingOperation:public BatchOperation {
public:
    void configure_plan(const TrackingPlan& p) noexcept {tracking_=p;plan_.kind=p.frame?BatchKind::PulseOnset:BatchKind::PulseTempo;plan_.outputs=p.outputs;plan_.shape=p.shape;}
protected:
    TrackingPlan tracking_;
};
template<class T> class TrackingKernel:public TrackingOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<T>(static_cast<const T&>(*this));}
    std::size_t object_bytes() const noexcept override {return sizeof(T);}
};
std::unique_ptr<AdapterOperation> prepare_global_tracking(const domain::ResolvedProcessingNode&,const TrackingPlan&);
std::unique_ptr<AdapterOperation> prepare_period_tracking(const domain::ResolvedProcessingNode&,const TrackingPlan&);
std::unique_ptr<AdapterOperation> prepare_legacy_tracking(const domain::ResolvedProcessingNode&,const TrackingPlan&);
std::size_t global_tracking_object_bytes(TrackingKind,bool) noexcept;
std::size_t period_tracking_object_bytes(TrackingKind) noexcept;
std::size_t legacy_tracking_object_bytes(TrackingKind) noexcept;
bool tracking_unsigned_vector(dataflow_runtime::NumericOperationOutput&,std::span<const std::uint32_t>) noexcept;
bool tracking_mask(dataflow_runtime::NumericOperationOutput&,std::span<const std::uint8_t>) noexcept;
bool tracking_matrix(dataflow_runtime::NumericOperationOutput&,std::span<const float>,std::size_t,std::size_t) noexcept;
bool tracking_frame_outputs(std::span<dataflow_runtime::NumericOperationOutput>,std::size_t,const rhythm_reference_ports::SpectralFrameStages&) noexcept;
} // namespace rhythm_graph_adapters::detail
