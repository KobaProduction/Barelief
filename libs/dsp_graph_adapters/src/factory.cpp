#include <dsp_graph_adapters/factory.h>
#include "common_operations.h"
#include "operation_support.h"
#include "operation_constraints.h"

namespace dsp_graph_adapters {
namespace {
using namespace dataflow_runtime;
using namespace signal_contracts;
using F=domain::SequenceOperationFamily;
bool phase_required(const NumericOperationNodeView& node) noexcept {
    std::size_t method=2;if(!detail::integer(node,"method",2,method))return true;
    return method==4 || (method>=6 && method<=10);
}
bool required(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId port) noexcept {
    const auto kind=n.kind;
    if(kind=="SpectralNovelty" && port==1)return phase_required(n);
    if(port==0 || (port==1 && (kind=="OnsetBacktrack" || kind=="DynamicBeatPath" ||
        kind=="BarStateSequence" || kind=="BeatFeatureSynchronization")))return true;
    return false;
}
ValueDomain input_domain(std::string_view kind,domain::PortId port) noexcept {
    if((kind=="BarStateSequence" || kind=="TempoEstimate") && port==1)return ValueDomain::AnyIeee;
    return ValueDomain::FiniteOnly;
}

bool model_ieee_output(std::string_view kind,domain::PortId port) noexcept {
    if(kind=="GaussianMixtureScore")return port==1;
    if(kind=="PatternStateSequence" || kind=="PcmPatternSequence")return port==3;
    if(kind=="DownbeatStateSequence")return port==2;
    return kind=="CrfBeatSequence" && (port==3 || port==4);
}
bool tempo_ieee_output(std::string_view kind,domain::PortId port) noexcept {
    if(kind=="TempoEstimate")return true;
    if(kind=="TempoAnalysis")return port==0;
    return false;
}
ValueDomain output_domain(std::string_view kind,domain::PortId port) noexcept {
    return model_ieee_output(kind,port) || tempo_ieee_output(kind,port) || (kind=="BeatFeatureSynchronization" && port==0)?
        ValueDomain::AnyIeee:ValueDomain::FiniteOnly;
}
void stream_policy(const NumericOperationNodeView& n,NumericOperationPolicy& p) noexcept {
    if(n.kind=="SpectralNovelty" && required(n,1)){
        p.match=NumericInputMatch::SessionSequence;p.session=NumericInputSession::SharedProvenance;
    }
    bool online=false,reset=true;
    if((n.kind=="BeatStateSequence" || n.kind=="TempoHistogram" || n.kind=="TempoAnalysis") && detail::flag(n,"online",false,online) &&
       detail::flag(n,"reset_state",true,reset) && online && !reset){
        p.mode=NumericOperationMode::ContinuingStream;p.match=NumericInputMatch::SessionSequence;p.session=NumericInputSession::SharedProvenance;
    }
}
NumericOperationPolicy policy(const dataflow_runtime::NumericOperationNodeView& n) {
    NumericOperationPolicy p;p.inputs.reserve(n.inputs.size());p.output_values.reserve(n.outputs.size());
    stream_policy(n,p);
    for(const auto& i:n.inputs)p.inputs.push_back({i.id,NumericInputUse::Frame,required(n,i.id),input_domain(n.kind,i.id)});
    for(std::size_t i=0;i<n.outputs.size();++i)p.output_values.push_back(output_domain(n.kind,domain::PortId(i)));
    detail::sequence_constraints(n,p);return p;
}
domain::ProcessingSupport assess_feature_family(const NumericOperationNodeView& n,const NumericOperationProfile& p,const ClassicalModel* model,F family) noexcept {
    if(family==F::Tempo)return detail::assess_tempo(n,p);
    if(family==F::BeatPulse)return detail::assess_beat_pulse(n,p);
    if(family==F::Histogram)return detail::assess_histogram(n,p);
    if(family==F::State)return detail::assess_state(n,p,model);
    return detail::assess_model(n,p,model);
}
domain::ProcessingSupport assess_sequence(const NumericOperationNodeView& n,const NumericOperationProfile& p,const ClassicalModel* model) noexcept {
    const auto* spec=domain::sequence_operation_spec(n.kind);if(!spec)return {};
    if(spec->family==F::Spectral)return detail::assess_spectral_sequence(n,p);
    if(spec->family==F::Onset)return detail::assess_onset(n,p);
    return assess_feature_family(n,p,model,spec->family);
}
domain::ProcessingSupport assess_with_model(const NumericOperationNodeView& n,const NumericOperationProfile& p,const ClassicalModel* model) noexcept {
    auto support=assess_sequence(n,p,model);
    if(model && support.supported){
        std::size_t bytes=0;if(!model->owned_bytes(bytes) || !numeric_operation_add_bytes(support.resources.plan_bytes,bytes))return {};
    }
    return support;
}
Status prepare_sequence_operation(std::shared_ptr<const NumericOperationDescriptor> descriptor,const NumericOperationProfile& profile,
    std::unique_ptr<PreparedNumericOperation>& output) {
    const auto node=detail::definition(descriptor->node);const auto model=std::static_pointer_cast<const ClassicalModel>(descriptor->immutable_resources);
    const auto support=assess_with_model(node,profile,model.get());if(!support.supported)return Status::incompatible_contract;
    auto next=detail::prepare_sequence(descriptor->node,model);if(!next)return Status::invalid_argument;
    next->bind(std::move(descriptor),profile.admission);domain::ProcessingResources actual;
    if(!next->owned_resources(actual) || !detail::within(actual,support.resources))return Status::exhausted;
    output=std::move(next);return Status::ok;
}
class DspOperationFactory final:public NumericOperationFactory {
public:
    explicit DspOperationFactory(DspOperationFactoryOptions o):options_(std::move(o)){}
    Status describe(const dataflow_runtime::NumericOperationNodeView& n,std::shared_ptr<const NumericOperationDescriptor>& out) const override {
        if(detail::common_operation_kind(n.kind))return detail::describe_common_operation(n,this,out);
        if(!domain::sequence_operation_kind(n.kind))return options_.fallback?options_.fallback->describe(n,out):Status::unavailable;
        std::shared_ptr<const NumericOperationDescriptor> next;
        const auto status=own_numeric_operation_descriptor(n,policy(n),next,this);if(status!=Status::ok)return status;
        if(const auto model=find_model(n))std::const_pointer_cast<NumericOperationDescriptor>(next)->immutable_resources=model;
        out=std::move(next);return Status::ok;
    }
    domain::ProcessingSupport assess(const dataflow_runtime::NumericOperationNodeView& n,const NumericOperationProfile& p) const noexcept override {
        if(detail::common_operation_kind(n.kind))return detail::assess_common_operation(n,p);
        if(!domain::sequence_operation_kind(n.kind))return options_.fallback?options_.fallback->assess(n,p):domain::ProcessingSupport{};
        const auto model=find_model(n);return assess_with_model(n,p,model.get());
    }
    Status prepare(std::shared_ptr<const NumericOperationDescriptor> d,const NumericOperationProfile& p,
        std::unique_ptr<PreparedNumericOperation>& out) const override {
        if(!d)return Status::invalid_argument;
        if(d->factory_identity!=this)return options_.fallback?options_.fallback->prepare(std::move(d),p,out):Status::unavailable;
        if(detail::common_operation_kind(d->node.kind))return detail::prepare_common_operation(std::move(d),p,out);
        return prepare_sequence_operation(std::move(d),p,out);
    }
private:
    std::shared_ptr<const ClassicalModel> find_model(const dataflow_runtime::NumericOperationNodeView& n) const noexcept {
        const auto key=detail::text(n,"model");return options_.models && !key.empty()?options_.models->find(key):nullptr;
    }
    DspOperationFactoryOptions options_;
};
}
std::shared_ptr<const NumericOperationFactory> make_dsp_operation_factory(DspOperationFactoryOptions options) {
    return std::make_shared<DspOperationFactory>(std::move(options));
}
namespace detail {
namespace {
PreparedSequence prepare_feature_family(const dataflow_runtime::NumericOperationNode& node,std::shared_ptr<const ClassicalModel> model,F family) {
    if(family==F::Tempo)return prepare_tempo(node);
    if(family==F::BeatPulse)return prepare_beat_pulse(node);
    if(family==F::Histogram)return prepare_histogram(node);
    if(family==F::State)return prepare_state(node,std::move(model));
    return prepare_model(node,std::move(model));
}
}
PreparedSequence prepare_sequence(const dataflow_runtime::NumericOperationNode& node,std::shared_ptr<const ClassicalModel> model) {
    const auto* spec=domain::sequence_operation_spec(node.kind);if(!spec)return {};
    if(spec->family==F::Spectral)return prepare_spectral_sequence(node);
    if(spec->family==F::Onset)return prepare_onset(node);
    return prepare_feature_family(node,std::move(model),spec->family);
}
}
}
