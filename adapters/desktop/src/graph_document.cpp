#include <desktop/graph_document.h>
#include <QJsonArray>
#include <config/config_migration.h>
#include <limits>
#include <cmath>
#include <QSet>
namespace desktop {
namespace {
bool integer(const QJsonValue& value, int& output) {
    if(!value.isDouble()) return false;
    const double n=value.toDouble();
    if(!std::isfinite(n) || std::floor(n)!=n || n<0 || n>=std::numeric_limits<int>::max()) return false;
    output=int(n); return true;
}
signal_contracts::FrameType frame_type(const QString& value) {
    using T=signal_contracts::FrameType;
    if(value=="window") return T::Window;
    if(value=="windowed") return T::Windowed;
    if(value=="stereo-spectrum") return T::StereoSpectrum;
    if(value=="spectrum") return T::Spectrum;
    if(value=="history") return T::History;
    if(value=="audio") return T::PCM;
    if(value=="scalar")return T::Scalar;
    if(value=="wled-packet")return T::WledPacket;
    return T::Mel;
}
}
GraphDocument read_graph_document(const QJsonObject& input) {
    GraphDocument result; result.document=input;
    result.error=configuration::check_version(input,"version",2);
    if(result.error.isEmpty())result.error=configuration::check_version(input,"processing_version",1);
    if(!result.error.isEmpty())return result;
    if(input.value("version")==1){result.error="Legacy graph migration required";return result;}
    result.graph.graph_id=input.value("graph_id").toString("main").toStdString();
    if(input.contains("processing_version")) {
        int version;
        if(!integer(input.value("processing_version"),version) || !version) {result.error="Invalid processing version";return result;}
        result.graph.processing_version=version;
    }
    if(!input.value("nodes").isArray() || !input.value("connections").isArray()) {
        result.error="Graph arrays missing"; return result;
    }
    for(const auto& value:input.value("nodes").toArray()) {
        const auto object=value.toObject(); auto p=object.value("internal-data").toObject();
        const auto kind=domain::node_kind(p.value("model-name").toString().toStdString());
        int id=-1;
        if(!kind || !integer(object.value("id"),id)) {result.error="Invalid node kind or ID";return result;}
        if((*kind==domain::NodeKind::NetworkInput || *kind==domain::NodeKind::NetworkOutput) &&
           !p.contains("data_type") && p.contains("type")) p["data_type"]=p.value("type");
        QStringList registered{"enabled"};
        using K=domain::NodeKind;
        switch(*kind) {
        case K::AudioSource: registered<<"source";break;
        case K::SpectrumMerge: registered<<"merge";break;
        case K::SpectrumGain: case K::WLED: case K::WLEDEncode: registered<<"gain";break;
        case K::Mel: registered<<"bands";break;
        case K::Spectrogram: registered<<"history_size";break;
        case K::FFTView: registered<<"channel"<<"merge";break;
        case K::Window: registered<<"size";break;
        case K::WindowFunction: registered<<"function";break;
        case K::NetworkInput: case K::NetworkOutput: registered<<"data_type"<<"provider_uuid"<<"stream_id"<<"host"<<"port";break;
        case K::Splitter: case K::Switch: case K::Gate: registered<<"data_type"<<"outputs"<<"selected";break;
        case K::ScalarReduce: registered<<"data_type"<<"operation";break;
        case K::TestSignal: registered<<"tones"<<"sample_rate"<<"minimum_hz"<<"maximum_hz"<<"amplitude"<<"period_seconds"<<"sweep_depth";break;
        default: break;
        }
        // Validate external JSON representation before numeric conversion. Domain
        // validates semantic ranges afterward; conversion must never erase errors.
        const QStringList integers{"source","merge","channel","bands","history_size","size","tones","sample_rate","port","outputs","selected","operation"};
        const QStringList numbers{"gain","minimum_hz","maximum_hz","amplitude","period_seconds","sweep_depth"};
        const QStringList strings{"provider_uuid","stream_id","host","function","data_type"};
        for(const auto& key:integers) if(registered.contains(key) && p.contains(key)) {
            int parsed;
            if(!integer(p.value(key),parsed)) {result.error="Invalid integer: "+key;return result;}
        }
        for(const auto& key:numbers) if(registered.contains(key) && p.contains(key) && (!p.value(key).isDouble() || !std::isfinite(p.value(key).toDouble()))) {
            result.error="Invalid number: "+key;return result;
        }
        for(const auto& key:strings) if(registered.contains(key) && p.contains(key) && !p.value(key).isString()) {result.error="Invalid string: "+key;return result;}
        if(p.contains("enabled") && !p.value("enabled").isBool()) {result.error="Invalid enabled flag";return result;}
        if(registered.contains("function") && p.contains("function") && !QStringList{"hann","hamming","blackman","rectangular"}.contains(p.value("function").toString())) {
            result.error="Invalid window function";return result;
        }
        QStringList types{"audio","window","windowed","stereo-spectrum","spectrum","mel","history","scalar"};
        if(*kind!=K::ScalarReduce)types<<"wled-packet";
        if(registered.contains("data_type") && p.contains("data_type") && !types.contains(p.value("data_type").toString())) {
            result.error="Invalid frame type";return result;
        }
        domain::NodeDefinition node{id,*kind,domain::default_parameters(*kind),p.value("enabled").toBool(true)};
        std::visit([&](auto& parameters) {
            using P=std::decay_t<decltype(parameters)>;
            if constexpr(std::is_same_v<P,domain::AudioSourceParameters>) parameters.source=domain::AudioSource(p.value("source").toInt(0));
            else if constexpr(std::is_same_v<P,domain::MergeParameters>) parameters.merge=domain::MergeMode(p.value("merge").toInt(2));
            else if constexpr(std::is_same_v<P,domain::GainParameters>) parameters.gain=p.value("gain").toDouble(1);
            else if constexpr(std::is_same_v<P,domain::MelParameters>) parameters.bands=p.value("bands").toInt(50);
            else if constexpr(std::is_same_v<P,domain::HistoryParameters>) parameters.history_size=p.value("history_size").toInt(400);
            else if constexpr(std::is_same_v<P,domain::FFTViewParameters>) {parameters.channel=domain::Channel(p.value("channel").toInt(0));parameters.merge=domain::MergeMode(p.value("merge").toInt(2));}
            else if constexpr(std::is_same_v<P,domain::WindowParameters>) parameters.size=p.value("size").toInt(4096);
            else if constexpr(std::is_same_v<P,domain::WindowFunctionParameters>) {
                const auto function=p.value("function").toString("hann");
                parameters.function=function=="hamming"?domain::WindowFunction::Hamming:function=="blackman"?domain::WindowFunction::Blackman:function=="rectangular"?domain::WindowFunction::Rectangular:domain::WindowFunction::Hann;
            } else if constexpr(std::is_same_v<P,domain::NetworkParameters>) {
                parameters.data_type=frame_type(p.value("data_type").toString("mel"));
                parameters.provider_uuid=p.value("provider_uuid").toString().toStdString();
                parameters.stream_id=p.value("stream_id").toString().toStdString();
                parameters.host=p.value("host").toString().toStdString(); parameters.port=p.value("port").toInt(8214);
            } else if constexpr(std::is_same_v<P,domain::RoutingParameters>) {
                const int outputs=p.value("outputs").toInt(2),selected=p.value("selected").toInt(0);
                if(outputs>8 || outputs<2 || selected<0 || selected>=outputs)result.error="Invalid route selection or output count";
                parameters.data_type=frame_type(p.value("data_type").toString("mel"));
                parameters.outputs=std::uint16_t(outputs);parameters.selected=std::uint16_t(selected);
            } else if constexpr(std::is_same_v<P,domain::ReductionParameters>) {
                parameters.data_type=frame_type(p.value("data_type").toString("mel"));
                parameters.operation=domain::Reduction(p.value("operation").toInt(0));
            } else if constexpr(std::is_same_v<P,domain::TestSignalParameters>) {
                parameters.tones=p.value("tones").toInt(8);parameters.sample_rate=p.value("sample_rate").toInt(48000);
                parameters.minimum_hz=p.value("minimum_hz").toDouble(73);parameters.maximum_hz=p.value("maximum_hz").toDouble(13901);
                parameters.amplitude=p.value("amplitude").toDouble(.4);parameters.period_seconds=p.value("period_seconds").toDouble(6);
                parameters.sweep_depth=p.value("sweep_depth").toDouble(.18);
            }
        },node.parameters);
        result.graph.nodes.push_back(std::move(node));
    }
    for(const auto& value:input.value("connections").toArray()) {
        const auto edge=value.toObject();int from,to,out,in;
        if(!integer(edge.value("outNodeId"),from) || !integer(edge.value("inNodeId"),to) ||
           !integer(edge.value("outPortIndex"),out) || !integer(edge.value("inPortIndex"),in) || out>65535 || in>65535) {
            result.error="Invalid edge encoding";return result;
        }
        result.graph.edges.push_back({from,to,std::uint16_t(out),std::uint16_t(in)});
    }
    return result;
}
GraphDocument read_processing_graph_document(const QJsonObject &document) {
    auto result=read_graph_document(document);
    if(result.error.isEmpty())for(auto &node:result.graph.nodes)node=domain::project_processing_node(node);
    return result;
}
ProcessingProjectionResult project_processing_graph(const QJsonObject &document) {
    const auto mapped=read_graph_document(document);
    if(!mapped.error.isEmpty())return {{},mapped.error};
    QJsonArray nodes;
    for(const auto &value:document.value("nodes").toArray()) {
        const auto n=value.toObject();
        auto p=n.value("internal-data").toObject();
        const auto kind=domain::node_kind(p.value("model-name").toString().toStdString());
        for(auto it=p.begin();it!=p.end();) {
            if(kind && domain::parameter_classification(*kind,it.key().toStdString())==domain::ParameterClassification::Presentation)
                it=p.erase(it);
            else ++it;
        }
        nodes.append(QJsonObject{{"id",n.value("id")},{"internal-data",p}});
    }
    return {QJsonObject{{"version",2},{"processing_version",1},
        {"graph_id",document.value("graph_id").toString("main")},{"nodes",nodes},{"connections",document.value("connections")}}, {}};
}
QJsonObject processing_graph_document(const QJsonObject &document) {
    return project_processing_graph(document).document;
}
ProcessingDTOProjectionResult project_processing_dto(const QJsonObject& document) {
    const auto schema=configuration::classify_document(document);
    if(!schema)return {{},schema.error};
    const auto version=configuration::check_version(document,"version",2);
    const auto processing_version=configuration::check_version(document,"processing_version",1);
    if(!version.isEmpty() || !processing_version.isEmpty())return {{},!version.isEmpty()?version:processing_version};
    if(document.contains("graph_id") && (!document["graph_id"].isString() || document["graph_id"].toString().isEmpty()))
        return {{},"Invalid graph identity"};
    if(!document["nodes"].isArray() || !document["connections"].isArray())return {{},"Graph arrays missing"};
    auto filtered=document;QJsonArray nodes;QSet<int> identities;
    for(const auto& value:document["nodes"].toArray()) {
        const auto node=value.toObject();auto p=node["internal-data"].toObject();
        const auto kind=domain::node_kind(p["model-name"].toString().toStdString());int id;
        if(!kind || !integer(node["id"],id) || identities.contains(id))return {{},"Invalid or duplicate node identity"};
        identities.insert(id);
        if((*kind==domain::NodeKind::NetworkInput || *kind==domain::NodeKind::NetworkOutput) &&
           !p.contains("data_type") && p.contains("type")) {
            // Insertion may invalidate QJsonValueRef indices in this object.
            const QJsonValue legacy_type=p.value("type");
            p["data_type"]=legacy_type;
        }
        QJsonObject registered{{"model-name",p["model-name"]}};
        for(auto it=p.begin();it!=p.end();++it) {
            const auto* descriptor=domain::parameter_descriptor(*kind,it.key().toStdString());
            if(descriptor && (descriptor->classification==domain::ParameterClassification::Processing ||
                              descriptor->classification==domain::ParameterClassification::ApplicationAdapter))
                registered[it.key()]=it.value();
        }
        nodes.append(QJsonObject{{"id",id},{"internal-data",registered}});
    }
    filtered["nodes"]=nodes;
    const auto mapped=read_processing_graph_document(filtered);
    if(!mapped.error.isEmpty())return {{},mapped.error};
    // Match the outgoing boundary after stripping local display/extensions.
    // Device capabilities and graph admission remain application concerns.
    for(const auto& node:mapped.graph.nodes)
        if(!domain::parameters_valid(node))return {{},"Invalid processing node"};
    return {{mapped.graph},{}};
}
ProcessingProjectionResult processing_dto_document(const application::ProcessingDTO& processing) {
    const auto& graph=processing.graph;
    if(graph.processing_version!=1 || graph.graph_id.empty())return {{},"Unsupported processing version or invalid graph identity"};
    QJsonArray nodes,edges;QSet<int> identities;
    for(const auto& value:graph.nodes) {
        const auto node=domain::project_processing_node(value);
        if(node.id<0 || node.id==std::numeric_limits<int>::max() || !domain::node_descriptor(node.kind) ||
           identities.contains(node.id) || !domain::parameters_valid(node))return {{},"Invalid processing node"};
        identities.insert(node.id);
        QJsonObject p{{"model-name",QString::fromUtf8(domain::kind_id(node.kind).data(),qsizetype(domain::kind_id(node.kind).size()))},
                      {"enabled",node.enabled}};
        auto type=[](signal_contracts::FrameType t) {
            const auto* descriptor=signal_contracts::frame_descriptor(t);
            return descriptor?QString::fromUtf8(descriptor->id.data(),qsizetype(descriptor->id.size())):QString{};
        };
        std::visit([&](const auto& params) {
            using P=std::decay_t<decltype(params)>;
            if constexpr(std::is_same_v<P,domain::AudioSourceParameters>)p["source"]=int(params.source);
            else if constexpr(std::is_same_v<P,domain::MergeParameters>)p["merge"]=int(params.merge);
            else if constexpr(std::is_same_v<P,domain::GainParameters>)p["gain"]=params.gain;
            else if constexpr(std::is_same_v<P,domain::MelParameters>)p["bands"]=int(params.bands);
            else if constexpr(std::is_same_v<P,domain::HistoryParameters>)p["history_size"]=int(params.history_size);
            else if constexpr(std::is_same_v<P,domain::WindowParameters>)p["size"]=int(params.size);
            else if constexpr(std::is_same_v<P,domain::WindowFunctionParameters>) {
                p["function"]=params.function==domain::WindowFunction::Hamming?"hamming":
                    params.function==domain::WindowFunction::Blackman?"blackman":
                    params.function==domain::WindowFunction::Rectangular?"rectangular":"hann";
            } else if constexpr(std::is_same_v<P,domain::NetworkParameters>) {
                p["data_type"]=type(params.data_type);p["provider_uuid"]=QString::fromStdString(params.provider_uuid);
                p["stream_id"]=QString::fromStdString(params.stream_id);p["host"]=QString::fromStdString(params.host);p["port"]=int(params.port);
            } else if constexpr(std::is_same_v<P,domain::RoutingParameters>) {
                p["data_type"]=type(params.data_type);p["outputs"]=int(params.outputs);p["selected"]=int(params.selected);
            } else if constexpr(std::is_same_v<P,domain::ReductionParameters>) {
                p["data_type"]=type(params.data_type);p["operation"]=int(params.operation);
            } else if constexpr(std::is_same_v<P,domain::TestSignalParameters>) {
                p["tones"]=int(params.tones);p["sample_rate"]=int(params.sample_rate);p["minimum_hz"]=params.minimum_hz;
                p["maximum_hz"]=params.maximum_hz;p["amplitude"]=params.amplitude;p["period_seconds"]=params.period_seconds;p["sweep_depth"]=params.sweep_depth;
            }
        },node.parameters);
        // Variant representation is translated above; membership/classification
        // is authoritative only in the configured domain descriptor.
        for(auto it=p.begin();it!=p.end();) {
            const auto* descriptor=domain::parameter_descriptor(node.kind,it.key().toStdString());
            if(it.key()!="model-name" && (!descriptor ||
                (descriptor->classification!=domain::ParameterClassification::Processing &&
                 descriptor->classification!=domain::ParameterClassification::ApplicationAdapter)))it=p.erase(it);
            else ++it;
        }
        nodes.append(QJsonObject{{"id",node.id},{"internal-data",p}});
    }
    for(const auto& edge:graph.edges) {
        if(!identities.contains(edge.from) || !identities.contains(edge.to))return {{},"Invalid processing edge identity"};
        edges.append(QJsonObject{{"outNodeId",edge.from},{"inNodeId",edge.to},
            {"outPortIndex",edge.output_port},{"inPortIndex",edge.input_port}});
    }
    return {QJsonObject{{"version",2},{"processing_version",int(graph.processing_version)},
        {"graph_id",QString::fromStdString(graph.graph_id)},{"nodes",nodes},{"connections",edges}}, {}};
}
QJsonObject restore_graph_layout(const QJsonObject &processing,const QJsonObject &presentation) {
    const auto projection=project_processing_graph(processing);
    if(!projection)return {};
    if(processing.value("graph_id").toString("main")!=presentation.value("graph_id").toString("main"))return projection.document;
    auto result=presentation;
    for(auto it=projection.document.begin();it!=projection.document.end();++it)result[it.key()]=it.value();
    QJsonArray nodes;
    for(const auto &value:projection.document.value("nodes").toArray()) {
        auto node=value.toObject();
        for(const auto &old:presentation.value("nodes").toArray()) {
            auto layout=old.toObject();
            if(layout.value("id")==node.value("id") &&
               layout.value("internal-data").toObject().value("model-name")==node.value("internal-data").toObject().value("model-name")) {
                const auto local=layout.value("internal-data").toObject();
                auto parameters=node.value("internal-data").toObject();
                const auto kind=domain::node_kind(parameters.value("model-name").toString().toStdString());
                for(auto it=local.begin();it!=local.end();++it)
                    if(kind && (domain::parameter_classification(*kind,it.key().toStdString())==domain::ParameterClassification::Presentation ||
                                domain::parameter_classification(*kind,it.key().toStdString())==domain::ParameterClassification::OpaqueExtension))
                        parameters[it.key()]=it.value();
                node["internal-data"]=parameters;
                for(auto it=node.begin();it!=node.end();++it)layout[it.key()]=it.value();
                node=layout;break;
            }
        }
        nodes.append(node);
    }
    result["nodes"]=nodes;
    QJsonArray edges;
    const QStringList identity{"outNodeId","inNodeId","outPortIndex","inPortIndex"};
    auto matching_kind=[&](const QJsonValue& id) {
        QString kind;
        for(const auto& n:nodes)if(n.toObject()["id"]==id)
            kind=n.toObject()["internal-data"].toObject()["model-name"].toString();
        int matches=0;
        for(const auto& n:presentation["nodes"].toArray())if(n.toObject()["id"]==id &&
            n.toObject()["internal-data"].toObject()["model-name"].toString()==kind)++matches;
        return !kind.isEmpty() && matches==1;
    };
    for(const auto& value:projection.document["connections"].toArray()) {
        auto edge=value.toObject();QJsonObject local;int matches=0;
        if(matching_kind(edge["outNodeId"]) && matching_kind(edge["inNodeId"])) {
            for(const auto& previous:presentation["connections"].toArray()) {
                const auto candidate=previous.toObject();bool same=true;
                for(const auto& key:identity)if(candidate[key]!=edge[key])same=false;
                if(same){local=candidate;++matches;}
            }
        }
        if(matches==1) {
            for(auto it=edge.begin();it!=edge.end();++it)local[it.key()]=it.value();
            edge=local;
        }
        edges.append(edge);
    }
    result["connections"]=edges;return result;
}
}
