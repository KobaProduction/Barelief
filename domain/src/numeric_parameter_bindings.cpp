#include <domain/processing_contracts.h>
#include <domain/graph/node_factory.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>
#include <charconv>
#include <bit>

namespace domain {
using namespace signal_contracts;
bool has_sample_rate_output(std::string_view kind) noexcept {
    return kind=="AudioSource" || kind=="SignalGenerator" || kind=="TestSignal" || kind=="NetworkInput" || kind=="PcmResampler";
}
bool has_window_gain_output(std::string_view kind) noexcept {return kind=="WindowFunction";}
namespace {
bool audio_vector_view(std::string_view kind) noexcept {
    return kind=="AudioLineView" || kind=="AudioPointView" || kind=="AudioBarView" || kind=="AudioPieView";
}
}
std::span<const ProcessingPort> data_inputs(const ConfiguredProcessingNode& node) noexcept {
    const auto ports=std::span<const ProcessingPort>(node.inputs);
    if(ports.empty())return ports;
    const auto first=std::find_if(ports.begin(),ports.end(),[&](const ProcessingPort& input){
        return !numeric_parameter_key(node.kind,input.id).empty();
    });
    return ports.first(std::size_t(first-ports.begin()));
}
std::span<const ProcessingPort> data_outputs(const ConfiguredProcessingNode& node) noexcept {
    auto ports=std::span<const ProcessingPort>(node.outputs);
    while(!ports.empty() && ((has_sample_rate_output(node.kind) && ports.back().id==sample_rate_output) || (has_window_gain_output(node.kind) && (ports.back().id==coherent_gain_output || ports.back().id==power_gain_output))))ports=ports.first(ports.size()-1);
    return ports;
}
namespace {
std::string_view spectral_parameter_key(std::string_view kind,PortId input) noexcept {
    if((kind=="FrequencyNormalizer" || kind=="AmplitudePhase") && input==target_sample_rate_input)return "sample_rate";
    if((kind=="FrequencyStep" || kind=="SpectrumAmplitude" || kind=="FrequencyNormalizer" || kind=="Fourier" || kind=="RealFourier" || kind=="RFFT" || kind=="SpectrumProjection") && input==window_size_input)return "fft_size";
    if((kind=="SpectrumPeak" || kind=="NoteSalience") && input==spectrum_first_bin_input)return "first_bin";
    if((kind=="SpectrumPeak" || kind=="NoteSalience" || kind=="MusicalSpectrogram" || kind=="BroadbandRow" || kind=="NoteSpectrum" || kind=="FFTView" || audio_vector_view(kind)) && input==frequency_step_input)return "bin_hz";
    return {};
}
}
std::string_view numeric_parameter_key(std::string_view kind,PortId input) noexcept {
    if(const auto key=spectral_parameter_key(kind,input);!key.empty())return key;
    if(kind=="PianoView" && input==note_first_input)return "first_midi";
    if((kind=="AmplitudePhase" || kind=="NoteSalience") && input==coherent_gain_input)return "coherent_gain";
    if(kind=="AmplitudePhase" && input==power_gain_input)return "power_gain";
    if(kind=="Window" && input==window_size_input)return "size";
    if((kind=="PcmResampler" || kind=="Window") && input==input_sample_rate_input)return "input_rate";
    return {};
}
namespace {
struct ParameterPortPlan {
    std::array<PortId,5> inputs{};
    std::array<PortId,2> outputs{};
    std::size_t input_count=0,output_count=0;
};
void plan_gain_inputs(std::string_view kind,ParameterPortPlan& plan) noexcept;
void plan_parameter_inputs(std::string_view kind,ParameterPortPlan& plan) noexcept {
    if(kind=="Window" || kind=="PcmResampler")plan.inputs[plan.input_count++]=input_sample_rate_input;
    if(!numeric_parameter_key(kind,window_size_input).empty())plan.inputs[plan.input_count++]=window_size_input;
    if(kind=="FrequencyNormalizer" || kind=="AmplitudePhase")plan.inputs[plan.input_count++]=target_sample_rate_input;
    if(kind=="SpectrumPeak" || kind=="NoteSalience")plan.inputs[plan.input_count++]=spectrum_first_bin_input;
    if(kind=="SpectrumPeak" || kind=="NoteSalience" || kind=="MusicalSpectrogram" || kind=="BroadbandRow" || kind=="NoteSpectrum" || kind=="FFTView" || audio_vector_view(kind))plan.inputs[plan.input_count++]=frequency_step_input;
    plan_gain_inputs(kind,plan);
}
void plan_gain_inputs(std::string_view kind,ParameterPortPlan& plan) noexcept {
    if(kind=="PianoView")plan.inputs[plan.input_count++]=note_first_input;
    if(kind=="NoteSalience")plan.inputs[plan.input_count++]=coherent_gain_input;
    if(kind=="AmplitudePhase"){
        plan.inputs[plan.input_count++]=coherent_gain_input;
        plan.inputs[plan.input_count++]=power_gain_input;
    }
}
Status plan_parameter_outputs(const ConfiguredProcessingNode& node,ParameterPortPlan& plan) noexcept {
    const bool window=has_window_gain_output(node.kind);
    if(!window && !has_sample_rate_output(node.kind))return Status::ok;
    if(node.outputs.empty())return Status::invalid_argument;
    const auto data=node.outputs.front().contract.view();
    const auto* numeric=std::get_if<NumericContract>(&data);
    if(node.kind=="NetworkInput" && (!numeric || !numeric->clock.sample_rate))return Status::ok;
    if(!numeric)return Status::invalid_argument;
    plan.outputs[plan.output_count++]=window?coherent_gain_output:sample_rate_output;
    if(window)plan.outputs[plan.output_count++]=power_gain_output;
    return Status::ok;
}
bool parameter_ports_fit(std::span<const ProcessingPort> ports,std::span<const PortId> ids,
                         std::size_t limit) noexcept {
    if(ports.size()>limit)return false;
    auto remaining=limit-ports.size();
    for(const auto id:ids){
        if(std::any_of(ports.begin(),ports.end(),[id](const auto& port){return port.id==id;}))continue;
        if(remaining==0)return false;
        --remaining;
    }
    return true;
}
bool floating_parameter_port(PortId id) noexcept {
    return id==coherent_gain_input || id==power_gain_input || id==frequency_step_input;
}
Status parameter_input_contract(PortId id,const ProcessingAdmissionLimits& limits,
                                OwnedProcessingContract& owned) {
    static constexpr std::uint32_t mono=0;
    NumericContract scalar;
    scalar.dtype=floating_parameter_port(id)?DType::Float64:DType::UInt32;
    scalar.semantic=Semantic::Scalar;
    scalar.unit=id==spectrum_first_bin_input || id==note_first_input || id==window_size_input || id==coherent_gain_input ||
                id==power_gain_input?NumericUnit::Values:NumericUnit::Hertz;
    scalar.channel_order={&mono,1};
    scalar.clock={"configuration",{1,120},0};
    scalar.representation.alignment=floating_parameter_port(id)?alignof(double):alignof(std::uint32_t);
    return own_processing_contract(scalar,limits.frame,limits.max_owned_metadata_bytes,owned);
}
Status stage_parameter_inputs(std::vector<ProcessingPort>& ports,std::span<const PortId> ids,
                               const ProcessingAdmissionLimits& limits) {
    for(const auto id:ids){
        if(std::any_of(ports.begin(),ports.end(),[id](const auto& port){return port.id==id;}))continue;
        OwnedProcessingContract owned;
        const auto status=parameter_input_contract(id,limits,owned);
        if(status!=Status::ok)return status;
        ports.push_back({id,PortDirection::Input,std::move(owned)});
    }
    return Status::ok;
}
Status stage_parameter_outputs(std::vector<ProcessingPort>& ports,std::span<const PortId> ids,
                                const ConfiguredProcessingNode& node) {
    if(ids.empty())return Status::ok;
    const auto data=node.outputs.front().contract.view();
    const auto& numeric=std::get<NumericContract>(data);
    for(const auto id:ids){
        OwnedProcessingContract owned;
        const auto status=has_window_gain_output(node.kind)?window_gain_output_contract(numeric,owned):
                                                          numeric_metadata_output_contract(numeric,true,owned);
        if(status!=Status::ok)return status;
        const auto existing=std::find_if(ports.begin(),ports.end(),[id](const auto& port){return port.id==id;});
        if(existing!=ports.end())existing->contract=std::move(owned);
        else ports.push_back({id,PortDirection::Output,std::move(owned)});
    }
    return Status::ok;
}
Status commit_parameter_ports(ConfiguredProcessingNode& node,std::span<const PortId> input_ids,
                              std::span<const PortId> output_ids,const ProcessingAdmissionLimits& limits) {
    auto inputs=node.inputs;
    auto outputs=node.outputs;
    const auto output_status=stage_parameter_outputs(outputs,output_ids,node);
    if(output_status!=Status::ok)return output_status;
    const auto input_status=stage_parameter_inputs(inputs,input_ids,limits);
    if(input_status!=Status::ok)return input_status;
    node.inputs.swap(inputs);
    node.outputs.swap(outputs);
    return Status::ok;
}
}

Status attach_numeric_parameter_ports(ConfiguredProcessingNode& node,const ProcessingAdmissionLimits& limits) noexcept {
#if defined(__cpp_exceptions)
    try {
#endif
    ParameterPortPlan plan;
    plan_parameter_inputs(node.kind,plan);
    const auto planned=plan_parameter_outputs(node,plan);
    if(planned!=Status::ok)return planned;
    const auto input_ids=std::span<const PortId>(plan.inputs).first(plan.input_count);
    const auto output_ids=std::span<const PortId>(plan.outputs).first(plan.output_count);
    if(!parameter_ports_fit(node.inputs,input_ids,limits.max_ports_per_node) ||
       !parameter_ports_fit(node.outputs,output_ids,limits.max_ports_per_node))return Status::exhausted;
    return commit_parameter_ports(node,input_ids,output_ids,limits);
#if defined(__cpp_exceptions)
    }catch(...){return Status::exhausted;}
#endif
}

namespace {
std::size_t at(const NumericProcessingGraph& graph,NodeId id){for(std::size_t i=0;i<graph.nodes.size();++i)if(graph.nodes[i].id==id)return i;return graph.nodes.size();}
ProcessingPort* port(std::vector<ProcessingPort>& ports,PortId id){for(auto& p:ports)if(p.id==id)return &p;return nullptr;}
const ProcessingPort* port(const std::vector<ProcessingPort>& ports,PortId id){for(const auto& p:ports)if(p.id==id)return &p;return nullptr;}
bool scalar_u32(const ProcessingPort& p){const auto data=p.contract.view();const auto* n=std::get_if<NumericContract>(&data);return n && n->dtype==DType::UInt32 && n->shape.rank==Rank::Scalar && n->channel_order.size()==1 && n->representation.encoding==StorageEncoding::Native && n->representation.normalization==Normalization::None;}
bool number(const ConfiguredProcessingNode& n,std::string_view key,std::uint64_t& out){
    const ResolvedProcessingNode view{n.id,n.kind,NodeRole::Processor,ProcessingOperation::Transform,n.inputs,n.outputs,&n.parameters,n.enabled};
    ProcessingParameterView field;if(resolve_processing_parameter(view,key,field)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&field);if(!literal)return false;
    const auto value=std::visit([](auto v){return double(v);},*literal);
    if(!std::isfinite(value) || value<0 || value>UINT32_MAX || std::floor(value)!=value)return false;
    out=std::uint64_t(value);return true;
}
std::string_view text(const ConfiguredProcessingNode& n,std::string_view key){
    const ResolvedProcessingNode view{n.id,n.kind,NodeRole::Processor,ProcessingOperation::Transform,n.inputs,n.outputs,&n.parameters,n.enabled};
    ProcessingParameterView field;if(resolve_processing_parameter(view,key,field)!=Status::ok || !std::holds_alternative<std::string_view>(field))return {};return std::get<std::string_view>(field);
}
bool node_clock_rate(const ConfiguredProcessingNode& node,std::uint64_t& result) {
    if(const auto* clock=std::get_if<SampleClockProcessingParameters>(&node.parameters)){result=clock->sample_rate;return result!=0;}
    const auto clock=signal_generator_clock(node);
    if(!clock)return false;
    result=clock->sample_rate;return result!=0;
}
std::optional<bool> bound_sample_rate(const NumericProcessingGraph& graph,const ConfiguredProcessingNode& node,std::uint64_t& result) {
    for(const auto& binding:graph.configuration_bindings) {
        if(binding.to!=node.id || binding.input!=ConfigurationRole::SampleClock)continue;
        const auto source=at(graph,binding.from);if(source==graph.nodes.size())return false;
        return node_clock_rate(graph.nodes[source],result);
    }
    return {};
}
bool declared_sample_rate(const NumericProcessingGraph& graph,const ConfiguredProcessingNode& node,std::uint64_t& result) {
    if(node.kind=="PcmResampler")return number(node,node.enabled && text(node,"mode")!="bypass"?"output_rate":"input_rate",result) && result!=0;
    if(const auto bound=bound_sample_rate(graph,node,result))return *bound;
    if(node.kind=="SignalGenerator") {
        const auto clock=signal_generator_clock(node);
        if(!clock)return false;
        result=clock->sample_rate;return true;
    }
    return number(node,"sample_rate",result) && result!=0;
}
bool contract_extent(const ConfiguredProcessingNode& node,const NumericContract& input,std::uint64_t& result) {
    std::uint64_t axis=0;
    if(!number(node,"axis",axis) || axis>1 || (axis && input.shape.rank!=Rank::Matrix))return false;
    const auto extent=input.shape.extents[axis];
    if(extent.minimum!=extent.maximum)return false;
    result=extent.maximum;return true;
}
bool explicit_contract_measurement(const ConfiguredProcessingNode& node,std::uint64_t& result) {
    if(node.inputs.empty())return false;
    const auto data=node.inputs.front().contract.view();const auto* input=std::get_if<NumericContract>(&data);
    if(!input)return false;
    if(node.kind=="SampleRate"){result=input->clock.sample_rate;return result!=0;}
    return contract_extent(node,*input,result);
}
bool scalar_node_value(const ConfiguredProcessingNode& node,PortId output,std::uint64_t& result) {
    if(node.kind=="NoteSpectrum" && output==note_first_output)return number(node,"first_midi",result);
    if(!node.enabled){result=0;return true;}
    if(numeric_constant_kind(node.kind))return output==0 && number(node,"value",result);
    if((node.kind=="SampleRate" || node.kind=="ArrayLength") && output==0)return explicit_contract_measurement(node,result);
    return false;
}
bool value(const NumericProcessingGraph& graph,NodeId id,PortId output,std::size_t,std::uint64_t& result) {
    const auto index=at(graph,id);if(index==graph.nodes.size())return false;
    const auto& node=graph.nodes[index];const auto* endpoint=port(node.outputs,output);
    if(!endpoint || !scalar_u32(*endpoint))return false;
    if(has_sample_rate_output(node.kind) && output==sample_rate_output)return declared_sample_rate(graph,node,result);
    return scalar_node_value(node,output,result);
}
bool window_sample_count(const ConfiguredProcessingNode& node,std::uint64_t& count) {
    if(node.kind=="WindowFunction") {
        if(!number(node,"size",count) || !count)return false;
    } else {
        if(node.inputs.empty())return false;
        const auto data=node.inputs.front().contract.view();const auto* input=std::get_if<NumericContract>(&data);
        if(!input || input->shape.rank!=Rank::Vector || input->shape.extents[0].minimum!=input->shape.extents[0].maximum)return false;
        count=input->shape.extents[0].maximum;
    }
    return true;
}
bool declared_window_gain(const ConfiguredProcessingNode& node,PortId output,double& result) {
    std::uint64_t count=0;
    if(!window_sample_count(node,count))return false;
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Metadata,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView field;bool power=output==power_gain_output;
    if(node.kind=="WindowGain") {
        if(resolve_processing_parameter(view,"power",field)!=Status::ok || !std::holds_alternative<bool>(field))return false;
        power=std::get<bool>(field);
    }
    result=processing_window_gain(node,std::size_t(count),power);return true;
}
bool window_preparation_value(const NumericProcessingGraph& graph,NodeId id,PortId output,std::size_t,double& result) {
    const auto index=at(graph,id);if(index==graph.nodes.size())return false;
    const auto& node=graph.nodes[index];
    if(node.kind=="WindowFunction" && (output==coherent_gain_output || output==power_gain_output))return declared_window_gain(node,output,result);
    return node.kind=="WindowGain" && output==0 && declared_window_gain(node,output,result);
}
std::vector<ProcessingParameter>* mutable_parameters(ConfiguredProcessingNode& node) {
    if(auto* transform=std::get_if<TransformProcessingParameters>(&node.parameters))return &transform->values;
    if(auto* endpoint=std::get_if<EndpointProcessingParameters>(&node.parameters))return &endpoint->values;
    return nullptr;
}
bool set(ConfiguredProcessingNode& node,std::string_view key,std::uint64_t value){
    auto* fields=mutable_parameters(node);if(!fields)return false;
    for(auto& field:*fields)if(field.key==key){field.value=NumericLiteral{value};return true;}
    fields->push_back({std::string(key),NumericLiteral{value}});return true;
}

bool own(const NumericContract& c,const ProcessingAdmissionLimits& limits,OwnedProcessingContract& out){return own_processing_contract(c,limits.frame,limits.max_owned_metadata_bytes,out)==Status::ok;}
bool derive_selected_contract(const ProcessingPort& source,ProcessingPort& destination,const ProcessingAdmissionLimits& limits) {
        const auto data=source.contract.view();const auto* numeric=std::get_if<NumericContract>(&data);
        if(!numeric){destination.contract=source.contract;return true;}
        auto value=*numeric;const auto previous=destination.contract.view();const auto* prior=std::get_if<NumericContract>(&previous);
        if(prior && value.shape.rank==Rank::Vector && value.shape.axes[0]==Axis::Sample)value.shape.extents=prior->shape.extents;
        if(!own(value,limits,destination.contract))return false;
    return true;
}
bool derive_selector_lane(ConfiguredProcessingNode& node,const ProcessingAdmissionLimits& limits,std::size_t channels,std::uint64_t selected,std::size_t lane) {
        const auto* source=port(node.inputs,PortId(channels==1?selected:selected*channels+lane));if(!source)return false;
        auto* destination=port(node.outputs,PortId(lane));if(!destination)return false;
    return derive_selected_contract(*source,*destination,limits);
}
bool derive_selector(ConfiguredProcessingNode& node,const ProcessingAdmissionLimits& limits) {
    std::uint64_t selected=0;if(!number(node,"selected_input",selected))return false;
    const auto channels=data_outputs(node).size();if(!channels)return false;
    for(std::size_t lane=0;lane<channels;++lane) {
        if(!derive_selector_lane(node,limits,channels,selected,lane))return false;
    }
    return attach_numeric_parameter_ports(node,limits)==Status::ok;
}
bool derive_switch(ConfiguredProcessingNode& node) {
    const auto* routing=std::get_if<RouteProcessingParameters>(&node.parameters);
    if(!routing || !routing->channels_per_stream)return false;
    for(auto& output:node.outputs) {
        const auto* input=port(node.inputs,PortId(output.id%routing->channels_per_stream));
        if(!input)return false;
        output.contract=input->contract;
    }
    return true;
}
bool derive_converter(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    const auto* parameters=std::get_if<ConverterProcessingParameters>(&node.parameters);
    const auto prior_data=node.outputs[0].contract.view();const auto* prior=std::get_if<NumericContract>(&prior_data);
    if(!parameters || !prior)return false;
    auto output=input;output.dtype=prior->dtype;output.representation=prior->representation;
    output.semantic=prior->semantic;output.history_element=prior->history_element;
    if(parameters->policy.scaling.mode==ScaleMode::PcmNormalize || parameters->policy.scaling.mode==ScaleMode::PcmDenormalize)output.unit=prior->unit;
    return own(output,limits,node.outputs[0].contract);
}
bool derive_extract(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    std::uint64_t channel=0,row=0,column=0;
    if(!number(node,"channel_index",channel) || !number(node,"index",row) || !number(node,"column",column))return false;
    if(input.shape.rank==Rank::Scalar || channel>=input.channel_order.size() || row>=input.shape.extents[0].maximum)return false;
    if(input.shape.rank==Rank::Vector?column!=0:column>=input.shape.extents[1].maximum)return false;
    auto output=input;output.shape={};output.coordinates={};
    output.channel_order=input.channel_order.subspan(std::size_t(channel),1);
    return own(output,limits,node.outputs[0].contract);
}

std::optional<bool> derive_numeric(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    if(node.kind=="Converter")return derive_converter(node,input,limits);
    if(node.kind=="IndexExtract")return derive_extract(node,input,limits);
    if(node.kind=="ArrayConcat")return array_concat_output_contract(node,node.outputs[0].contract)==Status::ok;
    if(node.kind=="ArrayMultiply")return array_product_output_contract(node,node.outputs[0].contract)==Status::ok;
    if(node.kind!="ArrayFill" && node.kind!="WindowFunction")return {};
    if(array_generator_output_contract(node,input,node.outputs[0].contract)!=Status::ok)return false;
    return attach_numeric_parameter_ports(node,limits)==Status::ok;
}
bool derive_fourier(ConfiguredProcessingNode& node,const NumericContract& input) {
    if(input.shape.extents[0].minimum!=input.shape.extents[0].maximum)return false;
    std::array<OwnedProcessingContract,4> outputs;
    if(fourier_output_contracts(node,input,outputs)!=Status::ok)return false;
    for(std::size_t i=0;i<4;++i)node.outputs[i].contract=std::move(outputs[i]);
    return true;
}
bool derive_polar(ConfiguredProcessingNode& node,const NumericContract& input) {
    std::array<OwnedProcessingContract,2> outputs;
    const auto status=node.kind=="AmplitudePhase"?amplitude_phase_output_contracts(node,input,outputs):complex_polar_output_contracts(input,outputs);
    if(status!=Status::ok)return false;
    for(std::size_t i=0;i<2;++i)node.outputs[i].contract=std::move(outputs[i]);
    return true;
}
bool derive_musical_rows(ConfiguredProcessingNode& node,const NumericContract& input) {
    std::array<OwnedProcessingContract,4> outputs;
    if(musical_spectrogram_output_contracts(node,input,outputs)!=Status::ok)return false;
    node.outputs.clear();const PortId count=node.kind=="BroadbandRow"?1:4;
    for(PortId i=0;i<count;++i)node.outputs.push_back({i,PortDirection::Output,std::move(outputs[i])});
    return true;
}
bool derive_peak(ConfiguredProcessingNode& node,const NumericContract& input) {
    if(node.outputs.size()!=2)return false;
    std::array<OwnedProcessingContract,2> output;
    if(spectrum_peak_output_contracts(input,output)!=Status::ok)return false;
    for(std::size_t i=0;i<output.size();++i)node.outputs[i].contract=std::move(output[i]);
    return true;
}
bool derive_wled(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    if(node.inputs.size()!=3)return false;
    for(std::size_t i=1;i<node.inputs.size();++i){
        const auto data=node.inputs[i].contract.view();
        const auto* prior=std::get_if<NumericContract>(&data);
        if(!prior)return false;
        auto scalar=*prior;scalar.clock=input.clock;
        if(!own(scalar,limits,node.inputs[i].contract))return false;
    }
    return true;
}

std::optional<bool> derive_signal_metadata(ConfiguredProcessingNode&,const NumericContract&);
std::optional<bool> derive_signal_math(ConfiguredProcessingNode& node,const NumericContract& input) {
    if(node.kind=="SpectrumPeak")return derive_peak(node,input);
    if(node.kind=="SpectrumAmplitude")return spectrum_amplitude_output_contract(node,input,node.outputs[0].contract)==Status::ok;
    if(node.kind=="Fourier" || node.kind=="RealFourier")return derive_fourier(node,input);
    if(node.kind=="ComplexPolar" || node.kind=="AmplitudePhase")return derive_polar(node,input);
    return derive_signal_metadata(node,input);
}
std::optional<bool> derive_signal_metadata(ConfiguredProcessingNode& node,const NumericContract& input) {
    if(node.kind=="FrequencyStep")return frequency_step_output_contract(input,node.outputs[0].contract)==Status::ok;
    if(node.kind=="WindowGain")return window_gain_output_contract(input,node.outputs[0].contract)==Status::ok;
    if(node.kind=="ArrayLength" || node.kind=="SampleRate")return numeric_metadata_output_contract(input,node.kind=="SampleRate",node.outputs[0].contract)==Status::ok;
    return {};
}
bool derive_rhythm(ConfiguredProcessingNode& node,const NumericContract& input) {
    std::array<OwnedProcessingContract,rhythm_output_count> outputs;
    if(rhythm_output_contracts(node,input,outputs)!=Status::ok)return false;
    node.outputs.clear();
    for(PortId i=0;i<rhythm_output_count;++i)node.outputs.push_back({i,PortDirection::Output,std::move(outputs[i])});
    return true;
}
std::optional<bool> derive_music(ConfiguredProcessingNode& node,const NumericContract& input) {
    if(node.kind=="RhythmFeatures")return rhythm_features_output_contract(node,input,node.outputs[0].contract)==Status::ok;
    if(node.kind=="RhythmAnalyzer")return derive_rhythm(node,input);
    if(node.kind=="MusicalSpectrogram" || node.kind=="BroadbandRow" || node.kind=="NoteSpectrum")return derive_musical_rows(node,input);
    if(node.kind!="RFFT" && node.kind!="SpectrumProjection" && node.kind!="FrequencyNormalizer" && node.kind!="BroadbandFeatures" && node.kind!="NoteSalience" && node.kind!="SpectralQuantize")return {};
    return music_output_contract(node,input,node.outputs[0].contract)==Status::ok;
}
std::optional<bool> derive_matrix(ConfiguredProcessingNode& node,const NumericContract& input) {
    if(node.kind=="MatrixTranspose")return matrix_transpose_output_contract(input,node.outputs[0].contract)==Status::ok;
    if(node.kind=="MatrixWindowFunction")return matrix_window_output_contract(input,node.outputs[0].contract)==Status::ok;
    if(node.kind=="MatrixCenter")return matrix_center_output_contract(input,node.outputs[0].contract)==Status::ok;
    if(node.kind=="MatrixRow")return matrix_row_output_contract(input,node.outputs[0].contract)==Status::ok;
    if(node.kind!="MatrixFFT")return {};
    std::uint64_t size=0;return number(node,"fft_size",size) && matrix_fft_output_contract(input,size,node.outputs[0].contract)==Status::ok;
}
bool derive_resampler(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    if(!node.enabled || text(node,"mode")=="bypass"){node.outputs[0].contract=node.inputs[0].contract;return attach_numeric_parameter_ports(node,limits)==Status::ok;}
    std::uint64_t rate=0;
    if(!number(node,"output_rate",rate) || !input.clock.sample_rate || !rate || rate>UINT32_MAX || input.shape.extents[0].maximum>UINT64_MAX/rate)return false;
    auto output=input;const auto samples=input.shape.extents[0].maximum*rate;
    output.clock.sample_rate=std::uint32_t(rate);output.clock.tick_seconds={1,rate};
    output.shape.extents[0]={1,samples/input.clock.sample_rate+std::uint64_t(samples%input.clock.sample_rate!=0)+1};
    return own(output,limits,node.outputs[0].contract) && attach_numeric_parameter_ports(node,limits)==Status::ok;
}
bool derive_history(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    const auto* parameters=std::get_if<HistoryProcessingParameters>(&node.parameters);
    if(!parameters || node.outputs.size()!=2)return false;
    auto output=input;output.semantic=Semantic::History;output.history_element=input.semantic;
    if(node.kind=="NumberAccumulator"){
        output.shape={Rank::Vector,{Axis::Time,Axis::None},{ExtentBounds{0,parameters->capacity},ExtentBounds{}}};
        output.coordinates={AxisCoordinates{CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}},AxisCoordinates{}};
    }else{
        output.shape={Rank::Matrix,{Axis::Time,input.shape.axes[0]},{ExtentBounds{0,parameters->capacity},input.shape.extents[0]}};
        output.coordinates={AxisCoordinates{CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}},input.coordinates[0]};
    }
    auto count=std::get<NumericContract>(node.outputs[1].contract.view());count.clock=input.clock;
    return own(count,limits,node.outputs[1].contract) && own(output,limits,node.outputs[0].contract);
}
bool derive_window(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    std::uint64_t size=0;if(!number(node,"size",size) || !size)return false;
    auto output=input;output.semantic=Semantic::Window;output.shape.rank=Rank::Vector;output.shape.extents[0]={size,size};
    return own(output,limits,node.outputs[0].contract);
}
bool derive_legacy_fft(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    const auto size=input.shape.extents[0].maximum;if(!size || !input.clock.sample_rate)return false;
    auto output=input;output.semantic=Semantic::SpectrumMagnitude;output.shape.axes={Axis::Bin,Axis::None};
    output.shape.extents[0]={size/2,size/2};output.representation.normalization=Normalization::None;
    output.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{std::int64_t(input.clock.sample_rate),size}};
    return own(output,limits,node.outputs[0].contract);
}
bool derive_mel(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    std::uint64_t bands=0;if(!number(node,"bands",bands))return false;
    auto output=input;output.semantic=Semantic::MelEnergy;output.shape.axes={Axis::Band,Axis::None};output.shape.extents[0]={bands,bands};
    output.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{1,1}};
    return own(output,limits,node.outputs[0].contract);
}
bool derive_passthrough_contract(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    const auto prior=std::get<NumericContract>(node.outputs[0].contract.view());auto output=input;
    if(node.kind=="ChannelComparison" || node.kind=="ScalarReduce"){output=prior;output.clock=input.clock;}
    else{output.semantic=prior.semantic;output.history_element=prior.history_element;}
    if(node.kind=="ScalarReduce"){output.quantity=input.quantity;output.unit=input.unit;}
    return own(output,limits,node.outputs[0].contract);
}
std::optional<bool> derive_stream_route(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    if(node.kind=="WLEDEncode")return derive_wled(node,input,limits);
    if(node.kind=="Window")return derive_window(node,input,limits);
    if(node.kind=="PcmResampler")return derive_resampler(node,input,limits);
    return {};
}
std::optional<bool> derive_signal_route(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    if(const auto result=derive_stream_route(node,input,limits))return *result;
    if(node.kind=="StereoFFT")return derive_legacy_fft(node,input,limits);
    if(node.kind=="Mel")return derive_mel(node,input,limits);
    if(node.kind=="Spectrogram" || node.kind=="NumberAccumulator")return derive_history(node,input,limits);
    if(node.kind!="BarkWeighting" && node.kind!="SpectrumGain" && node.kind!="SpectrumMerge" && node.kind!="Mixer" && node.kind!="ChannelComparison" && node.kind!="ScalarReduce")return {};
    return derive_passthrough_contract(node,input,limits);
}
bool derive_specialized_input(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    if(const auto result=derive_music(node,input))return *result;
    if(const auto result=derive_matrix(node,input))return *result;
    if(const auto result=derive_signal_route(node,input,limits))return *result;
    return false;
}
bool derive_numeric_input(ConfiguredProcessingNode& node,const NumericContract& input,const ProcessingAdmissionLimits& limits) {
    if(const auto result=derive_numeric(node,input,limits))return *result;
    if(const auto result=derive_signal_math(node,input))return *result;
    return derive_specialized_input(node,input,limits);
}
std::optional<bool> derive_routing_contract(ConfiguredProcessingNode& node,const ProcessingAdmissionLimits& limits) {
    if(node.kind=="Reroute" || node.kind=="Gate" || node.kind=="Splitter"){for(auto& p:node.outputs)p.contract=node.inputs.front().contract;return true;}
    if(node.kind=="Switch")return derive_switch(node);
    if(node.kind=="InputSelector")return derive_selector(node,limits);
    return {};
}
bool derive(ConfiguredProcessingNode& node,const ProcessingAdmissionLimits& limits) {
    if(const auto kind=node_kind(node.kind);kind && node_role(*kind)==NodeRole::Sink)return true;
    if(node.inputs.empty() || node.outputs.empty())return true;
    if(const auto routed=derive_routing_contract(node,limits))return *routed;
    const auto data=node.inputs.front().contract.view();const auto* input=std::get_if<NumericContract>(&data);if(!input)return false;
    return derive_numeric_input(node,*input,limits);
}
}
ProcessingDiagnostic derive_processing_node_contracts(ConfiguredProcessingNode& node,const ProcessingAdmissionLimits& limits) {
    auto candidate=node;
    if(!derive(candidate,limits))return {ProcessingError::InvalidParameters,node.id};
    node=std::move(candidate);return {};
}
ProcessingDiagnostic configure_processing_input(ConfiguredProcessingNode& node,const DataContract& input,const ProcessingAdmissionLimits& limits) {
    auto candidate=node;
    if(candidate.inputs.empty())return {ProcessingError::InvalidPort,node.id};
    const auto status=own_processing_contract(input,limits.frame,limits.max_owned_metadata_bytes,candidate.inputs.front().contract);
    if(status!=Status::ok)return {ProcessingError::InvalidContract,node.id,candidate.inputs.front().id};
    if(candidate.kind=="PcmResampler" || candidate.kind=="Window") {
        const auto* numeric=std::get_if<NumericContract>(&input);
        if(!numeric || !numeric->clock.sample_rate || !set(candidate,"input_rate",numeric->clock.sample_rate))return {ProcessingError::InvalidParameters,node.id};
    }
    if(!derive(candidate,limits))return {ProcessingError::InvalidParameters,node.id};
    node=std::move(candidate);return {};
}
static PreparationValueCapability primary_value_capability(const ConfiguredProcessingNode&,PortId) noexcept;
PreparationValueCapability preparation_value_capability(const ConfiguredProcessingNode& node,PortId output) noexcept {
    using K=PreparationValueKind;
    if(has_sample_rate_output(node.kind) && output==sample_rate_output)return {K::UnsignedInteger};
    if(has_window_gain_output(node.kind) && (output==coherent_gain_output || output==power_gain_output))return {K::FiniteScalar};
    if(node.kind=="NoteSpectrum" && output==note_first_output)return {K::UnsignedInteger};
    return primary_value_capability(node,output);
}
static PreparationValueCapability primary_value_capability(const ConfiguredProcessingNode& node,PortId output) noexcept {
    using K=PreparationValueKind;
    if(output!=0 && node.kind!="InputSelector")return {};
    if(node.kind=="Reroute" || node.kind=="InputSelector")return {K::Routed};
    if(node.kind=="WindowGain" || node.kind=="FrequencyStep" || node.kind=="Number")return {K::FiniteScalar};
    if(numeric_constant_kind(node.kind) || node.kind=="ArrayLength" || node.kind=="SampleRate")return {K::UnsignedInteger};
    return {};
}
namespace {
Status resolve_preparation_value_impl(const NumericProcessingGraph&,NodeId,PortId,NumericLiteral&,const PreparationValueProvider*,std::size_t) noexcept;
Status routed_preparation_value(const NumericProcessingGraph& graph,const ConfiguredProcessingNode& node,PortId output,
    NumericLiteral& out,const PreparationValueProvider* provider,std::size_t depth) noexcept {
    PortId selected=0;
    if(node.kind=="InputSelector") {
        std::uint64_t group=0;if(!number(node,"selected_input",group))return Status::invalid_argument;
        const auto id=group*data_outputs(node).size()+output;
        if(id>UINT16_MAX)return Status::invalid_argument;
        selected=PortId(id);
    }
    for(const auto& edge:graph.edges)if(edge.to==node.id && edge.input_port==selected)return resolve_preparation_value_impl(graph,edge.from,edge.output_port,out,provider,depth+1);
    return Status::unavailable;
}
Status frequency_step_from_source(const NumericProcessingGraph& graph,const Edge& edge,std::uint64_t size,
    NumericLiteral& out,const PreparationValueProvider* provider,std::size_t depth) noexcept {
    NumericLiteral source;
    const auto status=resolve_preparation_value_impl(graph,edge.from,edge.output_port,source,provider,depth+1);
    if(status!=Status::ok)return status;
    const auto rate=std::visit([](auto v){return static_cast<double>(v);},source);
    if(!std::isfinite(rate) || rate<=0)return Status::invalid_argument;
    out=rate/double(size);return Status::ok;
}
Status frequency_step_value(const NumericProcessingGraph& graph,const ConfiguredProcessingNode& node,
    NumericLiteral& out,const PreparationValueProvider* provider,std::size_t depth) noexcept {
    std::uint64_t size=0;if(!number(node,"fft_size",size) || !size)return Status::invalid_argument;
    for(const auto& edge:graph.edges)if(edge.to==node.id && edge.input_port==0) {
        return frequency_step_from_source(graph,edge,size,out,provider,depth);
    }
    return Status::unavailable;
}
Status number_preparation_value(const ConfiguredProcessingNode& node,NumericLiteral& out) noexcept {
    if(!node.enabled){out=std::uint64_t{0};return Status::ok;}
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Source,ProcessingOperation::Source,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView field;
    if(resolve_processing_parameter(view,"value",field)!=Status::ok)return Status::invalid_argument;
    const auto* literal=std::get_if<NumericLiteral>(&field);if(!literal)return Status::invalid_argument;
    out=*literal;return Status::ok;
}
Status builtin_preparation_value(const NumericProcessingGraph& graph,const ConfiguredProcessingNode& node,PortId output,NumericLiteral& out) noexcept {
    if(node.kind=="Number" && output==0)return number_preparation_value(node,out);
    std::uint64_t integer=0;
    if(value(graph,node.id,output,0,integer)){out=integer;return Status::ok;}
    double scalar=0;
    if(window_preparation_value(graph,node.id,output,0,scalar)){out=scalar;return Status::ok;}
    return Status::unavailable;
}
Status node_preparation_value(const NumericProcessingGraph& graph,NodeId id,PortId output,
    NumericLiteral& out,const PreparationValueProvider* provider,std::size_t depth) noexcept {
    const auto index=at(graph,id);if(index==graph.nodes.size())return Status::unavailable;
    const auto& node=graph.nodes[index];const auto capability=preparation_value_capability(node,output);
    if(!capability)return Status::unavailable;
    if(capability.kind==PreparationValueKind::Routed)return routed_preparation_value(graph,node,output,out,provider,depth);
    if(node.kind=="FrequencyStep")return frequency_step_value(graph,node,out,provider,depth);
    return builtin_preparation_value(graph,node,output,out);
}
Status resolve_preparation_value_impl(const NumericProcessingGraph& graph,NodeId id,PortId output,
    NumericLiteral& out,const PreparationValueProvider* provider,std::size_t depth) noexcept {
    if(depth>graph.nodes.size())return Status::invalid_argument;
    if(provider && provider->resolve) {
        const auto status=provider->resolve(provider->context,graph,id,output,out);
        if(status!=Status::unavailable)return status;
    }
    return node_preparation_value(graph,id,output,out,provider,depth);
}
}
Status resolve_preparation_value(const NumericProcessingGraph& graph,NodeId id,PortId output,
    NumericLiteral& out,const PreparationValueProvider* provider) noexcept {
    NumericLiteral candidate;
    const auto status=resolve_preparation_value_impl(graph,id,output,candidate,provider,0);
    if(status!=Status::ok)return status;
    if(validate_literal(candidate)!=Status::ok)return Status::invalid_argument;
    if(!std::visit([](auto value){return std::isfinite(static_cast<long double>(value));},candidate))return Status::invalid_argument;
    out=candidate;return Status::ok;
}
namespace {
ProcessingError preparation_status(Status status) {
    switch(status) {
    case Status::ok:return ProcessingError::None;
    case Status::unavailable:case Status::disconnected:case Status::would_block:return ProcessingError::PreparationValueUnavailable;
    case Status::buffer_too_small:case Status::exhausted:return ProcessingError::ResourceLimit;
    case Status::overflow:return ProcessingError::PreparationValueOutOfRange;
    case Status::incompatible_contract:return ProcessingError::PreparationTypeMismatch;
    default:return ProcessingError::PreparationValueInvalid;
    }
}
ProcessingError preparation_u32(const NumericProcessingGraph& graph,NodeId id,PortId port_id,const PreparationValueProvider* provider,std::uint64_t& out) {
    NumericLiteral literal;
    const auto status=resolve_preparation_value(graph,id,port_id,literal,provider);
    if(status!=Status::ok)return preparation_status(status);
    const auto value=std::visit([](auto v){return static_cast<long double>(v);},literal);
    if(value<0 || value>UINT32_MAX || std::floor(value)!=value)return ProcessingError::PreparationValueOutOfRange;
    out=static_cast<std::uint64_t>(value);return ProcessingError::None;
}
ProcessingError preparation_scalar(const NumericProcessingGraph& graph,NodeId id,PortId port_id,const PreparationValueProvider* provider,double& out) {
    NumericLiteral literal;
    const auto status=resolve_preparation_value(graph,id,port_id,literal,provider);
    if(status!=Status::ok)return preparation_status(status);
    out=std::visit([](auto v){return static_cast<double>(v);},literal);
    return out>0?ProcessingError::None:ProcessingError::PreparationValueOutOfRange;
}
}
namespace {
ProcessingDiagnostic validate_parameter_connection(const NumericProcessingGraph& graph,std::size_t index,NodeId changed,const ProcessingPort& source,const ProcessingPort& target) {
    const auto& edge=graph.edges[index];
    for(std::size_t i=0;i<index;++i)if(graph.edges[i].to==edge.to && graph.edges[i].input_port==edge.input_port)return {ProcessingError::MultipleInputs,edge.to,edge.input_port,index};
    if(edge.from!=changed && numeric_parameter_key(graph.nodes[at(graph,edge.to)].kind,edge.input_port).empty() && !processing_ports_compatible(source,target) && !processing_ports_dtype_convertible(source,target))return {ProcessingError::IncompatiblePorts,edge.to,edge.input_port,index};
    return {};
}
ProcessingDiagnostic parameter_edge(const NumericProcessingGraph& graph,std::size_t index,NodeId changed) {
    const auto& edge=graph.edges[index];const auto from=at(graph,edge.from),to=at(graph,edge.to);
    if(from==graph.nodes.size() || to==graph.nodes.size())return {ProcessingError::MissingEndpoint,edge.to,edge.input_port,index};
    const auto* source=port(graph.nodes[from].outputs,edge.output_port);const auto* target=port(graph.nodes[to].inputs,edge.input_port);
    if(!source || !target)return {ProcessingError::InvalidPort,edge.to,edge.input_port,index};
    return validate_parameter_connection(graph,index,changed,*source,*target);
}
ProcessingDiagnostic unique_parameter_node_ids(const NumericProcessingGraph& graph) {
    for(std::size_t i=0;i<graph.nodes.size();++i)for(std::size_t j=0;j<i;++j)if(graph.nodes[i].id==graph.nodes[j].id)return {ProcessingError::InvalidNode,graph.nodes[i].id};
    return {};
}
ProcessingDiagnostic prepare_incoming_counts(const NumericProcessingGraph& graph,NodeId changed,std::vector<std::size_t>& incoming) {
    const auto unique=unique_parameter_node_ids(graph);if(!unique)return unique;
    for(std::size_t i=0;i<graph.edges.size();++i) {
        const auto valid=parameter_edge(graph,i,changed);if(!valid)return valid;
        ++incoming[at(graph,graph.edges[i].to)];
    }
    return {};
}
void append_ready_successors(const NumericProcessingGraph& graph,NodeId source,std::vector<std::size_t>& incoming,std::vector<std::size_t>& order) {
    for(const auto& edge:graph.edges)if(edge.from==source)if(!--incoming[at(graph,edge.to)])order.push_back(at(graph,edge.to));
}
void append_initial_ready(std::span<const std::size_t> incoming,std::vector<std::size_t>& order) {
    for(std::size_t i=0;i<incoming.size();++i)if(!incoming[i])order.push_back(i);
}
ProcessingDiagnostic preparation_order(const NumericProcessingGraph& graph,NodeId changed,std::vector<std::size_t>& order) {
    std::vector<std::size_t> incoming(graph.nodes.size(),0);
    const auto valid=prepare_incoming_counts(graph,changed,incoming);if(!valid)return valid;
    append_initial_ready(incoming,order);
    for(std::size_t cursor=0;cursor<order.size();++cursor)append_ready_successors(graph,graph.nodes[order[cursor]].id,incoming,order);
    return order.size()==graph.nodes.size()?ProcessingDiagnostic{}:ProcessingDiagnostic{ProcessingError::Cycle};
}
struct BindingContext {
    NumericProcessingGraph& graph;
    const ProcessingAdmissionLimits& limits;
    const PreparationValueProvider* provider;
    std::vector<bool>& changed;
};
struct NodeBindings {
    ConfiguredProcessingNode& node;
    bool update=false;
    std::optional<std::uint64_t> asserted_rate{};
};
bool bound_parameter_range(const ConfiguredProcessingNode& node,std::string_view key,long double value) {
    if(key=="input_rate")return value>0 && value<=UINT32_MAX;
    const auto kind=node_kind(node.kind);if(!kind)return false;
    const auto* descriptor=parameter_descriptor(*kind,key);if(!descriptor)return false;
    return value>=descriptor->minimum && value<=descriptor->maximum;
}
ProcessingDiagnostic store_bound_integer(NodeBindings& target,std::string_view key,std::uint64_t value,PortId input) {
    std::uint64_t previous=0;
    if(number(target.node,key,previous) && previous==value)return {};
    if(!set(target.node,key,value))return {ProcessingError::InvalidParameters,target.node.id,input};
    target.update=true;return {};
}
ProcessingDiagnostic generator_size(BindingContext& context,NodeBindings& target,const Edge& edge,const ProcessingPort& source,ProcessingPort& input) {
    if(!scalar_u32(source))return {ProcessingError::PreparationTypeMismatch,target.node.id,edge.input_port};
    std::uint64_t count=0;
    const auto reason=preparation_u32(context.graph,edge.from,edge.output_port,context.provider,count);
    if(reason!=ProcessingError::None)return {reason,target.node.id,edge.input_port};
    if(!count || count>UINT32_MAX)return {ProcessingError::PreparationValueOutOfRange,target.node.id,edge.input_port};
    target.update=target.update || context.changed[at(context.graph,edge.from)] || !same_contract(input.contract.view(),source.contract.view());input.contract=source.contract;
    return store_bound_integer(target,"size",count,edge.input_port);
}
bool scalar_f64(const ProcessingPort& port) {
    const auto contract=port.contract.view();const auto* scalar=std::get_if<NumericContract>(&contract);
    return scalar && scalar->dtype==DType::Float64 && scalar->shape.rank==Rank::Scalar && scalar->channel_order.size()==1 && scalar->representation.encoding==StorageEncoding::Native && scalar->representation.normalization==Normalization::None;
}
bool store_bound_scalar(NodeBindings& target,std::string_view key,double value) {
    auto* fields=mutable_parameters(target.node);if(!fields)return false;
    const auto old=std::find_if(fields->begin(),fields->end(),[&](const auto& field){return field.key==key;});
    const ProcessingParameterValue replacement=NumericLiteral{value};
    if(old!=fields->end() && old->value==replacement)return true;
    if(old==fields->end())fields->push_back({std::string(key),replacement});
    else old->value=replacement;
    target.update=true;return true;
}
ProcessingDiagnostic floating_binding(BindingContext& context,NodeBindings& target,const Edge& edge,std::string_view key,const ProcessingPort& source,ProcessingPort& input) {
    if(!scalar_f64(source) || !scalar_f64(input))return {ProcessingError::PreparationTypeMismatch,target.node.id,edge.input_port};
    double value=0;const auto reason=preparation_scalar(context.graph,edge.from,edge.output_port,context.provider,value);
    if(reason!=ProcessingError::None)return {reason,target.node.id,edge.input_port};
    if(!bound_parameter_range(target.node,key,value))return {ProcessingError::PreparationValueOutOfRange,target.node.id,edge.input_port};
    if(!store_bound_scalar(target,key,value))return {ProcessingError::InvalidParameters,target.node.id,edge.input_port};
    input.contract=source.contract;return {};
}
ProcessingDiagnostic integer_binding(BindingContext& context,NodeBindings& target,const Edge& edge,std::string_view key,const ProcessingPort& source,ProcessingPort& input) {
    if(!scalar_u32(source) || !scalar_u32(input))return {ProcessingError::PreparationTypeMismatch,target.node.id,edge.input_port};
    std::uint64_t value=0;
    const auto reason=preparation_u32(context.graph,edge.from,edge.output_port,context.provider,value);
    if(reason!=ProcessingError::None)return {reason,target.node.id,edge.input_port};
    if(!bound_parameter_range(target.node,key,value))return {ProcessingError::PreparationValueOutOfRange,target.node.id,edge.input_port};
    input.contract=source.contract;
    if(key=="input_rate") {
        target.asserted_rate=value;
        if(target.node.kind!="PcmResampler" && target.node.kind!="Window")return {};
    }
    return store_bound_integer(target,key,value,edge.input_port);
}
ProcessingDiagnostic propagate_numeric_output(BindingContext& context,NodeBindings& target,const DataContract& previous,NumericContract& numeric) {
    const auto* old=std::get_if<NumericContract>(&previous);
    const auto kind=node_kind(target.node.kind);const bool inherit_dtype=(target.node.kind=="Spectrogram" || target.node.kind=="NumberAccumulator") || (kind && is_view(*kind));
    if(numeric.dtype!=old->dtype && !inherit_dtype){numeric.dtype=old->dtype;numeric.representation=old->representation;}
    const auto* converter=std::get_if<ConverterProcessingParameters>(&target.node.parameters);
    if(converter && target.node.outputs.size()==1 && converter->policy.scaling.mode==ScaleMode::Identity && same_physical_contract(previous,target.node.outputs.front().contract.view())) {
        if(!own(numeric,context.limits,target.node.outputs.front().contract))return {ProcessingError::InvalidContract,target.node.id};
    }
    return {};
}
ProcessingDiagnostic propagate_numeric_contract(BindingContext& context,NodeBindings& target,const Edge& edge,const DataContract& previous,NumericContract& numeric,ProcessingPort& input) {
    const auto* old=std::get_if<NumericContract>(&previous);
    if(!old)return {ProcessingError::IncompatiblePorts,target.node.id,edge.input_port};
    const auto valid=propagate_numeric_output(context,target,previous,numeric);if(!valid)return valid;
    if(!own(numeric,context.limits,input.contract))return {ProcessingError::InvalidContract,target.node.id,edge.input_port};
    target.update=true;return {};
}
ProcessingDiagnostic propagate_data_contract(BindingContext& context,NodeBindings& target,const Edge& edge,const ProcessingPort& source,ProcessingPort& input) {
    if(!context.changed[at(context.graph,edge.from)])return {};
    auto replacement=source.contract.view();const auto previous=input.contract.view();
    auto* numeric=std::get_if<NumericContract>(&replacement);
    if(!numeric){input.contract=source.contract;target.update=true;return {};}
    return propagate_numeric_contract(context,target,edge,previous,*numeric,input);
}
ProcessingDiagnostic bind_input(BindingContext& context,NodeBindings& target,const Edge& edge) {
    const auto& source=*port(context.graph.nodes[at(context.graph,edge.from)].outputs,edge.output_port);
    auto& input=*port(target.node.inputs,edge.input_port);
    if((target.node.kind=="WindowFunction" || target.node.kind=="ArrayFill") && edge.input_port==0)return generator_size(context,target,edge,source,input);
    const auto key=numeric_parameter_key(target.node.kind,edge.input_port);
    if(key.empty())return propagate_data_contract(context,target,edge,source,input);
    if(key=="coherent_gain" || key=="power_gain" || key=="bin_hz")return floating_binding(context,target,edge,key,source,input);
    return integer_binding(context,target,edge,key,source,input);
}
ProcessingDiagnostic validate_bound_rate(const NodeBindings& target) {
    const auto& node=target.node;
    if(target.asserted_rate) {
        if(node.inputs.empty())return {ProcessingError::InvalidParameters,node.id,input_sample_rate_input};
        const auto data=node.inputs.front().contract.view();const auto* input=std::get_if<NumericContract>(&data);
        if(!input || input->clock.sample_rate!=*target.asserted_rate)return {ProcessingError::IncompatiblePorts,node.id,input_sample_rate_input};
    }
    return {};
}
ProcessingDiagnostic refresh_bound_node(BindingContext& context,NodeBindings& target) {
    const auto& node=target.node;
    const auto valid=validate_bound_rate(target);if(!valid)return valid;
    if(!target.update && node.kind!="ArrayMultiply" && node.kind!="ArrayConcat" && node.kind!="InputSelector")return {};
    const auto previous=node.outputs;
    if(!derive(target.node,context.limits))return {ProcessingError::UnsupportedCombination,node.id};
    for(std::size_t i=0;i<std::min(previous.size(),node.outputs.size());++i)target.update=target.update || !same_contract(previous[i].contract.view(),node.outputs[i].contract.view());
    return {};
}
void attribute_rate_edge(const BindingContext& context,const NodeBindings& target,ProcessingDiagnostic& result) {
        if(target.asserted_rate && result.port==input_sample_rate_input)for(std::size_t i=0;i<context.graph.edges.size();++i) {
            const auto& edge=context.graph.edges[i];
            if(edge.to==target.node.id && edge.input_port==input_sample_rate_input){result.edge=i;break;}
        }

}
ProcessingDiagnostic bind_node_inputs(BindingContext& context,NodeBindings& target) {
    for(std::size_t i=0;i<context.graph.edges.size();++i)if(context.graph.edges[i].to==target.node.id) {
        auto result=bind_input(context,target,context.graph.edges[i]);
        if(!result){if(!result.edge)result.edge=i;return result;}
    }
    return {};
}
ProcessingDiagnostic bind_node(BindingContext& context,std::size_t index,NodeId changed) {
    NodeBindings target{context.graph.nodes[index],context.graph.nodes[index].id==changed};
    const auto bound=bind_node_inputs(context,target);if(!bound)return bound;
    auto result=refresh_bound_node(context,target);
    if(!result) {
        attribute_rate_edge(context,target,result);
        return result;
    }
    context.changed[index]=target.update;return {};
}
ProcessingDiagnostic validate_executable_edges(const NumericProcessingGraph& evaluated,const NumericProcessingGraph& candidate) {
    for(std::size_t i=0;i<candidate.edges.size();++i) {
        auto result=parameter_edge(candidate,i,-1);
        if(!result){
            const auto original=std::find(evaluated.edges.begin(),evaluated.edges.end(),candidate.edges[i]);
            result.edge=original==evaluated.edges.end()?std::optional<std::size_t>{}:std::optional<std::size_t>{std::size_t(original-evaluated.edges.begin())};
            return result;
        }
    }
    return {};
}
void strip_parameter_connections(NumericProcessingGraph& candidate) {
    std::erase_if(candidate.edges,[&](const auto& edge){return !numeric_parameter_key(candidate.nodes[at(candidate,edge.to)].kind,edge.input_port).empty();});
    for(auto& node:candidate.nodes)std::erase_if(node.inputs,[&](const auto& port){return !numeric_parameter_key(node.kind,port.id).empty();});
}
ProcessingDiagnostic executable_parameter_graph(const NumericProcessingGraph& evaluated,const ProcessingAdmissionLimits& limits,NumericProcessingGraph& out) {
    auto candidate=evaluated;
    strip_parameter_connections(candidate);
    for(const auto& node:candidate.nodes){ResolvedProcessingNode resolved;const auto result=resolve_processing_node(node,limits,resolved);if(!result)return result;}
    const auto valid=validate_executable_edges(evaluated,candidate);if(!valid)return valid;
    out=std::move(candidate);return {};
}
}
namespace {
ProcessingDiagnostic publish_parameter_graph(const NumericProcessingGraph& candidate,const ProcessingAdmissionLimits& limits,NumericProcessingGraph& evaluated,NumericProcessingGraph& executable) {
    NumericProcessingGraph canonical,processing;
    const auto bindings=resolve_configuration_bindings(candidate,limits,canonical);if(!bindings)return bindings;
    const auto result=executable_parameter_graph(canonical,limits,processing);if(!result)return result;
    evaluated=std::move(canonical);executable=std::move(processing);return {};
}

}
ProcessingDiagnostic resolve_numeric_parameter_bindings(const NumericProcessingGraph& source,const ProcessingAdmissionLimits& limits,
    NumericProcessingGraph& evaluated,NumericProcessingGraph& executable,NodeId changed_node,const PreparationValueProvider* provider) {
    if(source.nodes.size()>limits.max_nodes || source.edges.size()>limits.max_edges)return {ProcessingError::Limit};
    std::vector<std::size_t> order;order.reserve(source.nodes.size());
    const auto valid=preparation_order(source,changed_node,order);if(!valid)return valid;
    auto candidate=source;std::vector<bool> changed(source.nodes.size(),false);
    BindingContext context{candidate,limits,provider,changed};
    for(const auto index:order){const auto result=bind_node(context,index,changed_node);if(!result)return result;}
    return publish_parameter_graph(candidate,limits,evaluated,executable);
}

namespace {
ConfiguredProcessingNode auxiliary_literal_node(NodeId id,const ProcessingPort& port,NumericLiteral value) {
    ConfiguredProcessingNode constant;constant.id=id;constant.kind="Number";
    constant.parameters=EndpointProcessingParameters{{{"value",std::move(value)}}};
    constant.outputs={{0,PortDirection::Output,port.contract}};
    return constant;
}
RoutingAlias auxiliary_alias(const ConfiguredProcessingNode& owner,PortId port,NodeId source) {
    RoutingAlias alias;alias.source=source;alias.node=owner.id;alias.port=port;
    alias.include_input=false;alias.kind=*node_kind(owner.kind);return alias;
}
void install_auxiliary(const ConfiguredProcessingNode& owner,const ProcessingPort& port_value,NodeId id,NumericLiteral value,NumericProcessingGraph& graph,std::vector<RoutingAlias>& aliases) {
    auto& lowered=graph.nodes[at(graph,owner.id)];
    std::erase_if(lowered.outputs,[&](const auto& p){return p.id==port_value.id;});
    for(auto& edge:graph.edges)if(edge.from==owner.id && edge.output_port==port_value.id){edge.from=id;edge.output_port=0;}
    aliases.push_back(auxiliary_alias(owner,port_value.id,id));
    graph.nodes.push_back(auxiliary_literal_node(id,port_value,std::move(value)));
}
ProcessingDiagnostic lower_auxiliary(const NumericProcessingGraph& source,const ConfiguredProcessingNode& owner,
    const ProcessingPort& port_value,const ProcessingAdmissionLimits& limits,NodeId id,
    NumericProcessingGraph& graph,std::vector<RoutingAlias>& aliases,const PreparationValueProvider* provider) {
    if(graph.nodes.size()>=limits.max_nodes || id==std::numeric_limits<NodeId>::max())return {ProcessingError::Limit,owner.id};
    NumericLiteral value;
    const auto status=resolve_preparation_value(source,owner.id,port_value.id,value,provider);
    if(status!=Status::ok)return {preparation_status(status),owner.id,port_value.id};
    install_auxiliary(owner,port_value,id,std::move(value),graph,aliases);return {};
}
ProcessingDiagnostic next_metadata_node_id(const NumericProcessingGraph& source,NodeId& next) {
    for(const auto& node:source.nodes){
        if(node.id==std::numeric_limits<NodeId>::max())return {ProcessingError::Limit,node.id};
        next=std::max(next,NodeId(node.id+1));
    }
    return {};
}
ProcessingDiagnostic lower_node_metadata(const NumericProcessingGraph& source,const ConfiguredProcessingNode& node,const ProcessingAdmissionLimits& limits,NodeId& next,NumericProcessingGraph& candidate,std::vector<RoutingAlias>& mapped,const PreparationValueProvider* provider) {
        const auto primary=data_outputs(node).size();
        for(const auto& auxiliary:std::span<const ProcessingPort>(node.outputs).subspan(primary)){
            const auto result=lower_auxiliary(source,node,auxiliary,limits,next,candidate,mapped,provider);
            if(!result)return result;
            ++next;
        }
    return {};
}

}
ProcessingDiagnostic lower_numeric_metadata_outputs(const NumericProcessingGraph& source,const ProcessingAdmissionLimits& limits,
    NumericProcessingGraph& out,std::vector<RoutingAlias>& aliases,const PreparationValueProvider* provider) {
    auto candidate=source;std::vector<RoutingAlias> mapped;NodeId next=0;
    const auto available=next_metadata_node_id(source,next);if(!available)return available;
    for(const auto& node:source.nodes){
        const auto result=lower_node_metadata(source,node,limits,next,candidate,mapped,provider);if(!result)return result;
    }
    out=std::move(candidate);aliases=std::move(mapped);return {};
}
}
