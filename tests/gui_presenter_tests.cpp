#include <core_app/gui_presenter.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/numeric_default_graph.h>
#include <QApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include <cstdio>
#include <stdexcept>
using namespace pipeline;
static void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
struct Port final:EnginePort {
 EngineSnapshot state;std::vector<EngineCommand> commands;std::vector<EndpointIdentity> endpoints;
 std::function<void(EngineSnapshot)> snapshots;std::function<void(EngineReply)> replies;
 quint64 next=0;int connections=0,previewHz=0;
 void set_numeric_preview_cadence_hz(int hz)override{previewHz=hz;}
 quint64 submit(EngineCommand c)override{commands.push_back(std::move(c));return ++next;}
 void set_snapshot_handler(std::function<void(EngineSnapshot)> f)override{snapshots=std::move(f);}
 void set_reply_handler(std::function<void(EngineReply)> f)override{replies=std::move(f);}
 EngineSnapshot snapshot()const override{return state;}
 bool configure_network(QJsonObject,QString*)override{return true;}
 QString network_warning()const override{return {};}
 void connect_remote()override{++connections;}void disconnect_remote()override{}
 bool subscribe_numeric_frames(std::vector<EndpointIdentity> e,QString*)override{endpoints=std::move(e);return true;}
 PortDeliveryMode port_delivery_mode()const noexcept override{return PortDeliveryMode::CanonicalNumericPortsV2;}
 void reply(quint64 id,bool accepted=true){if(accepted&&commands[id-1].kind==EngineCommand::ApplyGraph){state.graph=commands[id-1].graph;++state.graph_revision;if(snapshots)snapshots(state);}if(replies)replies({id,accepted,accepted?QString{}:QStringLiteral("Rejected fixture"),state});}
};
struct Provider final:EngineProvider {
 std::vector<Port*> ports;
 std::unique_ptr<EnginePort> create(const QString&,quint16)override{auto p=std::make_unique<Port>();p->state.epoch=ports.size()+1;ports.push_back(p.get());return p;}
};
int main(int argc,char**argv){QApplication app(argc,argv);try{
 GuiState state;state.delegate_model_registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
 for(const auto& kind:numeric_editor::catalog())state.delegate_model_registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
 auto model=std::make_shared<NumericEditorGraphModel>(state.delegate_model_registry);state.data_flow_graph=model;require(model->set_numeric_graph(numeric_default_graph()),"editor fixture");
 auto provider=std::make_shared<Provider>();GuiPresenter presenter(state,provider);auto* local=provider->ports[0];
 require(local->previewHz==60,"default preview cadence absent");
 for(const int fps:{90,120}){presenter.set_numeric_preview_cadence_hz(fps);require(local->previewHz==fps&&local->commands.empty(),"preview cadence mutates processing commands or loses high rate");}
 presenter.initialize(QJsonObject{});require(local->commands.size()==1&&local->commands[0].kind==EngineCommand::Snapshot,"initialize implicit Apply/Run");
 int statusChanges=0,telemetryChanges=0;
 QObject::connect(&presenter,&GuiPresenter::component_status_changed,&app,[&](QString,int,QString){++statusChanges;});
 QObject::connect(&presenter,&GuiPresenter::processing_state_changed,&app,[&]{++telemetryChanges;});
 local->state.statuses["capture"]={State::Stopped,{},0};
 local->snapshots(local->state);local->snapshots(local->state);
 require(statusChanges==1&&telemetryChanges==2,"data delivery repeats control status or suppresses diagnostics");
 local->state.statuses["capture"].revision++;local->snapshots(local->state);
 require(statusChanges==1,"revision-only snapshot repeats component status");
 local->state.statuses["capture"].error="device unavailable";local->snapshots(local->state);
 require(statusChanges==2,"error-only status change lost");
 ++local->state.epoch;local->snapshots(local->state);
 require(statusChanges==3,"new incarnation initial status lost");
 QJsonObject adapter{{"stream_id","first"}};
 presenter.set_graph_adapter_provider([&]{return QJsonObject{{"graph_id","main"},{"nodes",QJsonArray{QJsonObject{{"id",1},{"adapter",adapter}}}}};});
 int finished=0,rejected=0,clears=0;QObject::connect(&presenter,&GuiPresenter::graph_apply_finished,&app,[&](quint64,bool ok,QString){++finished;rejected+=!ok;});QObject::connect(&presenter,&GuiPresenter::views_cleared,&app,[&]{++clears;});
 require(presenter.apply_pipeline_graph({})&&presenter.graph_request_pending(),"Apply absent");
 adapter["stream_id"]="second";presenter.apply_pipeline_graph({});adapter["stream_id"]="third";presenter.apply_pipeline_graph({});adapter["stream_id"]="not-submitted";
 require(local->commands.size()==2&&rejected==1,"queued Apply not coalesced");require(local->commands[1].graph["nodes"].toArray()[0].toObject()["adapter"].toObject()["stream_id"]=="first","pending adapter not owning");
 local->reply(2);require(local->commands.size()==3&&local->commands[2].expected_revision==1,"queued Apply stale revision");require(local->commands[2].graph["nodes"].toArray()[0].toObject()["adapter"].toObject()["stream_id"]=="third","queued adapter changed");
 local->reply(3,false);require(!presenter.graph_request_pending()&&finished==3&&rejected==2&&presenter.applied_numeric_graph(),"rejection lost accepted graph");require(local->commands.size()==3,"rejected Apply starts producer");
 presenter.apply_pipeline_graph({});local->reply(4);require(local->commands.size()==4&&local->commands.back().kind==EngineCommand::ApplyGraph,"Apply emitted a separate lifecycle command");
 const EndpointIdentity pcm{"main",9,domain::PortDirection::Input,0},history{"main",7,domain::PortDirection::Output,0};
 presenter.set_numeric_endpoints({pcm});presenter.set_editor_numeric_endpoints({pcm,history});require(local->endpoints.size()==2,"subscription union");presenter.set_numeric_endpoints({});require(local->endpoints.size()==2,"view hide removes editor subscription");presenter.set_editor_numeric_endpoints({});require(local->endpoints.empty(),"hidden subscriptions retained");
 state.file_configs.scene_graph=model->save();state.file_configs.ui.pipeline_presets=QJsonArray{QJsonObject{{"id","local-preset"}}};state.file_configs.raw_document["devices"]=QJsonObject{{"local",QJsonObject{{"unknown",42}}}};
 auto stale=local->snapshots;auto late=local->replies;presenter.apply_pipeline_graph({});const auto oldrequest=local->next;presenter.select_device("remote","127.0.0.1",8213);auto* remote=provider->ports[1];const auto afterSwitch=clears;
 require(remote->connections==1&&!presenter.graph_request_pending()&&state.file_configs.ui.pipeline_presets.isEmpty(),"device selection leaks state or auto applies");auto staleState=local->state;staleState.epoch=900;stale(staleState);late({oldrequest,true,{},staleState});require(clears==afterSwitch&&presenter.is_remote(),"stale device callback accepted");
 require(remote->previewHz==120,"new endpoint lost preview cadence");
 presenter.select_device("local",{},0);require(provider->ports.size()==2&&state.file_configs.ui.pipeline_presets.size()==1,"local engine/presets not retained");require(state.file_configs.raw_document["devices"].toObject()["local"].toObject()["unknown"].toInt()==42,"device extension overwritten");
 require(local->previewHz==120,"reactivated endpoint lost preview cadence");
 QTemporaryDir directory;require(directory.isValid(),"CLI isolation");const auto executable=QCoreApplication::applicationDirPath()+"/core_app.exe";
 auto cli=[&](QStringList args,int code,QByteArray expected,bool error){QProcess p;p.setWorkingDirectory(directory.path());p.start(executable,args);require(p.waitForStarted(3000),"GUI CLI start");if(!p.waitForFinished(3000)){p.kill();p.waitForFinished();throw std::runtime_error("GUI CLI blocked");}require(p.exitStatus()==QProcess::NormalExit&&p.exitCode()==code,"GUI CLI exit");require((error?p.readAllStandardError():p.readAllStandardOutput()).contains(expected),"GUI CLI diagnostic");require(!QFile::exists(directory.filePath("config.json")),"GUI CLI creates config");};
 cli({"--help"},0,"--remote-host",false);cli({"--version"},0,"1.0.0",false);cli({"--remote-port","invalid"},2,"Invalid --remote-port",true);cli({"--remote-port","65536"},2,"Invalid --remote-port",true);cli({"--unknown-option"},2,"Unknown option",true);
 std::puts("canonical presenter ordering/device/subscription/CLI PASS");return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
