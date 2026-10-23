#include "parameter_field_specs.h"
#include <core_app/spectrum_projection_ui.h>
#include <core_app/resampler_presentation.h>
#include <QMap>
namespace numeric_editor {
bool parameter_is_primary(const QString& kind,const QString& key) {
    static const QMap<QString,QStringList> fields{
        {"AudioSource",{"source"}},{"WindowFunction",{"function"}},{"InputSelector",{"numericSelected","selected_input"}},
        {"Switch",{"numericSelected","numericOutputs"}},{"Gate",{"numericGate"}},{"Splitter",{"numericOutputs"}},
        {"MatrixFFT",{"fft_size"}},{"MatrixCenter",{"method"}},{"MatrixWindowFunction",{"function"}},{"MatrixRow",{"row"}},
        {"PcmResampler",{"mode","output_rate"}},{"Window",{"size","input_rate"}},
        {"RFFT",{"fft_size"}},{"Fourier",{"fft_size"}},{"RealFourier",{"fft_size"}},
        {"BarkWeighting",{"sample_rate","bin_hz","first_bin"}},
        {"Mel",{"bands","sample_rate","bin_hz","first_bin"}},
        {"SpectrumProjection",{"representation"}},{"AmplitudePhase",{"representation","normalize","positive_half"}},
        {"ArrayFill",{"value"}},{"ArrayMultiply",{"factorCount","operation"}},
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
QString parameter_title(const QString& key) {
static const QMap<QString, const char *> titles{{"profile","Frequency profile"},{"window_function","Window function"},
{"feature_rate_fps","Feature cadence, frames/s"},{"feature_bins","R4 feature bands"},{"budget_ops","ACF budget, cost units"},
{"input","Input contract"},{"grid","Frequency grid"},{"training","Training settings"},{"resampler","Resampler settings"},
{"input_representation","Input representation"},{"peak_weight","Peak weighting"},{"power_bandwidth_hz","Power bandwidth, Hz"},
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
                                                         {"size", "Window size, samples"},
                                                         {"fft_size", "FFT size, values"},{"row", "Selected band"},
                                                         {"bin_hz", "Frequency step, Hz"},{"first_bin", "First bin index"},{"positive_half", "One-sided spectrum (DC separate)"},{"coherent_gain", "Window coefficient sum"},
                                                         {"power_gain", "Window coefficient square sum"},{"normalize", presentation::spectrum_compensation_caption},
                                                         {"function", "Window function"},
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
    else result={{"real","imaginary","phase","magnitude","peak_amplitude","power","psd"},
                 {"Real","Imaginary","Phase, radians","Raw magnitude","Peak amplitude","Power","PSD"}};
    return result;
}
}

namespace {
ParameterChoices representation_parameter_choices(const QString& kind,const QString& key) {
    if(key=="input_representation") {
        if(kind=="BroadbandRow" || kind=="NoteSpectrum")
            return {{"linear","magnitude","peak_amplitude","power","psd"},{"Linear values","Raw magnitude","Peak amplitude","Power","PSD"}};
        if(kind=="MusicalSpectrogram" || kind=="NoteSalience")
            return {{"magnitude","peak_amplitude","power","psd"},{"Raw magnitude","Peak amplitude","Power","PSD"}};
    }
    if(key=="representation")return spectrum_representation_choices(kind);
    return {};
}
ParameterChoices generic_parameter_choices(const QString& key) {
    if(key=="peak_weight")return {{"value","squared_value"},{"Input value","Squared input value"}};
    if(key=="function" || key=="window_function")return {{"hann","hamming","blackman","rectangular","bartlett","blackman_harris","flat_top"},{"Hann","Hamming","Blackman","Rectangular","Bartlett","Blackman-Harris","Flat top"}};
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
      {"FrequencyNormalizer/mode",{{"off","manual","profile"},{"Off","Manual","Loaded profile"}}},
      {"ArrayMultiply/operation",{{"multiply","add"},{"Multiply","Add"}}},
      {"BroadbandFeatures/statistic",{{"sum_energy","mean_power","rms","maximum","flux"},{"Sum of bin power","Mean bin power","RMS of spectral power","Maximum bin power","Positive spectral flux"}}},
      {"AudioSource/source",{{"0","1"},{"System audio (loopback)","Microphone"}}},
      {"MatrixCenter/method",{{"0","1","2"},{"Subtract mean","Center between minimum and maximum","Manual offset"}}},
      {"SpectralQuantize/bits",{{"8","16"},{"UInt8","UInt16"}}}
    };
    if((kind=="PcmResampler" && key=="mode") || key=="resampler_mode")return resampler_choices();
    if(specs.contains(kind+"/"+key))return specs.value(kind+"/"+key);
    const auto representation=representation_parameter_choices(kind,key);
    return representation.values.empty()?generic_parameter_choices(key):representation;
}
}
