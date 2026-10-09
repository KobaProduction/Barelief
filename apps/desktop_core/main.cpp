#include <pipeline/numeric_engine.h>
#include <control/numeric_remote_endpoint.h>
#include <control/numeric_control_server.h>
#include <control/numeric_routing_adapter.h>
#include <core_app/numeric_default_graph.h>
#include <desktop/numeric_project_document.h>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>
#include <cstdio>
#include <numeric>
#include <pipeline/serial_config.h>
#include <aggregator/aggregator_client.h>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);app.setApplicationName("pipeline_cli");
    QCommandLineParser parser;parser.addHelpOption();
    parser.addOption({"config","Canonical numeric project","file"});
    parser.addOption({"preset","Numeric preset stable ID or unique name","id/name"});
    parser.addOption({"duration","Exit after milliseconds (0: continuous)","ms","0"});
    parser.addOption({"synthetic","Use eight silent changing tones instead of physical capture"});
    parser.addOption({"analysis-interval-ms","Analysis interval in milliseconds (1..60000)","ms"});
    parser.addOption({"server","Enable the numeric CONTROL/DATA server"});
    parser.addOption({"listen","Explicit TCP listen address","address","127.0.0.1"});
    parser.addOption({"port","Numeric endpoint TCP port","port","8213"});
    parser.addOption({"allow-mutations","Allow remote processing commands"});
    parser.addOption({"remote","Connect to a numeric endpoint","host"});
    parser.addOption({"registry-host","Explicit discovery registry address","address"});
    parser.addOption({"registry-port","Discovery registry port","port","8212"});
    parser.addOption({"uuid","Stable registry identity","uuid"});
    pipeline::add_serial_options(parser);parser.process(app);
    bool duration_ok=false,port_ok=false;
    const auto duration=parser.value("duration").toInt(&duration_ok);
    const auto port=parser.value("port").toUInt(&port_ok);
    if(!duration_ok || duration<0 || !port_ok || !port || port>65535)return 2;
    domain::NumericProcessingGraph graph=numeric_default_graph();
    QJsonObject runtime_config,adapter_graph;
    if(parser.isSet("config")) {
        QFile file(parser.value("config"));
        if(!file.open(QIODevice::ReadOnly) || file.size()>16777216){qCritical()<<"Cannot read bounded numeric project";return 2;}
        const auto source_bytes=file.readAll();
        desktop::NumericProjectDocument source;auto result=desktop::read_numeric_project(source_bytes,source);
        if(result)result=desktop::read_numeric_draft(source,graph);
        if(!result){qCritical()<<result.path<<result.error;return 2;}
        runtime_config=QJsonDocument::fromJson(source_bytes).object();
        adapter_graph=runtime_config.value("scene_graph").toObject();
        if(parser.isSet("preset")) {
            QJsonObject settings;result=desktop::read_numeric_application_settings(source,settings);
            const auto presets=settings["ui"].toObject()["pipeline_presets"].toArray();
            const auto selected=parser.value("preset");QJsonObject match;int count=0;
            for(const auto& value:presets){const auto p=value.toObject();if(p["id"].toString()==selected){match=p;++count;}}
            if(!count)for(const auto& value:presets){const auto p=value.toObject();if(p["name"].toString()==selected){match=p;++count;}}
            if(count!=1){qCritical()<<"Preset identity must resolve once";return 2;}
            adapter_graph=match["graph"].toObject();
            result=desktop::decode_numeric_editor_graph(QJsonDocument(match["graph"].toObject()).toJson(QJsonDocument::Compact),graph);
            if(!result){qCritical()<<result.error;return 2;}
        }
    } else if(parser.isSet("preset")){qCritical()<<"Preset requires a canonical project";return 2;}
    if(parser.isSet("analysis-interval-ms")) {
        bool interval_ok=false;
        const auto interval=parser.value("analysis-interval-ms").toLongLong(&interval_ok);
        if(!interval_ok || interval<1 || interval>60000){qCritical()<<"Invalid analysis interval";return 2;}
        const auto divisor=std::gcd(interval,qlonglong{1000});
        graph.execution_interval={interval/divisor,std::uint64_t(1000/divisor)};
    }
    pipeline::AudioSourceFactory capture;
    if(parser.isSet("synthetic")){auto physical=pipeline::default_audio_source_factory();capture=[physical](auto){return physical(pipeline::AudioSourceKind::TestSignal);};}
    std::unique_ptr<pipeline::EnginePort> engine;
    QJsonObject network=runtime_config;QString serial_error;
    if(!pipeline::apply_serial_options(parser,network,serial_error)){qCritical()<<serial_error;return 2;}
    const auto serial=pipeline::read_serial_binding(network);
    const bool remote=parser.isSet("remote") || serial.configured;
    if(serial.configured && parser.isSet("remote")){qCritical()<<"Select one explicit transport";return 2;}
    if(serial.configured)engine=std::make_unique<pipeline::NumericRemoteEndpoint>(serial.endpoint);
    else if(parser.isSet("remote"))engine=std::make_unique<pipeline::NumericRemoteEndpoint>(parser.value("remote"),quint16(port));
    else engine=std::make_unique<pipeline::NumericEngine>(capture);
    std::unique_ptr<pipeline::NumericControlServer> server;
    if(parser.isSet("server")) {
        auto* local=dynamic_cast<pipeline::NumericEngine*>(engine.get());if(!local)return 2;
        server=std::make_unique<pipeline::NumericControlServer>(*local);
        if(!server->listen(QHostAddress(parser.value("listen")),quint16(port),parser.isSet("allow-mutations"))){qCritical()<<server->error_string();return 2;}
    }
    QByteArray bytes;auto encoded=desktop::encode_numeric_graph(graph,bytes);
    if(!encoded){qCritical()<<encoded.error;return 2;}
    auto document=QJsonDocument::fromJson(bytes).object();
    QMap<int,QJsonObject> adapters;
    for(const auto& value:adapter_graph.value("nodes").toArray()) {
        const auto node=value.toObject();
        const auto adapter=node.contains("adapter")?node.value("adapter"):node.value("internal-data").toObject().value("adapter");
        if(adapter.isObject())adapters.insert(node.value("id").toInt(),adapter.toObject());
    }
    auto nodes=document.value("nodes").toArray();
    for(qsizetype i=0;i<nodes.size();++i){auto node=nodes[i].toObject();if(adapters.contains(node.value("id").toInt()))node["adapter"]=adapters.value(node.value("id").toInt());nodes[i]=node;}
    document["nodes"]=nodes;
    bool applied=false,submitted=false;
    std::unique_ptr<AggregatorClient> registry;
    if(parser.isSet("registry-host")) {
        bool valid=false;const auto registry_port=parser.value("registry-port").toUInt(&valid);
        const QHostAddress address(parser.value("registry-host"));
        const auto uuid=parser.isSet("uuid")?QUuid(parser.value("uuid")):QUuid::createUuid();
        if(!valid || !registry_port || registry_port>65535 || address.isNull() || uuid.isNull() || remote)return 2;
        registry=std::make_unique<AggregatorClient>(AggregatorClientConfig{uuid,ComponentType::core,server?server->port():quint16(0)});
        registry->connect_to_aggregator_server(address,quint16(registry_port));
    }
    if(auto* local=dynamic_cast<pipeline::NumericEngine*>(engine.get())) {
        auto owner=std::make_unique<pipeline::NumericRoutingAdapter>(*local,registry.get());
        auto* routing=owner.get();
        local->set_adapter_handlers(std::move(owner),{
            [routing](const QJsonObject& graph,QString* error){return routing->validate_graph_routes(graph,error);},
            [routing](const QJsonObject& graph){routing->configure_graph_routes(graph);},
            [routing](const QJsonObject& config,QString* error){return routing->configure(config,error);},
            [routing]{return routing->diagnostics().value("error").toString();}});
    }
    QString network_error;
    if(!engine->configure_network(network,&network_error)){qCritical()<<network_error;return 2;}
    quint64 advertised_revision=std::numeric_limits<quint64>::max();
    engine->set_reply_handler([&](const auto& reply) {
        if(!reply.accepted){std::fprintf(stderr,"%s\n",reply.error.toUtf8().constData());app.exit(3);return;}
        if(!applied && reply.snapshot.graph_revision)applied=true;
    });
    engine->set_snapshot_handler([&](const auto& snapshot){
        if(registry && snapshot.graph_revision!=advertised_revision) {
            domain::NumericProcessingGraph current;
            if(desktop::decode_numeric_graph(QJsonDocument(snapshot.graph).toJson(QJsonDocument::Compact),current)) {
                QJsonArray streams;
                for(const auto& node:current.nodes)for(const auto* ports:{&node.inputs,&node.outputs})for(const auto& p:*ports) {
                    const auto direction=p.direction==domain::PortDirection::Input?QStringLiteral("input"):QStringLiteral("output");
                    const auto identity=QString::fromLatin1(QJsonDocument(QJsonArray{QString::fromStdString(current.graph_id),node.id,direction,p.id}).toJson(QJsonDocument::Compact).toHex());
                    streams.append(QJsonObject{{"id",identity},{"graph_id",QString::fromStdString(current.graph_id)},{"node_id",node.id},
                        {"direction",direction},{"port_index",p.id},{"payload_version",4}});
                }
                if(streams.size()<=128)registry->set_capabilities({{"control_version",2},{"data_version",4},{"stream_port",server?server->port():0},
                    {"publisher_incarnation",QString::number(snapshot.epoch)},{"graph_revision",QString::number(snapshot.graph_revision)},
                    {"allow_mutations",parser.isSet("allow-mutations")},{"streams",streams}});
                advertised_revision=snapshot.graph_revision;
            }
        }
        if(remote && snapshot.epoch && !submitted){submitted=true;pipeline::EngineCommand apply;apply.kind=pipeline::EngineCommand::ApplyGraph;apply.graph=document;engine->submit(apply);}
    });
    if(remote)engine->connect_remote();
    else {pipeline::EngineCommand apply;apply.kind=pipeline::EngineCommand::ApplyGraph;apply.graph=document;engine->submit(apply);}
    if(duration)QTimer::singleShot(duration,&app,[&]{
        if(!applied){app.exit(3);return;}
        app.quit();
    });
    return app.exec();
}
