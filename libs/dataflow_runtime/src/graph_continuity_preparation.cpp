#include "numeric_preparation.h"
#include "preparation_support.h"
#include "mixer_execution.h"
#include <algorithm>
namespace dataflow_runtime {
namespace {
bool clock_owner_changed(const PlanNode& current,const PlanNode& prior){
    const auto* a=current.numeric_outputs.empty()?nullptr:std::get_if<NumericContract>(&current.numeric_outputs[0]);
    const auto* b=prior.numeric_outputs.empty()?nullptr:std::get_if<NumericContract>(&prior.numeric_outputs[0]);
    if(current.node.id!=prior.node.id||current.node.kind!=prior.node.kind||bool(a)!=bool(b))return true;
    return a&&(a->clock.domain!=b->clock.domain||a->clock.tick_seconds!=b->clock.tick_seconds||a->clock.sample_rate!=b->clock.sample_rate);
}
}
bool NumericPreparation::clock_precedes(std::size_t candidate,std::size_t selected)const {
    return selected==absent||plan->impl_->nodes[candidate].node.id<plan->impl_->nodes[selected].node.id;
}
bool NumericPreparation::scalar_clock_matches(const PlanNode& source,const ClockContract& declared)const {
    if(source.operation!=ProcessingOperation::Source||source.numeric_outputs.empty())return false;
    const auto* contract=std::get_if<NumericContract>(&source.numeric_outputs[0]);
    return contract&&!declared.domain.empty()&&declared.tick_seconds.numerator>0&&declared.domain==contract->clock.domain&&declared.tick_seconds==contract->clock.tick_seconds&&declared.sample_rate==contract->clock.sample_rate;
}
void NumericPreparation::bind_scalar_clock(PlanNode::ScalarInput& input){
    input.clock=absent;input.clock_known=false;const auto& declared=std::get<NumericContract>(input.contract).clock;
    choose_scalar_clock(input,declared);
    if(input.clock!=absent)return;
    for(std::size_t source=0;source<plan->impl_->nodes.size();++source)if(plan->impl_->nodes[source].operation==ProcessingOperation::Source&&clock_precedes(source,input.clock))input.clock=source;
}
void NumericPreparation::choose_scalar_clock(PlanNode::ScalarInput& input,const ClockContract& declared){
    for(std::size_t source=0;source<plan->impl_->nodes.size();++source)
        if(scalar_clock_matches(plan->impl_->nodes[source],declared)&&clock_precedes(source,input.clock)){
            input.clock=source;input.clock_known=true;
        }
}
bool NumericPreparation::selector_clock_matches(const PlanNode& selector,std::size_t position,std::size_t candidate,PortId port)const {
    if(candidate==absent||candidate==position||port>=plan->impl_->nodes[candidate].numeric_outputs.size())return false;
    const auto& declared=std::get<NumericContract>(selector.numeric_input).clock;
    const auto* value=std::get_if<NumericContract>(&plan->impl_->nodes[candidate].numeric_outputs[port]);
    return value&&!declared.domain.empty()&&declared.sample_rate&&value->clock.domain==declared.domain&&value->clock.sample_rate==declared.sample_rate&&value->clock.tick_seconds==declared.tick_seconds;
}
void NumericPreparation::retain_selector_clock(PlanNode& node,std::size_t position){
    if(!previous||node.reset)return;
    const auto old=index(previous->impl_->nodes,node.node.id);if(old==absent)return;
    const auto& prior=previous->impl_->nodes[old];const auto anchor=prior.parent!=absent?prior.parent:prior.numeric_clock_parent;
    if(prior.selected_input!=node.selected_input||anchor==absent)return;
    const auto candidate=index(plan->impl_->nodes,previous->impl_->nodes[anchor].node.id);
    if(selector_clock_matches(node,position,candidate,prior.parent_port)){node.numeric_clock_parent=candidate;node.parent_port=prior.parent_port;}
}
void NumericPreparation::choose_selector_clock(PlanNode& node,std::size_t position){
    retain_selector_clock(node,position);
    if(node.numeric_clock_parent==absent)for(std::size_t candidate=0;candidate<plan->impl_->nodes.size();++candidate){
        if(plan->impl_->nodes[candidate].operation!=ProcessingOperation::Source||!selector_clock_matches(node,position,candidate,0))continue;
        if(clock_precedes(candidate,node.numeric_clock_parent)){node.numeric_clock_parent=candidate;node.parent_port=0;}
    }
    if(node.numeric_clock_parent==absent)node.clock=absent;
}
void NumericPreparation::restore_node_clock(PlanNode& node){
    if(node.parent!=absent&&node.operation!=ProcessingOperation::InputSelector)node.clock=plan->impl_->nodes[node.parent].clock;
    if(node.clock!=absent||node.operation==ProcessingOperation::InputSelector||node.reset||!previous)return;
    const auto old=index(previous->impl_->nodes,node.node.id);const auto prior=previous->impl_->nodes[old].clock;
    if(prior!=absent)node.clock=index(plan->impl_->nodes,previous->impl_->nodes[prior].node.id);
}
void NumericPreparation::restore_disconnected_parent(PlanNode& node){
    if(node.parent!=absent||node.operation==ProcessingOperation::Source||node.operation==ProcessingOperation::InputSelector||node.reset||!previous)return;
    const auto old=index(previous->impl_->nodes,node.node.id);const auto& prior=previous->impl_->nodes[old];
    const auto anchor=prior.parent!=absent?prior.parent:prior.numeric_clock_parent;if(anchor==absent)return;
    const auto current=index(plan->impl_->nodes,previous->impl_->nodes[anchor].node.id);
    if(current!=absent&&prior.parent_port<plan->impl_->nodes[current].numeric_outputs.size()&&same_physical_contract(node.numeric_input,plan->impl_->nodes[current].numeric_outputs[prior.parent_port])){
        node.numeric_clock_parent=current;node.parent_port=prior.parent_port;
    }else node.clock=absent;
}
void NumericPreparation::update_clock_reset(PlanNode& node){
    if(!previous)return;
    const auto old=index(previous->impl_->nodes,node.node.id);if(old==absent)return;
    const auto prior=previous->impl_->nodes[old].clock;node.clock_reset=(node.clock==absent)!=(prior==absent);
    if(!node.clock_reset&&node.clock!=absent)node.clock_reset=clock_owner_changed(plan->impl_->nodes[node.clock],previous->impl_->nodes[prior]);
    if(node.clock_reset)node.reset=true;
}
void NumericPreparation::reconcile_clocks(){
    for(std::size_t position=0;position<plan->impl_->nodes.size();++position)reconcile_node_clock(position);
}
void NumericPreparation::reconcile_node_clock(std::size_t position){
    auto& node=plan->impl_->nodes[position];
    if(node.operation==ProcessingOperation::Clock)return;
    if(node.scalar_view())for(auto& input:node.endpoint_plan->inputs)if(input.parent==absent)bind_scalar_clock(input);
    if(node.operation==ProcessingOperation::InputSelector&&node.parent==absent)choose_selector_clock(node,position);
    restore_node_clock(node);restore_disconnected_parent(node);update_clock_reset(node);
}
namespace {
ContentToken output_token(const PlanNode* node,std::size_t output) {
    return node && output<node->output_content.size()?node->output_content[output]:ContentToken{};
}
// Ownership accounting only: this walks immutable token references, never
// processing ancestors or produced frame metadata. Retired tokens may outlive
// removed graph nodes through a compatible disconnected History.
class ContentAccounting {
    std::span<const ContentLineage* const> allocated;
    const std::size_t capacity;
    std::vector<const ContentLineage*> unique;
    Status retain(const ContentToken& token) {
        if(!token || std::find(unique.begin(),unique.end(),token.get())!=unique.end())return Status::ok;
        if(unique.size()==capacity)return Status::exhausted;
        unique.push_back(token.get());return Status::ok;
    }
    Status expand() {
        for(std::size_t at=0;at<unique.size();++at)
            for(const auto& input:unique[at]->inputs){const auto status=retain(input.token);if(status!=Status::ok)return status;}
        return Status::ok;
    }
    Status charge(const ContentLineage& token,std::size_t& owned,std::size_t& retired) const {
        std::size_t bytes=sizeof(ContentLineage)+2*sizeof(ContentToken),inputs=0;
        if(!numeric_mul(token.inputs.capacity(),sizeof(ContentContribution),inputs) || !numeric_add(bytes,inputs) || !numeric_add(owned,bytes))return Status::overflow;
        if(std::find(allocated.begin(),allocated.end(),&token)==allocated.end() && !numeric_add(retired,bytes))return Status::overflow;
        return Status::ok;
    }
    Status retain_outputs(std::span<const PlanNode> nodes) {
        for(const auto& node:nodes)for(const auto& token:node.output_content){const auto status=retain(token);if(status!=Status::ok)return status;}
        return Status::ok;
    }
public:
    ContentAccounting(std::span<const ContentLineage* const> created,std::size_t maximum):allocated(created),capacity(maximum){unique.reserve(capacity);}
    Status measure(std::span<const PlanNode> nodes,std::size_t& owned,std::size_t& retired,std::size_t& count) {
        {const auto status=retain_outputs(nodes);if(status!=Status::ok)return status;}
        {const auto status=expand();if(status!=Status::ok)return status;}
        for(const auto* token:unique){const auto status=charge(*token,owned,retired);if(status!=Status::ok)return status;}
        count=unique.size();return Status::ok;
    }
};
class ContentPreparation {
    std::vector<PlanNode>& nodes;
    std::span<const PlanNode> before;
    const NumericProcessingGraph& graph;
    const NumericProcessingGraph* prior_graph;
    const ProcessingAdmissionLimits& limits;
    PlanNode* node=nullptr;
    const PlanNode* prior=nullptr;
    ResolvedProcessingNode current,old;
    std::vector<const ContentLineage*> allocated;
    bool same_local(PortId output) const {
        return prior && same_processing_output_content(current,output,old,output);
    }
    ContentToken retained_input(PortId input,PortId output) const {
        const auto token=output_token(prior,output);
        if(!token || !same_local(output))return {};
        if(old.inputs.size()==1 && processing_output_continuity(old,output)==ProcessingContentContinuity::PreserveInput)return token;
        const auto found=std::find_if(token->inputs.begin(),token->inputs.end(),[=](const auto& value){return value.input==input;});
        return found==token->inputs.end()?ContentToken{}:found->token;
    }
    ContentContribution contribution(PortId input,std::size_t parent,PortId port,PortId output) const {
        if(parent!=absent)return {input,output_token(&nodes[parent],port)};
        // A disconnected compatible declared input emits typed zero without
        // changing the meaning of already retained rows.
        return {input,retained_input(input,output)};
    }
    const PlanNode::ScalarInput* projected_input(PortId output) const {
        const auto& bindings=node->endpoint_plan->inputs;
        if(node->operation==ProcessingOperation::InputSelector)return output<bindings.size()?&bindings[output]:nullptr;
        const auto* parameters=std::get_if<RouteProcessingParameters>(current.parameters);
        if(!parameters || !parameters->channels_per_stream)return nullptr;
        const auto lane=output%parameters->channels_per_stream;
        const auto found=std::find_if(bindings.begin(),bindings.end(),[=](const auto& input){return input.port==lane;});
        return found==bindings.end()?nullptr:&*found;
    }
    void append_bound_inputs(PortId output,std::vector<ContentContribution>& result) const {
        const auto& bindings=node->endpoint_plan->inputs;result.reserve(bindings.size());
        if(node->operation==ProcessingOperation::InputSelector || node->operation==ProcessingOperation::Switch){
            if(const auto* input=projected_input(output))result.push_back(contribution(input->port,input->parent,input->parent_port,output));
            return;
        }
        for(const auto& input:bindings)result.push_back(contribution(input.port,input.parent,input.parent_port,output));
    }
    std::vector<ContentContribution> inputs(PortId output) const {
        std::vector<ContentContribution> result;
        if(current.inputs.empty())return result;
        if(!node->endpoint_plan || node->endpoint_plan->inputs.empty()){
            result.reserve(1);result.push_back(contribution(current.inputs.front().id,node->parent,node->parent_port,output));return result;
        }
        append_bound_inputs(output,result);
        return result;
    }
    ContentToken build_output(PortId output) {
        auto contributions=inputs(output);
        const bool partition=processing_output_continuity(current,output)==ProcessingContentContinuity::PartitionOutput;
        if(!partition && contributions.size()==1)return contributions.front().token;
        if(!partition && std::none_of(contributions.begin(),contributions.end(),[](const auto& input){return bool(input.token);}))return {};
        const auto previous=output_token(prior,output);
        const auto boundary=partition?node->node.id:NodeId{-1};
        if(previous && previous->boundary==boundary && previous->output==output && previous->inputs==contributions && same_local(output))return previous;
        auto token=std::make_shared<ContentLineage>();token->boundary=boundary;token->output=output;token->inputs=std::move(contributions);allocated.push_back(token.get());return token;
    }
    Status prepare_node(PlanNode& value) {
        node=&value;
        const auto found=index(before,value.node.id);prior=found==absent?nullptr:&before[found];
        if(!resolve_processing_node(graph.nodes[value.configured],limits,current))return Status::invalid_argument;
        if(prior && !resolve_processing_node(prior_graph->nodes[prior->configured],limits,old)){prior=nullptr;value.reset=true;}
        value.output_content.resize(value.numeric_outputs.size());
        for(std::size_t output=0;output<value.output_content.size();++output)value.output_content[output]=build_output(PortId(output));
        if(value.operation==ProcessingOperation::History && prior && output_token(node,0)!=output_token(prior,0))value.reset=true;
        return Status::ok;
    }
public:
    ContentPreparation(std::vector<PlanNode>& current_nodes,std::span<const PlanNode> previous_nodes,
        const NumericProcessingGraph& configured,const NumericProcessingGraph* previous_configured,const ProcessingAdmissionLimits& admission,std::size_t output_count)
        :nodes(current_nodes),before(previous_nodes),graph(configured),prior_graph(previous_configured),limits(admission){allocated.reserve(output_count);}
    Status prepare() {
        for(auto& value:nodes){const auto status=prepare_node(value);if(status!=Status::ok)return status;}
        return Status::ok;
    }
    Status measure(std::size_t capacity,std::size_t& owned,std::size_t& retired,std::size_t& count) const {
        return ContentAccounting(allocated,capacity).measure(nodes,owned,retired,count);
    }
};
}
Status NumericPreparation::reconcile_history(){
    const auto before=previous?std::span<const PlanNode>(previous->impl_->nodes):std::span<const PlanNode>{};
    ContentPreparation contents(plan->impl_->nodes,before,*plan->impl_->numeric_graph,
        previous?previous->impl_->numeric_graph.get():nullptr,profile.admission,content_outputs);
    {const auto status=contents.prepare();if(status!=Status::ok)return status;}
    std::size_t retired=0;
    const auto status=contents.measure(content_capacity,plan->impl_->content_owned_bytes,retired,plan->impl_->content_token_count);
    if(status!=Status::ok)return status;
    if(retired>retained_content_budget)return Status::exhausted;
    const auto unused=retained_content_budget-retired;
    plan->impl_->memory.state_bytes-=unused;
    plan->impl_->numeric_admission->resources.plan_bytes-=unused;
    return Status::ok;
}
void NumericPreparation::reserve_mixer_destinations(){
    for(std::size_t parent=0;parent<plan->impl_->nodes.size();++parent){std::size_t count=0;
        for(const auto& mixer:plan->impl_->nodes)if(mixer.operation==ProcessingOperation::Mixer || mixer.operation==ProcessingOperation::ChannelComparison)
            for(const auto& input:mixer.endpoint_plan->inputs)if(input.parent==parent)++count;
        plan->impl_->nodes[parent].mixer_destinations.reserve(count);}
}
void NumericPreparation::bind_mixer_destinations(){
    for(std::size_t m=0;m<plan->impl_->nodes.size();++m)bind_mixer_inputs(m);
}
void NumericPreparation::bind_mixer_inputs(std::size_t m){
        const auto& mixer=plan->impl_->nodes[m];if(mixer.operation!=ProcessingOperation::Mixer && mixer.operation!=ProcessingOperation::ChannelComparison)return;
        const auto anchor=std::find_if(mixer.endpoint_plan->inputs.begin(),mixer.endpoint_plan->inputs.end(),[](const auto& input){return input.parent!=absent;});
        for(std::size_t q=0;q<mixer.endpoint_plan->inputs.size();++q){const auto& input=mixer.endpoint_plan->inputs[q];
            if(input.parent!=absent)plan->impl_->nodes[input.parent].mixer_destinations.push_back({m,q,input.parent_port});
            else if(aligned_operation(mixer,*plan->impl_->numeric_graph)&&anchor!=mixer.endpoint_plan->inputs.end())
                plan->impl_->nodes[anchor->parent].mixer_destinations.push_back({m,q,anchor->parent_port});}
}
Status NumericPreparation::check_clock_cycles(){
    // Retained clock edges participate in the same event schedule and cannot form a cycle.
    for(std::size_t i=0;i<plan->impl_->nodes.size();++i){auto cursor=i;std::size_t count=0;
        while(cursor!=absent){if(++count>plan->impl_->nodes.size())return Status::invalid_argument;
            const auto& node=plan->impl_->nodes[cursor];cursor=node.parent!=absent?node.parent:node.numeric_clock_parent;}}
    return Status::ok;
}
void NumericPreparation::bind_destination(std::size_t i){
    const auto& n=plan->impl_->nodes[i];const auto parent=n.parent!=absent?n.parent:n.numeric_clock_parent;
    if((n.operation==ProcessingOperation::InputSelector || n.operation==ProcessingOperation::Switch || n.operation==ProcessingOperation::WLEDEncoder || paired_processing(n.operation))&&n.endpoint_plan){
        bind_selector_destinations(i,parent);
    }else if(parent!=absent)plan->impl_->nodes[parent].children.push_back(i);
    if(n.operation==ProcessingOperation::History)clear_node_state(n,c->impl_->states[i]);
    c->impl_->states[i].pending_discontinuity=n.reset;
}
void NumericPreparation::bind_selector_destinations(std::size_t i,std::size_t parent){
    const auto& n=plan->impl_->nodes[i];
    for(std::size_t lane=0;lane<n.endpoint_plan->inputs.size();++lane){const auto& input=n.endpoint_plan->inputs[lane];
        if(input.parent!=absent)plan->impl_->nodes[input.parent].selector_destinations.push_back({i,lane,input.parent_port});}
    if(n.parent==absent&&parent!=absent)plan->impl_->nodes[parent].selector_destinations.push_back({i,0,n.parent_port});
}
void NumericPreparation::bind_destinations(){
    for(std::size_t i=0;i<plan->impl_->nodes.size();++i)bind_destination(i);
}
}
