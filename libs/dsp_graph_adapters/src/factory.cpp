#include <dsp_graph_adapters/factory.h>
#include "common_operations.h"
#include "operation_support.h"
#include "sequence_settings.h"

namespace dsp_graph_adapters {
namespace {
using namespace dataflow_runtime;
using namespace signal_contracts;
using F=domain::SequenceOperationFamily;
bool required(const domain::ResolvedProcessingNode& n,domain::PortId port) noexcept {
    const auto kind=n.kind;
    if(kind=="SpectralNovelty" && port==1){
        std::size_t method=2;
        if(!detail::integer(n,"method",2,method))return true;
        return method==4 || (method>=6 && method<=10);
    }
    if(port==0 || (port==1 && (kind=="OnsetBacktrack" || kind=="DynamicBeatPath" ||
        kind=="BarStateSequence" || kind=="BeatFeatureSynchronization" || kind=="LocalPulse")))return true;
    bool dynamic=false;if(!detail::flag(n,"dynamic_frequencies",false,dynamic) || !dynamic)return false;
    return (kind=="MetricalTempogram" && port==1) || (port==3 && (kind=="TempoRatios" || kind=="EnvelopeTempoRatios" || kind=="PcmTempoRatios"));
}
ValueDomain input_domain(std::string_view kind,domain::PortId port) noexcept {
    if(kind=="OnsetDetection" && port==0)return ValueDomain::AnyIeee;
    if(kind=="BarStateSequence" && port==1)return ValueDomain::AnyIeee;
    if((kind=="MetricalTempogram" && port==1) ||
       (port==3 && (kind=="TempoRatios" || kind=="EnvelopeTempoRatios" || kind=="PcmTempoRatios")))return ValueDomain::AnyIeee;
    if((port==1 && (kind=="TempoEstimate" || kind=="EnvelopeTempo" || kind=="EnvelopeLocalPulse" || kind=="PcmTempo" || kind=="PcmLocalPulse")) ||
       (port==2 && (kind=="LocalPulse" || kind=="TempoRatios" || kind=="EnvelopeTempoRatios" || kind=="PcmTempoRatios" || kind=="EnvelopeBeatPath" || kind=="PcmBeatPath")))return ValueDomain::AnyIeee;
    return ValueDomain::FiniteOnly;
}
ValueDomain output_domain(std::string_view kind,domain::PortId port) noexcept {
    if(kind=="TempoEstimate" || kind=="EnvelopeTempo" || kind=="MetricalTempogram")return ValueDomain::AnyIeee;
    if(kind=="BeatFeatureSynchronization" && port==0)return ValueDomain::AnyIeee;
    if(kind=="GaussianMixtureScore" && port==1)return ValueDomain::AnyIeee;
    if(kind=="PatternStateSequence" || kind=="PcmPatternSequence")return port==3?ValueDomain::AnyIeee:ValueDomain::FiniteOnly;
    if(kind=="DownbeatStateSequence" && port==2)return ValueDomain::AnyIeee;
    if(kind=="TempoAnalysis" && port==0)return ValueDomain::AnyIeee;
    if(kind=="CrfBeatSequence" && (port==3 || port==4))return ValueDomain::AnyIeee;
    if(kind=="PcmTempogram" && port==3)return ValueDomain::AnyIeee;
    if(kind=="PcmTempo" && port==0)return ValueDomain::AnyIeee;
    return ValueDomain::FiniteOnly;
}
NumericOperationPolicy policy(const domain::ResolvedProcessingNode& n) {
    NumericOperationPolicy p;p.inputs.reserve(n.inputs.size());p.output_values.reserve(n.outputs.size());
    if(n.kind=="SpectralNovelty" && required(n,1)){
        p.match=NumericInputMatch::SessionSequence;p.session=NumericInputSession::SharedProvenance;
    }
    bool online=false,reset=true;
    if((n.kind=="BeatStateSequence" || n.kind=="TempoHistogram" || n.kind=="TempoAnalysis") && detail::flag(n,"online",false,online) &&
       detail::flag(n,"reset_state",true,reset) && online && !reset){
        p.mode=NumericOperationMode::ContinuingStream;p.match=NumericInputMatch::SessionSequence;p.session=NumericInputSession::SharedProvenance;
    }
    for(const auto& i:n.inputs)p.inputs.push_back({i.id,NumericInputUse::Frame,required(n,i.id),input_domain(n.kind,i.id)});
    for(std::size_t i=0;i<n.outputs.size();++i)p.output_values.push_back(output_domain(n.kind,domain::PortId(i)));
    return p;
}
domain::ProcessingSupport assess_sequence(const domain::ResolvedProcessingNode& n,const NumericRuntimeProfile& p,const ClassicalModel* model) noexcept {
    const auto* s=domain::sequence_operation_spec(n.kind);if(!s)return {};
    if(s->family==F::Spectral)return detail::assess_spectral_sequence(n,p);
    if(n.kind.starts_with("Pcm") && n.kind!="PcmPatternSequence")return detail::assess_pcm(n,p);
    if(s->family==F::Onset)return detail::assess_onset(n,p);
    if(s->family==F::Tempo)return detail::assess_tempo(n,p);
    if(s->family==F::BeatPulse)return detail::assess_beat_pulse(n,p);
    if(s->family==F::Histogram)return detail::assess_histogram(n,p);
    if(s->family==F::State)return detail::assess_state(n,p,model);
    return detail::assess_model(n,p,model);
}
class DspOperationFactory final:public NumericOperationFactory {
public:
    explicit DspOperationFactory(DspOperationFactoryOptions o):options_(std::move(o)){}
    Status describe(const domain::ResolvedProcessingNode& n,std::shared_ptr<const NumericOperationDescriptor>& out) const override {
        if(detail::common_operation_kind(n.kind))return detail::describe_common_operation(n,this,out);
        if(!domain::sequence_operation_kind(n.kind))return options_.fallback?options_.fallback->describe(n,out):Status::unavailable;
        std::shared_ptr<const NumericOperationDescriptor> next;
        const auto status=own_numeric_operation_descriptor(n,policy(n),next,this);if(status!=Status::ok)return status;
        if(const auto model=find_model(n))std::const_pointer_cast<NumericOperationDescriptor>(next)->immutable_resources=model;
        out=std::move(next);return Status::ok;
    }
    domain::ProcessingSupport assess(const domain::ResolvedProcessingNode& n,const NumericRuntimeProfile& p) const noexcept override {
        if(detail::common_operation_kind(n.kind))return detail::assess_common_operation(n,p);
        if(!domain::sequence_operation_kind(n.kind))return options_.fallback?options_.fallback->assess(n,p):domain::ProcessingSupport{};
        const auto model=find_model(n);auto support=assess_sequence(n,p,model.get());
        if(model && support.supported){
            std::size_t bytes=0;if(!model->owned_bytes(bytes) || !numeric_operation_add_bytes(support.resources.plan_bytes,bytes))return {};
        }
        return support;
    }
    Status prepare(std::shared_ptr<const NumericOperationDescriptor> d,const NumericRuntimeProfile& p,
        std::unique_ptr<PreparedNumericOperation>& out) const override {
        if(!d)return Status::invalid_argument;
        if(d->factory_identity!=this)return options_.fallback?options_.fallback->prepare(std::move(d),p,out):Status::unavailable;
        if(detail::common_operation_kind(d->node.kind))return detail::prepare_common_operation(std::move(d),p,out);
        const auto n=detail::definition(d->node);const auto model=std::static_pointer_cast<const ClassicalModel>(d->immutable_resources);
        auto support=assess_sequence(n,p,model.get());std::size_t model_bytes=0;
        if(!support.supported || (model && (!model->owned_bytes(model_bytes) || !numeric_operation_add_bytes(support.resources.plan_bytes,model_bytes))))return Status::incompatible_contract;
        auto next=detail::prepare_sequence(d->node,model);if(!next)return Status::invalid_argument;
        next->bind(std::move(d),p.admission);domain::ProcessingResources actual;
        if(!next->owned_resources(actual) || !detail::within(actual,support.resources))return Status::exhausted;
        out=std::move(next);return Status::ok;
    }
private:
    std::shared_ptr<const ClassicalModel> find_model(const domain::ResolvedProcessingNode& n) const noexcept {
        const auto key=detail::text(n,"model");return options_.models && !key.empty()?options_.models->find(key):nullptr;
    }
    DspOperationFactoryOptions options_;
};
}
std::shared_ptr<const NumericOperationFactory> make_dsp_operation_factory(DspOperationFactoryOptions options) {
    return std::make_shared<DspOperationFactory>(std::move(options));
}
namespace detail {
PreparedSequence prepare_sequence(const domain::ConfiguredProcessingNode& n,std::shared_ptr<const ClassicalModel> model) {
    const auto* s=domain::sequence_operation_spec(n.kind);if(!s)return {};
    if(s->family==F::Spectral)return prepare_spectral_sequence(n);
    if(n.kind.starts_with("Pcm") && n.kind!="PcmPatternSequence")return prepare_pcm(n);
    if(s->family==F::Onset)return prepare_onset(n);
    if(s->family==F::Tempo)return prepare_tempo(n);
    if(s->family==F::BeatPulse)return prepare_beat_pulse(n);
    if(s->family==F::Histogram)return prepare_histogram(n);
    if(s->family==F::State)return prepare_state(n,std::move(model));
    return prepare_model(n,std::move(model));
}
}
}
