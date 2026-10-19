#include "operation_kernel_state.h"
#include "node_state.h"
#include "preparation_support.h"
#include <algorithm>
#include <cstring>
#include <cmath>
namespace dataflow_runtime::detail {
using namespace domain;using namespace signal_contracts;
namespace {
dsp_kernels::SpectralReduction reduction(std::string_view value) {
    using R=dsp_kernels::SpectralReduction;
    if(value=="mean")return R::Mean;
    if(value=="rms")return R::Rms;
    if(value=="maximum")return R::Maximum;
    if(value=="weighted_mean")return R::WeightedMean;
    return R::PassThrough;
}
bool read_ranges(const ResolvedProcessingNode& node,dsp_kernels::MusicalSpectrogramConfig& config) {
    if(!config.include_broadband)return true;
    const std::array prefixes{"low","mid","high"};
    const std::array ranges{&config.low,&config.mid,&config.high};
    for(std::size_t i=0;i<ranges.size();++i){
        const std::string prefix=prefixes[i];double count=0;
        if(!numeric_parameter(node,prefix+"_low_hz",ranges[i]->low_hz) || !numeric_parameter(node,prefix+"_high_hz",ranges[i]->high_hz) || !numeric_parameter(node,prefix+"_count",count))return false;
        ranges[i]->output_count=std::size_t(count);ranges[i]->reduction=reduction(operation_text(node,prefix+"_reduction"));
    }
    return true;
}
bool read_notes(const ResolvedProcessingNode& node,dsp_kernels::MusicalSpectrogramConfig& config) {
    if(!config.include_notes)return true;
    double first=0,last=0,assess_first=0,assess_last=0;
    if(!numeric_parameter(node,"first_midi",first) || !numeric_parameter(node,"last_midi",last) ||
       !numeric_parameter(node,"assessment_first_midi",assess_first) || !numeric_parameter(node,"assessment_last_midi",assess_last))return false;
    config.first_midi=int(first);config.last_midi=int(last);
    config.assessment_first_midi=int(assess_first);config.assessment_last_midi=int(assess_last);
    return numeric_parameter(node,"a4_hz",config.a4_hz) && numeric_parameter(node,"cents_tolerance",config.cents_tolerance) && numeric_parameter(node,"minimum_peak_level",config.minimum_peak_level);
}
bool read_weights(const ResolvedProcessingNode& node,std::span<float> weights) {
    ProcessingParameterView supplied;
    if(resolve_processing_parameter(node,"weights",supplied)!=Status::ok)return false;
    const auto* values=std::get_if<std::span<const float>>(&supplied);if(!values)return false;
    if(values->empty())return true;
    if(values->size()!=weights.size())return false;
    std::copy(values->begin(),values->end(),weights.begin());return true;
}
bool frequency_grid(const ResolvedProcessingNode& node,std::span<double> frequencies){
    double step=0,first=0;
    if(!numeric_parameter(node,"bin_hz",step) || !numeric_parameter(node,"first_bin",first))return false;
    if(!std::isfinite(step) || step<=0 || !std::isfinite(first) || first<0 || first>UINT32_MAX || std::floor(first)!=first)return false;
    for(std::size_t index=0;index<frequencies.size();++index){
        frequencies[index]=(first+double(index))*step;if(!std::isfinite(frequencies[index]))return false;
    }
    return true;
}
void write_coordinates(std::span<std::byte> output,std::span<const double> frequencies) {
    for(std::size_t i=0;i<frequencies.size();++i){
        std::uint64_t bits=0;std::memcpy(&bits,&frequencies[i],8);
        for(std::size_t byte=0;byte<8;++byte)output[i*8+byte]=std::byte((bits>>(byte*8))&255);
    }
}
void feature_coordinates(State& state,const ResolvedProcessingNode& node,const dsp_kernels::MusicalSpectrogram& feature,bool notes) {
    if(node.kind=="BroadbandRow")write_coordinates(state.numeric_outputs[0].coordinates[0],feature.frequencies_hz());
    if(!notes)return;
    const auto notes_hz=feature.frequencies_hz().subspan(feature.offsets()[3],feature.counts()[3]);
    write_coordinates(state.numeric_outputs[node.kind=="NoteSpectrum"?0:3].coordinates[0],notes_hz);
}
bool explicit_peak_weight(const ResolvedProcessingNode& node,dsp_kernels::PeakWeight& weight){
    const auto value=operation_text(node,"peak_weight");
    if(value=="value")weight=dsp_kernels::PeakWeight::Value;
    else if(value=="squared_value")weight=dsp_kernels::PeakWeight::SquaredValue;
    else return false;
    return true;
}

Status prepare_spectrogram(OperationKernelState& kernel,PlanNode&,State& state,const ResolvedProcessingNode& node){
    dsp_kernels::MusicalSpectrogramConfig config;config.include_broadband=node.kind!="NoteSpectrum";config.include_notes=node.kind!="BroadbandRow";
    if(!read_ranges(node,config) || !read_notes(node,config) || !explicit_peak_weight(node,config.peak_weight))return Status::invalid_argument;
    kernel.note_first_midi=std::uint32_t(config.first_midi);
    std::vector<double> hz(kernel.input.size());std::vector<float> weights(hz.size(),1);
    if(!frequency_grid(node,hz) || (config.include_broadband && !read_weights(node,weights)))return Status::invalid_argument;
    kernel.operation.emplace<dsp_kernels::MusicalSpectrogram>();auto& feature=std::get<dsp_kernels::MusicalSpectrogram>(kernel.operation);
    if(!feature.prepare(hz,config,weights) || feature.output_size()!=kernel.output.size())return Status::incompatible_contract;
    feature_coordinates(state,node,feature,config.include_notes);return Status::ok;
}
bool explicit_power_policy(const ResolvedProcessingNode& node,dsp_kernels::SpectrumPowerPolicy& policy){
    double first=0;
    ProcessingParameterView nyquist;
    if(!numeric_parameter(node,"first_bin",first) || (first!=0 && first!=1) ||
       !numeric_parameter(node,"bin_hz",policy.bin_hz) ||
       !numeric_parameter(node,"coherent_gain",policy.coherent_gain) ||
       !numeric_parameter(node,"power_bandwidth_hz",policy.power_bandwidth_hz) ||
       !dsp_kernels::parse_spectrum_projection(operation_text(node,"input_representation"),policy.representation) ||
       !dsp_kernels::is_amplitude_spectrum_projection(policy.representation))return false;
    if(resolve_processing_parameter(node,"include_nyquist",nyquist)!=Status::ok ||
       !std::holds_alternative<bool>(nyquist))return false;
    policy.first_bin=std::uint32_t(first);
    policy.include_nyquist=std::get<bool>(nyquist);
    return true;
}
bool prepare_power_conversion(OperationKernelState& kernel,const ResolvedProcessingNode& node){
    dsp_kernels::SpectrumPowerPolicy policy;
    if(!explicit_power_policy(node,policy) || !kernel.spectrum_power.prepare(kernel.input.size(),policy))return false;
    kernel.bin_hz=policy.bin_hz;kernel.note_power.resize(kernel.spectrum_power.output_bins());return true;
}

Status prepare_salience(OperationKernelState& kernel,PlanNode&,const ResolvedProcessingNode& node){
    double first=0,notes=0,a4=0,harmonics=0;
    if(!numeric_parameter(node,"first_midi",first) || !numeric_parameter(node,"notes",notes) || !numeric_parameter(node,"a4_hz",a4) || !numeric_parameter(node,"harmonics",harmonics) ||
       !prepare_power_conversion(kernel,node))return Status::invalid_argument;
    kernel.operation.emplace<dsp_kernels::NoteSalience>();
    return std::get<dsp_kernels::NoteSalience>(kernel.operation).prepare(kernel.spectrum_power.output_bins(),kernel.bin_hz,int(first),std::size_t(notes),a4,std::size_t(harmonics))?Status::ok:Status::invalid_argument;
}
}
Status prepare_music_operation(OperationKernelState& kernel,PlanNode& plan,State& state,const ResolvedProcessingNode& node) {
    if(node.kind=="MusicalSpectrogram" || node.kind=="BroadbandRow" || node.kind=="NoteSpectrum")return prepare_spectrogram(kernel,plan,state,node);
    if(node.kind=="NoteSalience")return prepare_salience(kernel,plan,node);
    return Status::unavailable;
}
}
