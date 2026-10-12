#include "runtime_internal.h"
#include "preparation_support.h"
#include "signal_time.h"
#include "mixer_execution.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <stdexcept>
#include <new>

namespace dataflow_runtime {
using namespace detail;
using namespace domain;
using namespace signal_contracts;
Status PreparedGraph::prepare_numeric(const NumericProcessingGraph& original,const NumericRuntimeProfile& profile,
    std::shared_ptr<const PreparedGraph>& result,std::unique_ptr<RuntimeContext>& context,const PreparedGraph* previous,const RuntimeContext* old_context) {
#if defined(__cpp_exceptions)
    try {
#endif
    NumericProcessingGraph lowered;
    if(!valid_execution_interval(original.execution_interval))return Status::invalid_argument;
    std::size_t preflight=0;const auto footprint=processing_graph_owned_bytes(original,profile.admission,preflight);
    if(footprint!=Status::ok)return footprint;
    const auto lowering=lower_dtype_edges(original,profile.admission,lowered);
    if(!lowering)return lowering.error==ProcessingError::Limit?Status::exhausted:Status::invalid_argument;
    const auto& g=lowered;
    if((previous && (!old_context || !old_context->impl_ || old_context->impl_->plan!=previous)) || (!previous && old_context))return Status::invalid_argument;
    if(g.nodes.size()>profile.admission.max_nodes || g.nodes.size()>SIZE_MAX/2)return Status::exhausted;
    std::vector<std::size_t> order(g.nodes.size()*2);ProcessingGraphAdmission admission;auto checked_profile=profile;
    std::size_t source_bytes=0;
    if(g.nodes.size()!=original.nodes.size()){
        const auto footprint=processing_graph_owned_bytes(original,profile.admission,source_bytes);if(footprint!=Status::ok)return footprint;
        if(!numeric_add(checked_profile.resources.retained_working_bytes,source_bytes))return Status::overflow;
    }
    if(previous && old_context){const auto mem=previous->memory_requirements();std::size_t old=mem.state_bytes;
        if(!numeric_add(old,mem.buffer_bytes) || !numeric_add(old,mem.scratch_bytes))return Status::overflow;
        if(previous->impl_->silence && previous->impl_->silence->size()==profile.admission.frame.max_payload_bytes)old-=previous->impl_->silence->size();
        if(!numeric_add(checked_profile.resources.retained_working_bytes,old))return Status::overflow;}
    const auto check=admit_processing_graph(g,profile.admission,numeric_capabilities(checked_profile),order,admission);
    if(!check)return check.error==ProcessingError::ResourceLimit || check.error==ProcessingError::Limit?Status::exhausted:Status::invalid_argument;
    // Domain peak already includes DTO ownership and topology workspace. Publish
    // retained plan ownership too, so a later Prepare cannot omit the old DTO.
    const auto metadata_and_plan=admission.peak_bytes-checked_profile.resources.retained_working_bytes-order.size()*sizeof(std::size_t)-
        admission.resources.payload_bytes-admission.resources.scratch_bytes-admission.resources.stack_bytes-profile.admission.frame.max_payload_bytes;
    auto plan=std::shared_ptr<PreparedGraph>(new PreparedGraph);auto c=std::make_unique<RuntimeContext>();
    plan->impl_->numeric_graph=std::make_shared<const NumericProcessingGraph>(g);plan->impl_->numeric_profile=profile;
    plan->impl_->numeric_source_graph=source_bytes?std::make_shared<const NumericProcessingGraph>(original):plan->impl_->numeric_graph;
    plan->impl_->silence=previous && previous->impl_->silence && previous->impl_->silence->size()==profile.admission.frame.max_payload_bytes?
        previous->impl_->silence:std::make_shared<const std::vector<std::byte>>(profile.admission.frame.max_payload_bytes);
    plan->impl_->memory={metadata_and_plan+source_bytes,admission.resources.payload_bytes+profile.admission.frame.max_payload_bytes,admission.resources.scratch_bytes};
    plan->impl_->numeric_admission=admission;
    plan->impl_->nodes.reserve(g.nodes.size());c->impl_->states.resize(g.nodes.size());
    for(std::size_t i=0;i<g.nodes.size();++i){const auto configured=order[g.nodes.size()+i];const auto& cn=plan->impl_->numeric_graph->nodes[configured];
        ResolvedProcessingNode resolved;if(!resolve_processing_node(cn,profile.admission,resolved))return Status::invalid_argument;
        PlanNode n;n.configured=configured;n.node.id=cn.id;n.operation=resolved.operation;n.node.enabled=cn.enabled;
        n.internal_conversion=std::none_of(original.nodes.begin(),original.nodes.end(),[&](const auto& node){return node.id==cn.id;});
        if(auto kind=node_kind(cn.kind))n.node.kind=*kind;
        if(!cn.inputs.empty())n.numeric_input=(resolved.operation==ProcessingOperation::InputSelector || resolved.operation==ProcessingOperation::Mixer || resolved.operation==ProcessingOperation::ChannelComparison || cn.kind=="ScalarView"?&cn.inputs.front():numeric_port(cn.inputs,0))->contract.view();
        if(n.operation==ProcessingOperation::Extract){
            n.endpoint_plan=std::make_unique<PlanNode::EndpointPlan>();
            for(std::size_t axis=0;axis<3;++axis){double value=0;if(!numeric_parameter(resolved,std::array{"channel_index","index","column"}[axis],value))return Status::invalid_argument;n.endpoint_plan->indices[axis]=std::size_t(value);}
        }
        if(n.operation==ProcessingOperation::ChannelComparison){double window=512;
            if(!numeric_parameter(resolved,"window_samples",window)||!numeric_parameter(resolved,"silence_threshold",n.comparison_threshold))return Status::invalid_argument;
            n.hop_base=std::size_t(window);}
        if(cn.kind=="ScalarView" || n.operation==ProcessingOperation::Mixer || n.operation==ProcessingOperation::ChannelComparison || (n.operation==ProcessingOperation::Switch&&std::get<RouteProcessingParameters>(cn.parameters).channels_per_stream>1)){
            n.endpoint_plan=std::make_unique<PlanNode::EndpointPlan>();n.endpoint_plan->inputs.reserve(cn.inputs.size());
            for(const auto& port:cn.inputs){PlanNode::ScalarInput input;input.contract=port.contract.view();input.port=port.id;
                for(const auto& edge:g.edges)if(edge.to==cn.id && edge.input_port==port.id){input.parent=index(plan->impl_->nodes,edge.from);input.parent_port=edge.output_port;if(input.parent==absent)return Status::invalid_argument;input.clock=plan->impl_->nodes[input.parent].clock;}
                n.endpoint_plan->inputs.push_back(std::move(input));}
        }
        if(n.operation==ProcessingOperation::Clock){plan->impl_->nodes.push_back(std::move(n));continue;}
        n.numeric_outputs.resize(cn.outputs.size());for(const auto& port:cn.outputs)n.numeric_outputs[port.id]=port.contract.view();
        n.identity_bypass=!cn.enabled && !cn.inputs.empty() && cn.outputs.size()==1 &&
            (n.operation==ProcessingOperation::Converter || n.operation==ProcessingOperation::Transform || n.operation==ProcessingOperation::Reduce) &&
            same_physical_contract(n.numeric_input,n.numeric_outputs[0]);
        auto& state=c->impl_->states[i];state.numeric_outputs.resize(cn.outputs.size());state.numeric_frames.resize(cn.outputs.size());state.source_stamps.resize(cn.outputs.size());state.source_markers.resize(cn.outputs.size());state.stream_storage.resize(profile.admission.frame.max_string_bytes);
        for(std::size_t j=0;j<cn.outputs.size();++j){const auto* output=std::get_if<NumericContract>(&n.numeric_outputs[j]);
            const bool shared=(n.operation==ProcessingOperation::Source && n.node.kind!=NodeKind::SignalGenerator) || (n.operation==ProcessingOperation::Transform && n.node.kind==NodeKind::Window && output && direct_window(*output));
            const auto status=numeric_buffer(n.numeric_outputs[j],state.numeric_outputs[j],shared?plan->impl_->silence:nullptr);if(status!=Status::ok)return status;}
        if(!cn.inputs.empty()){
            const auto& zero_contract=n.operation==ProcessingOperation::InputSelector?n.numeric_outputs.front():n.numeric_input;
            if(numeric_buffer(zero_contract,state.numeric_zero,plan->impl_->silence)!=Status::ok)return Status::invalid_argument;
            if(const auto* input=std::get_if<NumericContract>(&n.numeric_input);input && input->shape.rank==Rank::Matrix && n.operation!=ProcessingOperation::History){
                state.route_masks.resize(std::max(std::size_t{1},cn.outputs.size()));
                state.route_state.resize(state.route_masks.size());for(auto& mask:state.route_masks)mask.resize(std::size_t(input->shape.extents[0].maximum)*2);
            }
        }
        if(n.operation==ProcessingOperation::InputSelector){double selected=0;
            if(!numeric_parameter(resolved,"selected_input",selected))return Status::invalid_argument;
            n.selected_input=std::uint16_t(selected);
            if(cn.outputs.size()>1) {
                n.endpoint_plan=std::make_unique<PlanNode::EndpointPlan>();
                n.endpoint_plan->inputs.resize(cn.outputs.size());
                for(std::size_t lane=0;lane<cn.outputs.size();++lane) {
                    auto& input=n.endpoint_plan->inputs[lane];input.contract=cn.outputs[lane].contract.view();
                    input.port=PortId(std::size_t(n.selected_input)*cn.outputs.size()+lane);
                    for(const auto& edge:g.edges)if(edge.to==cn.id&&edge.input_port==input.port) {
                        input.parent=index(plan->impl_->nodes,edge.from);input.parent_port=edge.output_port;
                        if(input.parent==absent)return Status::invalid_argument;
                        auto ancestor=input.parent;
                        while(ancestor!=absent){const auto& upstream=plan->impl_->nodes[ancestor];
                            if(upstream.operation==ProcessingOperation::Source||upstream.operation==ProcessingOperation::InputSelector||upstream.operation==ProcessingOperation::Mixer)break;
                            ancestor=upstream.parent;
                        }
                        if(ancestor==absent)input.parent=absent;
                    }
                }
            }
        }
        for(const auto& edge:g.edges)if(!n.scalar_view() && n.operation!=ProcessingOperation::Mixer && n.operation!=ProcessingOperation::ChannelComparison && !(n.operation==ProcessingOperation::Switch&&n.endpoint_plan) && edge.to==cn.id &&
            (n.operation!=ProcessingOperation::InputSelector || edge.input_port==std::size_t(n.selected_input)*cn.outputs.size())){
            n.parent=index(plan->impl_->nodes,edge.from);n.parent_port=edge.output_port;
            if(n.parent==absent)return Status::invalid_argument;
            n.clock=plan->impl_->nodes[n.parent].clock;}
        if((n.operation==ProcessingOperation::InputSelector || n.operation==ProcessingOperation::Switch)&&n.endpoint_plan) {
            n.parent=absent;
            for(const auto& lane:n.endpoint_plan->inputs)if(lane.parent!=absent){n.parent=lane.parent;n.parent_port=lane.parent_port;n.clock=plan->impl_->nodes[lane.parent].clock;break;}
        }
        // A selector owns the continuity of its compatible output clock, even
        // when the physical producer of its chosen input changes.
        if(n.operation==ProcessingOperation::Source || n.operation==ProcessingOperation::InputSelector)n.clock=i;
        const auto old=previous&&previous->impl_->numeric_graph&&previous->impl_->numeric_graph->graph_id==g.graph_id?index(previous->impl_->nodes,cn.id):absent;
        n.reset=old==absent || !numeric_compatible_node(cn,previous->impl_->numeric_graph->nodes[previous->impl_->nodes[old].configured]);
        if(n.operation==ProcessingOperation::InputSelector && old!=absent){const auto& prior=previous->impl_->nodes[old];
            n.selector_changed=n.selected_input!=prior.selected_input || n.parent_port!=prior.parent_port ||
                (n.parent==absent)!=(prior.parent==absent) ||
                (n.parent!=absent && prior.parent!=absent && plan->impl_->nodes[n.parent].node.id!=previous->impl_->nodes[prior.parent].node.id);
            if(n.endpoint_plan&&prior.endpoint_plan&&n.endpoint_plan->inputs.size()==prior.endpoint_plan->inputs.size())
                for(std::size_t lane=0;lane<n.endpoint_plan->inputs.size();++lane){const auto& a=n.endpoint_plan->inputs[lane];const auto& b=prior.endpoint_plan->inputs[lane];
                    n.selector_changed=n.selector_changed||a.parent_port!=b.parent_port||(a.parent==absent)!=(b.parent==absent)||
                        (a.parent!=absent&&b.parent!=absent&&plan->impl_->nodes[a.parent].node.id!=previous->impl_->nodes[b.parent].node.id);}
        }
        if(!n.reset && old_context->impl_->states[old].numeric_stamp.stream_id.size()>state.stream_storage.size())return Status::exhausted;
        if(n.clock==absent && !n.reset){const auto old_clock=previous->impl_->nodes[old].clock;
            if(old_clock!=absent)n.clock=index(plan->impl_->nodes,previous->impl_->nodes[old_clock].node.id);}
        if(n.node.kind==NodeKind::SignalGenerator){state.kernel=OscillatorKernel{};
            if(!std::get<OscillatorKernel>(state.kernel).value->prepare(std::get<SignalGeneratorProcessingParameters>(cn.parameters).oscillators,std::get<NumericContract>(n.numeric_outputs[0]).clock.sample_rate))return Status::invalid_argument;
        }
        if(n.operation==ProcessingOperation::Mixer || n.operation==ProcessingOperation::ChannelComparison){const auto params=n.operation==ProcessingOperation::Mixer?std::get<MixerProcessingParameters>(cn.parameters):MixerProcessingParameters{};state.mixer_queues.resize(cn.inputs.size());
            if(n.operation==ProcessingOperation::Mixer){n.endpoint_plan->weights.resize(cn.inputs.size());for(std::size_t q=0;q<cn.inputs.size();++q)for(const auto& weight:params.weights)if(weight.port==cn.inputs[q].id){n.endpoint_plan->weights[q]=weight.weight;n.endpoint_plan->weight_scale=std::max(n.endpoint_plan->weight_scale,weight.weight);}}
            const auto& output=std::get<NumericContract>(n.numeric_outputs[0]);
            if(spectral_vector(output)){
                std::uint64_t hop=0;
                if(!divide_products(std::uint64_t(original.execution_interval.numerator),output.clock.sample_rate,original.execution_interval.denominator,1,hop,&n.hop_fraction))return Status::overflow;
                if(!hop || hop>SIZE_MAX || (hop==SIZE_MAX && n.hop_fraction))return Status::invalid_argument;
                n.hop_base=std::size_t(hop);n.hop_denominator=original.execution_interval.denominator;
            }
            for(std::size_t j=0;j<cn.inputs.size();++j){const auto contract=std::get<NumericContract>(cn.inputs[j].contract.view());
                const auto capacity=!spectral_vector(contract)?std::max<std::uint64_t>(contract.shape.extents[0].maximum,(std::uint64_t(contract.clock.sample_rate)*params.fifo_milliseconds+999)/1000):contract.shape.extents[0].maximum*2;
                state.mixer_queues[j].samples.resize(capacity);
                if(!spectral_vector(contract))state.mixer_queues[j].provenance.resize(capacity);
                if(aligned_operation(n,*plan->impl_->numeric_graph))state.mixer_queues[j].aligned.resize(capacity);
                if(contract.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE)for(auto& coords:state.mixer_queues[j].coordinates)coords.resize(contract.shape.extents[0].maximum*8);}
        }
        if(n.operation==ProcessingOperation::Converter){n.conversion=std::make_unique<dsp_kernels::PreparedConversion>();
            auto status=dsp_kernels::prepare_conversion({std::get<NumericContract>(n.numeric_input),std::get<NumericContract>(n.numeric_outputs[0]),
                std::get<ConverterProcessingParameters>(cn.parameters).policy,profile.kernels,profile.admission.frame},profile.capabilities,*n.conversion);
            if(status!=dsp_kernels::NumericStatus::Ok)return numeric_kernel_status(status);
            state.numeric_scratch.resize(n.conversion->requirements.scratch_bytes);
            if(std::get<NumericContract>(n.numeric_input).shape.rank==Rank::Matrix){
                const NumericGatherRequest request{std::get<NumericContract>(n.numeric_input),{},
                    {profile.admission.frame,profile.admission.frame.max_payload_bytes,profile.admission.frame.max_metadata_bytes,profile.kernels.max_scratch_bytes},true};
                NumericGatherRequirements req;if(numeric_gather_requirements(request,req)!=Status::ok)return Status::invalid_argument;
                auto owner=std::make_shared<GatherOwner>(req.plan_metadata_bytes);
                if(prepare_numeric_gather(request,owner->pool,owner->gather)!=Status::ok)return Status::invalid_argument;
                n.input_gather=std::move(owner);state.input_gather_payload.resize(req.payload_bytes);state.input_gather_metadata.resize(req.output_metadata_bytes);state.input_gather_scratch.resize(req.scratch_bytes);
                state.row_planes.resize(std::get<NumericContract>(n.numeric_outputs[0]).channel_order.size());
                n.row_conversion=std::make_unique<dsp_kernels::PreparedConversion>();
                status=dsp_kernels::prepare_conversion({n.input_gather->gather.output_contract(),numeric_projection(std::get<NumericContract>(n.numeric_outputs[0]),{},true),
                    std::get<ConverterProcessingParameters>(cn.parameters).policy,profile.kernels,profile.admission.frame},profile.capabilities,*n.row_conversion);
                if(status!=dsp_kernels::NumericStatus::Ok)return numeric_kernel_status(status);
            }}
        if(n.operation==ProcessingOperation::WLEDEncoder){double gain=0;if(!numeric_parameter(resolved,"gain",gain))return Status::invalid_argument;
            n.gain=float(gain);state.values.resize(std::size_t(std::get<NumericContract>(n.numeric_input).shape.extents[0].maximum));}
        if(n.operation==ProcessingOperation::Transform){const auto& in=std::get<NumericContract>(n.numeric_input);const auto& out=std::get<NumericContract>(n.numeric_outputs[0]);
            const auto columns=std::size_t(in.shape.extents[0].maximum),count=std::size_t(out.shape.extents[0].maximum);
            switch(n.node.kind){
            case NodeKind::PcmResampler:
                state.left.resize(columns*in.channel_order.size());state.values.resize(count*in.channel_order.size());
                state.kernel.emplace<ResamplerKernel>();
                if(!std::get<ResamplerKernel>(state.kernel).operation().prepare(in.clock.sample_rate,out.clock.sample_rate,in.channel_order.size()))return Status::invalid_argument;
                break;
            case NodeKind::Window:{
                const auto numerator=std::uint64_t(original.execution_interval.numerator),denominator=original.execution_interval.denominator;
                std::uint64_t hop=0;
                if(!divide_products(numerator,in.clock.sample_rate,denominator,1,hop,&n.hop_fraction))return Status::overflow;
                if(!hop || hop>SIZE_MAX || (hop==SIZE_MAX && n.hop_fraction))return Status::invalid_argument;
                n.hop_base=std::size_t(hop);n.hop_denominator=denominator;
                const auto emissions=columns/n.hop_base+(columns%n.hop_base!=0);
                if(emissions>profile.max_windows_per_ingress || (emissions && g.nodes.size()>profile.max_callback_deliveries_per_ingress/emissions) ||
                   (emissions && g.nodes.size() && profile.admission.max_ports_per_node>profile.max_callback_deliveries_per_ingress/emissions/g.nodes.size()))return Status::exhausted;
                state.pcm.resize(count*in.channel_order.size());break;}
            case NodeKind::WindowFunction: {
                ProcessingParameterView value;resolve_processing_parameter(resolved,"function",value);const auto function=std::get<std::string_view>(value);
                if(function=="hamming")n.window_function=dsp_kernels::WindowFunction::Hamming;
                else if(function=="blackman")n.window_function=dsp_kernels::WindowFunction::Blackman;
                else if(function=="rectangular")n.window_function=dsp_kernels::WindowFunction::Rectangular;
                state.coefficients.resize(columns);dsp_kernels::window(state.coefficients,n.window_function);break;}
            case NodeKind::BarkWeighting:state.coefficients.resize(columns);dsp_kernels::bark_weights(state.coefficients,float(in.clock.sample_rate)/float(columns*2));break;
            case NodeKind::StereoFFT:state.kernel.emplace<dsp_kernels::FFT>();if(!std::get<dsp_kernels::FFT>(state.kernel).prepare(columns))return Status::unavailable;break;
            case NodeKind::Mel:state.left.resize(columns);state.values.resize(count);state.kernel.emplace<dsp_kernels::MelFilterBank>();if(!std::get<dsp_kernels::MelFilterBank>(state.kernel).prepare(columns,in.clock.sample_rate,count))return Status::unavailable;
                state.mel_boundaries.resize(count+2);
                for(std::size_t band=0;band<count+2;++band){const double mel_max=2595.*std::log10(1.+double(in.clock.sample_rate)*.5/700.);
                    state.mel_boundaries[band]=700.*std::expm1(mel_max*double(band)/double(count+1)/2595.*std::log(10.));}break;
            case NodeKind::SpectrumMerge: {double merge_value=0;if(!numeric_parameter(resolved,"merge",merge_value))return Status::invalid_argument;n.merge_mode=unsigned(merge_value);break;}
            case NodeKind::SpectrumGain: {double gain=0;if(!numeric_parameter(resolved,"gain",gain))return Status::invalid_argument;n.gain=float(gain);break;}
            default:return Status::invalid_argument;
            }
        }
        if(n.operation==ProcessingOperation::Source && std::holds_alternative<NumericContract>(n.numeric_outputs[0]) &&
           std::get<NumericContract>(n.numeric_outputs[0]).shape.rank==Rank::Matrix){
            state.route_masks.resize(cn.outputs.size());
            state.route_state.resize(state.route_masks.size());for(auto& mask:state.route_masks)mask.resize(std::size_t(std::get<NumericContract>(n.numeric_outputs[0]).shape.extents[0].maximum)*2);}
        if(n.operation==ProcessingOperation::History || n.operation==ProcessingOperation::Reduce ||
           ((n.operation==ProcessingOperation::Source || n.operation==ProcessingOperation::Converter) && std::holds_alternative<NumericContract>(n.numeric_outputs[0]) && std::get<NumericContract>(n.numeric_outputs[0]).shape.rank==Rank::Matrix) ||
           (n.operation==ProcessingOperation::Sink && std::holds_alternative<NumericContract>(n.numeric_input) && std::get<NumericContract>(n.numeric_input).shape.rank==Rank::Matrix)){
            const auto request=numeric_gather_request(resolved,profile);NumericGatherRequirements req;
            auto status=numeric_gather_requirements(request,req);if(status!=Status::ok)return status;
            auto owner=std::make_shared<GatherOwner>(req.plan_metadata_bytes);status=prepare_numeric_gather(request,owner->pool,owner->gather);if(status!=Status::ok)return status;
            n.gather=std::move(owner);state.gather_payload.resize(req.payload_bytes);state.gather_metadata.resize(req.output_metadata_bytes);state.gather_scratch.resize(req.scratch_bytes);
            if(n.operation==ProcessingOperation::Reduce){const auto& r=std::get<ReduceProcessingParameters>(cn.parameters);
                n.reduction=std::make_unique<dsp_kernels::PreparedReduction>();
                auto k=dsp_kernels::prepare_reduction({n.gather->gather.output_contract(),std::get<NumericContract>(n.numeric_outputs[0]),numeric_reduction(r.operation),r.precision_loss,r.rounding,profile.kernels,profile.admission.frame},profile.capabilities,*n.reduction);
                if(k!=dsp_kernels::NumericStatus::Ok)return numeric_kernel_status(k);
                state.numeric_scratch.resize(n.reduction->requirements.scratch_bytes);}}
        plan->impl_->nodes.push_back(std::move(n));
    }
    for(std::size_t i=0;i<plan->impl_->nodes.size();++i){auto& n=plan->impl_->nodes[i];
        if(n.operation==ProcessingOperation::Clock)continue;
        if(n.scalar_view())for(auto& input:n.endpoint_plan->inputs)if(input.parent==absent){
            input.clock=absent;input.clock_known=false;const auto& declared=std::get<NumericContract>(input.contract).clock;
            for(std::size_t source=0;source<plan->impl_->nodes.size();++source){const auto& candidate=plan->impl_->nodes[source];if(candidate.operation!=ProcessingOperation::Source || candidate.numeric_outputs.empty())continue;
                const auto* contract=std::get_if<NumericContract>(&candidate.numeric_outputs[0]);
                if(contract && !declared.domain.empty() && declared.tick_seconds.numerator>0 && declared.domain==contract->clock.domain && declared.tick_seconds==contract->clock.tick_seconds && declared.sample_rate==contract->clock.sample_rate &&
                    (input.clock==absent || candidate.node.id<plan->impl_->nodes[input.clock].node.id)){input.clock=source;input.clock_known=true;}}
            if(input.clock==absent)for(std::size_t source=0;source<plan->impl_->nodes.size();++source)if(plan->impl_->nodes[source].operation==ProcessingOperation::Source &&
                (input.clock==absent || plan->impl_->nodes[source].node.id<plan->impl_->nodes[input.clock].node.id))input.clock=source;
        }
        if(n.operation==ProcessingOperation::InputSelector && n.parent==absent){
            const auto& declared=std::get<NumericContract>(n.numeric_input).clock;
            auto compatible_clock=[&](std::size_t candidate,std::uint16_t port){
                if(candidate==absent || candidate==i || port>=plan->impl_->nodes[candidate].numeric_outputs.size())return false;
                const auto* value=std::get_if<NumericContract>(&plan->impl_->nodes[candidate].numeric_outputs[port]);
                return value && !declared.domain.empty() && declared.sample_rate &&
                    value->clock.domain==declared.domain && value->clock.sample_rate==declared.sample_rate && value->clock.tick_seconds==declared.tick_seconds;
            };
            // Retain the selected producer's event clock when a wire is removed.
            // Otherwise pick one compatible source deterministically; unrelated
            // ingress and the second source must not create additional ticks.
            if(previous && !n.reset){const auto old=index(previous->impl_->nodes,n.node.id);
                if(old!=absent){const auto& prior=previous->impl_->nodes[old];
                    const auto old_anchor=prior.parent!=absent?prior.parent:prior.numeric_clock_parent;
                    if(prior.selected_input==n.selected_input && old_anchor!=absent){
                        const auto candidate=index(plan->impl_->nodes,previous->impl_->nodes[old_anchor].node.id);
                        if(compatible_clock(candidate,prior.parent_port)){n.numeric_clock_parent=candidate;n.parent_port=prior.parent_port;}
                    }
                }
            }
            if(n.numeric_clock_parent==absent)for(std::size_t candidate=0;candidate<plan->impl_->nodes.size();++candidate){
                if(plan->impl_->nodes[candidate].operation!=ProcessingOperation::Source || !compatible_clock(candidate,0))continue;
                if(n.numeric_clock_parent==absent || plan->impl_->nodes[candidate].node.id<plan->impl_->nodes[n.numeric_clock_parent].node.id){n.numeric_clock_parent=candidate;n.parent_port=0;}
            }
            if(n.numeric_clock_parent==absent)n.clock=absent;
        }
        if(n.parent!=absent && n.operation!=ProcessingOperation::InputSelector)n.clock=plan->impl_->nodes[n.parent].clock;
        if(n.clock==absent && n.operation!=ProcessingOperation::InputSelector && !n.reset && previous){const auto old=index(previous->impl_->nodes,n.node.id);const auto old_clock=previous->impl_->nodes[old].clock;
            if(old_clock!=absent)n.clock=index(plan->impl_->nodes,previous->impl_->nodes[old_clock].node.id);}
        if(n.parent==absent && n.operation!=ProcessingOperation::Source && n.operation!=ProcessingOperation::InputSelector && !n.reset && previous){
            const auto old=index(previous->impl_->nodes,n.node.id);const auto& old_node=previous->impl_->nodes[old];
            const auto old_anchor=old_node.parent!=absent?old_node.parent:old_node.numeric_clock_parent;
            if(old_anchor!=absent){const auto anchor=index(plan->impl_->nodes,previous->impl_->nodes[old_anchor].node.id);
                if(anchor!=absent && old_node.parent_port<plan->impl_->nodes[anchor].numeric_outputs.size() &&
                    same_physical_contract(n.numeric_input,plan->impl_->nodes[anchor].numeric_outputs[old_node.parent_port])){
                    n.numeric_clock_parent=anchor;n.parent_port=old_node.parent_port;
                }else n.clock=absent;
            }
        }
        // A new or reconfigured processor does not invalidate a descendant's
        // compatible storage. Reset that descendant only when its own contract
        // or its logical source clock changes, not because an ancestor rebuilt.
        if(previous){const auto old=index(previous->impl_->nodes,n.node.id);
            if(old!=absent){const auto old_clock=previous->impl_->nodes[old].clock;
            n.clock_reset=(n.clock==absent)!=(old_clock==absent);
            if(!n.clock_reset && n.clock!=absent){const auto& root=plan->impl_->nodes[n.clock];const auto& prior=previous->impl_->nodes[old_clock];
                const auto* current=root.numeric_outputs.empty()?nullptr:std::get_if<NumericContract>(&root.numeric_outputs[0]);
                const auto* before=prior.numeric_outputs.empty()?nullptr:std::get_if<NumericContract>(&prior.numeric_outputs[0]);
                if(root.node.id!=prior.node.id || root.node.kind!=prior.node.kind || bool(current)!=bool(before) ||
                   (current && (current->clock.domain!=before->clock.domain || current->clock.tick_seconds!=before->clock.tick_seconds ||
                    current->clock.sample_rate!=before->clock.sample_rate)))n.clock_reset=true;
            }
            if(n.clock_reset)n.reset=true;
            }
        }
    }
    for(std::size_t parent=0;parent<plan->impl_->nodes.size();++parent){std::size_t count=0;
        for(const auto& mixer:plan->impl_->nodes)if(mixer.operation==ProcessingOperation::Mixer || mixer.operation==ProcessingOperation::ChannelComparison)
            for(const auto& input:mixer.endpoint_plan->inputs)if(input.parent==parent)++count;
        plan->impl_->nodes[parent].mixer_destinations.reserve(count);}
    for(std::size_t m=0;m<plan->impl_->nodes.size();++m){const auto& mixer=plan->impl_->nodes[m];if(mixer.operation!=ProcessingOperation::Mixer && mixer.operation!=ProcessingOperation::ChannelComparison)continue;
        const auto anchor=std::find_if(mixer.endpoint_plan->inputs.begin(),mixer.endpoint_plan->inputs.end(),[](const auto& input){return input.parent!=absent;});
        for(std::size_t q=0;q<mixer.endpoint_plan->inputs.size();++q){const auto& input=mixer.endpoint_plan->inputs[q];
            if(input.parent!=absent)plan->impl_->nodes[input.parent].mixer_destinations.push_back({m,q,input.parent_port});
            else if(aligned_operation(mixer,*plan->impl_->numeric_graph)&&anchor!=mixer.endpoint_plan->inputs.end())
                plan->impl_->nodes[anchor->parent].mixer_destinations.push_back({m,q,anchor->parent_port});}}
    // Retained clock edges participate in the same event schedule and cannot form a cycle.
    for(std::size_t i=0;i<plan->impl_->nodes.size();++i){auto cursor=i;std::size_t count=0;
        while(cursor!=absent){if(++count>plan->impl_->nodes.size())return Status::invalid_argument;
            const auto& node=plan->impl_->nodes[cursor];cursor=node.parent!=absent?node.parent:node.numeric_clock_parent;}}
    for(std::size_t i=0;i<plan->impl_->nodes.size();++i){const auto& n=plan->impl_->nodes[i];const auto parent=n.parent!=absent?n.parent:n.numeric_clock_parent;
        if((n.operation==ProcessingOperation::InputSelector || n.operation==ProcessingOperation::Switch)&&n.endpoint_plan) {
            for(std::size_t lane=0;lane<n.endpoint_plan->inputs.size();++lane){const auto& input=n.endpoint_plan->inputs[lane];
                if(input.parent!=absent)plan->impl_->nodes[input.parent].selector_destinations.push_back({i,lane,input.parent_port});}
            if(n.parent==absent&&parent!=absent)plan->impl_->nodes[parent].selector_destinations.push_back({i,0,n.parent_port});
        } else if(parent!=absent)plan->impl_->nodes[parent].children.push_back(i);
        if(n.operation==ProcessingOperation::History)clear_node_state(n,c->impl_->states[i]);
        c->impl_->states[i].pending_discontinuity=n.reset;
    }
    c->impl_->plan=plan.get();if(previous && old_context)plan->refresh_state(*c,*previous,*old_context);
    result=std::move(plan);context=std::move(c);return Status::ok;
#if defined(__cpp_exceptions)
    }catch(const std::bad_alloc&){return Status::exhausted;}
    catch(const std::length_error&){return Status::exhausted;}
#endif
}

} // namespace dataflow_runtime
