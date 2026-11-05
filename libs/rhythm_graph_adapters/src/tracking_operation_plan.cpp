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
    {"InformationGain",TrackingKind::InformationGain},{"GlobalOnset",TrackingKind::InformationGain},{"BandBeatEmphasis",TrackingKind::BandEmphasis},{"ErbFilterBank",TrackingKind::Erb},
    {"MovingThreshold",TrackingKind::Threshold},{"PeriodPath",TrackingKind::PeriodPath},{"RenewalBeatDecoder",TrackingKind::Renewal},{"PeriodBeatTracker",TrackingKind::PeriodBeat},
    {"BeatAgreement",TrackingKind::Agreement},{"MultiFeatureBeatTracker",TrackingKind::MultiBeat},{"PcmBeat",TrackingKind::PcmBeat},{"BandTempoFeatures",TrackingKind::BandFeatures},
    {"CombPeriodPhase",TrackingKind::CombPeriod},{"FeaturePeriodTracker",TrackingKind::FeaturePeriod},{"PeriodPhaseTicks",TrackingKind::PhaseTicks},{"HarmonicComb",TrackingKind::HarmonicComb},
    {"PeriodicPhaseResponse",TrackingKind::PhaseResponse},{"FrameSilencePolicy",TrackingKind::Silence},{"LagObservationPool",TrackingKind::LagPool},{"TickSequenceRefinement",TrackingKind::TickRefinement},
    {"LagTempoVote",TrackingKind::LagVote},{"LegacyRhythm",TrackingKind::Legacy}};
bool dimension(const domain::ResolvedProcessingNode& n,std::size_t id,Rank rank,std::size_t& rows,std::size_t& columns,DType type=DType::Float32) noexcept {
    NumericContract c;if(!numeric_port(n,true,domain::PortId(id),c) || c.dtype!=type || c.shape.rank!=rank || c.channel_order.size()!=1 || c.representation.encoding!=StorageEncoding::Native || c.representation.valid_bits || c.representation.layout!=ChannelLayout::Planar || (c.representation.normalization!=Normalization::None && c.representation.normalization!=Normalization::FloatUnit))return false;
    if(c.shape.extents[0].maximum>SIZE_MAX || c.shape.extents[1].maximum>SIZE_MAX)return false;
    rows=std::size_t(c.shape.extents[0].maximum);columns=std::size_t(c.shape.extents[1].maximum);return rows && columns;
}
bool config_global(const domain::ResolvedProcessingNode& n,TrackingPlan& p) noexcept {
    std::size_t method=0;
    return scalar_parameter(n,"sample_rate",44100,p.global.sample_rate) && scalar_parameter(n,"band_sample_rate",44100,p.global.band_sample_rate) &&
        integer_parameter(n,"frame_size",2048,p.global.frame_size) && integer_parameter(n,"hop",512,p.global.hop) && integer_parameter(n,"method",0,method) && method<=1 &&
        (p.global.method=GlobalOnsetMethod(method),true) && integer_parameter(n,"first_bin",0,p.first) && integer_parameter(n,"stop_bin",p.rows,p.stop) &&
        scalar_parameter(n,"frame_rate",44100.f/512.f,p.emphasis.frame_rate) && integer_parameter(n,"bands",40,p.emphasis.bands) &&
        scalar_parameter(n,"low_hz",80,p.erb.low_hz) && scalar_parameter(n,"high_hz",22050,p.erb.high_hz) && scalar_parameter(n,"width",1,p.erb.width) &&
        (p.erb.sample_rate=p.global.sample_rate,p.erb.bands=p.emphasis.bands,true);
}
bool config_period(const domain::ResolvedProcessingNode& n,TrackingPlan& p) noexcept {
    std::size_t up=1,method=1;
    if(!scalar_parameter(n,"frame_rate",44100.f/512.f,p.path.frame_rate) || !scalar_parameter(n,"minimum_bpm",40,p.path.min_bpm) || !scalar_parameter(n,"maximum_bpm",208,p.path.max_bpm) ||
       !scalar_parameter(n,"frame_duration_s",5.944308390022676f,p.path.frame_duration_s) || !scalar_parameter(n,"noise_amount",.0001f,p.path.noise_amount) ||
       !integer_parameter(n,"frame_length",512,p.path.frame_length) || !integer_parameter(n,"period_hop",128,p.path.hop) || !integer_parameter(n,"upsampling",1,up) || up>4 || !integer_parameter(n,"method",1,method) || method>1)return false;
    p.beat={p.path.frame_rate,p.path.min_bpm,p.path.max_bpm,unsigned(up),{}};p.pcm.method=PcmBeatMethod(method);
    if(!std::isfinite(p.path.min_bpm) || !std::isfinite(p.path.max_bpm) || p.path.min_bpm<0 || p.path.max_bpm<0 || double(p.path.min_bpm)>double(INT_MAX) || double(p.path.max_bpm)>double(INT_MAX))return false;
    p.pcm.minimum_bpm=int(p.path.min_bpm);p.pcm.maximum_bpm=int(p.path.max_bpm);
    p.renewal.frame_rate=p.path.frame_rate;p.renewal.upsampling=unsigned(up);
    if(!scalar_parameter(n,"maximum_period_s",1.5f,p.renewal.maximum_period_s) || !scalar_parameter(n,"sigma_ibi",.025f,p.renewal.sigma_ibi) || !scalar_parameter(n,"transition_mix",.5f,p.renewal.transition_mix) || !scalar_parameter(n,"minimum_time",5,p.minimum_time))return false;
    p.multi.minimum_bpm=p.path.min_bpm;p.multi.maximum_bpm=p.path.max_bpm;p.multi.agreement_minimum_seconds=p.minimum_time;
    const auto rates=array_parameter(n,"frame_rates"),factors=array_parameter(n,"upsampling_factors");if((!rates.empty() && rates.size()!=5) || (!factors.empty() && factors.size()!=5))return false;
    for(std::size_t i=0;i<5;++i){if(!rates.empty())p.multi.frame_rates[i]=rates[i];if(!factors.empty()){if(!std::isfinite(factors[i]) || factors[i]<1 || factors[i]>4 || std::floor(factors[i])!=factors[i])return false;p.multi.upsampling[i]=unsigned(factors[i]);}}
    return true;
}
bool config_legacy(const domain::ResolvedProcessingNode& n,TrackingPlan& p) noexcept {
    auto& c=p.legacy;
    if(!scalar_parameter(n,"sample_rate",44100,c.sample_rate) || !integer_parameter(n,"audio_frame",1024,c.audio_frame) || !integer_parameter(n,"audio_hop",256,c.audio_hop) ||
       !integer_parameter(n,"period_window",1024,c.period_window) || !integer_parameter(n,"evaluation_hop",1024,c.evaluation_hop) || !batch_bool(n,"use_onset",true,c.use_onset) || !batch_bool(n,"use_bands",true,c.use_bands))return false;
    if(!integer_parameter(n,"maximum_rows",4096,p.maximum_rows) || !integer_parameter(n,"maximum_matches",65536,p.maximum_matches) || !integer_parameter(n,"maximum_ticks",65536,p.maximum_ticks) ||
       !integer_parameter(n,"half_window",8,p.half) || !integer_parameter(n,"harmonics",4,p.harmonics) || !integer_parameter(n,"first",1,p.first) || !integer_parameter(n,"stop",p.rows,p.stop) ||
       !integer_parameter(n,"leading_zeros",0,p.leading) || !integer_parameter(n,"acquired",p.rows,p.acquired) || !integer_parameter(n,"total_frames",0,p.total_frames) || !integer_parameter(n,"response_length",p.rows,p.response_length))return false;
    if(!scalar_parameter(n,"period",1,p.period) || !scalar_parameter(n,"frame_time_samples",512,p.frame_time) || !scalar_parameter(n,"start_silence_frame",0,p.start_silence) || !scalar_parameter(n,"stop_silence_frame",float(p.total_frames),p.stop_silence) ||
       !scalar_parameter(n,"minimum_interval_seconds",.24f,c.minimum_interval_seconds) || !scalar_parameter(n,"last_beat_interval_seconds",.1f,c.last_beat_interval_seconds))return false;
    c.minimum_bpm=p.pcm.minimum_bpm;c.maximum_bpm=p.pcm.maximum_bpm;
    p.comb={c.period_window,c.audio_hop,c.sample_rate,c.minimum_bpm,c.maximum_bpm};p.ticks={c.evaluation_hop,c.audio_hop,c.sample_rate,float(c.period_window)};
    p.vote={c.sample_rate,1000,c.audio_hop};p.refinement={c.sample_rate/float(c.audio_hop),c.minimum_interval_seconds,c.last_beat_interval_seconds};
    return scalar_parameter(n,"maximum_vote_bpm",1000,p.vote.maximum_bpm);
}
bool silence_config(const domain::ResolvedProcessingNode& n,TrackingPlan& p) noexcept {
    std::size_t boundaries=1,seed=0;
    if(!scalar_parameter(n,"threshold_power",1e-6f,p.silence.threshold_power) || !scalar_parameter(n,"noise_trigger_power",1e-10f,p.silence.noise_trigger_power) || !scalar_parameter(n,"noise_gain",1e-10f,p.silence.noise_gain) ||
       !batch_bool(n,"add_noise",true,p.silence.add_noise) || !integer_parameter(n,"silence_boundaries",1,boundaries) || boundaries>1 || !integer_parameter(n,"noise_seed",0,seed) || seed>UINT32_MAX)return false;
    p.silence.boundaries=SilenceBoundaryRecipe(boundaries);p.silence.noise_seed=std::uint32_t(seed);p.legacy.silence=p.silence;return true;
}
bool extra_inputs(const domain::ResolvedProcessingNode& n,TrackingPlan& p) noexcept {
    std::size_t r=0,c=0;
    if(p.kind==TrackingKind::Renewal){p.inputs=3;return dimension(n,1,Rank::Vector,p.counts[0],c) && dimension(n,2,Rank::Vector,r,c) && r==p.counts[0];}
    if(p.kind==TrackingKind::Agreement){p.inputs=2;return dimension(n,1,Rank::Vector,r,c,DType::UInt64) && r==p.rows;}
    if(p.kind==TrackingKind::MultiBeat){p.inputs=5;p.counts[0]=p.rows;for(std::size_t i=1;i<5;++i)if(!dimension(n,i,Rank::Vector,p.counts[i],c))return false;return true;}
    if(p.kind==TrackingKind::HarmonicComb){p.inputs=2;return dimension(n,1,Rank::Vector,p.counts[0],c);}
    if(p.kind==TrackingKind::PhaseTicks){p.inputs=2;return dimension(n,1,Rank::Vector,r,c) && r==p.rows;}
    return true;
}
bool protocol_input(const domain::ResolvedProcessingNode& n,TrackingPlan& p) noexcept {
    NumericContract input;p.control=numeric_port(n,true,tracking_control_port,input);
    if(p.control && (p.kind!=TrackingKind::Silence || input.dtype!=DType::UInt8 || input.shape.rank!=Rank::Scalar ||
        input.channel_order.size()!=1 || input.representation.normalization!=Normalization::None))return false;
    if(n.inputs.size()!=p.inputs+std::size_t(p.control))return false;
    for(std::size_t i=0;i<n.inputs.size();++i)
        if(n.inputs[i].id!=(i<p.inputs?domain::PortId(i):tracking_control_port))return false;
    return true;
}
}
bool tracking_operation_kind(std::string_view id) noexcept {for(const auto& x:names)if(x.name==id)return true;return false;}
bool tracking_operation_plan(const domain::ResolvedProcessingNode& n,TrackingPlan& p) noexcept {
    bool found=false;for(const auto& x:names)if(x.name==n.kind){p.kind=x.kind;found=true;break;}if(!found)return false;p.global_recipe=n.kind=="GlobalOnset";
    const bool matrix=p.kind==TrackingKind::BandEmphasis || p.kind==TrackingKind::PeriodPath || p.kind==TrackingKind::Agreement;
    if(!dimension(n,0,matrix?Rank::Matrix:Rank::Vector,p.rows,p.columns) || !extra_inputs(n,p))return false;
    if(!config_global(n,p) || !config_period(n,p) || !config_legacy(n,p) || !silence_config(n,p))return false;
    if(p.kind==TrackingKind::HarmonicComb){p.response_length=p.counts[0];if(!integer_parameter(n,"stop",p.response_length/2,p.stop))return false;}
    if(p.kind==TrackingKind::InformationGain && !p.global_recipe && (!integer_parameter(n,"first_bin",0,p.first) || !integer_parameter(n,"stop_bin",p.rows,p.stop)))return false;
    if(p.kind==TrackingKind::BandEmphasis && p.columns!=p.emphasis.bands)return false;
    if(p.kind==TrackingKind::PeriodPath && p.columns!=p.path.frame_length)return false;
    if(p.kind==TrackingKind::CombPeriod && p.rows!=p.comb.number_frames)return false;
    if(p.kind==TrackingKind::BandFeatures){const auto gains=array_parameter(n,"gains");if(p.rows!=(gains.empty()?8:gains.size()))return false;}
    if(p.kind==TrackingKind::PhaseTicks){NumericContract a,b;if(!numeric_port(n,true,0,a) || !numeric_port(n,true,1,b) || !equal_frame_clock(a.clock,b.clock) || a.coordinates!=b.coordinates)return false;}
    if(p.global_recipe || p.kind==TrackingKind::PcmBeat || p.kind==TrackingKind::Legacy){
        NumericContract input;if(!numeric_port(n,true,0,input))return false;
        const auto rate=p.kind==TrackingKind::PcmBeat?44100.f:p.kind==TrackingKind::Legacy?p.legacy.sample_rate:p.global.sample_rate;
        if((p.kind==TrackingKind::PcmBeat && p.global.sample_rate!=44100) || (input.clock.sample_rate && double(input.clock.sample_rate)!=double(rate)))return false;
    }
    p.frame=(!p.global_recipe && p.kind==TrackingKind::InformationGain) || p.kind==TrackingKind::BandFeatures || p.kind==TrackingKind::FeaturePeriod || p.kind==TrackingKind::PhaseTicks || p.kind==TrackingKind::Silence || p.kind==TrackingKind::LagPool;
    return protocol_input(n,p);
}
} // namespace rhythm_graph_adapters::detail
