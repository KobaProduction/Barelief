#include "sequence_settings.h"

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
bool read_tempo_settings(const domain::ResolvedProcessingNode& n,TempoEstimatorConfig& c) noexcept {
    std::size_t mode=1;
    if(!number(n,"frame_rate",44100.f/512,c.frame_rate) || !number(n,"start_bpm",120,c.start_bpm) || !number(n,"std_bpm",1,c.std_bpm) ||
       !number(n,"max_tempo",320,c.max_tempo) || !flag(n,"limit_max_tempo",true,c.limit_max_tempo) || !integer(n,"aggregation",1,mode) || mode>2)return false;
    c.aggregate=TempoAggregation(mode);return true;
}
bool read_envelope_settings(const domain::ResolvedProcessingNode& n,EnvelopeTempoConfig& c) noexcept {
    float seconds=8;
    if(!read_tempo_settings(n,c.estimator) || !number(n,"sample_rate",44100,c.sample_rate) || !integer(n,"hop_samples",512,c.hop_samples) ||
       !number(n,"autocorrelation_seconds",8,seconds))return false;
    c.autocorrelation_seconds=seconds;return true;
}
bool read_pcm_settings(const domain::ResolvedProcessingNode& n,PcmOnsetStrengthConfig& c,bool median) noexcept {
    float rate=44100;std::size_t fft=2048,hop=512,bands=128,scale=1,normalization=0,aggregate=median?1:0;
    if(!number(n,"sample_rate",44100,rate) || !integer(n,"n_fft",2048,fft) || !integer(n,"hop_samples",512,hop) ||
       !integer(n,"mel_bands",128,bands) || !integer(n,"mel_scale",1,scale) || scale>1 ||
       !integer(n,"mel_normalization",0,normalization) || normalization>6 || !integer(n,"onset_aggregate",aggregate,aggregate) || aggregate>2)return false;
    c.spectrum=mel_power_spectral_parameters(rate,fft,hop);c.spectrum.mel_filters.bands=bands;
    c.spectrum.mel_filters.scale=MelScale(scale);c.spectrum.mel_filters.normalization=MelNormalization(normalization);
    float low=0,high=0;
    if(!number(n,"mel_minimum_hz",0,low) || !number(n,"mel_maximum_hz",0,high))return false;
    c.spectrum.mel_filters.fmin_hz=low;c.spectrum.mel_filters.fmax_hz=high;
    c.onset.strength.n_fft=fft;c.onset.strength.hop_length=hop;c.onset.strength.aggregate=OnsetAggregate(aggregate);
    return integer(n,"lag",1,c.onset.strength.lag) && integer(n,"max_size",1,c.onset.strength.max_size) &&
        flag(n,"onset_center",true,c.onset.strength.center) && flag(n,"detrend",false,c.onset.strength.detrend) &&
        number(n,"db_reference",1,c.decibels.reference) && number(n,"db_amin",1e-10f,c.decibels.amin) &&
        number(n,"top_db",80,c.decibels.top_db) && flag(n,"top_db_enabled",true,c.decibels.top_db_enabled);
}
bool read_ratio_settings(const domain::ResolvedProcessingNode& n,TempoRatioSequenceConfig& c,FeatureReduction default_reduction) noexcept {
    std::size_t interpolation=0,reduction=std::size_t(default_reduction);
    if(!read_tempo_settings(n,c.tempo) || !integer(n,"interpolation",0,interpolation) || interpolation>8 ||
       !integer(n,"spline_order",1,c.interpolation.spline_order) || !integer(n,"reduction",std::size_t(default_reduction),reduction) || reduction>3 ||
       !number(n,"fill",0,c.fill) || !flag(n,"dynamic_frequencies",false,c.dynamic_frequencies))return false;
    c.interpolation.kind=CurveInterpolation(interpolation);c.time_reduction.operation=FeatureReduction(reduction);return true;
}
bool read_envelope_ratio_settings(const domain::ResolvedProcessingNode& n,EnvelopeTempoRatiosConfig& c) noexcept {
    if(!read_ratio_settings(n,c.ratios) || !integer(n,"window_length",384,c.tempogram.autocorrelation.window.length) ||
       !integer(n,"hop",1,c.tempogram.autocorrelation.window.hop) || !flag(n,"center",true,c.tempogram.autocorrelation.window.center))return false;
    c.tempogram.frame_rate=c.ratios.tempo.frame_rate;return true;
}
bool read_tracking_settings(const domain::ResolvedProcessingNode& n,EnvelopeBeatTrackingConfig& c) noexcept {
    std::size_t units=0;
    if(!read_envelope_settings(n,c.tempo) || !number(n,"tightness",100,c.tracking.tightness) ||
       !integer(n,"maximum_period_frames",2048,c.tracking.maximum_period_frames) || !flag(n,"trim",true,c.tracking.trim) ||
       !flag(n,"sparse",true,c.sparse) || !integer(n,"units",0,units) || units>2)return false;
    c.units=EventCoordinateUnit(units);return true;
}
bool read_pulse_settings(const domain::ResolvedProcessingNode& n,EnvelopeLocalPulseConfig& c) noexcept {
    return number(n,"sample_rate",44100,c.sample_rate) && integer(n,"hop_samples",512,c.hop_samples) &&
        integer(n,"window_length",384,c.pulse.window.length) && integer(n,"hop",1,c.pulse.window.hop) && flag(n,"center",true,c.pulse.window.center) &&
        number(n,"minimum_bpm",30,c.pulse.min_bpm) && number(n,"maximum_bpm",300,c.pulse.max_bpm);
}
bool read_histogram_settings(const domain::ResolvedProcessingNode& n,TempoHistogramSequenceConfig& c) noexcept {
    std::size_t method=1;
    if(!integer(n,"method",1,method) || method>2 || !number(n,"frame_rate",100,c.frame_rate) ||
       !number(n,"minimum_bpm",40,c.minimum_bpm) || !number(n,"maximum_bpm",250,c.maximum_bpm) ||
       !number(n,"history_seconds",10,c.history_seconds) || !number(n,"alpha",.79f,c.alpha) || !flag(n,"online",false,c.online) ||
       !number(n,"transition_lambda",100,c.state_model.transition_lambda) || !number(n,"observation_lambda",16,c.state_model.observation_lambda) ||
       !number(n,"transition_floor",2.220446049250313e-16f,c.state_model.transition_floor) ||
       !integer(n,"num_intervals",0,c.state_model.num_intervals) || !flag(n,"fixed_interval",false,c.state_model.fixed_interval) ||
       !integer(n,"channels",1,c.channels))return false;
    c.method=TempoHistogramEstimator(method);return true;
}
bool read_analysis_settings(const domain::ResolvedProcessingNode& n,TempoAnalysisSequenceConfig& c) noexcept {
    std::size_t precision=0;
    if(!integer(n,"precision",0,precision) || precision>1)return false;
    c.precision=NumericalPrecision(precision);
    return read_histogram_settings(n,c.histogram) && number(n,"activation_smoothing_seconds",.14f,c.activation_smoothing_seconds) &&
        integer(n,"histogram_smoothing",9,c.histogram_smoothing) && flag(n,"interpolate",false,c.interpolate) &&
        flag(n,"smooth_state_model_activations",false,c.smooth_state_model_activations);
}
}
