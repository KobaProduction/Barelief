#include "parameter_field_specs.h"
#include <core_app/spectrum_projection_ui.h>
#include <core_app/resampler_presentation.h>
#include <domain/numeric/matrix_center.h>
#include <QMap>
namespace numeric_editor {
domain::ArrayMathOperation array_operation(const domain::ConfiguredProcessingNode* node) {
    if(!node)return domain::ArrayMathOperation::Multiply;
    const auto* parameters=std::get_if<domain::TransformProcessingParameters>(&node->parameters);
    if(parameters)for(const auto& field:parameters->values)if(field.key=="operation"){
        if(const auto* value=std::get_if<std::string>(&field.value))
            if(const auto parsed=domain::array_math_operation(*value))return *parsed;
    }
    return domain::ArrayMathOperation::Multiply;
}
namespace {
const char* arithmetic_input_title(domain::ArrayMathOperation operation,bool left) noexcept {
    switch(operation){
    case domain::ArrayMathOperation::Multiply:return left?"Multiplicand":"Multiplier";
    case domain::ArrayMathOperation::Add:return left?"Base value":"Added value";
    case domain::ArrayMathOperation::Subtract:return left?"Minuend":"Subtrahend";
    case domain::ArrayMathOperation::Divide:return left?"Numerator":"Denominator";
    }
    return "Values";
}
const char* arithmetic_output_title(domain::ArrayMathOperation operation) noexcept {
    switch(operation){
    case domain::ArrayMathOperation::Multiply:return "Product";
    case domain::ArrayMathOperation::Add:return "Sum";
    case domain::ArrayMathOperation::Subtract:return "Difference";
    case domain::ArrayMathOperation::Divide:return "Quotient";
    }
    return "Values";
}
}
const char* arithmetic_port_title(domain::ArrayMathOperation operation,bool input,bool left) noexcept {
    return input?arithmetic_input_title(operation,left):arithmetic_output_title(operation);
}
namespace {
ParameterChoices array_input_choices(const domain::ConfiguredProcessingNode& node,bool automatic) {
    ParameterChoices result;
    if(automatic){
        result.values.push_back(QString::number(int(domain::ArrayTrigger::Automatic)));
        result.labels.push_back("Automatic");
    }
    for(const auto& port:domain::data_inputs(node)){
        result.values.push_back(QString::number(port.id));
        result.labels.push_back(arithmetic_port_title(array_operation(&node),true,
            port.id==domain::PortId(domain::BinaryInput::Left)));
    }
    return result;
}
}
ParameterChoices parameter_choices(const domain::ConfiguredProcessingNode& node,const QString& key) {
    if(node.kind=="ArrayMultiply" && (key=="frame_input" || key=="trigger_input"))
        return array_input_choices(node,key=="trigger_input");
    return parameter_choices(QString::fromStdString(node.kind),key);
}

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
        {"IndexExtract",{"mode","axis","index","column","first","length"}},
        {"AxisSum",{"axis","cumulative","accumulation"}},
        {"FrameStride",{"every_n"}},{"FeatureAggregate",{"method"}},
        {"RFFT",{"fft_size"}},{"Fourier",{"fft_size"}},{"RealFourier",{"fft_size"}},
        {"BarkWeighting",{"sample_rate","bin_hz","first_bin"}},
        {"Mel",{"bands","sample_rate","fft_size","bin_hz","first_bin","scale","fmin_hz","fmax_hz","norm"}},
        {"TempoEstimate",{"sample_rate","hop_samples","start_bpm","max_tempo"}},
        {"DynamicBeatPath",{"sample_rate","hop_samples","tightness","trim"}},
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
        {"WindowFunction/evaluation","Window evaluation"},
        {"WindowFunction/pi","Pi for direct window evaluation"},
        {"Window/size","Window size, values"},
        {"FrameBuffer/size","Frame size, samples"},
        {"FrameBuffer/hop_samples","Frame step, samples"},
        {"ArrayConcat/axis","Concatenation axis"},
        {"IndexExtract/mode","Selection mode"},
        {"IndexExtract/axis","Selected dimension"},
        {"IndexExtract/index","Element or row index"},
        {"IndexExtract/column","Column index"},
        {"IndexExtract/indices","Selection indices (empty uses range)"},
        {"IndexExtract/first","First selected index"},
        {"IndexExtract/length","Selected length (0: remaining)"},
        {"IndexExtract/output_length","Output length"},
        {"IndexExtract/pad_before","Leading padding"},
        {"IndexExtract/pad_after","Trailing padding"},
        {"IndexExtract/constant","Padding value"},
        {"AxisSum/accumulation","Summation order"},
        {"ArrayMultiply/trigger_input","Update trigger"},
        {"ArrayMultiply/frame_input","Shape and time source"},
        {"ArrayMultiply/zero_division","Division by zero"},
        {"DirectCorrelation/lags","Correlation lag count"},
        {"DirectCorrelation/first_lag","First correlation lag"},
        {"DirectCorrelation/normalization","Correlation normalization"},
        {"VectorNormalize/method","Normalization method"},
        {"VectorNormalize/minimum_scale","Minimum normalization scale"},
        {"VectorNormalize/axis","Normalization dimension"},
        {"RecursiveFilter/state_policy","Filter state handling"},
        {"RecursiveFilter/numerator","Feedforward coefficients"},
        {"RecursiveFilter/denominator","Feedback coefficients"},
        {"RecursiveFilter/input_zero_threshold","Input zero threshold"},
        {"RecursiveFilter/axis","Filtering dimension"},
        {"Clamp/minimum","Lower bound"},
        {"Clamp/maximum","Upper bound"},
        {"LinearExpansion/factor","Interpolation factor"},
        {"QuantileMask/fraction","Quantile fraction"},
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
        {"PowerToDb/reference","Reference power used by 10 log10(P/reference)."},
        {"ArrayMultiply/frame_input","Input that defines the output size, coordinates and time."},
        {"ArrayMultiply/trigger_input","Input whose update publishes a result. Automatic follows the main data input."}};
    return QString::fromUtf8(help.value(kind+"/"+key,""));
}

QString parameter_title(const QString& key,const QString& kind) {
    if(const auto* title=contextual_parameter_title(kind,key))return QString::fromUtf8(title);
static const QMap<QString, const char *> titles{{"start_bpm","Preferred tempo, BPM"},
{"std_bpm","Tempo preference width, octaves"},
{"precision","Numerical precision"},
{"accumulation","Accumulation mode"},
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
ParameterChoices array_parameter_choices(const QString& kind,const QString& key) {
    static const QMap<QString,ParameterChoices> specs{
        {"NumericMap/operation",{{"0","1"},{"Scale values","Scaled reciprocal"}}},
        {"NumericMap/output_dtype",{{"0","1","2"},{"Float32","Float64","UInt64"}}},
        {"CombFilterBank/direction",{{"0","1"},{"Forward comb","Backward comb"}}},
        {"SameConvolution/crop",{{"0","1"},{"Centered on larger operand","Centered on input"}}},
        {"VectorNormalize/method",{{"0","1","2","3","4","5","6"},{"None","Maximum norm","Minimum norm","Nonzero count","Lp norm","Sum norm","Standard deviation scale"}}},
        {"RecursiveFilter/state_policy",{{"0","1","2"},{"Continue filter state","Reset for each input","Reset for each row"}}},
        {"RecursiveFilter/form",{{"0","1"},{"Transposed direct form II","Direct form I"}}},
        {"RecursiveFilter/subnormal",{{"0","1","2"},{"Preserve subnormal state","Flush subnormal state","Flush after first sample"}}},
        {"DirectCorrelation/normalization",{{"0","1","2","3"},{"Raw correlation","Unbiased correlation","Biased correlation","Overlap-energy correlation"}}},
        {"LogCompression/base",{{"0","1","2"},{"Base 10","Natural logarithm","Base 2"}}}
    };
    if(key=="axis" && (kind=="VectorNormalize" || kind=="RecursiveFilter" || kind=="IndexExtract" || kind=="AxisMaximum" || kind=="AxisSum" || kind=="SameConvolution"))
        return {{"0","1","2"},{"Rows (each column)","Columns (each row)","Last dimension"}};
    return specs.value(kind+"/"+key);
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
    const auto array=array_parameter_choices(kind,key);if(!array.values.empty())return array;
    static const QMap<QString,ParameterChoices> specs{
      {"IndexExtract/mode",{{"0","1"},{"One element","Select along axis"}}},
      {"AxisSum/accumulation",{{"0","1"},{"Sequential accumulation","Pairwise accumulation"}}},
      {"IndexExtract/padding",{{"0","1"},{"Constant padding","Repeat edge value"}}},
      {"ArrayMultiply/zero_division",{{"reject"},{"Reject division by zero"}}},
      {"ArrayConcat/axis",{{"0","1"},{"First dimension (rows)","Second dimension (columns)"}}},
      {"WindowFunction/evaluation",{{"0","1","2"},{"Default evaluation","Direct evaluation","Symmetric cosine series"}}},
      {"RhythmAnalyzer/feature_bins",{{"64","96"},{"64 bands","96 bands"}}},
      {"RhythmFeatures/feature_bins",{{"64","96"},{"64 bands","96 bands"}}},
      {"RhythmAnalyzer/feature_rate_fps",{{"30","45","60"},{"30 fps","45 fps","60 fps"}}},
      {"RhythmAnalyzer/mode",{{"gated","fixed-light","fixed-full"},{"Gated refinement","Light only","Full refinement"}}},
      {"CrfBeatSequence/precision",{{"0","1"},{"Portable Float32","Desktop reference"}}},
      {"TempoEstimate/aggregation",{{"0","1","2"},{"Per-frame tempo","Mean","Median"}}},
      {"FrequencyNormalizer/mode",{{"off","manual","profile"},{"Off","Manual","Loaded profile"}}},
      {"ArrayMultiply/operation",{{"multiply","add","subtract","divide"},{"Multiply","Add","Subtract","Divide"}}},
      {"BroadbandFeatures/statistic",{{"sum_energy","mean_power","rms","maximum","flux"},{"Sum of spectral power values","Mean of spectral power values","Root sum of spectral power values","Maximum spectral power value","Positive spectral power flux"}}},
      {"AudioSource/source",{{"0","1"},{"System audio (loopback)","Microphone"}}},
      {"MatrixCenter/method",{{QString::number(int(domain::MatrixCenterMethod::Mean)),
                               QString::number(int(domain::MatrixCenterMethod::Midrange)),
                               QString::number(int(domain::MatrixCenterMethod::ManualOffset))},
                              {"Subtract mean","Center between minimum and maximum","Manual offset"}}},
      {"OnsetStrength/aggregate",{{"mean","median","sum","none"},{"Mean","Median","Sum","Per-band differences"}}},
      {"FeatureAggregate/method",{{"1","2","3","4"},{"Mean","Median","Sum","Maximum"}}},
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
