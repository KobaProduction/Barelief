#include <desktop/numeric_source_format.h>
#include <algorithm>
#include <cmath>
namespace desktop {
namespace {
using namespace domain;
using namespace signal_contracts;
bool declared_source_bound(std::string_view key,double value) noexcept {
    const auto kind=node_kind("AudioSource");
    const auto* descriptor=kind?parameter_descriptor(*kind,key):nullptr;
    return descriptor && std::isfinite(value) && value>=descriptor->minimum && value<=descriptor->maximum;
}
ConfiguredProcessingNode* find_source(NumericProcessingGraph& graph,NodeId id) {
    auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& value){return value.id==id;});
    return found==graph.nodes.end()?nullptr:&*found;
}
bool set_rate(ConfiguredProcessingNode& source,std::uint32_t rate) {
    auto* fields=std::get_if<EndpointProcessingParameters>(&source.parameters);
    if(!fields)return false;
    auto found=std::find_if(fields->values.begin(),fields->values.end(),[](const auto& p){return p.key=="sample_rate";});
    if(found==fields->values.end())fields->values.push_back({"sample_rate",NumericLiteral{std::uint64_t(rate)}});
    else found->value=NumericLiteral{std::uint64_t(rate)};
    return true;
}
bool rebuild_outputs(ConfiguredProcessingNode& source,std::uint32_t rate,std::uint32_t outputs,const ProcessingAdmissionLimits& limits) {
    const auto old=source.outputs.front().contract.view();
    const auto* numeric=std::get_if<NumericContract>(&old);
    if(!numeric)return false;
    const std::string clock(numeric->clock.domain);
    auto value=*numeric;value.clock={clock,{1,rate},rate};
    OwnedProcessingContract contract;
    if(own_processing_contract(value,limits.frame,limits.max_owned_metadata_bytes,contract)!=Status::ok)return false;
    source.outputs.clear();
    for(PortId port=0;port<outputs;++port)source.outputs.push_back({port,PortDirection::Output,contract});
    return set_rate(source,rate) && attach_numeric_parameter_ports(source,limits)==Status::ok;
}
}
std::uint32_t numeric_source_output_count(const domain::ConfiguredProcessingNode& source,std::uint32_t native_outputs) noexcept {
    const auto* fields=std::get_if<domain::EndpointProcessingParameters>(&source.parameters);
    if(!fields)return 0;
    for(const auto& parameter:fields->values)if(parameter.key=="output_count") {
        const auto* literal=std::get_if<signal_contracts::NumericLiteral>(&parameter.value);
        if(!literal)return 0;
        const auto value=std::visit([](auto n){return static_cast<double>(n);},*literal);
        if(!declared_source_bound("output_count",value) || std::floor(value)!=value)return 0;
        return value?std::min(native_outputs,std::uint32_t(value)):native_outputs;
    }
    return native_outputs;
}
namespace {
class SourceFormatFailure {
    QString* message;
    NumericSourceFormatDiagnostic* diagnostic;
    NodeId node;
public:
    SourceFormatFailure(QString* text,NumericSourceFormatDiagnostic* detail,NodeId id)
        :message(text),diagnostic(detail),node(id) {
        if(message)message->clear();
        if(diagnostic)*diagnostic={NumericSourceFormatError::None,node,{}};
    }
    bool reject(NumericSourceFormatError reason,const char* text,std::optional<PortId> port={}) const {
        if(message)*message=QString::fromUtf8(text);
        if(diagnostic)*diagnostic={reason,node,port};
        return false;
    }
};
class SourceFormatResolution {
    NumericProcessingGraph candidate;
    NodeId id;
    std::uint32_t rate,outputs;
    const SourceFormatFailure& failure;
    const ProcessingAdmissionLimits& limits;
    ConfiguredProcessingNode* source=nullptr;
    bool connected_outputs_present() const {
        for(const auto& edge:candidate.edges) {
            if(edge.from!=id || edge.output_port<outputs || edge.output_port==sample_rate_output)continue;
            return failure.reject(NumericSourceFormatError::ConnectedOutputUnavailable,
                "Native source format has no connected output",edge.output_port);
        }
        return true;
    }
    bool prepare_source() {
        source=find_source(candidate,id);
        if(!source || source->kind!="AudioSource" || source->outputs.empty())
            return failure.reject(NumericSourceFormatError::MissingSource,"Missing physical source");
        outputs=numeric_source_output_count(*source,outputs);
        if(!outputs)return failure.reject(NumericSourceFormatError::InvalidContract,"Invalid physical source contract");
        if(outputs>=limits.max_ports_per_node)
            return failure.reject(NumericSourceFormatError::AdmissionLimit,"Native source output count exceeds execution admission limits");
        return connected_outputs_present();
    }
    bool evaluate(NumericProcessingGraph& out) {
        NumericProcessingGraph evaluated,executable;
        const auto result=resolve_numeric_parameter_bindings(candidate,limits,evaluated,executable,id);
        if(!result)return failure.reject(NumericSourceFormatError::IncompatibleRoute,"Native source format requires an explicit compatible route");
        out=std::move(evaluated);
        return true;
    }
public:
    SourceFormatResolution(const NumericProcessingGraph& graph,NodeId node,std::uint32_t fs,std::uint32_t count,const ProcessingAdmissionLimits& admission,const SourceFormatFailure& error)
        :candidate(graph),id(node),rate(fs),outputs(count),failure(error),limits(admission){}
    bool resolve(NumericProcessingGraph& out) {
        if(!prepare_source())return false;
        if(!rebuild_outputs(*source,rate,outputs,limits))
            return failure.reject(NumericSourceFormatError::InvalidContract,"Invalid physical source contract");
        return evaluate(out);
    }
};
}
bool resolve_numeric_source_format(domain::NumericProcessingGraph& graph,domain::NodeId id,
    std::uint32_t rate,std::uint32_t outputs,const domain::ProcessingAdmissionLimits& admission,
    QString* error,NumericSourceFormatDiagnostic* diagnostic) {
    const SourceFormatFailure failure(error,diagnostic,id);
    if(!outputs || !declared_source_bound("sample_rate",rate) || !declared_source_bound("output_count",outputs))
        return failure.reject(NumericSourceFormatError::UnsupportedBounds,"Native source format exceeds supported bounds");
    std::size_t owned=0;
    if(processing_graph_owned_bytes(graph,admission,owned)!=Status::ok)
        return failure.reject(NumericSourceFormatError::AdmissionLimit,"Native source graph exceeds execution admission limits");
    return SourceFormatResolution(graph,id,rate,outputs,admission,failure).resolve(graph);
}
}
