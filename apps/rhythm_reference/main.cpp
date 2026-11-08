// SPDX-License-Identifier: AGPL-3.0-or-later
#include "export_graphs.h"
#include "value_probe.h"
#include "fourier_profile.h"
#include "research_profile.h"
#include "offline_run.h"
#include <pipeline/numeric_engine.h>
#include <control/numeric_control_server.h>
#include <control/numeric_remote_support.h>
#include <desktop/numeric_project_document.h>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QDebug>
#include <QTimer>
#include <algorithm>
#include <limits>
#include <optional>

namespace {
void admission_ledger(const NumericExampleGraph& example,const dataflow_runtime::NumericRuntimeProfile& profile) {
    auto capabilities=dataflow_runtime::PreparedGraph::numeric_capabilities(profile);
    const auto unlimited=std::numeric_limits<std::size_t>::max();
    capabilities.profile.maximum={unlimited,unlimited,unlimited,unlimited};capabilities.profile.max_peak_bytes=unlimited;
    std::vector<std::size_t> workspace(example.graph.nodes.size()*2);domain::ProcessingGraphAdmission ledger;
    const auto result=domain::admit_processing_graph(example.graph,profile.admission,capabilities,workspace,ledger);
    qInfo()<<"Research admission ledger"<<unsigned(result.error)<<"plan"<<qulonglong(ledger.resources.plan_bytes)
        <<"payload"<<qulonglong(ledger.resources.payload_bytes)<<"scratch"<<qulonglong(ledger.resources.scratch_bytes)
        <<"stack"<<qulonglong(ledger.resources.stack_bytes)<<"peak"<<qulonglong(ledger.peak_bytes)
        <<"plan_limit"<<qulonglong(profile.resources.maximum.plan_bytes)<<"peak_limit"<<qulonglong(profile.resources.max_peak_bytes);
}
pipeline::NumericEngine::ProfileDecorator profile_for(const NumericExampleGraph& example) {
    return [&example,fourier=research_fourier_backend(example.graph)](auto profile){
        auto result=research_profile(std::move(profile),fourier);admission_ledger(example,result);return result;};
}
void options(QCommandLineParser& parser) {
    add_whole_input_options(parser);
    parser.addHelpOption();parser.addOption({"export","Export fresh loadable research graph projects","directory"});
    parser.addOption({"preset","Library reference stable ID","id","aubio-reference"});
    parser.addOption({"project","Load a versioned graph project instead of a built-in recipe","path"});
    parser.addOption({"duration","Bounded run in milliseconds","ms","15000"});
    parser.addOption({"port","Loopback CONTROL/DATA port (0 disables)","port","0"});
    parser.addOption({"synthetic","Use internal changing tones without speakers"});
    parser.addOption({"observe","Require an available nonempty output and report its value","node:port"});
}
pipeline::AudioSourceFactory capture_factory(bool synthetic) {
    if(!synthetic)return {};
    auto source=pipeline::default_audio_source_factory();
    return [source=std::move(source)](auto){return source(pipeline::AudioSourceKind::TestSignal);};
}
bool apply(pipeline::NumericEngine& engine,const NumericExampleGraph& example,QCoreApplication& app,bool& applied) {
    QByteArray bytes;const auto encoded=desktop::encode_numeric_graph(example.graph,bytes);
    if(!encoded){qCritical()<<encoded.error;return false;}
    engine.set_reply_handler([&app,&applied](const pipeline::EngineReply& reply){applied=reply.accepted;if(!applied){qCritical()<<reply.error;app.exit(3);}});
    pipeline::EngineCommand command;command.kind=pipeline::EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(bytes).object();
    engine.submit(std::move(command));return true;
}
void finish(QCoreApplication& app,pipeline::NumericEngine& engine,const ValueProbe& probe,bool applied) {
    qInfo().noquote()<<QJsonDocument(engine.snapshot().diagnostics).toJson(QJsonDocument::Compact);
    qInfo().noquote()<<"Observed output"<<QJsonDocument(probe.report()).toJson(QJsonDocument::Compact);
    app.exit(applied?(probe.satisfied()?0:4):3);
}
void control_ledger(pipeline::NumericEngine& engine,const NumericExampleGraph& example) {
    const auto capabilities=engine.capabilities();auto limits=pipeline::remote_numeric::control_limits();
    control::numeric::GraphProjection ordinary,research;
    const auto first=ordinary.assign(example.graph,limits);
    limits.frame=capabilities.numeric->runtime.admission.frame;
    const auto second=research.assign(example.graph,limits);
    qInfo()<<"Research CONTROL projection"<<unsigned(first.status)<<"profile projection"<<unsigned(second.status);
}
struct RunSettings {int duration=0,port=0;};
std::optional<RunSettings> run_settings(const QCommandLineParser& parser) {
    bool duration_ok=false,port_ok=false;const auto duration=parser.value("duration").toInt(&duration_ok),port=parser.value("port").toInt(&port_ok);
    if(!duration_ok || duration<1 || !port_ok || port<0 || port>65535)return {};
    return RunSettings{duration,port};
}
int run(QCoreApplication& app,const QCommandLineParser& parser,const NumericExampleGraph& example) {
    const auto settings=run_settings(parser);if(!settings)return 2;
    pipeline::NumericEngine engine(capture_factory(parser.isSet("synthetic")),nullptr,profile_for(example));
    control_ledger(engine,example);
    ValueProbe probe(engine,example.graph,parser.value("observe"));
    bool applied=false;if(!apply(engine,example,app,applied))return 3;
    pipeline::NumericControlServer server(engine);
    if(settings->port && !server.listen(QHostAddress::LocalHost,quint16(settings->port),true)){qCritical()<<server.error_string();return 3;}
    QTimer::singleShot(settings->duration,&app,[&]{finish(app,engine,probe,applied);});
    qInfo()<<"Research graph"<<example.id<<"port"<<server.port()<<"duration_ms"<<settings->duration;
    return app.exec();
}
int run_project(QCoreApplication& app,const QCommandLineParser& parser) {
    desktop::NumericProjectDocument document;domain::NumericProcessingGraph graph;
    const auto loaded=desktop::load_numeric_project(parser.value("project"),document,graph);
    if(!loaded){qCritical()<<loaded.path<<loaded.error;return 2;}
    const auto id=graph.graph_id;
    const NumericExampleGraph example{id.c_str(),id.c_str(),std::move(graph)};
    return run(app,parser,example);
}
}
namespace {
int run_builtin(QCoreApplication& app,const QCommandLineParser& parser) {
    const auto presets=numeric_library_reference_presets();
    if(parser.isSet("export"))return export_reference_graphs(parser.value("export"),presets)?0:2;
    const auto selected=parser.value("preset").toStdString();
    const auto matches=[&](const auto& example){return selected==example.id;};
    const auto preset=std::find_if(presets.begin(),presets.end(),matches);
    if(preset!=presets.end())return run(app,parser,*preset);
    const auto variants=numeric_library_reference_examples();
    const auto variant=std::find_if(variants.begin(),variants.end(),matches);
    return variant==variants.end()?2:run(app,parser,*variant);
}
}

int main(int argc,char** argv) {
    qputenv("QT_FORCE_STDERR_LOGGING","1");
    QCoreApplication app(argc,argv);app.setApplicationName("rhythm_reference_runner");
    QCommandLineParser parser;options(parser);parser.process(app);
    try {
        if(parser.isSet("whole-input"))return run_whole_input(whole_input_arguments(parser));
        if(parser.isSet("input") || parser.isSet("output") || parser.isSet("packet-samples"))return 2;
        if(parser.isSet("project"))return run_project(app,parser);
        return run_builtin(app,parser);
    }catch(const std::exception& error){qCritical()<<error.what();return 2;}
}
