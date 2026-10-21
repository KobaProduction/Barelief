#include <QApplication>
#include <QLabel>
#include <QTimer>
#include <QJsonDocument>
#include <core_app/gui_presenter.h>
#include <core_app/numeric_view_adapter.h>
#include "numeric_render_fixture.h"
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <desktop/numeric_project_document.h>
#include <cstdio>
using namespace pipeline;
using namespace signal_contracts;
struct FakePort final:EnginePort {
 EngineSnapshot state;std::function<void(EngineSnapshot)> snapshots;std::function<void(DeliveredNumericFrame)> frames;std::function<void(DeliveredNumericHistoryRow)> histories;
 quint64 submit(EngineCommand)override{return 1;}
 void set_snapshot_handler(std::function<void(EngineSnapshot)> f)override{snapshots=std::move(f);}
 void set_reply_handler(std::function<void(EngineReply)>)override{}
 EngineSnapshot snapshot()const override{return state;}
 bool configure_network(QJsonObject,QString*)override{return true;}
 QString network_warning()const override{return {};}
 void connect_remote()override{}void disconnect_remote()override{}
 PortDeliveryMode port_delivery_mode()const noexcept override{return PortDeliveryMode::CanonicalNumericPortsV2;}
 bool subscribe_numeric_frames(std::vector<EndpointIdentity>,QString*)override{return true;}
 void set_numeric_frame_handler(std::function<void(DeliveredNumericFrame)> f)override{frames=std::move(f);}
 void set_numeric_history_handler(std::function<void(DeliveredNumericHistoryRow)> f)override{histories=std::move(f);}
 void publish(EngineSnapshot value){state=value;QTimer::singleShot(0,QCoreApplication::instance(),[this,value]{if(snapshots)snapshots(value);});QCoreApplication::processEvents();}
 void publish(DeliveredNumericFrame value){QTimer::singleShot(0,QCoreApplication::instance(),[this,value]{if(frames)frames(value);});QCoreApplication::processEvents();}
 void publish(DeliveredNumericHistoryRow value){QTimer::singleShot(0,QCoreApplication::instance(),[this,value]{if(histories)histories(value);});QCoreApplication::processEvents();}
};
struct Provider final:EngineProvider{FakePort* port=nullptr;std::unique_ptr<EnginePort> create(const QString&,quint16)override{auto result=std::make_unique<FakePort>();port=result.get();return result;}};
static void frame_incarnation(GuiPresenter& presenter,auto& provider,auto& snapshot,const QByteArray& bytes,int& clears,auto& label,const auto& frame,const auto& check) {
 snapshot.graph=QJsonDocument::fromJson(bytes).object();snapshot.graph_revision=1;snapshot.epoch=90;provider->port->publish(snapshot);provider->port->publish(frame(90,90));check(label.text().contains("90"),"initial selected incarnation renders");
 const auto oldClears=clears;snapshot.epoch=10;provider->port->publish(snapshot);provider->port->publish(frame(10,10));
 check(clears>oldClears,"same selected graph lower service incarnation resets presentation");check(label.text().contains("10") && !label.text().contains("90"),"restarted service lower epoch frame renders current value");
 snapshot.epoch=100;provider->port->publish(snapshot);provider->port->publish(frame(100,100));
 const auto beforeRetire=clears;provider->port->publish(EngineSnapshot{});
 check(clears>beforeRetire && !presenter.applied_numeric_graph(),"observed empty retirement resets owners and applied graph");
 snapshot.epoch=5;provider->port->publish(snapshot);provider->port->publish(frame(5,5));check(label.text().contains("5") && !label.text().contains("100"),"observed retirement then lower incarnation renders");
 snapshot.epoch=100;provider->port->publish(snapshot);provider->port->publish(frame(100,100));
 const auto beforeCoalesced=clears;provider->port->state={};snapshot.epoch=1;provider->port->state=snapshot;
 provider->port->publish(frame(1,1));check(clears>beforeCoalesced && label.text().contains("1") && !label.text().contains("100"),"coalesced retirement first frame before snapshot notification resets fences");
}
static void history_incarnation(auto& app,auto& provider,auto& presenter,auto& snapshot,auto& context,int& clears,auto& label,const auto& frame,const auto& check) {
 snapshot.epoch=100;provider->port->publish(snapshot);const auto beforeHistory=clears;int histories=0;bool historyAfterClear=false;
 const auto connection=QObject::connect(&presenter,&GuiPresenter::numeric_history_ready,&app,[&](const auto&){++histories;historyAfterClear=clears>beforeHistory;});
 provider->port->state={};snapshot.epoch=2;provider->port->state=snapshot;DeliveredNumericHistoryRow row;row.compact.context=context;row.compact.delivery={1,2};provider->port->publish(row);
 check(histories==1 && historyAfterClear,"coalesced retirement first History callback synchronizes incarnation before delivery");
 provider->port->publish(frame(2,20));const auto beforeCompatible=clears;snapshot.graph_revision=2;provider->port->publish(snapshot);provider->port->publish(frame(2,30,2,2));
 check(clears==beforeCompatible && label.text().contains("30"),"same incarnation compatible revision does not clear presentation and accepts current frame");
 QObject::disconnect(connection);
}
int main(int argc,char**argv){
 QApplication app(argc,argv);int failures=0;auto check=[&](bool b,const char* s){std::printf("%s %s\n",b?"PASS":"FAIL",s);failures+=!b;};
 GuiState state;auto provider=std::make_shared<Provider>();GuiPresenter presenter(state,provider);
 domain::NumericProcessingGraph graph;graph.graph_id="main";auto node=numeric_editor::default_node("ScalarView");node.id=12;
 check(numeric_editor::set_dtype(node,domain::PortDirection::Input,0,DType::UInt64),"contract fixture");graph.nodes.push_back(node);
 QByteArray bytes;check(bool(desktop::encode_numeric_graph(graph,bytes)),"canonical same graph fixture");
 auto context=std::make_shared<NumericDeliveryContext>();context->endpoint={"main",12,domain::PortDirection::Input,0};context->contract=node.inputs[0].contract;
 presentation::NumericViewAdapter adapter({8,{8,8,4096,{true,64}},{true,128}});adapter.bind(context->endpoint);QLabel label;render_fixture::Target target{nullptr,nullptr,&label,true};
 int clears=0;QObject::connect(&presenter,&GuiPresenter::views_cleared,&app,[&]{++clears;adapter.clear();});
 QObject::connect(&presenter,&GuiPresenter::numeric_frame_ready,&app,[&](const auto& frame){adapter.accept(frame);adapter.render(context->endpoint,0,target);});
 std::array<std::byte,8192> payload{},metadata{};std::array<BufferSlot,16> ps{},ms{};BufferPool p(payload,ps,512),m(metadata,ms,512);std::array<PlaneLayout,1> planes{{{0,{8,8}}}};
 auto frame=[&](std::uint64_t epoch,std::uint64_t value,std::uint64_t revision=1,std::uint64_t sequence=1){auto contract=std::get<NumericContract>(context->contract.view());NumericView v{&contract,std::as_bytes(std::span(&value,1)),planes,{1,1}};FrameStamp stamp;stamp.epoch=epoch;stamp.graph_revision=revision;stamp.producer_epoch=1;stamp.sequence=sequence;DeliveredNumericFrame out;check(retain_numeric_delivery(context,{revision,epoch},{stamp,v},Status::ok,{2,4096,32768,8192,128,4096},p,m,out)==Status::ok,"own frame fixture");return out;};
 EngineSnapshot snapshot;frame_incarnation(presenter,provider,snapshot,bytes,clears,label,frame,check);
 history_incarnation(app,provider,presenter,snapshot,context,clears,label,frame,check);
 std::printf("failures=%d\n",failures);return failures?1:0;
}
