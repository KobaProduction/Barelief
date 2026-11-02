#include <domain/numeric/sequence_operations.h>
#include <domain/graph/processing_model.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <new>

namespace domain {
namespace {
using F=SequenceOperationFamily;
// Source chains are the existing portable pinned-source implementations. No
// row here asserts numerical parity or real-time confirmed beat semantics.
constexpr SequenceOperationSpec catalogue[]{
    {"SpectralNovelty",F::Spectral,2,5,"explicit raw feature/phase rows -> twelve spectral novelty modes; differences/phase mask/alignment intermediates"},
    {"PcmSpectralNovelty",F::Spectral,1,9,"PCM -> explicit source framing/window/raw Fourier/filter/log -> spectral novelty; last spectral and novelty intermediates"},
    {"ActivationPeakMatrix",F::Spectral,1,2,"activation matrix -> independent time-axis input-sized smoothing/picking -> row-major frame/channel indices"},
    {"OnsetStrengthSequence",F::Onset,2,1,"spectral difference -> explicit feature groups -> aggregation -> detrend"},
    {"OnsetDetection",F::Onset,2,1,"envelope normalization -> peak picker -> optional backtrack -> event coordinates"},
    {"OnsetBacktrack",F::Onset,2,1,"explicit sparse event indices -> energy minima"},
    {"PcmOnsetEnvelope",F::Onset,1,1,"PCM -> spectral frame -> power Mel -> measured dB -> onset strength -> scalar envelope"},
    {"TempoEstimate",F::Tempo,2,1,"explicit ACF rows -> time aggregation -> prior -> lag argmax -> BPM"},
    {"TempoRatios",F::Tempo,4,1,"explicit tempogram -> tempo -> curve interpolation -> harmonic ratios -> time reduction"},
    {"MetricalTempogram",F::Tempo,2,1,"explicit tempogram -> harmonic interpolation -> explicit tempo-axis reduction"},
    {"EnvelopeTempo",F::Tempo,2,1,"envelope -> ACF tempogram -> tempo estimator"},
    {"EnvelopeTempoRatios",F::Tempo,4,1,"envelope -> ACF tempogram -> tempo ratios"},
    {"PcmTempo",F::Tempo,2,2,"PCM onset envelope -> envelope tempo; onset intermediate"},
    {"PcmTempogram",F::Tempo,1,4,"PCM onset envelope -> tempogram real/imaginary matrices; onset/BPM intermediates"},
    {"PcmTempoRatios",F::Tempo,4,2,"PCM onset envelope -> envelope tempo ratios; onset intermediate"},
    {"DynamicBeatPath",F::BeatPulse,2,4,"envelope + explicit BPM -> local scores -> dynamic program -> trim; score/backlink intermediates"},
    {"EnvelopeBeatPath",F::BeatPulse,3,5,"envelope -> optional tempo -> dynamic beat path; BPM/score/backlink intermediates"},
    {"PcmBeatPath",F::BeatPulse,3,3,"PCM median onset envelope -> envelope beat path; BPM/onset intermediates"},
    {"LocalPulse",F::BeatPulse,3,1,"explicit real/imaginary Fourier matrices -> lazy complex rows -> phase selection -> inverse -> overlap-add -> normalize"},
    {"EnvelopeLocalPulse",F::BeatPulse,2,2,"envelope -> complex Fourier tempogram -> local pulse; BPM grid"},
    {"PcmLocalPulse",F::BeatPulse,2,3,"PCM onset envelope -> envelope local pulse; onset/BPM intermediates"},
    {"TempoHistogram",F::Histogram,1,2,"activation -> ACF/comb/explicit-probability HMM histogram; original interval coordinates"},
    {"TempoAnalysis",F::Histogram,1,4,"activation smoothing -> tempo histogram -> histogram smoothing -> candidate peaks"},
    {"BeatTrackingSequence",F::Histogram,1,3,"activation smoothing -> histogram -> global/local interval beat search"},
    {"CrfBeatSequence",F::Histogram,1,5,"tempo candidates -> trial intervals -> CRF sequence search -> normalized scores"},
    {"BeatStateSequence",F::State,1,4,"explicit beat probabilities -> sparse HMM -> corrected beat events; state path/BPM/path origin"},
    {"DownbeatStateSequence",F::State,1,6,"explicit beat/downbeat probabilities -> independent meter HMMs -> selected path/events/path origin"},
    {"BarStateSequence",F::State,2,2,"beat times + explicit downbeat probabilities -> bar HMM -> beat numbers/state path"},
    {"GaussianMixtureScore",F::Model,1,3,"explicit classical GMM parameters -> component log densities -> log-sum-exp/responsibilities"},
    {"PatternStateSequence",F::Model,1,4,"explicit features -> classical GMM observations -> stacked pattern HMM -> beat changes"},
    {"PatternFeatures",F::Model,1,1,"PCM -> source spectral/filter/log/difference/group recipe"},
    {"PcmPatternSequence",F::Model,1,4,"PCM -> pattern features -> supplied classical GMM pattern HMM -> beat changes"},
    {"BeatFeatureSynchronization",F::Model,2,3,"explicit feature rows + beat times -> source subdivisions -> means/empty diagnostics"},
    {"ModelPredictionSelection",F::Model,1,4,"explicit predictions -> reference average -> candidate SSE -> strict first-best selection"},
    {"TempoProbabilityHistogram",F::Model,1,2,"explicit tempo-class probabilities -> source BPM slice + coordinates"}
};
using C=ParameterClassification;using V=ParameterValueKind;
constexpr ParameterDescriptor onset[]{
    {"frame_rate",C::Processing,V::Number,.000001,192000,120,{}},
    {"sample_rate",C::Processing,V::Number,1,192000,44100,{}},
    {"hop_samples",C::Processing,V::Integer,1,65536,512,{}},
    {"lag",C::Processing,V::Integer,1,65536,1,{}},
    {"max_size",C::Processing,V::Integer,1,65536,1,{}},
    {"n_fft",C::Processing,V::Integer,2,65536,2048,{}},
    {"center",C::Processing,V::Boolean,0,1,1,{}},
    {"detrend",C::Processing,V::Boolean,0,1,0,{}},
    {"aggregate",C::Processing,V::Integer,0,2,0,{}},
    {"aggregate_features",C::Processing,V::Boolean,0,1,1,{}},
    {"group_offsets",C::Processing,V::NumericArray,0,0,0,{}},
    {"group_indices",C::Processing,V::NumericArray,0,0,0,{}},
    {"sparse",C::Processing,V::Boolean,0,1,1,{}},
    {"backtrack",C::Processing,V::Boolean,0,1,0,{}},
    {"units",C::Processing,V::Integer,0,2,0,{}},
    {"custom_peaks",C::Processing,V::Boolean,0,1,0,{}},
    {"pre_max",C::Processing,V::Integer,0,65536,1,{}},
    {"post_max",C::Processing,V::Integer,1,65536,1,{}},
    {"pre_average",C::Processing,V::Integer,0,65536,1,{}},
    {"post_average",C::Processing,V::Integer,1,65536,1,{}},
    {"wait",C::Processing,V::Integer,0,65536,0,{}},
    {"delta",C::Processing,V::Number,0,1e20,.07,{}},
    {"normalize",C::Processing,V::Boolean,0,1,1,{}},
    {"selection",C::Processing,V::Integer,0,2,0,{}}
};
constexpr ParameterDescriptor tempo[]{
    {"frame_rate",C::Processing,V::Number,.000001,192000,44100./512,{}},
    {"start_bpm",C::Processing,V::Number,.000001,1e6,120,{}},
    {"std_bpm",C::Processing,V::Number,.000001,1e6,1,{}},
    {"max_tempo",C::Processing,V::Number,.000001,1e6,320,{}},
    {"limit_max_tempo",C::Processing,V::Boolean,0,1,1,{}},
    {"aggregation",C::Processing,V::Integer,0,2,1,{}},
    {"sample_rate",C::Processing,V::Number,1,192000,44100,{}},
    {"hop_samples",C::Processing,V::Integer,1,65536,512,{}},
    {"autocorrelation_seconds",C::Processing,V::Number,.000001,3600,8,{}},
    {"factors",C::Processing,V::NumericArray,0,0,0,{}},
    {"coordinates",C::Processing,V::NumericArray,0,0,0,{}},
    {"fill",C::Processing,V::Number,-1e30,1e30,0,{}},
    {"interpolation",C::Processing,V::Integer,0,8,0,{}},
    {"spline_order",C::Processing,V::Integer,0,16,1,{}},
    {"reduction",C::Processing,V::Integer,0,3,0,{}},
    {"dynamic_frequencies",C::Processing,V::Boolean,0,1,0,{}}
};
constexpr ParameterDescriptor beat[]{
    {"samples",C::Processing,V::Integer,1,65536,512,{}},
    {"frame_rate",C::Processing,V::Number,.000001,192000,44100./512,{}},
    {"tightness",C::Processing,V::Number,.000001,1e20,100,{}},
    {"maximum_period_frames",C::Processing,V::Integer,2,65536,2048,{}},
    {"trim",C::Processing,V::Boolean,0,1,1,{}},
    {"window_length",C::Processing,V::Integer,2,16384,384,{}},
    {"hop",C::Processing,V::Integer,1,65536,1,{}},
    {"center",C::Processing,V::Boolean,0,1,1,{}},
    {"coefficients",C::Processing,V::NumericArray,0,0,0,{}},
    {"minimum_bpm",C::Processing,V::Number,0,1e6,30,{}},
    {"maximum_bpm",C::Processing,V::Number,0,1e6,300,{}},
    {"sample_rate",C::Processing,V::Number,1,192000,44100,{}},
    {"hop_samples",C::Processing,V::Integer,1,65536,512,{}},
    {"sparse",C::Processing,V::Boolean,0,1,1,{}},
    {"units",C::Processing,V::Integer,0,2,0,{}}
};
constexpr ParameterDescriptor model[]{
    {"model",C::Processing,V::String,0,0,0,{}},
    {"frame_rate",C::Processing,V::Number,.000001,192000,100,{}},
    {"reference_count",C::Processing,V::Integer,0,65536,0,{}},
    {"minimum_bpm",C::Processing,V::Number,0,1e6,40,{}},
    {"maximum_bpm",C::Processing,V::Number,0,1e6,250,{}},
    {"subdivisions",C::Processing,V::Integer,1,65536,4,{}},
    {"empty",C::Processing,V::Integer,0,1,0,{}},
    {"diagonal_retry",C::Processing,V::Number,0,1e10,1e-7,{}}
};
constexpr ParameterDescriptor pcm[]{
    {"sample_rate",C::Processing,V::Number,1,192000,44100,{}},
    {"n_fft",C::Processing,V::Integer,2,65536,2048,{}},
    {"hop_samples",C::Processing,V::Integer,1,65536,512,{}},
    {"mel_bands",C::Processing,V::Integer,1,65536,128,{}},
    {"mel_scale",C::Processing,V::Integer,0,1,1,{}},
    {"mel_normalization",C::Processing,V::Integer,0,6,0,{}},
    {"mel_minimum_hz",C::Processing,V::Number,0,192000,0,{}},
    {"mel_maximum_hz",C::Processing,V::Number,0,192000,0,{}},
    {"spectral_window",C::Processing,V::NumericArray,0,0,0,{}},
    {"onset_aggregate",C::Processing,V::Integer,0,2,0,{}},
    {"lag",C::Processing,V::Integer,1,65536,1,{}},
    {"max_size",C::Processing,V::Integer,1,65536,1,{}},
    {"onset_center",C::Processing,V::Boolean,0,1,1,{}},
    {"detrend",C::Processing,V::Boolean,0,1,0,{}},
    {"db_reference",C::Processing,V::Number,-1e20,1e20,1,{}},
    {"db_amin",C::Processing,V::Number,1e-30,1e20,1e-10,{}},
    {"top_db",C::Processing,V::Number,0,1e20,80,{}},
    {"top_db_enabled",C::Processing,V::Boolean,0,1,1,{}}
};
constexpr ParameterDescriptor histogram[]{
    {"method",C::Processing,V::Integer,0,2,1,{}},
    {"frame_rate",C::Processing,V::Number,.000001,192000,100,{}},
    {"minimum_bpm",C::Processing,V::Number,.000001,1e6,40,{}},
    {"maximum_bpm",C::Processing,V::Number,.000001,1e6,250,{}},
    {"history_seconds",C::Processing,V::Number,.000001,3600,10,{}},
    {"alpha",C::Processing,V::Number,-1e20,1e20,.79,{}},
    {"channels",C::Processing,V::Integer,1,32,1,{}},
    {"online",C::Processing,V::Boolean,0,1,0,{}},
    {"reset_state",C::Processing,V::Boolean,0,1,1,{}},
    {"transition_lambda",C::Processing,V::Number,-1e20,1e20,100,{}},
    {"observation_lambda",C::Processing,V::Number,1.000001,1e20,16,{}},
    {"transition_floor",C::Processing,V::Number,0,.999999,2.220446049250313e-16,{}},
    {"num_intervals",C::Processing,V::Integer,0,65536,0,{}},
    {"fixed_interval",C::Processing,V::Boolean,0,1,0,{}},
    {"comb_alpha",C::Processing,V::NumericArray,0,0,0,{}},
    {"activation_smoothing_seconds",C::Processing,V::Number,0,3600,.14,{}},
    {"histogram_smoothing",C::Processing,V::Integer,0,65536,9,{}},
    {"interpolate",C::Processing,V::Boolean,0,1,0,{}},
    {"smooth_state_model_activations",C::Processing,V::Boolean,0,1,0,{}},
    {"activation_kernel",C::Processing,V::NumericArray,0,0,0,{}},
    {"histogram_kernel",C::Processing,V::NumericArray,0,0,0,{}},
    {"look_aside",C::Processing,V::Number,0,1,.2,{}},
    {"look_ahead_seconds",C::Processing,V::Number,.000001,3600,10,{}},
    {"variable_tempo",C::Processing,V::Boolean,0,1,1,{}},
    {"interval_sigma",C::Processing,V::Number,.000001,1e20,.18,{}},
    {"use_factors",C::Processing,V::Boolean,0,1,0,{}},
    {"num_candidates",C::Processing,V::Integer,1,65536,5,{}},
    {"factors",C::Processing,V::NumericArray,0,0,0,{}}
};
constexpr ParameterDescriptor state[]{
    {"model",C::Processing,V::String,0,0,0,{}},
    {"frame_rate",C::Processing,V::Number,.000001,192000,100,{}},
    {"minimum_bpm",C::Processing,V::Number,.000001,1e6,55,{}},
    {"maximum_bpm",C::Processing,V::Number,.000001,1e6,215,{}},
    {"transition_lambda",C::Processing,V::Number,-1e20,1e20,100,{}},
    {"observation_lambda",C::Processing,V::Number,1.000001,1e20,16,{}},
    {"transition_floor",C::Processing,V::Number,0,.999999,2.220446049250313e-16,{}},
    {"activation_threshold",C::Processing,V::Number,0,1,0,{}},
    {"num_intervals",C::Processing,V::Integer,0,65536,0,{}},
    {"correct",C::Processing,V::Boolean,0,1,1,{}},
    {"fixed_interval",C::Processing,V::Boolean,0,1,0,{}},
    {"online",C::Processing,V::Boolean,0,1,0,{}},
    {"reset_state",C::Processing,V::Boolean,0,1,1,{}},
    {"observation_weight",C::Processing,V::Number,1.000001,1e20,100,{}},
    {"meter_change_probability",C::Processing,V::Number,0,1,1e-7,{}}
};
template<std::size_t N,std::size_t M>
consteval auto select(const ParameterDescriptor (&source)[M],const std::array<std::string_view,N>& keys) {
    std::array<ParameterDescriptor,N> result{};
    for(std::size_t i=0;i<N;++i){
        const auto found=std::find_if(std::begin(source),std::end(source),[&](const auto& d){return d.key==keys[i];});
        if(found==std::end(source)){
#if defined(__cpp_exceptions)
            throw "unknown sequence parameter";
#else
            std::abort();
#endif
        }
        result[i]=*found;
    }
    return result;
}
using namespace std::literals;
template<std::size_t A,std::size_t B> consteval auto join(const std::array<ParameterDescriptor,A>& a,const std::array<ParameterDescriptor,B>& b) {
    std::array<ParameterDescriptor,A+B> result{};std::copy(a.begin(),a.end(),result.begin());std::copy(b.begin(),b.end(),result.begin()+A);return result;
}
constexpr auto strength=select(onset,std::array{"frame_rate"sv,"lag"sv,"max_size"sv,"n_fft"sv,"hop_samples"sv,"center"sv,"detrend"sv,"aggregate"sv,"aggregate_features"sv,"group_offsets"sv,"group_indices"sv});
constexpr auto detection=select(onset,std::array{"sample_rate"sv,"hop_samples"sv,"sparse"sv,"backtrack"sv,"units"sv,"custom_peaks"sv,"pre_max"sv,"post_max"sv,"pre_average"sv,"post_average"sv,"wait"sv,"delta"sv,"normalize"sv,"selection"sv});
constexpr auto source_rate=std::to_array<ParameterDescriptor>({
    {"sample_rate",C::Processing,V::Number,0,192000,0,{}},
    {"hop_samples",C::Processing,V::Integer,1,65536,512,{}}});
constexpr auto no_activity=std::to_array<ParameterDescriptor>({{"silence_zero",C::Processing,V::Boolean,0,1,0,{}}});
constexpr auto estimate=join(join(select(tempo,std::array{"frame_rate"sv,"start_bpm"sv,"std_bpm"sv,"max_tempo"sv,"limit_max_tempo"sv,"aggregation"sv}),source_rate),no_activity);
constexpr auto envelope_estimate=select(tempo,std::array{"sample_rate"sv,"hop_samples"sv,"autocorrelation_seconds"sv,"start_bpm"sv,"std_bpm"sv,"max_tempo"sv,"limit_max_tempo"sv,"aggregation"sv});
constexpr auto dp=join(select(beat,std::array{"frame_rate"sv,"tightness"sv,"maximum_period_frames"sv,"trim"sv}),source_rate);
constexpr auto pulse=select(beat,std::array{"samples"sv,"frame_rate"sv,"window_length"sv,"hop"sv,"center"sv,"coefficients"sv,"minimum_bpm"sv,"maximum_bpm"sv});
constexpr auto envelope_pulse=select(beat,std::array{"sample_rate"sv,"hop_samples"sv,"window_length"sv,"hop"sv,"center"sv,"coefficients"sv,"minimum_bpm"sv,"maximum_bpm"sv});
constexpr auto mixture=select(model,std::array{"model"sv,"diagonal_retry"sv});
constexpr auto selection=select(model,std::array{"reference_count"sv});
constexpr auto synchronization=select(model,std::array{"frame_rate"sv,"subdivisions"sv,"empty"sv});
constexpr auto probability=select(model,std::array{"minimum_bpm"sv,"maximum_bpm"sv});
constexpr auto pcm_base=std::to_array(pcm);
constexpr auto ratio_params=select(tempo,std::array{"frame_rate"sv,"start_bpm"sv,"std_bpm"sv,"max_tempo"sv,"limit_max_tempo"sv,"factors"sv,"coordinates"sv,"fill"sv,"interpolation"sv,"spline_order"sv,"reduction"sv,"dynamic_frequencies"sv});
constexpr auto temporal_params=select(beat,std::array{"window_length"sv,"hop"sv,"center"sv,"coefficients"sv});
constexpr auto envelope_ratio_params=join(ratio_params,temporal_params);
constexpr auto pcm_tempo_params=join(pcm_base,select(tempo,std::array{"start_bpm"sv,"std_bpm"sv,"max_tempo"sv,"limit_max_tempo"sv,"aggregation"sv,"autocorrelation_seconds"sv}));
constexpr auto pcm_ratio_params=join(pcm_base,join(select(tempo,std::array{"start_bpm"sv,"std_bpm"sv,"max_tempo"sv,"limit_max_tempo"sv,"factors"sv,"coordinates"sv,"fill"sv,"interpolation"sv,"spline_order"sv,"reduction"sv,"dynamic_frequencies"sv}),temporal_params));
constexpr auto pcm_beat_params=join(pcm_tempo_params,select(beat,std::array{"tightness"sv,"maximum_period_frames"sv,"trim"sv,"sparse"sv,"units"sv}));
constexpr auto pcm_pulse_params=join(pcm_base,join(temporal_params,select(beat,std::array{"minimum_bpm"sv,"maximum_bpm"sv})));
constexpr auto pcm_tempogram_params=join(pcm_base,join(temporal_params,select(histogram,std::array{"method"sv})));
constexpr auto envelope_beat_params=join(envelope_estimate,select(beat,std::array{"tightness"sv,"maximum_period_frames"sv,"trim"sv,"sparse"sv,"units"sv}));
constexpr auto pattern_params=select(model,std::array{"model"sv,"frame_rate"sv});
constexpr auto model_key=select(model,std::array{"model"sv});
constexpr auto histogram_base=select(histogram,std::array{"method"sv,"frame_rate"sv,"minimum_bpm"sv,"maximum_bpm"sv,"history_seconds"sv,"alpha"sv,"channels"sv,"transition_lambda"sv,"observation_lambda"sv,"transition_floor"sv,"num_intervals"sv,"fixed_interval"sv});
constexpr auto histogram_params=join(histogram_base,select(histogram,std::array{"online"sv,"reset_state"sv,"comb_alpha"sv}));
constexpr auto analysis_params=join(histogram_params,select(histogram,std::array{"activation_smoothing_seconds"sv,"histogram_smoothing"sv,"interpolate"sv,"smooth_state_model_activations"sv,"activation_kernel"sv,"histogram_kernel"sv}));
constexpr auto tracking_params=join(histogram_base,select(histogram,std::array{"activation_smoothing_seconds"sv,"histogram_smoothing"sv,"look_aside"sv,"look_ahead_seconds"sv,"variable_tempo"sv}));
constexpr auto crf_params=join(histogram_base,select(histogram,std::array{"activation_smoothing_seconds"sv,"histogram_smoothing"sv,"interpolate"sv,"smooth_state_model_activations"sv,"interval_sigma"sv,"use_factors"sv,"num_candidates"sv,"factors"sv}));
constexpr auto beat_state_params=select(state,std::array{"frame_rate"sv,"minimum_bpm"sv,"maximum_bpm"sv,"transition_lambda"sv,"observation_lambda"sv,"transition_floor"sv,"activation_threshold"sv,"num_intervals"sv,"correct"sv,"fixed_interval"sv,"online"sv,"reset_state"sv});
constexpr auto downbeat_params=[] {auto p=select(state,std::array{"model"sv,"frame_rate"sv,"observation_lambda"sv,"activation_threshold"sv,"correct"sv});p[3].default_number=.05;return p;}();
constexpr auto bar_params=select(state,std::array{"model"sv,"observation_weight"sv,"meter_change_probability"sv});
constexpr auto pattern_sequence_params=[] {auto p=pattern_params;p[1].default_number=50;return p;}();
constexpr auto metrical_params=[] {auto p=select(tempo,std::array{"factors"sv,"coordinates"sv,"interpolation"sv,"spline_order"sv,"reduction"sv,"dynamic_frequencies"sv});p[4].default_number=3;return p;}();
}
std::span<const SequenceOperationSpec> sequence_operation_catalogue() noexcept {return catalogue;}
const SequenceOperationSpec* sequence_operation_spec(std::string_view kind) noexcept {
    for(const auto& s:catalogue)if(s.kind==kind)return &s;return nullptr;
}
bool sequence_operation_kind(std::string_view kind) noexcept {return sequence_operation_spec(kind)!=nullptr;}
std::size_t sequence_operation_output_count(std::string_view kind) noexcept {
    const auto* s=sequence_operation_spec(kind);return s?s->outputs:0;
}

namespace {
std::span<const ParameterDescriptor> onset_parameters(std::string_view kind) noexcept {
    if(kind=="OnsetStrengthSequence")return strength;
    if(kind=="OnsetDetection")return detection;
    if(kind=="PcmOnsetEnvelope")return pcm_base;
    return {};
}
std::span<const ParameterDescriptor> pcm_tempo_parameters(std::string_view kind) noexcept {
    if(kind=="PcmTempo")return pcm_tempo_params;
    if(kind=="PcmTempoRatios")return pcm_ratio_params;
    return pcm_tempogram_params;
}

std::span<const ParameterDescriptor> tempo_parameters(std::string_view kind) noexcept {
    if(kind=="TempoEstimate")return estimate;
    if(kind=="EnvelopeTempo")return envelope_estimate;
    if(kind=="TempoRatios")return ratio_params;
    if(kind=="MetricalTempogram")return metrical_params;
    if(kind=="EnvelopeTempoRatios")return envelope_ratio_params;
    return pcm_tempo_parameters(kind);
}
std::span<const ParameterDescriptor> beat_parameters(std::string_view kind) noexcept {
    if(kind=="DynamicBeatPath")return dp;
    if(kind=="LocalPulse")return pulse;
    if(kind=="EnvelopeLocalPulse")return envelope_pulse;
    if(kind=="EnvelopeBeatPath")return envelope_beat_params;
    if(kind=="PcmBeatPath")return pcm_beat_params;
    return pcm_pulse_params;
}
std::span<const ParameterDescriptor> histogram_parameters(std::string_view kind) noexcept {
    if(kind=="TempoHistogram")return histogram_params;
    if(kind=="TempoAnalysis")return analysis_params;
    if(kind=="BeatTrackingSequence")return tracking_params;
    return crf_params;
}
std::span<const ParameterDescriptor> state_parameters(std::string_view kind) noexcept {
    if(kind=="BeatStateSequence")return beat_state_params;
    return kind=="DownbeatStateSequence"?std::span<const ParameterDescriptor>(downbeat_params):std::span<const ParameterDescriptor>(bar_params);
}
std::span<const ParameterDescriptor> model_parameters(std::string_view kind) noexcept {
    if(kind=="GaussianMixtureScore")return mixture;
    if(kind=="ModelPredictionSelection")return selection;
    if(kind=="BeatFeatureSynchronization")return synchronization;
    if(kind=="TempoProbabilityHistogram")return probability;
    if(kind=="PatternFeatures")return {};
    return kind=="PatternStateSequence"?std::span<const ParameterDescriptor>(pattern_sequence_params):std::span<const ParameterDescriptor>(model_key);
}
}
std::span<const ParameterDescriptor> sequence_operation_parameters(std::string_view kind) noexcept {
    const auto* s=sequence_operation_spec(kind);if(!s)return {};
    if(s->family==F::Spectral)return spectral_sequence_parameters(kind);
    if(s->family==F::Onset)return onset_parameters(kind);
    if(s->family==F::Tempo)return tempo_parameters(kind);
    if(s->family==F::BeatPulse)return beat_parameters(kind);
    if(s->family==F::Histogram)return histogram_parameters(kind);
    return s->family==F::State?state_parameters(kind):model_parameters(kind);
}
namespace {
signal_contracts::Status sequence_output_contract(std::string_view kind,const signal_contracts::NumericContract& input,
    const OwnedProcessingContract& declared,OwnedProcessingContract& output) {
    using namespace signal_contracts;
    if(kind!="DynamicBeatPath"){output=declared;return Status::ok;}
    const auto data=declared.view();const auto* numeric=std::get_if<NumericContract>(&data);
    if(!numeric || numeric->shape.rank!=Rank::Vector || input.shape.rank!=Rank::Vector)return Status::incompatible_contract;
    auto value=*numeric;value.shape.extents[0]={0,input.shape.extents[0].maximum};
    return own_processing_contract(value,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,output);
}
signal_contracts::Status sequence_output_contracts(const ConfiguredProcessingNode& node,const signal_contracts::NumericContract& input,
    std::vector<OwnedProcessingContract>& output) {
    using signal_contracts::Status;
    std::vector<OwnedProcessingContract> next;next.reserve(node.outputs.size());
    for(std::size_t i=0;i<node.outputs.size();++i){
        const auto& p=node.outputs[i];if(p.id!=i || !p.contract)return Status::incompatible_contract;
        OwnedProcessingContract contract;
        const auto status=sequence_output_contract(node.kind,input,p.contract,contract);if(status!=Status::ok)return status;
        next.push_back(std::move(contract));
    }
    output=std::move(next);return Status::ok;
}
}

signal_contracts::Status sequence_operation_output_contracts(const ConfiguredProcessingNode& node,
    const signal_contracts::NumericContract& input,std::vector<OwnedProcessingContract>& out) noexcept {
    // The DP mask and its diagnostics follow the explicit input grid.
    // Supplied classical model grids and event dtypes remain authoritative.
    const auto* s=sequence_operation_spec(node.kind);
    if(!s || node.outputs.size()!=s->outputs)return signal_contracts::Status::incompatible_contract;
#if defined(__cpp_exceptions)
    try {
#endif
        return sequence_output_contracts(node,input,out);
#if defined(__cpp_exceptions)
    } catch(const std::bad_alloc&) {return signal_contracts::Status::exhausted;}
#endif
}
}
