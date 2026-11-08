// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "batch_operation_plan.h"
#include <rhythm_reference_ports/global_onset.h>
#include <rhythm_reference_ports/period_path.h>
#include <rhythm_reference_ports/renewal_beat_decoder.h>
#include <rhythm_reference_ports/beat_agreement.h>
#include <rhythm_reference_ports/erb_filter_bank.h>
#include <rhythm_reference_ports/band_tempo_features.h>
#include <rhythm_reference_ports/frame_silence_policy.h>
#include <rhythm_reference_ports/period_phase_ticks.h>
#include <rhythm_reference_ports/lag_observation_pool.h>
#include <rhythm_reference_ports/tick_sequence_refinement.h>
#include <rhythm_reference_ports/lag_tempo_vote.h>
namespace rhythm_graph_adapters::detail {
inline constexpr domain::PortId tracking_control_port=65500;
enum class TrackingKind {InformationGain,Erb,PeriodPath,Renewal,Agreement,BandFeatures,PhaseTicks,HarmonicComb,PhaseResponse,Silence,LagPool,TickRefinement,LagVote};
struct TrackingPlan {
    TrackingKind kind{};std::size_t rows=0,columns=1,inputs=1,outputs=0,first=0,stop=0,half=8,harmonics=4,maximum_rows=4096,maximum_matches=65536,maximum_ticks=65536,evaluation_hop=1024,leading=0,acquired=0,total_frames=0,response_length=0;
    std::array<std::size_t,5> counts{};
    std::array<BatchShape,32> shape{};
    float period=1,minimum_time=5,start_silence=0,stop_silence=0,frame_time=512;
    bool frame=false,control=false,matrix=false;
    rhythm_reference_ports::ErbFilterBankConfig erb;
    float sample_rate=44100;
    rhythm_reference_ports::PeriodPathConfig path;
    rhythm_reference_ports::RenewalBeatConfig renewal;
    rhythm_reference_ports::PeriodPhaseTickConfig ticks;
    rhythm_reference_ports::FrameSilenceConfig silence;
    rhythm_reference_ports::TickSequenceRefinementConfig refinement;
    rhythm_reference_ports::LagTempoVoteConfig vote;
    struct PrimitiveSettings {
        float sample_rate=44100,minimum_interval_seconds=.24f,last_beat_interval_seconds=.1f;
        std::size_t audio_frame=1024,audio_hop=256,period_window=1024,evaluation_hop=1024;
        int minimum_bpm=40,maximum_bpm=208;
        bool use_onset=true,use_bands=true;
    } legacy;
};
bool tracking_operation_kind(std::string_view) noexcept;
bool tracking_operation_plan(const dataflow_runtime::NumericOperationNodeView&,TrackingPlan&) noexcept;
bool tracking_operation_shapes(const dataflow_runtime::NumericOperationNodeView&,TrackingPlan&) noexcept;
bool tracking_operation_storage(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan&,std::size_t&) noexcept;
dataflow_runtime::NumericOperationPolicy tracking_operation_policy(const dataflow_runtime::NumericOperationNodeView&);
domain::ProcessingSupport assess_tracking_operation(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
std::unique_ptr<AdapterOperation> prepare_tracking_operation(const dataflow_runtime::NumericOperationNode&);
std::size_t tracking_operation_object_bytes(TrackingKind) noexcept;
class TrackingOperation:public BatchOperation {
public:
    void configure_plan(const TrackingPlan& p) noexcept {tracking_=p;plan_.kind=p.frame?BatchKind::Stream:BatchKind::Independent;plan_.outputs=p.outputs;plan_.shape=p.shape;}
protected:
    TrackingPlan tracking_;
};
template<class T> class TrackingKernel:public TrackingOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<T>(static_cast<const T&>(*this));}
    std::size_t object_bytes() const noexcept override {return sizeof(T);}
};
std::unique_ptr<AdapterOperation> prepare_global_tracking(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan&);
std::unique_ptr<AdapterOperation> prepare_period_tracking(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan&);
std::unique_ptr<AdapterOperation> prepare_legacy_tracking(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan&);
std::size_t global_tracking_object_bytes(TrackingKind) noexcept;
std::size_t period_tracking_object_bytes(TrackingKind) noexcept;
std::size_t legacy_tracking_object_bytes(TrackingKind) noexcept;
bool tracking_unsigned_vector(dataflow_runtime::NumericOperationOutput&,std::span<const std::uint32_t>) noexcept;
bool tracking_mask(dataflow_runtime::NumericOperationOutput&,std::span<const std::uint8_t>) noexcept;
bool tracking_matrix(dataflow_runtime::NumericOperationOutput&,std::span<const float>,std::size_t,std::size_t) noexcept;
} // namespace rhythm_graph_adapters::detail
