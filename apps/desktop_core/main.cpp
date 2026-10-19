#include <pipeline/numeric_engine.h>
#include <bdsp_protocol/version_admission.h>
#include <pipeline/serial_config.h>
#include <control/numeric_remote_endpoint.h>
#include <control/numeric_control_server.h>
#include <control/numeric_routing_adapter.h>
#include <core_app/numeric_default_graph.h>
#include <desktop/numeric_project_document.h>
#include <aggregator/aggregator_client.h>
#include <aggregator/capabilities.h>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
namespace {
void project_options(QCommandLineParser& parser) {
    parser.addHelpOption();
    parser.addOption({"config","Canonical numeric project","file"});
    parser.addOption({"preset","Numeric preset stable ID or unique name","id/name"});
    parser.addOption({"duration","Exit after milliseconds (0: continuous)","ms","0"});
    parser.addOption({"synthetic","Use eight silent changing tones instead of physical capture"});
    parser.addOption({"analysis-interval-ms","Analysis interval in milliseconds (1..60000)","ms"});
    parser.addOption({"analysis-rate","Analysis results per second (positive integer or numerator/denominator)","Hz"});
}
void network_options(QCommandLineParser& parser) {
    parser.addOption({"server","Enable the numeric CONTROL/DATA server"});
    parser.addOption({"listen","Explicit TCP listen address","address","127.0.0.1"});
    parser.addOption({"port","Numeric endpoint TCP port","port","8213"});
    parser.addOption({"allow-mutations","Allow remote processing commands"});
    parser.addOption({"remote","Connect to a numeric endpoint","host"});
    parser.addOption({"registry-host","Explicit discovery registry address","address"});
    parser.addOption({"registry-port","Discovery registry port","port","8212"});
    parser.addOption({"uuid","Stable registry identity","uuid"});
    pipeline::add_serial_options(parser);
}
struct LaunchOptions {int duration=0;quint16 port=0;};
bool read_launch_options(const QCommandLineParser& parser,LaunchOptions& out) {
    bool duration_ok=false,port_ok=false;
    const auto duration=parser.value("duration").toInt(&duration_ok);
    const auto port=parser.value("port").toUInt(&port_ok);
    if(!duration_ok || duration<0 || !port_ok || !port || port>65535)return false;
    out={duration,quint16(port)};return true;
}
std::optional<signal_contracts::Ratio> analysis_rate(const QString& text) {
    const auto parts=text.split('/');
    bool numerator_ok=false,denominator_ok=parts.size()==1;
    const auto numerator=parts.value(0).toLongLong(&numerator_ok);
    const auto denominator=parts.size()==1?qlonglong{1}:parts.value(1).toLongLong(&denominator_ok);
    if(parts.size()>2 || !numerator_ok || !denominator_ok || numerator<=0 || denominator<=0)return {};
    const auto divisor=std::gcd(numerator,denominator);
    const signal_contracts::Ratio interval{denominator/divisor,std::uint64_t(numerator/divisor)};
    return domain::valid_execution_interval(interval)?std::optional{interval}:std::nullopt;
}
std::optional<signal_contracts::Ratio> analysis_interval(const QString& text) {
    bool valid=false;const auto interval=text.toLongLong(&valid);
    if(!valid || interval<1 || interval>60000)return {};
    const auto divisor=std::gcd(interval,qlonglong{1000});
    return signal_contracts::Ratio{interval/divisor,std::uint64_t(1000/divisor)};
}
bool configure_analysis(const QCommandLineParser& parser,domain::NumericProcessingGraph& graph) {
    if(parser.isSet("analysis-rate") && parser.isSet("analysis-interval-ms")) {
        qCritical()<<"Select analysis-rate or analysis-interval-ms, not both";return false;
    }
    const bool rate=parser.isSet("analysis-rate");
    if(!rate && !parser.isSet("analysis-interval-ms"))return true;
    const auto interval=rate?analysis_rate(parser.value("analysis-rate")):analysis_interval(parser.value("analysis-interval-ms"));
    if(!interval){qCritical()<<"Invalid or unsupported analysis interval/rate";return false;}
    graph.execution_interval=*interval;return true;
}
QJsonObject matching_preset(const QJsonArray& presets,const QString& key,const QString& selected,int& count) {
    QJsonObject result;count=0;
    for(const auto& value:presets){const auto preset=value.toObject();if(preset[key].toString()==selected){result=preset;++count;}}
    return result;
}
struct CliProject {
    domain::NumericProcessingGraph graph=numeric_default_graph();
    QJsonObject runtime_config,adapter_graph;
    bool select_preset(const QString& selected) {
        const auto presets=runtime_config["ui"].toObject()["pipeline_presets"].toArray();
        int count=0;auto match=matching_preset(presets,"id",selected,count);
        if(!count)match=matching_preset(presets,"name",selected,count);
        if(count!=1){qCritical()<<"Preset identity must resolve once";return false;}
        adapter_graph=match["graph"].toObject();
        const auto decoded=desktop::decode_numeric_editor_graph(QJsonDocument(adapter_graph).toJson(QJsonDocument::Compact),graph);
        if(!decoded){qCritical()<<decoded.error;return false;}
        return true;
    }
    static QJsonObject adapter_graph_from(const desktop::NumericDraftPresentation& presentation) {
        QJsonArray nodes;
        for(auto it=presentation.node_adapters.begin();it!=presentation.node_adapters.end();++it)
            nodes.append(QJsonObject{{"id",it.key().toInt()},{"adapter",it.value()}});
        return QJsonObject{{"nodes",nodes}};
    }
    bool project_settings(const desktop::NumericProjectDocument& source) {
        auto result=desktop::read_numeric_application_settings(source,runtime_config);
        QJsonObject network_settings;
        if(result)result=desktop::read_numeric_network_settings(source,pipeline::NumericRoutingAdapter::MaxRoutes,network_settings);
        desktop::NumericDraftPresentation presentation;
        if(result)result=desktop::read_numeric_presentation(source,presentation);
        if(!result){qCritical()<<result.path<<result.error;return false;}
        runtime_config["network"]=network_settings;
        adapter_graph=adapter_graph_from(presentation);return true;
    }
    bool load(const QCommandLineParser& parser) {
        if(!parser.isSet("config")) {
            if(parser.isSet("preset")){qCritical()<<"Preset requires a canonical project";return false;}
            return true;
        }
        desktop::NumericProjectDocument source;const auto loaded=desktop::load_numeric_project(parser.value("config"),source,graph);
        if(!loaded){qCritical()<<loaded.path<<loaded.error;return false;}
        if(!project_settings(source))return false;
        return !parser.isSet("preset") || select_preset(parser.value("preset"));
    }
};
QJsonValue node_adapter(const QJsonObject& node) {
    return node.contains("adapter")?node.value("adapter"):node.value("internal-data").toObject().value("adapter");
}
std::map<domain::NodeId,QString> capture_devices(const QJsonObject& adapter_graph) {
    std::map<domain::NodeId,QString> result;
    const auto nodes=adapter_graph.value("nodes").toArray();
    for(const auto& value:nodes) {
        const auto node=value.toObject();const auto identity=node_adapter(node).toObject().value("device_identity").toString();
        if(!identity.isEmpty())result.emplace(node.value("id").toInt(),identity);
    }
    return result;
}
QMap<int,QJsonObject> graph_node_adapters(const QJsonObject& adapter_graph) {
    QMap<int,QJsonObject> adapters;
    const auto source_nodes=adapter_graph.value("nodes").toArray();
    for(const auto& value:source_nodes) {
        const auto node=value.toObject();const auto adapter=node_adapter(node);
        if(adapter.isObject())adapters.insert(node.value("id").toInt(),adapter.toObject());
    }
    return adapters;
}
void merge_adapters(QJsonObject& document,const QJsonObject& adapter_graph) {
    const auto adapters=graph_node_adapters(adapter_graph);
    auto nodes=document.value("nodes").toArray();
    for(qsizetype i=0;i<nodes.size();++i){auto node=nodes[i].toObject();if(adapters.contains(node.value("id").toInt()))node["adapter"]=adapters.value(node.value("id").toInt());nodes[i]=node;}
    document["nodes"]=nodes;
}
pipeline::AudioSourceFactory capture_factory(bool synthetic) {
    if(!synthetic)return {};
    auto physical=pipeline::default_audio_source_factory();
    return [physical=std::move(physical)](auto){return physical(pipeline::AudioSourceKind::TestSignal);};
}
QJsonObject stream_identity(const domain::NumericProcessingGraph& graph,domain::NodeId node,const domain::ProcessingPort& port) {
    const auto direction=port.direction==domain::PortDirection::Input?QStringLiteral("input"):QStringLiteral("output");
    const auto graph_id=QString::fromStdString(graph.graph_id);
    const auto identity=QString::fromLatin1(QJsonDocument(QJsonArray{graph_id,node,direction,port.id}).toJson(QJsonDocument::Compact).toHex());
    return {{"id",identity},{"graph_id",graph_id},{"node_id",node},{"direction",direction},
        {"port_index",port.id},{"payload_version",int(bdsp_protocol::data_version)}};
}
std::optional<QJsonArray> registry_streams(const domain::NumericProcessingGraph& graph) {
    QJsonArray streams;
    for(const auto& node:graph.nodes)for(const auto* ports:{&node.inputs,&node.outputs})for(const auto& port:*ports) {
        if(streams.size()==aggregator_capabilities::MaxStreams)return {};
        streams.append(stream_identity(graph,node.id,port));
    }
    return streams;
}
class CliRuntime {
    QCoreApplication& app;
    const QCommandLineParser& parser;
    const LaunchOptions options;
    CliProject project;
    QJsonObject network,document;
    bool remote=false,applied=false,submitted=false;
    quint64 advertised_revision=std::numeric_limits<quint64>::max(),advertised_epoch=0;
    // Routing belongs to the engine and may use discovery during teardown.
    // The CONTROL server must be destroyed before its referenced engine.
    std::unique_ptr<AggregatorClient> registry;
    std::unique_ptr<pipeline::EnginePort> engine;
    std::unique_ptr<pipeline::NumericControlServer> server;
    QTimer exit_timer;
    bool create_engine() {
        network=project.runtime_config;QString error;
        if(!pipeline::apply_serial_options(parser,network,error)){qCritical()<<error;return false;}
        const auto serial=pipeline::read_serial_binding(network);
        remote=parser.isSet("remote") || serial.configured;
        if(serial.configured && parser.isSet("remote")){qCritical()<<"Select one explicit transport";return false;}
        if(serial.configured)engine=std::make_unique<pipeline::NumericRemoteEndpoint>(serial.endpoint);
        else if(parser.isSet("remote"))engine=std::make_unique<pipeline::NumericRemoteEndpoint>(parser.value("remote"),options.port);
        else engine=std::make_unique<pipeline::NumericEngine>(capture_factory(parser.isSet("synthetic")));
        return true;
    }
    bool create_server() {
        if(!parser.isSet("server"))return true;
        auto* local=dynamic_cast<pipeline::NumericEngine*>(engine.get());if(!local)return false;
        server=std::make_unique<pipeline::NumericControlServer>(*local);
        if(server->listen(QHostAddress(parser.value("listen")),options.port,parser.isSet("allow-mutations")))return true;
        qCritical()<<server->error_string();return false;
    }
    bool synchronize_capture() {
        if(remote || parser.isSet("synthetic"))return true;
        const auto* local=dynamic_cast<pipeline::NumericEngine*>(engine.get());
        if(!local || !local->capabilities().numeric)return false;
        QString error;
        if(pipeline::resolve_native_capture_graph(project.graph,local->capabilities().numeric->runtime.admission,capture_devices(project.adapter_graph),&error))return true;
        qCritical()<<error;return false;
    }
    bool prepare_document() {
        QByteArray bytes;const auto encoded=desktop::encode_numeric_graph(project.graph,bytes);
        if(!encoded){qCritical()<<encoded.error;return false;}
        document=QJsonDocument::fromJson(bytes).object();merge_adapters(document,project.adapter_graph);
        return true;
    }
    bool create_registry() {
        if(!parser.isSet("registry-host"))return true;
        bool valid=false;const auto port=parser.value("registry-port").toUInt(&valid);
        const QHostAddress address(parser.value("registry-host"));
        const auto uuid=parser.isSet("uuid")?QUuid(parser.value("uuid")):QUuid::createUuid();
        if(!valid || !port || port>65535 || address.isNull() || uuid.isNull() || remote)return false;
        registry=std::make_unique<AggregatorClient>(AggregatorClientConfig{uuid,ComponentType::core,server?server->port():quint16(0)});
        registry->connect_to_aggregator_server(address,quint16(port));return true;
    }
    void bind_routing() {
        auto* local=dynamic_cast<pipeline::NumericEngine*>(engine.get());if(!local)return;
        auto owner=std::make_unique<pipeline::NumericRoutingAdapter>(*local,registry.get());auto* routing=owner.get();
        local->set_adapter_handlers(std::move(owner),{
            [routing](const QJsonObject& graph,QString* error){return routing->validate_graph_routes(graph,error);},
            [routing](const QJsonObject& graph){routing->configure_graph_routes(graph);},
            [routing](const QJsonObject& config,QString* error){return routing->configure(config,error);},
            [routing]{return routing->diagnostics().value("error").toString();}});
    }
    bool configure_network() {
        bind_routing();QString error;
        if(engine->configure_network(network,&error))return true;
        qCritical()<<error;return false;
    }
    QJsonObject registry_metadata(const pipeline::EngineSnapshot& snapshot) const {
        return {{"control_version",int(bdsp_protocol::control_version)},
            {"data_version",int(bdsp_protocol::data_version)},{"stream_port",server?server->port():0},
            {"publisher_incarnation",QString::number(snapshot.epoch)},{"graph_revision",QString::number(snapshot.graph_revision)},
            {"allow_mutations",parser.isSet("allow-mutations")},{"streams",QJsonArray{}}};
    }
    bool publish_registry(QJsonObject metadata,const domain::NumericProcessingGraph& graph) {
        const auto streams=registry_streams(graph);
        if(streams){metadata["streams"]=*streams;if(registry->set_capabilities(metadata))return true;}
        metadata["streams"]=QJsonArray{};
        if(!registry->set_capabilities(metadata)){qWarning()<<"Cannot publish current discovery identity";return false;}
        qWarning()<<"Discovery stream limits exceeded; publishing current identity without stream bindings";
        return true;
    }
    void advertise(const pipeline::EngineSnapshot& snapshot) {
        if(!registry || !snapshot.epoch || (snapshot.graph_revision==advertised_revision && snapshot.epoch==advertised_epoch))return;
        domain::NumericProcessingGraph graph;
        if(!desktop::decode_numeric_graph(QJsonDocument(snapshot.graph).toJson(QJsonDocument::Compact),graph))return;
        if(!publish_registry(registry_metadata(snapshot),graph))return;
        advertised_revision=snapshot.graph_revision;advertised_epoch=snapshot.epoch;
    }
    void submit_graph() {
        pipeline::EngineCommand command;command.kind=pipeline::EngineCommand::ApplyGraph;command.graph=document;
        engine->submit(std::move(command));
    }
    void receive_snapshot(const pipeline::EngineSnapshot& snapshot) {
        advertise(snapshot);
        if(remote && snapshot.epoch && !submitted){submitted=true;submit_graph();}
    }
    void receive_reply(const pipeline::EngineReply& reply) {
        if(!reply.accepted){std::fprintf(stderr,"%s\n",reply.error.toUtf8().constData());app.exit(3);return;}
        if(!applied && reply.snapshot.graph_revision)applied=true;
    }
    void connect_callbacks() {
        engine->set_reply_handler([this](const auto& reply){receive_reply(reply);});
        engine->set_snapshot_handler([this](const auto& snapshot){receive_snapshot(snapshot);});
        if(!options.duration)return;
        exit_timer.setSingleShot(true);
        QObject::connect(&exit_timer,&QTimer::timeout,&exit_timer,[this]{if(applied)app.quit();else app.exit(3);});
        exit_timer.start(options.duration);
    }
public:
    CliRuntime(QCoreApplication& application,const QCommandLineParser& arguments,LaunchOptions launch,CliProject source)
        :app(application),parser(arguments),options(launch),project(std::move(source)){}
    int run() {
        if(!create_engine() || !create_server())return 2;
        if(!synchronize_capture())return 3;
        if(!prepare_document() || !create_registry() || !configure_network())return 2;
        connect_callbacks();
        if(remote)engine->connect_remote();else submit_graph();
        return app.exec();
    }
};
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);app.setApplicationName("pipeline_cli");
    QCommandLineParser parser;project_options(parser);network_options(parser);parser.process(app);
    LaunchOptions options;
    if(!read_launch_options(parser,options))return 2;
    CliProject project;
    if(!project.load(parser) || !configure_analysis(parser,project.graph))return 2;
    CliRuntime runtime(app,parser,options,std::move(project));return runtime.run();
}
