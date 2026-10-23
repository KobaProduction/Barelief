#include "../graph/parameter_sets.h"
#include <domain/graph/processing_model.h>
#include <limits>
#include "../graph/calibration_parameter_specs.h"
namespace domain::detail {
namespace {
constexpr float broadband_boundaries[]{0,60,120,250,400,600,900,1300,1800,2500,3500,4800,6500,8500,11000,14000,16384};
constexpr ParameterDescriptor projection[]{
        {"fft_size",C::Processing,V::Integer,2,4294967295.,4096,{}},
        {"sample_rate",C::Processing,V::Number,1,4294967295.,32768,{}},
        {"representation",C::Processing,V::String,0,0,0,"magnitude"},
        {"window_function",C::Processing,V::WindowFunction,0,0,0,"hann"}};
constexpr auto normalizer=with_calibration_frontend(std::array{
    ParameterDescriptor{"mode",C::Processing,V::String,0,0,0,"off"},
    ParameterDescriptor{"manual_gain",C::Processing,V::Number,0,1000,1,{}},
    ParameterDescriptor{"gains",C::Processing,V::NumericArray,0,32768,0,{}}});
constexpr ParameterDescriptor broad[]{
        {"boundaries_hz",C::Processing,V::NumericArray,0,129,0,{},broadband_boundaries},
        {"statistic",C::Processing,V::String,0,0,0,"sum_energy"},
        {"bin_hz",C::Processing,V::Number,1e-9,4294967295.,1,{}}};
constexpr ParameterDescriptor notes[]{
        {"first_midi",C::Processing,V::Integer,0,127,48,{}},
        {"notes",C::Processing,V::Integer,1,128,48,{}},
        {"a4_hz",C::Processing,V::Number,400,480,440,{}},
        {"harmonics",C::Processing,V::Integer,1,16,3,{}},
        {"input_representation",C::Processing,V::String,0,0,0,"power"},
        {"first_bin",C::Processing,V::Integer,0,1,0,{}},
        {"include_nyquist",C::Processing,V::Boolean,0,1,1,{}},
        {"bin_hz",C::Processing,V::Number,1e-9,4294967295.,1,{}},
        {"power_bandwidth_hz",C::Processing,V::Number,1e-9,4294967295.,1,{}},
        {"coherent_gain",C::Processing,V::Number,1e-15,1e15,1,{}}};
constexpr ParameterDescriptor musical_row[]{
        {"low_low_hz",C::Processing,V::Number,0,96000,40,{}},
        {"low_high_hz",C::Processing,V::Number,0,96000,250,{}},
        {"low_count",C::Processing,V::Integer,0,2048,0,{}},
        {"low_reduction",C::Processing,V::String,0,0,0,"pass_through"},
        {"mid_low_hz",C::Processing,V::Number,0,96000,250,{}},
        {"mid_high_hz",C::Processing,V::Number,0,96000,4000,{}},
        {"mid_count",C::Processing,V::Integer,0,2048,16,{}},
        {"mid_reduction",C::Processing,V::String,0,0,0,"mean"},
        {"high_low_hz",C::Processing,V::Number,0,96000,4000,{}},
        {"high_high_hz",C::Processing,V::Number,0,96000,16000,{}},
        {"high_count",C::Processing,V::Integer,0,2048,8,{}},
        {"high_reduction",C::Processing,V::String,0,0,0,"maximum"},
        {"first_midi",C::Processing,V::Integer,0,127,21,{}},
        {"last_midi",C::Processing,V::Integer,0,127,108,{}},
        {"assessment_first_midi",C::Processing,V::Integer,0,127,0,{}},
        {"assessment_last_midi",C::Processing,V::Integer,0,127,127,{}},
        {"a4_hz",C::Processing,V::Number,400,480,440,{}},
        {"cents_tolerance",C::Processing,V::Number,0,50,20,{}},
        {"minimum_peak_level",C::Processing,V::Number,0,1e30,0,{}},
        {"weights",C::Processing,V::NumericArray,0,32768,0,{}},
        {"peak_weight",C::Processing,V::String,0,0,0,"squared_value"},
        {"bin_hz",C::Processing,V::Number,1e-9,4294967295.,1,{}},
        {"first_bin",C::Processing,V::Integer,0,4294967295.,1,{}},
        {"input_representation",C::Processing,V::String,0,0,0,"peak_amplitude"}};
constexpr auto broadband_row=[] {
        std::array<ParameterDescriptor,17> fields{};
        for(std::size_t i=0;i<12;++i)fields[i]=musical_row[i];
        fields[12]=musical_row[19];
        fields[13]={"bin_hz",C::Processing,V::Number,1e-9,4294967295.,48000./4096,{}};
        fields[14]={"first_bin",C::Processing,V::Integer,0,4294967295.,1,{}};
        fields[15]=musical_row[20];fields[16]=musical_row[23];return fields;
    }();
constexpr auto note_spectrum=[] {
        std::array<ParameterDescriptor,11> fields{};
        for(std::size_t i=0;i<7;++i)fields[i]=musical_row[12+i];
        fields[7]={"bin_hz",C::Processing,V::Number,1e-9,4294967295.,48000./4096,{}};
        fields[8]={"first_bin",C::Processing,V::Integer,0,4294967295.,1,{}};
        fields[9]=musical_row[20];fields[10]=musical_row[23];return fields;
    }();
constexpr ParameterDescriptor quantize[]{
        {"bits",C::Processing,V::Integer,8,16,8,{}},
        {"min_value",C::Processing,V::Number,-1e30,1e30,0,{}},
        {"max_value",C::Processing,V::Number,-1e30,1e30,1,{}}};
constexpr ParameterDescriptor rhythm[]{
    {"feature_rate_fps",C::Processing,V::Integer,30,60,60,{}},
    {"feature_bins",C::Processing,V::Integer,64,96,64,{}},
    {"sample_rate",C::Processing,V::Number,1,4294967295.,32768,{}},
    {"mode",C::Processing,V::String,0,0,0,"gated"},
    {"budget_ops",C::Processing,V::Integer,0,4294967295.,30000,{}}};
constexpr ParameterDescriptor rhythm_features[]{{"feature_bins",C::Processing,V::Integer,64,96,64,{}}};
constexpr ParameterSet sets[]{
    {NodeKind::RhythmAnalyzer,rhythm},
    {NodeKind::RhythmFeatures,rhythm_features},
    {NodeKind::SpectrumProjection,projection},
    {NodeKind::FrequencyNormalizer,normalizer},
    {NodeKind::MusicalSpectrogram,musical_row},
    {NodeKind::BroadbandRow,broadband_row},
    {NodeKind::NoteSpectrum,note_spectrum},
    {NodeKind::BroadbandFeatures,broad},
    {NodeKind::NoteSalience,notes},
    {NodeKind::SpectralQuantize,quantize},
};
}
std::span<const ParameterDescriptor> music_parameter_descriptors(NodeKind kind) noexcept {
    return find_parameters(sets,kind);
}
}
namespace domain {
ProcessingContentContinuity processing_output_continuity(const ResolvedProcessingNode& node,PortId) noexcept {
    const auto kind=node_kind(node.kind);
    if(!kind)return ProcessingContentContinuity::PreserveInput;
    switch(*kind) {
    case NodeKind::BroadbandFeatures:case NodeKind::NoteSalience:case NodeKind::MusicalSpectrogram:
    case NodeKind::BroadbandRow:case NodeKind::NoteSpectrum:case NodeKind::SpectralQuantize:
        return ProcessingContentContinuity::PartitionOutput;
    default:return ProcessingContentContinuity::PreserveInput;
    }
}
}
