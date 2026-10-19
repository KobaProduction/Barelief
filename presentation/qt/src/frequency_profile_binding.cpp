#include <core_app/frequency_profile_binding.h>
#include <barelief/ui_text.h>
#include <core_app/resampler_presentation.h>
#include <core_app/spectrum_projection_ui.h>
#include <array>
#include <algorithm>
#include <optional>
namespace core_app {
namespace {
QString resampler_name(dsp_kernels::ResamplerMode mode){return ui::text(presentation::resampler_caption(mode));}
std::optional<dsp_kernels::ProfileCompatibility> to_compatibility(const application::ProfileFrontend& declared){
    dsp_kernels::ProfileCompatibility result;
    result.sample_rate=declared.sample_rate;result.fft_size=declared.fft_size;
    result.coherent_gain=declared.coherent_gain;result.power_gain=declared.power_gain;
    result.include_dc=declared.include_dc;result.include_nyquist=declared.include_nyquist;
    result.projection.compensate=declared.normalize;
    result.channel_policy=declared.channel_policy=="select"?dsp_kernels::ChannelPolicy::Select:dsp_kernels::ChannelPolicy::Mean;
    result.channel_index=declared.channel_index;result.resampler_quality=declared.resampler_quality;
    if(declared.channel_policy!="mean" && declared.channel_policy!="select")return std::nullopt;
    if(!dsp_kernels::parse_window_function(declared.window_function,result.window))return std::nullopt;
    if(!dsp_kernels::parse_spectrum_projection(declared.representation,result.projection.representation))return std::nullopt;
    if(!signal_contracts::parse_resampler_mode(declared.resampler_mode,result.resampler_mode))return std::nullopt;
    return result;
}
dsp_kernels::ProfileTraining to_training(const application::ProfileTrainingCadence& declared){
    dsp_kernels::ProfileTraining result;
    result.interval_numerator=declared.interval_numerator;
    result.interval_denominator=declared.interval_denominator;
    return result;
}
application::ProfileMismatchReason to_app_reason(dsp_kernels::ProfileMismatchReason reason){
    using R=application::ProfileMismatchReason;
    static constexpr std::array reasons{R::InvalidSettings,R::InvalidCadence,R::SampleRate,R::FftSize,
        R::Window,R::CoherentGain,R::PowerGain,R::Projection,R::DcLayout,R::NyquistLayout,
        R::ChannelPolicy,R::ChannelIndex,R::ResamplerMode,R::ResamplerQuality,
        R::ResamplerDesignVersion,R::Reference,R::Floor,R::FrequencyGridLength,
        R::FrequencyPresence,R::InvalidCoefficient};
    const auto index=std::size_t(reason);
    return index<reasons.size()?reasons[index]:R::InvalidSettings;
}
dsp_kernels::ProfileMismatchReason to_dsp_reason(application::ProfileMismatchReason reason){
    using R=dsp_kernels::ProfileMismatchReason;
    static constexpr std::array reasons{R::InvalidSettings,R::InvalidCadence,R::SampleRate,R::FftSize,
        R::Window,R::CoherentGain,R::PowerGain,R::Projection,R::DcLayout,R::NyquistLayout,
        R::ChannelPolicy,R::ChannelIndex,R::ResamplerMode,R::ResamplerQuality,
        R::ResamplerDesignVersion,R::Reference,R::Floor,R::FrequencyGridLength,
        R::FrequencyPresence,R::InvalidCoefficient};
    const auto index=std::size_t(reason);
    return index<reasons.size()?reasons[index]:R::InvalidSettings;
}
}
bool DspProfileMatcher::valid_declaration(const application::ProfileRoute& route) const {
    const auto frontend=to_compatibility(route.frontend);
    if(!frontend)return false;
    std::string error;
    return dsp_kernels::valid_profile_settings(*frontend,to_training(route.training),error);
}
std::vector<application::ProfileMismatchReason> DspProfileMatcher::mismatches(const application::ProfileRoute& route,
    std::span<const float> gains) const {
    const auto frontend=to_compatibility(route.frontend);
    if(!frontend || !profile_ || !std::ranges::equal(gains,profile_->gains))
        return {application::ProfileMismatchReason::InvalidCoefficient};
    std::vector<application::ProfileMismatchReason> result;
    for(const auto reason:dsp_kernels::profile_mismatches(*profile_,*frontend))result.push_back(to_app_reason(reason));
    return result;
}
QString frequency_profile_target_caption(const FrequencyProfileTarget& target){
    const auto& c=target.options.compatibility;
    const auto representation=dsp_kernels::spectrum_projection_name(c.projection.representation);
    return ui::text("Coefficients #%1 → spectrum #%2 (%3 Hz, N=%4, %5)").arg(target.coefficients).arg(target.arithmetic).arg(c.sample_rate).arg(c.fft_size).arg(QString::fromUtf8(representation));
}
std::vector<FrequencyProfileTarget> frequency_profile_targets(const domain::NumericProcessingGraph& graph){
    std::vector<FrequencyProfileTarget> result;
    for(auto route:application::frequency_profile_routes(graph,DspProfileMatcher{})){
        FrequencyProfileTarget target;
        target.route=std::move(route);
        target.coefficients=target.route.coefficients;
        target.arithmetic=target.route.arithmetic;
        target.gain_first_bin=target.route.gain_first_bin;
        target.gain_count=target.route.gain_count;
        target.options.compatibility=*to_compatibility(target.route.frontend);
        target.options.training=to_training(target.route.training);
        target.label=frequency_profile_target_caption(target);
        result.push_back(std::move(target));
    }
    return result;
}
namespace {
using ProfileReason=dsp_kernels::ProfileMismatchReason;
using ProfileFrontend=dsp_kernels::ProfileCompatibility;
QString mismatch_caption(ProfileReason reason){
    static constexpr const char* keys[]{"Invalid profile settings","Invalid profile cadence","Sample rate, Hz","FFT size","Window function","Coherent gain, C","Power gain, E","Spectral representation","DC layout","Nyquist layout","Channel policy","Channel index","Resampling method","Resampler quality","Resampler design version","Spectral reference","Floor, dB","Profile frequency grid length","Profile frequency presence","Invalid profile coefficient"};
    const auto index=std::size_t(reason);return ui::text(index<std::size(keys)?keys[index]:"Invalid profile settings");
}
QString numeric_profile_field(ProfileReason reason,const ProfileFrontend& value){
    switch(reason){
    case ProfileReason::SampleRate:return QString::number(value.sample_rate,'g',12);
    case ProfileReason::FftSize:return QString::number(value.fft_size);
    case ProfileReason::CoherentGain:return QString::number(dsp_kernels::profile_window_scaling(value).sum,'g',12);
    case ProfileReason::PowerGain:return QString::number(dsp_kernels::profile_window_scaling(value).sum_squares,'g',12);
    case ProfileReason::ChannelIndex:return QString::number(value.channel_index);
    case ProfileReason::ResamplerQuality:return QString::number(value.resampler_quality);
    case ProfileReason::ResamplerDesignVersion:return QString::number(value.resampler_design_version);
    case ProfileReason::Reference:return QString::number(value.reference,'g',12);
    case ProfileReason::Floor:return QString::number(value.floor_db,'g',12);
    default:return {};
    }
}
QString profile_field(ProfileReason reason,const ProfileFrontend& value){
    using namespace dsp_kernels;
    switch(reason){
    case ProfileReason::Window:return ui::text(window_function_name(value.window).data());
    case ProfileReason::Projection:return ui::text(presentation::spectrum_projection_caption(value.projection.representation))+" / "+ui::text(value.projection.compensate?"On":"Off");
    case ProfileReason::ChannelPolicy:return ui::text(value.channel_policy==ChannelPolicy::Mean?"Mean channels":"Selected channel");
    case ProfileReason::ResamplerMode:return resampler_name(value.resampler_mode);
    case ProfileReason::DcLayout:return ui::text(value.include_dc?"Included":"Excluded");
    case ProfileReason::NyquistLayout:return ui::text(value.include_nyquist?"Included":"Excluded");
    default:return numeric_profile_field(reason,value);
    }
}
QString mismatch_detail(ProfileReason reason,const ProfileFrontend& stored,const ProfileFrontend& expected){
    const auto caption=mismatch_caption(reason),a=profile_field(reason,stored),b=profile_field(reason,expected);
    return a.isEmpty()&&b.isEmpty()?caption:ui::text("%1: profile %2; target %3").arg(caption,a,b);
}
}
QString frequency_profile_incompatibility(const FrequencyProfileTarget& target,const dsp_kernels::FrequencyProfile& profile){
    QStringList reasons;
    for(const auto reason:dsp_kernels::profile_mismatches(profile,target.options.compatibility))reasons.append(mismatch_detail(reason,profile.compatibility,target.options.compatibility));
    return reasons.join('\n');
}
bool frequency_profile_edits(const domain::NumericProcessingGraph& graph,const FrequencyProfileTarget& selected,const dsp_kernels::FrequencyProfile& profile,
    domain::ConfiguredProcessingNode& coefficients,domain::ConfiguredProcessingNode& arithmetic,QString& error){
    auto result=application::apply_frequency_profile(graph,selected.route,profile.gains,DspProfileMatcher{&profile});
    switch(result.refusal){
    case application::ProfileApplyRefusal::None:break;
    case application::ProfileApplyRefusal::RouteUnavailable:
        error=ui::text("The coefficient route is no longer available");return false;
    case application::ProfileApplyRefusal::SharedCoefficients:
        error=ui::text("The coefficient source feeds multiple routes; select an isolated source");return false;
    case application::ProfileApplyRefusal::CoefficientRange:
        error=ui::text("Profile coefficient range does not match the declared array size");return false;
    case application::ProfileApplyRefusal::ProfileMismatch:
        break;
    }
    if(result.refusal==application::ProfileApplyRefusal::ProfileMismatch){
        QStringList reasons;
        const auto expected=to_compatibility(result.expected_frontend);
        for(const auto reason:result.mismatches)reasons.append(mismatch_detail(to_dsp_reason(reason),profile.compatibility,*expected));
        error=reasons.join('\n');return false;
    }
    error.clear();
    coefficients=std::move(result.coefficients);
    arithmetic=std::move(result.arithmetic);
    return true;
}
}
