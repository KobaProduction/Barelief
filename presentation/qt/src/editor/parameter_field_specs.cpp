#include "parameter_field_specs.h"
#include <core_app/spectrum_projection_ui.h>
#include <core_app/resampler_presentation.h>
#include <QMap>
namespace numeric_editor {
bool parameter_is_primary(const QString& kind,const QString& key) {
    static const QMap<QString,QStringList> fields{
        {"AudioSource",{"source"}},{"WindowFunction",{"function","periodic"}},{"InputSelector",{"numericSelected","selected_input"}},
        {"Switch",{"numericSelected","numericOutputs"}},{"Gate",{"numericGate"}},{"Splitter",{"numericOutputs"}},
        {"MatrixFFT",{"fft_size"}},{"MatrixCenter",{"method"}},{"MatrixWindowFunction",{"function","periodic"}},{"MatrixRow",{"row"}},
        {"MatrixAutocorrelation",{"max_lag","frame_rate_fps"}},
        {"Spectrogram",{"numericCapacity","numericFilledOnly"}},{"NumberAccumulator",{"numericCapacity","numericFilledOnly"}},
        {"TempoEvidence",{"frame_rate_fps","short_fft","long_fft","min_bpm","max_bpm"}},
        {"OnsetStrength",{"lag","max_size","n_fft","hop_length","center","detrend","aggregate"}},
        {"SpectralFlux",{"half_wave"}},{"ComplexSpectralDifference",{"half_wave"}},
        {"PhaseDeviation",{"weighted","magnitude_threshold"}},{"HighFrequencyContent",{"squared"}},
        {"PowerToDb",{"reference_mode","reference"}},
        {"PcmResampler",{"mode","output_rate"}},{"Window",{"size"}},
        {"FrameBuffer",{"size","hop_samples"}},
        {"FrameStride",{"every_n"}},{"FeatureAggregate",{"method"}},
        {"RFFT",{"fft_size"}},{"Fourier",{"fft_size"}},{"RealFourier",{"fft_size"}},
        {"BarkWeighting",{"sample_rate","bin_hz","first_bin"}},
        {"Mel",{"bands","sample_rate","fft_size","bin_hz","first_bin","scale","fmin_hz","fmax_hz","norm"}},
        {"TempoEstimate",{"sample_rate","hop_samples","start_bpm","max_tempo"}},
        {"DynamicBeatPath",{"sample_rate","hop_samples","tightness","trim"}},
        {"AutocorrelationTempogram",{"window_length","sample_rate","hop_samples","normalization","center"}},
        {"SpectrumProjection",{"representation","normalize"}},{"AmplitudePhase",{"representation","normalize","positive_half"}},
        {"ArrayFill",{"value"}},{"ArrayMultiply",{"factorCount","operation"}},
        {"Converter",{"output0Dtype","numericScaling"}},
        {"SpectrumPeak",{"bin_hz","first_bin"}},
        {"FrequencyNormalizer",{"mode","manual_gain"}},{"BroadbandFeatures",{"statistic","bin_hz"}},
        {"NoteSalience",{"notes","input_representation","first_bin","include_nyquist"}},
        {"BroadbandRow",{"input_representation","peak_weight"}},
        {"MusicalSpectrogram",{"first_midi","last_midi","input_representation","peak_weight"}},
        {"NoteSpectrum",{"first_midi","last_midi","input_representation","peak_weight"}},
        {"SpectralQuantize",{"bits"}},{"Number",{"value"}},{"Fs",{"value"}},{"UInt32Constant",{"value"}},
        {"RhythmAnalyzer",{"feature_rate_fps","feature_bins","mode"}},{"RhythmFeatures",{"feature_bins"}}
    };
    return fields.value(kind).contains(key);
}
QString numeric_array_text(const domain::NumericArray& array) {
    QStringList values;values.reserve(int(array.values.size()));
    for(const auto value:array.values)values.append(QString::number(value,'g',9));
    return values.join(',');
}
namespace {
const char* contextual_parameter_title(const QString& kind,const QString& key) {
    static const QMap<QString,const char*> titles{
        {"AutocorrelationTempogram/center","Center analysis window"},
        {"AutocorrelationTempogram/fft_size","Correlation FFT size (0: automatic)"},
        {"PowerToDb/reference","Reference power"}};
    return titles.value(kind+"/"+key,nullptr);
}
}
QString parameter_help(const QString& kind,const QString& key) {
    static const QMap<QString,const char*> help{
        {"DynamicBeatPath/tightness","Penalty for beat intervals that differ from the tempo period. Higher values favor a steadier grid."},
        {"DynamicBeatPath/trim","Remove weak beats at the beginning and end of the analyzed history."},
        {"TempoEstimate/start_bpm","Center of the tempo preference. The detected tempo may differ."},
        {"TempoEstimate/std_bpm","Width of the tempo preference in octaves."},
        {"Window/size","Number of retained elements. New values replace the oldest values."},
        {"PowerToDb/reference","Reference power used by 10 log10(P/reference)."}};
    return QString::fromUtf8(help.value(kind+"/"+key,""));
}

QString parameter_title(const QString& key,const QString& kind) {
    if(const auto* title=contextual_parameter_title(kind,key))return QString::fromUtf8(title);
static const QMap<QString, const char *> titles{{"start_bpm","Preferred tempo, BPM"},
{"std_bpm","Tempo preference width, octaves"},
{"max_tempo","Maximum tempo, BPM"},
{"limit_max_tempo","Limit maximum tempo"},
{"aggregation","Tempo aggregation"},
{"tightness","Beat interval strictness"},
{"trim","Trim weak edge beats"},
{"maximum_period_frames","Maximum beat interval, frames"},
{"window_length","Analysis window length, frames"},
{"hop","Window step, frames"},
{"frame_index","Analysis window index"},
{"input_origin_s","Time origin, seconds"},
{"tail","Last window rule"},
{"coefficients","Explicit window coefficients"},
{"odd_center_ceil","Round odd-window centering upward"},
{"empty_centered_frame","Emit centered frame for empty input"},
{"fft_length","Autocorrelation FFT length rule"},
{"normalization","Profile normalization"},
{"norm_exponent","Normalization exponent"},
{"padding","Window padding"},
{"frame_rate","Input frame rate, Hz"},
{"spectrum_divisor","Spectrum divisor"},
{"spectral_exponent","Spectral magnitude exponent"},
{"inverse_scale","Inverse transform scale"},
{"correlation_normalization","Correlation scaling"},
{"inverse_output","Inverse transform result"},
{"transform","Fourier transform mode"},
{"profile","Frequency profile"},{"window_function","Window function"},
{"half_wave","Only increasing magnitudes"},{"weighted","Weight phase deviation by magnitude"},
{"squared","Use squared magnitudes"},{"magnitude_threshold","Minimum phase magnitude"},
{"feature_rate_fps","Feature cadence, frames/s"},{"feature_bins","R4 feature bands"},{"budget_ops","ACF budget, cost units"},
{"max_lag","Maximum lag, frames"},{"frame_rate_fps","Input cadence, frames/s"},
{"short_fft","Short Fourier size"},{"long_fft","Long Fourier size"},
{"lag","Onset lag, frames"},{"max_size","Frequency max-filter width"},
{"n_fft","FFT size, samples"},{"hop_length","Frontend hop, samples"},
{"hop_samples","Hop, samples"},
{"every_n","Emit every N input frames"},{"silence_zero","Return zero tempo without activity"},
{"center","Center onset envelope"},{"detrend","Remove onset DC trend"},{"aggregate","Feature aggregation"},
{"fmin_hz","Mel minimum frequency, Hz"},{"fmax_hz","Mel maximum frequency, Hz (0: Nyquist)"},
{"scale","Mel scale"},{"norm","Mel filter normalization"},{"norm_p","Mel Lp exponent"},
{"reference_mode","dB reference mode"},{"amin","Minimum power for logarithm"},
{"top_db_enabled","Limit dB dynamic range"},{"top_db","Dynamic range, dB"},{"reduction_axes","Reference and peak reduction axes"},
{"min_bpm","Minimum tempo, BPM"},{"max_bpm","Maximum tempo, BPM"},
{"min_observation_s","Minimum observation, seconds"},{"mature_observation_s","Mature observation, seconds"},
{"input","Input contract"},{"grid","Frequency grid"},{"training","Training settings"},{"resampler","Resampler settings"},
{"input_representation","Input representation"},{"peak_weight","Peak weighting"},{"power_bandwidth_hz","Power bandwidth, Hz"},
{"power","Sum squared window coefficients"},
{"window_coefficients","Window coefficients"},
{"frontend_declared","Declared audio frontend"}, {"include_dc","Include DC"}, {"include_nyquist","Include Nyquist"},
{"channel_policy","Channel policy"}, {"channel_index","Channel index"}, {"analysis_interval_num","Analysis interval numerator"}, {"analysis_interval_den","Analysis interval denominator"},
{"resampler_mode","Resampling method"}, {"resampler_quality","Resampler quality"}, {"representation","Spectral representation"}, {"positive_half","One-sided spectrum (DC separate)"},
{"frame_input","Frame input"},{"selected_input","Selected input"},{"source", "Capture source"},
                                                         {"merge", "Merge mode"},
                                                         {"gain", "Gain"},
                                                         {"offset", "Manual offset"},
                                                         {"peak_hz", "Peak frequency, Hz"}, {"magnitude", "Peak magnitude"},
                                                         {"quality", "SpeexDSP quality"}, {"boundaries_hz", "Band boundaries, Hz"},
                                                         {"manual_gain", "Manual frequency gain"}, {"gains", "Per-bin gain curve"},
                                                         {"profile_json", "Loaded profile data (JSON)"},
                                                         {"reference", "Spectral reference"}, {"floor_db", "Floor, dB"},
                                                         {"first_midi", "First note, MIDI"}, {"notes", "Note cell count"},
                                                         {"last_midi", "Last note, MIDI"},
                                                         {"assessment_first_midi", "Assessment first note, MIDI"}, {"assessment_last_midi", "Assessment last note, MIDI"},
                                                         {"cents_tolerance", "Peak agreement tolerance, cents"}, {"minimum_peak_level", "Minimum linear peak level"},
                                                         {"weights", "Spectral weights (CSV; empty: uniform)"},
                                                         {"values", "Array values (CSV; empty: fill value)"},
                                                         {"floor_enabled", "Preserve input floor when adding"}, {"floor", "Input floor"},
                                                         {"low_low_hz", "Low range start, Hz"}, {"low_high_hz", "Low range end, Hz"}, {"low_count", "Low output count (0: pass-through)"},
                                                         {"mid_low_hz", "Mid range start, Hz"}, {"mid_high_hz", "Mid range end, Hz"}, {"mid_count", "Mid output count (0: pass-through)"},
                                                         {"high_low_hz", "High range start, Hz"}, {"high_high_hz", "High range end, Hz"}, {"high_count", "High output count (0: pass-through)"},
                                                         {"a4_hz", "A4 tuning, Hz"}, {"harmonics", "Harmonic count"},
                                                         {"min_value", "Quantization minimum"}, {"max_value", "Quantization maximum"},
                                                         {"bands", "Mel bands"},{"frequencies_hz","Input bin centers, Hz (empty: regular grid)"},{"index","Element index"},{"column","Column index"},
                                                         {"size", "Array size, elements"},
                                                         {"fft_size", "FFT size, values"},{"row", "Selected band"},
                                                         {"bin_hz", "Frequency step, Hz"},{"first_bin", "First bin index"},{"positive_half", "One-sided spectrum (DC separate)"},{"coherent_gain", "Window coefficient sum"},
                                                         {"power_gain", "Window coefficient square sum"},{"normalize", presentation::spectrum_compensation_caption},
                                                         {"function", "Window function"},{"periodic","Periodic window (DFT)"},
                                                         {"tones", "Tones"},
                                                         {"sample_rate", "Sample rate, Hz"},
                                                         {"output_rate", "Output sample rate, Hz"},
                                                         {"input_rate", "Input sample rate, Hz (0: unknown)"},
                                                         {"output_count", "Audio outputs (0: native)"},
                                                         {"minimum_hz", "Minimum frequency, Hz"},
                                                         {"maximum_hz", "Maximum frequency, Hz"},
                                                         {"amplitude", "Amplitude"},
                                                         {"period_seconds", "Period, seconds"},
                                                         {"sweep_depth", "Sweep depth"},
                                                         {"destination", "Destination IP address"},
                                                         {"interface", "Interface IP address"},
                                                         {"port", "Port"},
                                                         {"cadence_ms", "Send interval, ms"}};
    return QString::fromUtf8(titles.value(key,key.toUtf8().constData()));
}
namespace {
ParameterChoices resampler_choices() {
    ParameterChoices result;
    for(const auto& policy:signal_contracts::resampler_policies()) {
        result.values.append(QString::fromUtf8(policy.id.data(),int(policy.id.size())));
        result.labels.append(presentation::resampler_caption(policy.mode));
    }
    return result;
}
ParameterChoices spectrum_representation_choices(const QString& kind) {
    ParameterChoices result;
    if(kind=="AmplitudePhase")presentation::amplitude_spectrum_choices(result.values,result.labels);
    else result={{"real","imaginary","phase","magnitude","peak_amplitude","power","psd","rms_amplitude","bin_power","window_scaled_magnitude"},
                 {"Real","Imaginary","Phase, radians","Raw FFT magnitude","Peak amplitude","Power spectrum","PSD","RMS amplitude","Bin power (PSD x bin width)","Window-scaled magnitude"}};
    return result;
}
}

namespace {
ParameterChoices representation_parameter_choices(const QString& kind,const QString& key) {
    if(key=="input_representation") {
        if(kind=="BroadbandRow" || kind=="NoteSpectrum")
            return {{"linear","magnitude","peak_amplitude","power","psd","rms_amplitude","bin_power","window_scaled_magnitude"},{"Linear values","Raw FFT magnitude","Peak amplitude","Power spectrum","PSD","RMS amplitude","Bin power","Window-scaled magnitude"}};
        if(kind=="MusicalSpectrogram" || kind=="NoteSalience")
            return {{"magnitude","peak_amplitude","power","psd","rms_amplitude","bin_power","window_scaled_magnitude"},{"Raw FFT magnitude","Peak amplitude","Power spectrum","PSD","RMS amplitude","Bin power","Window-scaled magnitude"}};
    }
    if(key=="representation")return spectrum_representation_choices(kind);
    return {};
}
ParameterChoices generic_parameter_choices(const QString& key) {
    if(key=="peak_weight")return {{"value","squared_value"},{"Input value","Squared input value"}};
    if(key=="function" || key=="window_function")return {{"hann","hamming","blackman","rectangular","bartlett","blackman_harris","flat_top","hann_periodic"},{"Hann","Hamming","Blackman","Rectangular","Bartlett","Blackman-Harris","Flat top","Periodic Hann"}};
    if(key=="radix")return {{"radix2","radix4"},{"Radix 2","Radix 4"}};
    if(key.endsWith("_reduction"))return {{"pass_through","mean","rms","weighted_mean","maximum"},{"Pass through bins","Arithmetic mean","RMS of spectral values","Weighted mean","Maximum"}};
    if(key=="channel_policy")return {{"mean","select"},{"Mean","Selected channel"}};
    return {};
}
}
ParameterChoices parameter_choices(const QString& kind,const QString& key) {
    if((kind=="RhythmAnalyzer" || kind=="RhythmFeatures") && key=="feature_bins")return {{"64","96"},{"64 bands","96 bands"}};
    if(kind=="RhythmAnalyzer" && key=="feature_rate_fps")return {{"30","45","60"},{"30 fps","45 fps","60 fps"}};
    if(kind=="RhythmAnalyzer" && key=="mode")return {{"gated","fixed-light","fixed-full"},{"Gated refinement","Light only","Full refinement"}};
    static const QMap<QString,ParameterChoices> specs{
      {"AutocorrelationTempogram/normalization",{{"0","1","2","3","4"},{"No normalization","Maximum absolute value","Minimum absolute value","Nonzero count","Lp norm"}}},
      {"AutocorrelationTempogram/padding",{{"0","1","2","3","4"},{"Zero padding","Linear ramp","Repeat edge","Reflect","Symmetric"}}},
      {"AutocorrelationTempogram/fft_length",{{"0","1"},{"Power of two","Smooth length (2, 3, 5)"}}},
      {"AutocorrelationTempogram/transform",{{"0","1","2"},{"Automatic","Complex FFT","Real FFT"}}},
      {"AutocorrelationTempogram/inverse_output",{{"0","1"},{"Real part","Complex magnitude"}}},
      {"AutocorrelationTempogram/correlation_normalization",{{"0","1","2"},{"Raw correlation","Divide by overlap length","Divide by full length"}}},
      {"AutocorrelationTempogram/tail",{{"0","1","2","3"},{"Complete windows only","Cover signal end","Center covers signal end","Window origin inside input"}}},
      {"TempoEstimate/aggregation",{{"0","1","2"},{"Per-frame tempo","Mean","Median"}}},
      {"FrequencyNormalizer/mode",{{"off","manual","profile"},{"Off","Manual","Loaded profile"}}},
      {"ArrayMultiply/operation",{{"multiply","add"},{"Multiply","Add"}}},
      {"BroadbandFeatures/statistic",{{"sum_energy","mean_power","rms","maximum","flux"},{"Sum of spectral power values","Mean of spectral power values","Root sum of spectral power values","Maximum spectral power value","Positive spectral power flux"}}},
      {"AudioSource/source",{{"0","1"},{"System audio (loopback)","Microphone"}}},
      {"MatrixCenter/method",{{"0","1","2"},{"Subtract mean","Center between minimum and maximum","Manual offset"}}},
      {"OnsetStrength/aggregate",{{"mean","median","sum","none"},{"Mean","Median","Sum","Per-band differences"}}},
      {"FeatureAggregate/method",{{"1","2","3"},{"Mean","Median","Sum"}}},
      {"Mel/scale",{{"htk","slaney"},{"HTK","Slaney"}}},
      {"Mel/norm",{{"slaney","none","l0","lp","maximum","minimum"},{"Slaney area","None","L0 count","Lp norm","Maximum norm","Minimum norm"}}},
      {"PowerToDb/reference_mode",{{"value","maximum"},{"Explicit reference","Matrix maximum"}}},
      {"PowerToDb/reduction_axes",{{"all","rows","columns"},{"Entire matrix","Reduce rows","Reduce columns"}}},
      {"SpectralQuantize/bits",{{"8","16"},{"UInt8","UInt16"}}}
    };
    if((kind=="PcmResampler" && key=="mode") || key=="resampler_mode")return resampler_choices();
    if(specs.contains(kind+"/"+key))return specs.value(kind+"/"+key);
    const auto representation=representation_parameter_choices(kind,key);
    return representation.values.empty()?generic_parameter_choices(key):representation;
}
}
