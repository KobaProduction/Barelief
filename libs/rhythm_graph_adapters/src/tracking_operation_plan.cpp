// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tracking_operations.h"
#include "frame_operation_io.h"
#include <cmath>
#include <climits>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace rhythm_reference_ports;
namespace {
struct Named {std::string_view name;TrackingKind kind;};
constexpr Named names[]{
    {"InformationGain",TrackingKind::InformationGain},{"ErbFilterBank",TrackingKind::Erb},
    {"PeriodPath",TrackingKind::PeriodPath},{"RenewalBeatDecoder",TrackingKind::Renewal},
    {"BeatAgreement",TrackingKind::Agreement},{"BandTempoFeatures",TrackingKind::BandFeatures},
    {"PeriodPhaseTicks",TrackingKind::PhaseTicks},{"HarmonicComb",TrackingKind::HarmonicComb},
    {"PeriodicPhaseResponse",TrackingKind::PhaseResponse},{"FrameSilencePolicy",TrackingKind::Silence},{"LagObservationPool",TrackingKind::LagPool},{"TickSequenceRefinement",TrackingKind::TickRefinement},
    {"LagTempoVote",TrackingKind::LagVote},};
bool dimension(const dataflow_runtime::NumericOperationNodeView& n,std::size_t id,Rank rank,std::size_t& rows,std::size_t& columns,DType type=DType::Float32) noexcept {
    NumericBufferContract c;if(!numeric_port(n,true,domain::PortId(id),c) || c.dtype!=type || c.shape.rank!=rank || c.plane_count!=1 || c.representation.encoding!=StorageEncoding::Native || c.representation.valid_bits || c.representation.layout!=ChannelLayout::Planar || (c.representation.normalization!=Normalization::None && c.representation.normalization!=Normalization::FloatUnit))return false;
    if(c.shape.extents[0].maximum>SIZE_MAX || c.shape.extents[1].maximum>SIZE_MAX)return false;
    rows=std::size_t(c.shape.extents[0].maximum);columns=std::size_t(c.shape.extents[1].maximum);return rows && columns;
}
bool config_global(const dataflow_runtime::NumericOperationNodeView& n,TrackingPlan& p) noexcept {
    return scalar_parameter(n,"sample_rate",44100,p.sample_rate) && scalar_parameter(n,"band_sample_rate",44100,p.erb.sample_rate) &&
        integer_parameter(n,"bands",40,p.erb.bands) && scalar_parameter(n,"low_hz",80,p.erb.low_hz) &&
        scalar_parameter(n,"high_hz",22050,p.erb.high_hz) && scalar_parameter(n,"width",1,p.erb.width);
}
bool period_path_settings(const dataflow_runtime::NumericOperationNodeView& n,TrackingPlan& p,std::size_t& up,std::size_t& method) noexcept {
    return scalar_parameter(n,"frame_rate",44100.f/512.f,p.path.frame_rate) && scalar_parameter(n,"minimum_bpm",40,p.path.min_bpm) && scalar_parameter(n,"maximum_bpm",208,p.path.max_bpm) &&
        scalar_parameter(n,"frame_duration_s",5.944308390022676f,p.path.frame_duration_s) && scalar_parameter(n,"noise_amount",.0001f,p.path.noise_amount) &&
        integer_parameter(n,"frame_length",512,p.path.frame_length) && integer_parameter(n,"period_hop",128,p.path.hop) && integer_parameter(n,"upsampling",1,up) && up<=4 && integer_parameter(n,"method",1,method) && method<=1;
}
bool period_rate_settings(TrackingPlan& p,std::size_t up) noexcept {
    p.renewal.frame_rate=p.path.frame_rate;p.renewal.upsampling=unsigned(up);return true;
}
bool renewal_settings(const dataflow_runtime::NumericOperationNodeView& n,TrackingPlan& p) noexcept {
    if(!scalar_parameter(n,"maximum_period_s",1.5f,p.renewal.maximum_period_s) || !scalar_parameter(n,"sigma_ibi",.025f,p.renewal.sigma_ibi) || !scalar_parameter(n,"transition_mix",.5f,p.renewal.transition_mix) || !scalar_parameter(n,"minimum_time",5,p.minimum_time))return false;
    return true;
}
bool config_period(const dataflow_runtime::NumericOperationNodeView& n,TrackingPlan& p) noexcept {
    std::size_t up=1,method=1;
    return period_path_settings(n,p,up,method) && period_rate_settings(p,up) && renewal_settings(n,p);
}
bool config_legacy(const dataflow_runtime::NumericOperationNodeView& n,TrackingPlan& p) noexcept {
    auto& c=p.legacy;
    if(!scalar_parameter(n,"sample_rate",44100,c.sample_rate) || !integer_parameter(n,"audio_frame",1024,c.audio_frame) || !integer_parameter(n,"audio_hop",256,c.audio_hop) ||
       !integer_parameter(n,"period_window",1024,c.period_window) || !integer_parameter(n,"evaluation_hop",1024,c.evaluation_hop) || !batch_bool(n,"use_onset",true,c.use_onset) || !batch_bool(n,"use_bands",true,c.use_bands))return false;
    if(!integer_parameter(n,"maximum_rows",4096,p.maximum_rows) || !integer_parameter(n,"maximum_matches",65536,p.maximum_matches) || !integer_parameter(n,"maximum_ticks",65536,p.maximum_ticks) ||
       !integer_parameter(n,"half_window",8,p.half) || !integer_parameter(n,"harmonics",4,p.harmonics) || !integer_parameter(n,"first",1,p.first) || !integer_parameter(n,"stop",p.rows,p.stop) ||
       !integer_parameter(n,"leading_zeros",0,p.leading) || !integer_parameter(n,"acquired",p.rows,p.acquired) || !integer_parameter(n,"total_frames",0,p.total_frames) || !integer_parameter(n,"response_length",p.rows,p.response_length))return false;
    if(!scalar_parameter(n,"period",1,p.period) || !scalar_parameter(n,"frame_time_samples",512,p.frame_time) || !scalar_parameter(n,"start_silence_frame",0,p.start_silence) || !scalar_parameter(n,"stop_silence_frame",float(p.total_frames),p.stop_silence) ||
       !scalar_parameter(n,"minimum_interval_seconds",.24f,c.minimum_interval_seconds) || !scalar_parameter(n,"last_beat_interval_seconds",.1f,c.last_beat_interval_seconds))return false;
    c.minimum_bpm=int(p.path.min_bpm);c.maximum_bpm=int(p.path.max_bpm);
    p.ticks={c.evaluation_hop,c.audio_hop,c.sample_rate,float(c.period_window)};
    p.vote={c.sample_rate,1000,c.audio_hop};p.refinement={c.sample_rate/float(c.audio_hop),c.minimum_interval_seconds,c.last_beat_interval_seconds};
    return scalar_parameter(n,"maximum_vote_bpm",1000,p.vote.maximum_bpm);
}
bool silence_config(const dataflow_runtime::NumericOperationNodeView& n,TrackingPlan& p) noexcept {
    std::size_t boundaries=1,seed=0;
    if(!scalar_parameter(n,"threshold_power",1e-6f,p.silence.threshold_power) || !scalar_parameter(n,"noise_trigger_power",1e-10f,p.silence.noise_trigger_power) || !scalar_parameter(n,"noise_gain",1e-10f,p.silence.noise_gain) ||
       !batch_bool(n,"add_noise",true,p.silence.add_noise) || !integer_parameter(n,"silence_boundaries",1,boundaries) || boundaries>1 || !integer_parameter(n,"noise_seed",0,seed) || seed>UINT32_MAX)return false;
    p.silence.boundaries=SilenceBoundaryRecipe(boundaries);p.silence.noise_seed=std::uint32_t(seed);return true;
}
bool renewal_inputs(const dataflow_runtime::NumericOperationNodeView& n,TrackingPlan& p) noexcept {
    std::size_t rows=0,columns=0;p.inputs=3;
    return dimension(n,1,Rank::Vector,p.counts[0],columns) && dimension(n,2,Rank::Vector,rows,columns) && rows==p.counts[0];
}
bool paired_inputs(const dataflow_runtime::NumericOperationNodeView& n,TrackingPlan& p) noexcept {
    std::size_t rows=0,columns=0;p.inputs=2;
    if(p.kind==TrackingKind::HarmonicComb)return dimension(n,1,Rank::Vector,p.counts[0],columns);
    return dimension(n,1,Rank::Vector,rows,columns,p.kind==TrackingKind::Agreement?DType::UInt64:DType::Float32) && rows==p.rows;
}
bool extra_inputs(const dataflow_runtime::NumericOperationNodeView& n,TrackingPlan& p) noexcept {
    if(p.kind==TrackingKind::Renewal)return renewal_inputs(n,p);
    if(p.kind==TrackingKind::Agreement || p.kind==TrackingKind::HarmonicComb || p.kind==TrackingKind::PhaseTicks)return paired_inputs(n,p);
    return true;
}
bool protocol_input(const dataflow_runtime::NumericOperationNodeView& n,TrackingPlan& p) noexcept {
    NumericBufferContract input;p.control=numeric_port(n,true,tracking_control_port,input);
    if(p.control && (p.kind!=TrackingKind::Silence || input.dtype!=DType::UInt8 || input.shape.rank!=Rank::Scalar ||
        input.plane_count!=1 || input.representation.normalization!=Normalization::None))return false;
    if(n.inputs.size()!=p.inputs+std::size_t(p.control))return false;
    for(std::size_t i=0;i<n.inputs.size();++i)
        if(n.inputs[i].id!=(i<p.inputs?domain::PortId(i):tracking_control_port))return false;
    return true;
}
}
bool tracking_operation_kind(std::string_view id) noexcept {for(const auto& x:names)if(x.name==id)return true;return false;}
namespace {
bool resolve_tracking_kind(std::string_view kind,TrackingPlan& plan) noexcept {
    for(const auto& entry:names)if(entry.name==kind){plan.kind=entry.kind;return true;}
    return false;
}
bool special_parameters(const dataflow_runtime::NumericOperationNodeView& n,TrackingPlan& p) noexcept {
    if(p.kind==TrackingKind::HarmonicComb){
        p.response_length=p.counts[0];if(!integer_parameter(n,"stop",p.response_length/2,p.stop))return false;
    }
    return p.kind!=TrackingKind::InformationGain ||
        (integer_parameter(n,"first_bin",0,p.first) && integer_parameter(n,"stop_bin",p.rows,p.stop));
}
bool feature_geometry(const dataflow_runtime::NumericOperationNodeView& n,const TrackingPlan& p) noexcept {
    if(p.kind==TrackingKind::PeriodPath && p.columns!=p.path.hop)return false;
    if(p.kind==TrackingKind::BandFeatures){const auto gains=array_parameter(n,"gains");if(p.rows!=(gains.empty()?8:gains.size()))return false;}
    return true;
}
bool frame_operation(const TrackingPlan& p) noexcept {
    return (p.kind==TrackingKind::InformationGain) || p.kind==TrackingKind::BandFeatures ||
        p.kind==TrackingKind::PhaseTicks || p.kind==TrackingKind::Silence || p.kind==TrackingKind::LagPool;
}
}
bool tracking_operation_plan(const dataflow_runtime::NumericOperationNodeView& n,TrackingPlan& p) noexcept {
    if(!resolve_tracking_kind(n.kind,p))return false;
    NumericBufferContract first;if(!numeric_port(n,true,0,first))return false;
    p.matrix=first.shape.rank==Rank::Matrix;
    const bool matrix=p.kind==TrackingKind::PeriodPath || p.kind==TrackingKind::Agreement ||
        (p.kind==TrackingKind::HarmonicComb && p.matrix);
    if(!dimension(n,0,matrix?Rank::Matrix:Rank::Vector,p.rows,p.columns) || !extra_inputs(n,p))return false;
    if(!config_global(n,p) || !config_period(n,p) || !config_legacy(n,p) || !silence_config(n,p))return false;
    if(!special_parameters(n,p) || !feature_geometry(n,p))return false;
    p.frame=frame_operation(p);return protocol_input(n,p);
}
} // namespace rhythm_graph_adapters::detail
