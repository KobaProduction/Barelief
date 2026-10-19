#include "numeric_preparation.h"
#include "preparation_support.h"
#include "frame_access.h"
#include "signal_time.h"
#include "mixer_execution.h"
#include <dsp_kernels/spectral_weights.h>
#include <algorithm>
#include <numeric>
#include <cmath>
namespace dataflow_runtime {
namespace {
Status convert_constant(const NumericView& source,MutableNumericView& output,const NumericRuntimeProfile& profile,std::uint64_t& bits){
    dsp_kernels::PreparedConversion conversion;
    const auto prepared=dsp_kernels::prepare_conversion({*source.contract,*output.contract,{},profile.kernels,profile.admission.frame},profile.capabilities,conversion);
    if(prepared!=dsp_kernels::NumericStatus::Ok)return numeric_kernel_status(prepared);
    std::vector<std::byte> scratch(conversion.requirements.scratch_bytes);
    const auto converted=dsp_kernels::convert(conversion,source,output,scratch);
    if(converted.status!=dsp_kernels::NumericStatus::Ok)return numeric_kernel_status(converted.status);
    return load_bits(output.view(),{},output.contract->dtype,bits);
}
template<class T> Status materialize_constant(T literal,MutableNumericView& output,const NumericRuntimeProfile& profile,std::uint64_t& bits) {
    if(output.contract->dtype==dtype_of<T>()){
        const auto status=store_element(output,{},literal);if(status!=Status::ok)return status;
        return load_bits(output.view(),{},output.contract->dtype,bits);
    }
    const auto input=constant_literal_contract(*output.contract,dtype_of<T>());
    const PlaneLayout plane{0,{0,0}};
    const NumericView source{&input,std::as_bytes(std::span(&literal,1)),std::span(&plane,1),{1,1},{},{}};
    return convert_constant(source,output,profile,bits);
}
Status prepare_constant_value(const ResolvedProcessingNode& configured,PlanNode& node,State& state,const NumericRuntimeProfile& profile) {
    ProcessingParameterView parameter;
    if(resolve_processing_parameter(configured,"value",parameter)!=Status::ok)return Status::invalid_argument;
    const auto* literal=std::get_if<NumericLiteral>(&parameter);if(!literal)return Status::invalid_argument;
    auto output=mutable_output(node,state,0);
    return std::visit([&](auto value){return materialize_constant(value,output,profile,node.constant_bits);},*literal);
}
bool declared_frequency_values(const ResolvedProcessingNode& node,std::size_t bins,
                               std::vector<double>& frequencies) {
    double sample_rate=0,bin_hz=0,first_bin=0;
    ProcessingParameterView supplied;
    if(!numeric_parameter(node,"sample_rate",sample_rate) || sample_rate<=0 ||
       !numeric_parameter(node,"bin_hz",bin_hz) || bin_hz<=0 ||
       !numeric_parameter(node,"first_bin",first_bin) || first_bin<0 || std::floor(first_bin)!=first_bin ||
       resolve_processing_parameter(node,"frequencies_hz",supplied)!=Status::ok)return false;
    const auto* explicit_hz=std::get_if<std::span<const float>>(&supplied);
    if(!explicit_hz || (!explicit_hz->empty() && explicit_hz->size()!=bins))return false;
    frequencies.resize(bins);
    double previous=-1;
    for(std::size_t i=0;i<bins;++i){
        const double hz=explicit_hz->empty()?(first_bin+double(i))*bin_hz:double((*explicit_hz)[i]);
        if(!std::isfinite(hz) || hz<0 || hz<=previous || hz>sample_rate*.5)return false;
        frequencies[i]=hz;previous=hz;
    }
    return true;
}
}
Status NumericPreparation::prepare_nodes(){
    for(std::size_t i=0;i<g.nodes.size();++i){const auto status=prepare_node(i);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status NumericPreparation::prepare_node(std::size_t position){
    const auto configured=order[g.nodes.size()+position];const auto& cn=plan->impl_->numeric_graph->nodes[configured];
    ResolvedProcessingNode resolved;if(!resolve_processing_node(cn,profile.admission,resolved))return Status::invalid_argument;
    PlanNode n;auto& state=c->impl_->states[position];NodeFrame frame{configured,position,cn,resolved,n,state};
    auto status=prepare_node_header(frame);if(status!=Status::ok)return status;
    if(n.operation!=ProcessingOperation::Clock)status=prepare_node_processing(frame);
    if(status!=Status::ok)return status;
    plan->impl_->nodes.push_back(std::move(n));return Status::ok;
}
Status NumericPreparation::prepare_node_processing(NodeFrame& frame){
    const auto status=prepare_node_buffers(frame);return status==Status::ok?prepare_node_operations(frame):status;
}
Status NumericPreparation::prepare_node_header(NodeFrame& frame){
    {const auto status=initialize_node(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_metadata_values(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_node_parameters(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_input_endpoints(frame);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status NumericPreparation::prepare_node_buffers(NodeFrame& frame){
    {const auto status=prepare_output_contracts(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_output_storage(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_constant(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_array_values(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_input_storage(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_selector(frame);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status NumericPreparation::prepare_node_operations(NodeFrame& frame){
    {const auto status=prepare_node_links(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_node_kernels(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_matrix_operations(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_signal_operations(frame);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status NumericPreparation::prepare_node_links(NodeFrame& frame){
    {const auto status=prepare_parent(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_node_continuity(frame);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status NumericPreparation::prepare_node_kernels(NodeFrame& frame){
    {const auto status=prepare_generator(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_mixer(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_converter(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_packet_gain(frame);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status NumericPreparation::prepare_matrix_operations(NodeFrame& frame){
    {const auto status=prepare_matrix_window(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_matrix_fft(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_matrix_values(frame);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status NumericPreparation::prepare_signal_operations(NodeFrame& frame){
    {const auto status=prepare_operation(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_transform(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_source_rows(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_gather(frame);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status NumericPreparation::initialize_node(NodeFrame& frame){
    const auto& cn=frame.cn;
    auto& resolved=frame.resolved;
    auto& n=frame.n;
        n.configured=frame.configured;n.node.id=cn.id;n.operation=resolved.operation;n.node.enabled=cn.enabled;
        n.internal_conversion=std::none_of(original.nodes.begin(),original.nodes.end(),[&](const auto& node){return node.id==cn.id;});
        if(auto kind=node_kind(cn.kind))n.node.kind=*kind;
        if(!cn.inputs.empty())n.numeric_input=(resolved.operation==ProcessingOperation::InputSelector || resolved.operation==ProcessingOperation::Mixer || resolved.operation==ProcessingOperation::ArrayProduct || resolved.operation==ProcessingOperation::ChannelComparison || cn.kind=="ScalarView"?&cn.inputs.front():numeric_port(cn.inputs,0))->contract.view();
        if(resolved.operation==ProcessingOperation::ArrayProduct){OwnedProcessingContract output;if(array_product_output_contract(cn,output,&n.product_frame_input)!=Status::ok)return Status::invalid_argument;n.numeric_input=cn.inputs[n.product_frame_input].contract.view();}
    return Status::ok;
}
Status NumericPreparation::prepare_metadata_values(NodeFrame& frame){
    {const auto status=prepare_frequency_step(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_array_length(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_window_gain(frame);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status NumericPreparation::prepare_frequency_step(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
        if(n.node.kind==NodeKind::FrequencyStep){double size=0;if(!numeric_parameter(resolved,"fft_size",size) || size<=0)return Status::invalid_argument;n.spectrum_reference=1./size;}
    return Status::ok;
}
Status NumericPreparation::prepare_array_length(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
        if(n.node.kind==NodeKind::ArrayLength){double axis=0;if(!numeric_parameter(resolved,"axis",axis))return Status::invalid_argument;n.matrix_row=std::size_t(axis);}
    return Status::ok;
}
Status NumericPreparation::prepare_window_gain(NodeFrame& frame){
    const auto& cn=frame.cn;
    auto& resolved=frame.resolved;
    auto& n=frame.n;
        if(n.node.kind==NodeKind::WindowGain){ProcessingParameterView power;if(resolve_processing_parameter(resolved,"power",power)!=Status::ok || !std::holds_alternative<bool>(power))return Status::invalid_argument;n.spectrum_reference=processing_window_gain(cn,std::size_t(std::get<NumericContract>(n.numeric_input).shape.extents[0].maximum),std::get<bool>(power));}
    return Status::ok;
}

Status NumericPreparation::prepare_spectrum_peak(NodeFrame& frame){
    if(frame.n.operation!=ProcessingOperation::SpectrumPeak)return Status::ok;
    double step=0,first=0;
    if(!numeric_parameter(frame.resolved,"bin_hz",step) || !numeric_parameter(frame.resolved,"first_bin",first) ||
       !std::isfinite(step) || step<=0 || first<0 || first>UINT32_MAX || std::floor(first)!=first)return Status::invalid_argument;
    frame.n.spectrum_peak={step,std::uint32_t(first)};return Status::ok;
}

Status NumericPreparation::prepare_node_parameters(NodeFrame& frame){
    {const auto status=prepare_fill_gain(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_extraction(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_comparison(frame);if(status!=Status::ok)return status;}
    return prepare_spectrum_peak(frame);
}
Status NumericPreparation::prepare_fill_gain(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
        if(n.operation==ProcessingOperation::ArrayFill){double value=0;if(!numeric_parameter(resolved,"value",value))return Status::invalid_argument;n.gain=float(value);}
    return Status::ok;
}
Status NumericPreparation::prepare_extraction(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
        if(n.operation==ProcessingOperation::Extract){
            n.endpoint_plan=std::make_unique<PlanNode::EndpointPlan>();
            for(std::size_t axis=0;axis<3;++axis){double value=0;if(!numeric_parameter(resolved,std::array{"channel_index","index","column"}[axis],value))return Status::invalid_argument;n.endpoint_plan->indices[axis]=std::size_t(value);}
        }
    return Status::ok;
}
Status NumericPreparation::prepare_comparison(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
        if(n.operation==ProcessingOperation::ChannelComparison){double window=512;
            if(!numeric_parameter(resolved,"window_samples",window)||!numeric_parameter(resolved,"silence_threshold",n.comparison_threshold))return Status::invalid_argument;
            n.hop_base=std::size_t(window);}
    return Status::ok;
}

Status NumericPreparation::bind_input_endpoint(PlanNode::ScalarInput& input,NodeId target){
    for(const auto& edge:g.edges)if(edge.to==target&&edge.input_port==input.port){
        input.parent=index(plan->impl_->nodes,edge.from);input.parent_port=edge.output_port;
        if(input.parent==absent)return Status::invalid_argument;
        input.clock=plan->impl_->nodes[input.parent].clock;
    }
    return Status::ok;
}
Status NumericPreparation::prepare_input_endpoints(NodeFrame& frame){
    const auto& cn=frame.cn;auto& n=frame.n;
    if(cn.kind!="ScalarView"&&!paired_processing(n.operation)&&n.operation!=ProcessingOperation::WLEDEncoder&&
       n.operation!=ProcessingOperation::Mixer&&n.operation!=ProcessingOperation::ChannelComparison&&
       !(n.operation==ProcessingOperation::Switch&&std::get<RouteProcessingParameters>(cn.parameters).channels_per_stream>1))return Status::ok;
    n.endpoint_plan=std::make_unique<PlanNode::EndpointPlan>();n.endpoint_plan->inputs.reserve(cn.inputs.size());
    for(const auto& port:cn.inputs){
        PlanNode::ScalarInput input;input.contract=port.contract.view();input.port=port.id;
        const auto status=bind_input_endpoint(input,cn.id);if(status!=Status::ok)return status;
        n.endpoint_plan->inputs.push_back(std::move(input));
    }
    return Status::ok;
}
Status NumericPreparation::prepare_output_contracts(NodeFrame& frame){
    const auto& cn=frame.cn;
    auto& n=frame.n;
        n.numeric_outputs.resize(cn.outputs.size());for(const auto& port:cn.outputs)n.numeric_outputs[port.id]=port.contract.view();
        n.identity_bypass=!cn.enabled && !cn.inputs.empty() && cn.outputs.size()==1 &&
            (n.operation==ProcessingOperation::Converter || n.operation==ProcessingOperation::Transform || n.operation==ProcessingOperation::MusicTransform || n.operation==ProcessingOperation::Reduce || n.operation==ProcessingOperation::MatrixWindowFunction || n.operation==ProcessingOperation::MatrixCenter) &&
            same_physical_contract(n.numeric_input,n.numeric_outputs[0]);
    return Status::ok;
}
Status NumericPreparation::prepare_output_storage(NodeFrame& frame){
    const auto& cn=frame.cn;
    auto& n=frame.n;
    auto& state=frame.state;
        state.numeric_outputs.resize(cn.outputs.size());state.numeric_frames.resize(cn.outputs.size());state.source_stamps.resize(cn.outputs.size());state.source_markers.resize(cn.outputs.size());state.stream_storage.resize(profile.admission.frame.max_string_bytes);
        for(std::size_t j=0;j<cn.outputs.size();++j){const auto* output=std::get_if<NumericContract>(&n.numeric_outputs[j]);
            const bool shared=(n.operation==ProcessingOperation::Source && n.node.kind!=NodeKind::SignalGenerator && !numeric_constant_kind(cn.kind)) || (n.operation==ProcessingOperation::Transform && n.node.kind==NodeKind::Window && output && direct_window(*output));
            const auto status=numeric_buffer(n.numeric_outputs[j],state.numeric_outputs[j],shared?plan->impl_->silence:nullptr);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status NumericPreparation::prepare_constant(NodeFrame& frame){
    const auto& cn=frame.cn;
    auto& resolved=frame.resolved;
    auto& n=frame.n;
    auto& state=frame.state;
        if(numeric_constant_kind(cn.kind)){
            const auto status=prepare_constant_value(resolved,n,state,profile);if(status!=Status::ok)return status;
        }
    return Status::ok;
}
Status NumericPreparation::prepare_array_values(NodeFrame& frame){
    const auto& cn=frame.cn;
    auto& n=frame.n;
    auto& state=frame.state;
        if(n.operation==ProcessingOperation::ArrayFill){auto output=mutable_output(n,state,0);std::vector<float> values(output.actual[0],n.gain);std::size_t supplied=0;
            if(array_generator_values(cn,{},supplied)!=Status::ok || (supplied && array_generator_values(cn,values,supplied)!=Status::ok))return Status::invalid_argument;
            for(std::size_t j=0;j<output.actual[0];++j){const auto status=store_element(output,{0,j,0},n.node.enabled?values[j]:0.f);if(status!=Status::ok)return status;}}
    return Status::ok;
}
Status NumericPreparation::prepare_input_storage(NodeFrame& frame){
    const auto& cn=frame.cn;
    auto& n=frame.n;
    auto& state=frame.state;
        if(!cn.inputs.empty()){
            const auto& zero_contract=n.operation==ProcessingOperation::InputSelector?n.numeric_outputs.front():n.numeric_input;
            if(numeric_buffer(zero_contract,state.numeric_zero,plan->impl_->silence)!=Status::ok)return Status::invalid_argument;
            if(const auto* input=std::get_if<NumericContract>(&n.numeric_input);input && (input->shape.rank==Rank::Matrix || input->semantic==Semantic::History) && n.operation!=ProcessingOperation::History && n.operation!=ProcessingOperation::MusicTransform){
                state.route_masks.resize(std::max(std::size_t{1},cn.outputs.size()));
                state.route_state.resize(state.route_masks.size());for(auto& mask:state.route_masks)mask.resize(std::size_t(input->shape.extents[0].maximum)*2);
            }
        }
    return Status::ok;
}
bool NumericPreparation::selector_ancestry_known(std::size_t ancestor)const {
    while(ancestor!=absent){const auto& upstream=plan->impl_->nodes[ancestor];
        if(upstream.operation==ProcessingOperation::Source||upstream.operation==ProcessingOperation::InputSelector||upstream.operation==ProcessingOperation::Mixer)return true;
        ancestor=upstream.parent;
    }
    return false;
}
Status NumericPreparation::prepare_selector_lane(NodeFrame& frame,std::size_t lane){
    auto& input=frame.n.endpoint_plan->inputs[lane];input.contract=frame.cn.outputs[lane].contract.view();
    input.port=PortId(std::size_t(frame.n.selected_input)*frame.cn.outputs.size()+lane);
    for(const auto& edge:g.edges)if(edge.to==frame.cn.id&&edge.input_port==input.port){
        input.parent=index(plan->impl_->nodes,edge.from);input.parent_port=edge.output_port;
        if(input.parent==absent)return Status::invalid_argument;
        if(!selector_ancestry_known(input.parent))input.parent=absent;
    }
    return Status::ok;
}
Status NumericPreparation::prepare_selector(NodeFrame& frame){
    auto& n=frame.n;if(n.operation!=ProcessingOperation::InputSelector)return Status::ok;
    double selected=0;if(!numeric_parameter(frame.resolved,"selected_input",selected))return Status::invalid_argument;
    n.selected_input=std::uint16_t(selected);if(frame.cn.outputs.size()<=1)return Status::ok;
    n.endpoint_plan=std::make_unique<PlanNode::EndpointPlan>();n.endpoint_plan->inputs.resize(frame.cn.outputs.size());
    for(std::size_t lane=0;lane<frame.cn.outputs.size();++lane){const auto status=prepare_selector_lane(frame,lane);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status NumericPreparation::prepare_parent(NodeFrame& frame){
    {const auto status=prepare_ordinary_parent(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_group_parent(frame);if(status!=Status::ok)return status;}
    {const auto status=prepare_paired_parent(frame);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status NumericPreparation::prepare_ordinary_parent(NodeFrame& frame){
    const auto& cn=frame.cn;auto& n=frame.n;
    for(const auto& edge:g.edges)if(!n.scalar_view() && !paired_processing(n.operation) && n.operation!=ProcessingOperation::Mixer && n.operation!=ProcessingOperation::ChannelComparison && !(n.operation==ProcessingOperation::Switch&&n.endpoint_plan) && edge.to==cn.id &&
        (n.operation!=ProcessingOperation::InputSelector || edge.input_port==std::size_t(n.selected_input)*cn.outputs.size()) &&
        (n.operation!=ProcessingOperation::WLEDEncoder || edge.input_port==0)){
        n.parent=index(plan->impl_->nodes,edge.from);n.parent_port=edge.output_port;
        if(n.parent==absent)return Status::invalid_argument;
        n.clock=plan->impl_->nodes[n.parent].clock;
    }
    return Status::ok;
}
Status NumericPreparation::prepare_group_parent(NodeFrame& frame){
    const auto& cn=frame.cn;auto& n=frame.n;
        if((n.operation==ProcessingOperation::InputSelector || n.operation==ProcessingOperation::Switch)&&n.endpoint_plan) {
            n.parent=absent;
            for(const auto& lane:n.endpoint_plan->inputs)if(lane.parent!=absent){n.parent=lane.parent;n.parent_port=lane.parent_port;n.clock=plan->impl_->nodes[lane.parent].clock;break;}
        }
    return Status::ok;
}
Status NumericPreparation::prepare_paired_parent(NodeFrame& frame){
    const auto& cn=frame.cn;auto& n=frame.n;
        if(paired_processing(n.operation)){
            for(const auto& input:n.endpoint_plan->inputs)if(input.parent!=absent){n.parent=input.parent;n.parent_port=input.parent_port;n.clock=plan->impl_->nodes[input.parent].clock;break;}
            if(n.operation==ProcessingOperation::ArrayProduct){const auto& driver=n.endpoint_plan->inputs[n.product_frame_input];if(driver.parent!=absent){n.parent=driver.parent;n.parent_port=driver.parent_port;n.clock=plan->impl_->nodes[driver.parent].clock;}}
        }
    return Status::ok;
}
bool NumericPreparation::parent_changed(std::size_t current,std::size_t prior)const {
    if((current==absent)!=(prior==absent))return true;
    return current!=absent&&plan->impl_->nodes[current].node.id!=previous->impl_->nodes[prior].node.id;
}
void NumericPreparation::update_selector_continuity(NodeFrame& frame){
    auto& n=frame.n;if(n.operation!=ProcessingOperation::InputSelector||frame.old==absent)return;
    const auto& prior=previous->impl_->nodes[frame.old];
    n.selector_changed=n.selected_input!=prior.selected_input||n.parent_port!=prior.parent_port||parent_changed(n.parent,prior.parent);
    if(!n.endpoint_plan||!prior.endpoint_plan||n.endpoint_plan->inputs.size()!=prior.endpoint_plan->inputs.size())return;
    for(std::size_t lane=0;lane<n.endpoint_plan->inputs.size();++lane){const auto& a=n.endpoint_plan->inputs[lane];const auto& b=prior.endpoint_plan->inputs[lane];
        n.selector_changed=n.selector_changed||a.parent_port!=b.parent_port||parent_changed(a.parent,b.parent);
    }
}
void NumericPreparation::recover_node_clock(NodeFrame& frame){
    auto& n=frame.n;if(n.clock!=absent||n.reset)return;
    const auto prior=previous->impl_->nodes[frame.old].clock;
    if(prior!=absent)n.clock=index(plan->impl_->nodes,previous->impl_->nodes[prior].node.id);
}
Status NumericPreparation::prepare_node_continuity(NodeFrame& frame){
    auto& n=frame.n;const auto& cn=frame.cn;
    if(n.operation==ProcessingOperation::Source||n.operation==ProcessingOperation::InputSelector)n.clock=frame.position;
    frame.old=previous&&previous->impl_->numeric_graph&&previous->impl_->numeric_graph->graph_id==g.graph_id?index(previous->impl_->nodes,cn.id):absent;
    n.reset=frame.old==absent||!numeric_compatible_node(cn,previous->impl_->numeric_graph->nodes[previous->impl_->nodes[frame.old].configured]);
    update_selector_continuity(frame);
    if(!n.reset&&old_context->impl_->states[frame.old].numeric_stamp.stream_id.size()>frame.state.stream_storage.size())return Status::exhausted;
    recover_node_clock(frame);return Status::ok;
}
Status NumericPreparation::prepare_generator(NodeFrame& frame){
    const auto& cn=frame.cn;
    auto& n=frame.n;
    auto& state=frame.state;
        if(n.node.kind==NodeKind::SignalGenerator){state.kernel=OscillatorKernel{};
            if(!std::get<OscillatorKernel>(state.kernel).value->prepare(std::get<SignalGeneratorProcessingParameters>(cn.parameters).oscillators,std::get<NumericContract>(n.numeric_outputs[0]).clock.sample_rate))return Status::invalid_argument;
        }
    return Status::ok;
}
Status NumericPreparation::prepare_hop(PlanNode& node,std::uint32_t rate){
    std::uint64_t hop=0;
    if(!divide_products(std::uint64_t(original.execution_interval.numerator),rate,original.execution_interval.denominator,1,hop,&node.hop_fraction))return Status::overflow;
    if(!hop||hop>SIZE_MAX||(hop==SIZE_MAX&&node.hop_fraction))return Status::invalid_argument;
    node.hop_base=std::size_t(hop);node.hop_denominator=original.execution_interval.denominator;return Status::ok;
}
void NumericPreparation::prepare_mixer_weights(NodeFrame& frame,const MixerProcessingParameters& parameters){
    auto& n=frame.n;n.endpoint_plan->weights.resize(frame.cn.inputs.size());
    for(std::size_t lane=0;lane<frame.cn.inputs.size();++lane)for(const auto& weight:parameters.weights)if(weight.port==frame.cn.inputs[lane].id){
        n.endpoint_plan->weights[lane]=weight.weight;n.endpoint_plan->weight_scale=std::max(n.endpoint_plan->weight_scale,weight.weight);
    }
}
void NumericPreparation::prepare_mixer_queue(NodeFrame& frame,std::size_t lane,const MixerProcessingParameters& parameters){
    const auto contract=std::get<NumericContract>(frame.cn.inputs[lane].contract.view());auto& queue=frame.state.mixer_queues[lane];
    const auto capacity=!spectral_vector(contract)?std::max<std::uint64_t>(contract.shape.extents[0].maximum,(std::uint64_t(contract.clock.sample_rate)*parameters.fifo_milliseconds+999)/1000):contract.shape.extents[0].maximum*2;
    queue.samples.resize(capacity);
    if(!spectral_vector(contract))queue.provenance.resize(capacity);
    if(aligned_operation(frame.n,*plan->impl_->numeric_graph))queue.aligned.resize(capacity);
    if(contract.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE)for(auto& coordinates:queue.coordinates)coordinates.resize(contract.shape.extents[0].maximum*8);
}
Status NumericPreparation::prepare_mixer(NodeFrame& frame){
    auto& n=frame.n;if(n.operation!=ProcessingOperation::Mixer&&n.operation!=ProcessingOperation::ChannelComparison)return Status::ok;
    const auto parameters=n.operation==ProcessingOperation::Mixer?std::get<MixerProcessingParameters>(frame.cn.parameters):MixerProcessingParameters{};
    frame.state.mixer_queues.resize(frame.cn.inputs.size());
    if(n.operation==ProcessingOperation::Mixer)prepare_mixer_weights(frame,parameters);
    const auto& output=std::get<NumericContract>(n.numeric_outputs[0]);
    if(spectral_vector(output)){const auto status=prepare_hop(n,output.clock.sample_rate);if(status!=Status::ok)return status;}
    for(std::size_t lane=0;lane<frame.cn.inputs.size();++lane)prepare_mixer_queue(frame,lane,parameters);
    return Status::ok;
}
Status NumericPreparation::prepare_conversion_rows(NodeFrame& frame){
    auto& n=frame.n;auto& state=frame.state;
                const NumericGatherRequest request{std::get<NumericContract>(n.numeric_input),{},
                    {profile.admission.frame,profile.admission.frame.max_payload_bytes,profile.admission.frame.max_metadata_bytes,profile.kernels.max_scratch_bytes},true};
                NumericGatherRequirements req;if(numeric_gather_requirements(request,req)!=Status::ok)return Status::invalid_argument;
                auto owner=std::make_shared<GatherOwner>(req.plan_metadata_bytes);
                if(prepare_numeric_gather(request,owner->pool,owner->gather)!=Status::ok)return Status::invalid_argument;
                n.input_gather=std::move(owner);state.input_gather_payload.resize(req.payload_bytes);state.input_gather_metadata.resize(req.output_metadata_bytes);state.input_gather_scratch.resize(req.scratch_bytes);
                state.row_planes.resize(std::get<NumericContract>(n.numeric_outputs[0]).channel_order.size());
                n.row_conversion=std::make_unique<dsp_kernels::PreparedConversion>();
                const auto status=dsp_kernels::prepare_conversion({n.input_gather->gather.output_contract(),numeric_projection(std::get<NumericContract>(n.numeric_outputs[0]),{},true),
                    std::get<ConverterProcessingParameters>(frame.cn.parameters).policy,profile.kernels,profile.admission.frame},profile.capabilities,*n.row_conversion);
                if(status!=dsp_kernels::NumericStatus::Ok)return numeric_kernel_status(status);
    return Status::ok;
}
Status NumericPreparation::prepare_converter(NodeFrame& frame){
    auto& n=frame.n;if(n.operation!=ProcessingOperation::Converter)return Status::ok;
    n.conversion=std::make_unique<dsp_kernels::PreparedConversion>();
    const auto status=dsp_kernels::prepare_conversion({std::get<NumericContract>(n.numeric_input),std::get<NumericContract>(n.numeric_outputs[0]),std::get<ConverterProcessingParameters>(frame.cn.parameters).policy,profile.kernels,profile.admission.frame},profile.capabilities,*n.conversion);
    if(status!=dsp_kernels::NumericStatus::Ok)return numeric_kernel_status(status);
    frame.state.numeric_scratch.resize(n.conversion->requirements.scratch_bytes);
    return std::get<NumericContract>(n.numeric_input).shape.rank==Rank::Matrix?prepare_conversion_rows(frame):Status::ok;
}
Status NumericPreparation::prepare_packet_gain(NodeFrame& frame){
    auto& node=frame.n;if(node.operation!=ProcessingOperation::WLEDEncoder)return Status::ok;
    double gain=0,peak=0,magnitude=0;
    if(!numeric_parameter(frame.resolved,"gain",gain) || !numeric_parameter(frame.resolved,"peak_hz",peak) || !numeric_parameter(frame.resolved,"magnitude",magnitude))return Status::invalid_argument;
    if(!node.endpoint_plan || node.endpoint_plan->inputs.size()!=3)return Status::invalid_argument;
    node.gain=float(gain);node.wled_peak_hz=float(peak);node.wled_magnitude=float(magnitude);
    frame.state.values.resize(std::size_t(std::get<NumericContract>(node.numeric_input).shape.extents[0].maximum));
    frame.state.wled_inputs.resize(1);
    frame.state.wled_inputs.front().stream.resize(profile.admission.frame.max_string_bytes);
    return Status::ok;
}
Status NumericPreparation::prepare_matrix_window(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
    auto& state=frame.state;
    const auto old=frame.old;
        if(n.operation==ProcessingOperation::MatrixWindowFunction){
            const auto& in=std::get<NumericContract>(n.numeric_input);
            ProcessingParameterView value;resolve_processing_parameter(resolved,"function",value);const auto function=std::get<std::string_view>(value);
            if(!dsp_kernels::parse_window_function(function,n.window_function))return Status::invalid_argument;
            const auto columns=std::size_t(in.shape.extents[1].maximum);
            if(old!=absent && previous->impl_->nodes[old].windows && previous->impl_->nodes[old].windows->maximum()==columns)
                n.windows=previous->impl_->nodes[old].windows;
            else n.windows=std::make_shared<const dsp_kernels::PreparedWindows>(columns);
            state.coefficients.resize(columns);
        }
    return Status::ok;
}
Status NumericPreparation::prepare_matrix_fft(NodeFrame& frame){
    auto& n=frame.n;
    auto& state=frame.state;
        if(n.operation==ProcessingOperation::MatrixFFT){
            const auto& in=std::get<NumericContract>(n.numeric_input);const auto& out=std::get<NumericContract>(n.numeric_outputs[0]);
            const auto samples=std::size_t(out.shape.extents[1].maximum)*2;
            state.left.resize(samples);state.values.resize(samples/2);
            state.kernel.emplace<dsp_kernels::MagnitudeSpectrum>();if(!std::get<dsp_kernels::MagnitudeSpectrum>(state.kernel).prepare(samples))return Status::unavailable;
        }
    return Status::ok;
}
Status NumericPreparation::prepare_matrix_values(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
        if(n.operation==ProcessingOperation::MatrixRow){double row=0;if(!numeric_parameter(resolved,"row",row))return Status::invalid_argument;n.matrix_row=std::size_t(row);}
        if(n.operation==ProcessingOperation::MatrixCenter){double method=0;
            if(!numeric_parameter(resolved,"method",method) || !numeric_parameter(resolved,"offset",n.center_offset))return Status::invalid_argument;
            n.center_method=unsigned(method);}
    return Status::ok;
}
Status NumericPreparation::prepare_operation(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
    auto& state=frame.state;
        if(n.operation==ProcessingOperation::MusicTransform || paired_processing(n.operation)){
            const auto status=prepare_music(n,state,resolved,profile);if(status!=Status::ok)return status;
        }
    return Status::ok;
}
Status NumericPreparation::prepare_transform_pcm_resampler(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
    auto& state=frame.state;
    const auto& in=std::get<NumericContract>(frame.n.numeric_input);
    const auto& out=std::get<NumericContract>(frame.n.numeric_outputs[0]);
    const auto columns=std::size_t(in.shape.extents[0].maximum);
    const auto count=std::size_t(out.shape.extents[0].maximum);

                state.left.resize(columns*in.channel_order.size());state.values.resize(count*in.channel_order.size());
                state.kernel.emplace<ResamplerKernel>();
                {dsp_kernels::ResamplerConfig config;if(!resampler_config(resolved,config) ||
                   !std::get<ResamplerKernel>(state.kernel).operation().prepare(config,profile.resampler_backend))return Status::invalid_argument;
                   n.resampler_mode=config.mode;n.resampler_quality=config.quality;}
                return Status::ok;

}
Status NumericPreparation::prepare_transform_window(NodeFrame& frame){
    auto& n=frame.n;
    auto& state=frame.state;
    const auto& in=std::get<NumericContract>(frame.n.numeric_input);
    const auto& out=std::get<NumericContract>(frame.n.numeric_outputs[0]);
    const auto columns=std::size_t(in.shape.extents[0].maximum);
    const auto count=std::size_t(out.shape.extents[0].maximum);
    double input_rate=0;
    if(!numeric_parameter(frame.resolved,"input_rate",input_rate) || input_rate<1 ||
       input_rate>UINT32_MAX || std::floor(input_rate)!=input_rate)return Status::invalid_argument;
    const auto status=prepare_hop(n,std::uint32_t(input_rate));
    if(status!=Status::ok)return status;
    const auto emissions=columns/n.hop_base+(columns%n.hop_base!=0);
    if(emissions>profile.max_windows_per_ingress ||
       (emissions && g.nodes.size()>profile.max_callback_deliveries_per_ingress/emissions) ||
       (emissions && g.nodes.size() &&
        profile.admission.max_ports_per_node>profile.max_callback_deliveries_per_ingress/emissions/g.nodes.size()))
        return Status::exhausted;
    state.pcm.resize(count*in.channel_order.size());
    return Status::ok;
}
Status NumericPreparation::prepare_transform_window_function(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
    auto& state=frame.state;
    const auto old=frame.old;
    const auto& out=std::get<NumericContract>(frame.n.numeric_outputs[0]);
    const auto count=std::size_t(out.shape.extents[0].maximum);
 {
                ProcessingParameterView value;resolve_processing_parameter(resolved,"function",value);const auto function=std::get<std::string_view>(value);
                if(!dsp_kernels::parse_window_function(function,n.window_function))return Status::invalid_argument;
                if(old!=absent && previous->impl_->nodes[old].windows && previous->impl_->nodes[old].windows->maximum()==count)
                    n.windows=previous->impl_->nodes[old].windows;
                else n.windows=std::make_shared<const dsp_kernels::PreparedWindows>(count);
                auto output=mutable_output(n,state,0);const auto coefficients=n.windows->coefficients(n.window_function);
                for(std::size_t j=0;j<count;++j){const auto status=store_element(output,{0,j,0},n.node.enabled?coefficients[j]:1.f);if(status!=Status::ok)return status;}
                return Status::ok;}

}
Status NumericPreparation::prepare_transform_bark_weighting(NodeFrame& frame){
    auto& state=frame.state;
    const auto& in=std::get<NumericContract>(frame.n.numeric_input);
    const auto columns=std::size_t(in.shape.extents[0].maximum);
    if(!declared_frequency_values(frame.resolved,columns,state.frequencies))return Status::invalid_argument;
    state.coefficients.resize(columns);
    for(std::size_t i=0;i<columns;++i)
        state.coefficients[i]=float(dsp_kernels::bark_weight(state.frequencies[i]));
    return Status::ok;
}
Status NumericPreparation::prepare_transform_stereo_fft(NodeFrame& frame){
    auto& state=frame.state;
    const auto& in=std::get<NumericContract>(frame.n.numeric_input);
    const auto columns=std::size_t(in.shape.extents[0].maximum);
state.kernel.emplace<dsp_kernels::MagnitudeSpectrum>();if(!std::get<dsp_kernels::MagnitudeSpectrum>(state.kernel).prepare(columns))return Status::unavailable;return Status::ok;

}
Status NumericPreparation::prepare_transform_mel(NodeFrame& frame){
    auto& state=frame.state;
    const auto& in=std::get<NumericContract>(frame.n.numeric_input);
    const auto& out=std::get<NumericContract>(frame.n.numeric_outputs[0]);
    const auto columns=std::size_t(in.shape.extents[0].maximum);
    const auto count=std::size_t(out.shape.extents[0].maximum);
    double sample_rate=0;
    if(!numeric_parameter(frame.resolved,"sample_rate",sample_rate) || sample_rate<1 ||
       sample_rate>UINT32_MAX || std::floor(sample_rate)!=sample_rate ||
       !declared_frequency_values(frame.resolved,columns,state.frequencies))return Status::invalid_argument;
    state.left.resize(columns);state.values.resize(count);
    state.kernel.emplace<dsp_kernels::MelFilterBank>();
    return std::get<dsp_kernels::MelFilterBank>(state.kernel).prepare(
        columns,std::size_t(sample_rate),count,state.frequencies)?Status::ok:Status::unavailable;
}
Status NumericPreparation::prepare_transform_spectrum_merge(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
 {double merge_value=0;if(!numeric_parameter(resolved,"merge",merge_value))return Status::invalid_argument;n.merge_mode=unsigned(merge_value);return Status::ok;}

}
Status NumericPreparation::prepare_transform_spectrum_gain(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
 {double gain=0;if(!numeric_parameter(resolved,"gain",gain))return Status::invalid_argument;n.gain=float(gain);return Status::ok;}

}
Status NumericPreparation::prepare_transform_spectrum_amplitude(NodeFrame& frame){
    auto& resolved=frame.resolved;
    auto& n=frame.n;
 {
                double size=0,coherent=0;ProcessingParameterView positive;
                if(!numeric_parameter(resolved,"fft_size",size) || !numeric_parameter(resolved,"coherent_gain",coherent) ||
                   resolve_processing_parameter(resolved,"positive_half",positive)!=Status::ok || !std::holds_alternative<bool>(positive))return Status::invalid_argument;
                const auto half=std::get<bool>(positive);n.half_last=half && (std::uint64_t(size)%2==0);n.gain=float((half?2.:1.)/(size*coherent));return Status::ok;
            }

}
Status NumericPreparation::prepare_transform(NodeFrame& frame){
    if(frame.n.operation!=ProcessingOperation::Transform)return Status::ok;
    switch(frame.n.node.kind){
    case NodeKind::PcmResampler:case NodeKind::Window:case NodeKind::WindowFunction:case NodeKind::StereoFFT:return prepare_signal_transform(frame);
    default:return prepare_spectrum_transform(frame);
    }
}
Status NumericPreparation::prepare_signal_transform(NodeFrame& frame){
    switch(frame.n.node.kind){
    case NodeKind::PcmResampler:return prepare_transform_pcm_resampler(frame);
    case NodeKind::Window:return prepare_transform_window(frame);
    case NodeKind::WindowFunction:return prepare_transform_window_function(frame);
    case NodeKind::StereoFFT:return prepare_transform_stereo_fft(frame);
    default:return Status::invalid_argument;
    }
}
Status NumericPreparation::prepare_spectrum_transform(NodeFrame& frame){
    switch(frame.n.node.kind){
    case NodeKind::BarkWeighting:return prepare_transform_bark_weighting(frame);
    case NodeKind::Mel:return prepare_transform_mel(frame);
    case NodeKind::SpectrumMerge:return prepare_transform_spectrum_merge(frame);
    case NodeKind::SpectrumGain:return prepare_transform_spectrum_gain(frame);
    case NodeKind::SpectrumAmplitude:return prepare_transform_spectrum_amplitude(frame);
    default:return Status::invalid_argument;
    }
}
Status NumericPreparation::prepare_source_rows(NodeFrame& frame){
    const auto& cn=frame.cn;
    auto& n=frame.n;
    auto& state=frame.state;
        if(n.operation==ProcessingOperation::Source && std::holds_alternative<NumericContract>(n.numeric_outputs[0]) &&
           (std::get<NumericContract>(n.numeric_outputs[0]).shape.rank==Rank::Matrix || std::get<NumericContract>(n.numeric_outputs[0]).semantic==Semantic::History)){
            state.route_masks.resize(cn.outputs.size());
            state.route_state.resize(state.route_masks.size());for(auto& mask:state.route_masks)mask.resize(std::size_t(std::get<NumericContract>(n.numeric_outputs[0]).shape.extents[0].maximum)*2);}
    return Status::ok;
}
Status NumericPreparation::prepare_gather_storage(NodeFrame& frame,const NumericGatherRequest& request){
    NumericGatherRequirements requirements;auto status=numeric_gather_requirements(request,requirements);if(status!=Status::ok)return status;
    auto owner=std::make_shared<GatherOwner>(requirements.plan_metadata_bytes);
    status=prepare_numeric_gather(request,owner->pool,owner->gather);if(status!=Status::ok)return status;
    frame.n.gather=std::move(owner);frame.state.gather_payload.resize(requirements.payload_bytes);
    frame.state.gather_metadata.resize(requirements.output_metadata_bytes);frame.state.gather_scratch.resize(requirements.scratch_bytes);return Status::ok;
}
Status NumericPreparation::prepare_reduction(NodeFrame& frame){
    auto& n=frame.n;if(n.operation!=ProcessingOperation::Reduce)return Status::ok;
    const auto& parameters=std::get<ReduceProcessingParameters>(frame.cn.parameters);
    n.reduction=std::make_unique<dsp_kernels::PreparedReduction>();
    const auto status=dsp_kernels::prepare_reduction({n.gather->gather.output_contract(),std::get<NumericContract>(n.numeric_outputs[0]),numeric_reduction(parameters.operation),parameters.precision_loss,parameters.rounding,profile.kernels,profile.admission.frame},profile.capabilities,*n.reduction);
    if(status!=dsp_kernels::NumericStatus::Ok)return numeric_kernel_status(status);
    frame.state.numeric_scratch.resize(n.reduction->requirements.scratch_bytes);return Status::ok;
}
Status NumericPreparation::prepare_gather(NodeFrame& frame){
    auto& n=frame.n;
    if(!((n.operation==ProcessingOperation::History && std::get<NumericContract>(n.numeric_outputs[0]).shape.rank==Rank::Matrix) || n.operation==ProcessingOperation::Reduce ||
           ((n.operation==ProcessingOperation::Source || n.operation==ProcessingOperation::Converter) && std::holds_alternative<NumericContract>(n.numeric_outputs[0]) && std::get<NumericContract>(n.numeric_outputs[0]).shape.rank==Rank::Matrix) ||
           (n.operation==ProcessingOperation::Sink && std::holds_alternative<NumericContract>(n.numeric_input) && std::get<NumericContract>(n.numeric_input).shape.rank==Rank::Matrix)))return Status::ok;
    const auto request=numeric_gather_request(frame.resolved,profile);
    const auto status=prepare_gather_storage(frame,request);return status==Status::ok?prepare_reduction(frame):status;
}

}
