// SPDX-License-Identifier: AGPL-3.0-or-later
#include "batch_operation_plan.h"
#include "frame_operation_io.h"
#include <cmath>
#include <climits>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace rhythm_reference_ports;
namespace {
struct Kind {std::string_view name;BatchKind kind;};
constexpr Kind kinds[]{{"PulseTempo",BatchKind::PulseTempo},{"PulseOnset",BatchKind::PulseOnset},{"PulseCorrelation",BatchKind::PulseCorrelation},{"PulseTrainScoring",BatchKind::PulseTrain},
    {"LagTempoConsensus",BatchKind::LagConsensus},{"BandNovelty",BatchKind::BandNovelty},{"FixedTempoCandidates",BatchKind::FixedCandidates},{"FourierTempoCandidates",BatchKind::FourierCandidates},
    {"WeightedTempoHistogram",BatchKind::WeightedHistogram},{"FrameTempoPath",BatchKind::FramePath},{"PhaseTempoPulse",BatchKind::PhasePulse},{"FourierTempoHistogram",BatchKind::FourierHistogram},{"NoveltyTempoHistogram",BatchKind::NoveltyHistogram}};
bool dimensions(const domain::ResolvedProcessingNode& n,domain::PortId id,Rank rank,std::size_t& a,std::size_t& b,DType dtype=DType::Float32) noexcept {
    NumericContract c;if(!numeric_port(n,true,id,c) || c.dtype!=dtype || c.shape.rank!=rank || c.channel_order.size()!=1 || c.representation.encoding!=StorageEncoding::Native || c.representation.valid_bits || c.representation.layout!=ChannelLayout::Planar)return false;
    if(c.shape.extents[0].maximum>SIZE_MAX || c.shape.extents[1].maximum>SIZE_MAX)return false;
    a=std::size_t(c.shape.extents[0].maximum);b=std::size_t(c.shape.extents[1].maximum);return a && b;
}
bool pulse_config(const domain::ResolvedProcessingNode& n,BatchPlan& p) noexcept {
    std::size_t fs=44100,lo=50,hi=210;
    if(!integer_parameter(n,"sample_rate",44100,fs) || fs>INT_MAX || !integer_parameter(n,"minimum_bpm",50,lo) || lo>INT_MAX || !integer_parameter(n,"maximum_bpm",210,hi) || hi>INT_MAX ||
       !integer_parameter(n,"frame_size",1024,p.pulse.frame_size) || !integer_parameter(n,"audio_hop",128,p.pulse.audio_hop) || !integer_parameter(n,"onset_window",2048,p.pulse.onset_window) || !integer_parameter(n,"onset_hop",128,p.pulse.onset_hop))return false;
    p.pulse.sample_rate=int(fs);p.pulse.minimum_bpm=int(lo);p.pulse.maximum_bpm=int(hi);
    const auto minimum=array_parameter(n,"classifier_minimum"),maximum=array_parameter(n,"classifier_maximum"),weights=array_parameter(n,"classifier_weights");
    if((!minimum.empty() && minimum.size()!=3) || (!maximum.empty() && maximum.size()!=3) || (!weights.empty() && weights.size()!=12))return false;
    for(std::size_t i=0;i<3;++i){if(!minimum.empty())p.classifier.minimum[i]=minimum[i];if(!maximum.empty())p.classifier.maximum[i]=maximum[i];for(std::size_t j=0;j<4;++j)if(!weights.empty())p.classifier.weights[i][j]=weights[4*i+j];}
    return true;
}
bool path_config(const domain::ResolvedProcessingNode& n,BatchPlan& p) noexcept {
    auto& c=p.histogram.path;
    return scalar_parameter(n,"frequency_step",1,c.frequency_step) && scalar_parameter(n,"frame_rate",44100.f/512.f,c.frame_rate) &&
        scalar_parameter(n,"minimum_bpm",30,c.minimum_bpm) && scalar_parameter(n,"maximum_bpm",560,c.maximum_bpm) && scalar_parameter(n,"tolerance_percent",3,c.tolerance_percent) &&
        scalar_parameter(n,"minimum_change_seconds",5,c.minimum_change_seconds) && scalar_parameter(n,"induced_bpm",0,c.induced_bpm) && integer_parameter(n,"hop",32,c.hop) &&
        batch_bool(n,"constant_tempo",false,c.constant_tempo) && integer_parameter(n,"maximum_peaks",50,p.histogram.maximum_peaks) && batch_bool(n,"weight_by_magnitude",true,p.histogram.weight_by_magnitude);
}
bool other_config(const domain::ResolvedProcessingNode& n,BatchPlan& p) noexcept {
    std::size_t weighting=9,window=0;
    if(!scalar_parameter(n,"onset_frame_rate",44100.f/128.f,p.rate) || !scalar_parameter(n,"sample_rate",44100,p.fixed.sample_rate) ||
       !scalar_parameter(n,"minimum_bpm",30,p.fixed.minimum_bpm) || !scalar_parameter(n,"maximum_bpm",560,p.fixed.maximum_bpm) || !scalar_parameter(n,"tolerance_percent",3,p.fixed.tolerance_percent) ||
       !integer_parameter(n,"hop_samples",512,p.fixed.hop_samples) || !integer_parameter(n,"weighting",9,weighting) || weighting>9 || !batch_bool(n,"normalize_bands",false,p.band.normalize_bands))return false;
    p.band.weighting=NoveltyWeight(weighting);p.band.frame_rate=p.histogram.path.frame_rate;
    auto& c=p.novelty;c.frame_rate=p.histogram.path.frame_rate;c.minimum_bpm=p.histogram.path.minimum_bpm;c.maximum_bpm=p.histogram.path.maximum_bpm;
    c.minimum_change_seconds=p.histogram.path.minimum_change_seconds;c.induced_bpm=p.histogram.path.induced_bpm;c.constant_tempo=p.histogram.path.constant_tempo;c.maximum_peaks=p.histogram.maximum_peaks;c.weight_by_magnitude=p.histogram.weight_by_magnitude;
    if(!scalar_parameter(n,"frame_seconds",4,c.frame_seconds) || !scalar_parameter(n,"overlap",16,c.overlap) || !integer_parameter(n,"zero_padding_factor",0,c.zero_padding_factor) || !integer_parameter(n,"window",0,window) || window>8)return false;
    c.window=SpectralWindowShape(window);return true;
}
bool secondary_geometry(const domain::ResolvedProcessingNode& n,BatchPlan& p) noexcept {
    std::size_t a=0,b=0;
    if(p.kind==BatchKind::PulseTrain)return dimensions(n,1,Rank::Vector,p.secondary,b);
    if(p.kind==BatchKind::WeightedHistogram){
        const auto maximum=std::ceil(p.histogram.path.maximum_bpm);
        return maximum>=0 && double(maximum)<double(SIZE_MAX/sizeof(float))-2 && p.columns==2 && dimensions(n,1,Rank::Matrix,p.secondary,b) && b==std::size_t(maximum)+1;
    }
    if(p.kind==BatchKind::FourierHistogram)return dimensions(n,1,Rank::Matrix,a,b) && a==p.rows && b==p.columns;
    if(p.kind==BatchKind::PhasePulse)return dimensions(n,1,Rank::Vector,a,b) && a==p.rows;
    if(p.kind==BatchKind::FramePath)return p.columns%2==0 && dimensions(n,1,Rank::Vector,a,b,DType::UInt64) && a==p.rows && dimensions(n,2,Rank::Scalar,a,b);
    return true;
}
bool geometry(const domain::ResolvedProcessingNode& n,BatchPlan& p) noexcept {
    const bool matrix=p.kind==BatchKind::BandNovelty || p.kind==BatchKind::WeightedHistogram || p.kind==BatchKind::FramePath || p.kind==BatchKind::PhasePulse || p.kind==BatchKind::FourierHistogram;
    if(!dimensions(n,0,matrix?Rank::Matrix:Rank::Vector,p.rows,p.columns,p.kind==BatchKind::LagConsensus?DType::Int32:DType::Float32))return false;
    p.frames=p.rows;p.bins=p.columns;
    if(p.kind==BatchKind::PulseOnset){p.bins=p.rows;return p.rows==p.pulse.frame_size/2+1;}
    return secondary_geometry(n,p);
}
bool phase_contracts(const domain::ResolvedProcessingNode& n,const BatchPlan& p) noexcept {
    if(p.kind!=BatchKind::FourierHistogram && p.kind!=BatchKind::PhasePulse)return true;
    NumericContract a,b;if(!numeric_port(n,true,0,a) || !numeric_port(n,true,1,b) || !equal_frame_clock(a.clock,b.clock))return false;
    if(p.kind==BatchKind::FourierHistogram && a.coordinates!=b.coordinates)return false;
    if(p.kind==BatchKind::PhasePulse && a.coordinates[0]!=b.coordinates[0])return false;
    const auto& frequency=a.coordinates[1];
    if(frequency.mode!=CoordinateMode::Regular || frequency.unit!=NumericUnit::Hertz)return true;
    if(frequency.origin.numerator || !frequency.origin.denominator || frequency.step.numerator<=0 || !frequency.step.denominator)return false;
    const double step=double(frequency.step.numerator)/double(frequency.step.denominator),expected=p.histogram.path.frequency_step;
    return std::abs(step-expected)<=1e-6*std::max(1.,std::abs(expected));
}
}
bool batch_bool(const domain::ResolvedProcessingNode& n,std::string_view key,bool fallback,bool& out) noexcept {
    const auto* p=parameter(n,key);if(!p){out=fallback;return true;}const auto* v=std::get_if<bool>(p);if(!v)return false;out=*v;return true;
}
bool batch_operation_kind(std::string_view id) noexcept {for(const auto& k:kinds)if(k.name==id)return true;return false;}
bool batch_operation_plan(const domain::ResolvedProcessingNode& n,BatchPlan& p) noexcept {
    bool found=false;for(const auto& k:kinds)if(k.name==n.kind){p.kind=k.kind;found=true;break;}
    if(!found)return false;
    if(p.kind==BatchKind::PulseTempo || p.kind==BatchKind::PulseOnset || p.kind==BatchKind::LagConsensus){if(!pulse_config(n,p))return false;}
    else if(p.kind==BatchKind::PulseCorrelation){
        if(!scalar_parameter(n,"onset_frame_rate",44100.f/128.f,p.rate) || !scalar_parameter(n,"minimum_bpm",50,p.histogram.path.minimum_bpm) || !scalar_parameter(n,"maximum_bpm",210,p.histogram.path.maximum_bpm))return false;
    }else if(p.kind!=BatchKind::PulseTrain && (!path_config(n,p) || !other_config(n,p)))return false;
    if(p.kind==BatchKind::BandNovelty && !scalar_parameter(n,"frame_rate",44100.f/128.f,p.band.frame_rate))return false;
    if(p.kind==BatchKind::LagConsensus && !scalar_parameter(n,"onset_frame_rate",44100.f/128.f,p.rate))return false;
    return geometry(n,p) && phase_contracts(n,p);
}
}
