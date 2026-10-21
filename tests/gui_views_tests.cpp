#include "numeric_gui_fixture.h"
#include <core/performance.h>
#include <core_app/numeric_view_adapter.h>
#include <core_app/graph_panel.h>
#include <barelief/charts/line.h>
#include <barelief/numeric_metrics_view.h>
#include <QPointer>
#include <QTimer>
#include <cstdio>
static QJsonObject current_node_positions(gui_fixture::App& app) {
    auto& model=*app.application->_ctx.data_flow_graph;
    QJsonObject positions;
    for(const auto ids=model.allNodeIds();const auto id:ids){
        const auto point=model.nodeData(id,QtNodes::NodeRole::Position).toPointF();
        positions[QString::number(id)]=QJsonObject{{"x",point.x()},{"y",point.y()}};
    }
    return positions;
}
static void load_view_draft(gui_fixture::App& app,const domain::NumericProcessingGraph& graph,const char* message) {
    const auto positions=current_node_positions(app);
    gui_fixture::check(app.editor->load_numeric_draft(graph),message);
    app.editor->restore_node_positions(positions);
}
static AbstractChartView* chart(WorkspaceDock* dock) {
    using namespace gui_fixture;
    auto* panel=qobject_cast<GraphPanel*>(dock->widget());
    auto* result=panel?panel->active_chart():nullptr;
    gui_fixture::check(result,"explicit active chart renderer");return result;
}
static void select_live_editor_preview(gui_fixture::App& app) {
    auto* scene=app.editor->findChild<QtNodes::DataFlowGraphicsScene*>();
    gui_fixture::check(scene && scene->nodeGraphicsObject(9),"live editor preview node");
    gui_fixture::check(app.editor->isVisible(),"editor preview demand requires visible editor");
    emit scene->nodeSelected(QtNodes::NodeId(9));
}
static void live_demand_lifecycle(gui_fixture::App& app) {
    using namespace gui_fixture;
        auto* rawDock=app.open(9,1);
        std::uint64_t liveFrames=0,totalFrames=0;QString liveError;QObject witness;
        const auto liveConnection=QObject::connect(&app.presenter(),&GuiPresenter::numeric_frame_ready,&witness,[&](const auto& frame){++totalFrames;if(frame.context && frame.context->endpoint.node==9 && frame.frame && frame.status==signal_contracts::Status::ok)++liveFrames;});
        const auto rejectedConnection=QObject::connect(&app.presenter(),&GuiPresenter::command_rejected,&witness,[&](quint64,const QString& error){liveError=error;});
        app.live_delivery->store(true,std::memory_order_release);const bool started=wait([&]{return liveFrames>=3;});
        if(!started)qWarning()<<"Run diagnostic"<<"live"<<liveFrames<<"total"<<totalFrames<<"error"<<liveError<<"state"<<int(app.presenter().client().snapshot().statuses.value("analyzer").state)<<"raw binding"<<rawDock->property("numericEndpoint");
        check(started,"silent actual Run delivery");
        select_live_editor_preview(app);
        const auto workspaceDocks=app.window->workspaceDocks();for(auto* dock:workspaceDocks)if(dock!=app.dock("dataFlowDock"))dock->toggleView(false);
        check(app.editor->isVisible(),"graph closure hid the editor preview demand");
        const auto hiddenFrames=liveFrames;check(wait([&]{return liveFrames>=hiddenFrames+3;}),"zero graph views stopped working processing/delivery");
        // Editor demand is independent. With both it and all graphs hidden,
        // presentation deliveries stop while the executor keeps its state.
        app.dock("dataFlowDock")->hide();
        wait([]{return false;},200);
        const auto noDemandFrames=totalFrames;
        wait([]{return false;},200);
        check(totalFrames==noDemandFrames,"hidden graphs retained transport preview demand");
        app.live_delivery->store(false,std::memory_order_release);
        auto* returning=rawDock;returning->toggleView(true);returning->raise();
        check(wait([&]{return chart(returning)->value_count()>0;}),"returning graph missed cold snapshot without new capture ingress");
        QObject::disconnect(liveConnection);QObject::disconnect(rejectedConnection);
}

static void restore_and_delete_endpoint(gui_fixture::App& app, WorkspaceDock* internal, WorkspaceDock* external) {
    using namespace gui_fixture;
        internal->toggleView(false);auto changed=app.graph;
        load_view_draft(app,changed,"restore chart contract draft");app.apply();
        auto* internalPanel=qobject_cast<GraphPanel*>(internal->widget());auto* externalPanel=qobject_cast<GraphPanel*>(external->widget());
        check(internalPanel && externalPanel,"universal panel after vector contract restoration");
        internalPanel->set_display_mode("bars");externalPanel->set_display_mode("bars");
        const auto timers=[&](WorkspaceDock* dock){return dock->widget()->findChildren<QTimer*>(QString{},Qt::FindDirectChildrenOnly);};
        check(!chart(internal)->updatesEnabled() && chart(external)->updatesEnabled(),"chart replacement exposure state");
        check(timers(internal).size()==1 && !timers(internal).constFirst()->isActive() && timers(external).size()==1 && timers(external).constFirst()->isActive(),"GraphPanel replacement information timer exposure");
        internal->toggleView(true);internal->raise();
        check(wait([&]{return chart(internal)->updatesEnabled() && timers(internal).constFirst()->isActive();}),"reopened chart timer did not resume");
        changed.nodes.erase(std::remove_if(changed.nodes.begin(),changed.nodes.end(),[](const auto& n){return n.id==10;}),changed.nodes.end());
        changed.edges.erase(std::remove_if(changed.edges.begin(),changed.edges.end(),[](const auto& e){return e.to==10 || e.from==10;}),changed.edges.end());
        changed.configuration_bindings.erase(std::remove_if(changed.configuration_bindings.begin(),changed.configuration_bindings.end(),[](const auto& binding){return binding.to==10 || binding.from==10;}),changed.configuration_bindings.end());
        load_view_draft(app,changed,"delete draft");app.apply();
        check(internal->property("numericEndpoint").toMap().isEmpty() && external->property("numericEndpoint").toMap().isEmpty(),"deleted endpoint remains bound");
        check(chart(external)->value_count()==0,"secondary deleted view retained stale values");
}

static void replace_live_contract(gui_fixture::App& app, WorkspaceDock* internal, WorkspaceDock* external) {
    using namespace gui_fixture;
        // A live contract change retains dock identities and layout, rebuilding
        // only the affected renderer and binding the new input contract.
        internal->toggleView(false);
        check(wait([&]{return !internal->property("renderExposed").toBool() && external->property("renderExposed").toBool();}),"hidden/exposed replacement precondition");
        QPointer<QWidget> priorInternal=internal->widget(),priorExternal=external->widget();
        auto changed=app.graph;auto node=std::find_if(changed.nodes.begin(),changed.nodes.end(),[](const auto& n){return n.id==10;});
        const auto historyNode=std::find_if(changed.nodes.begin(),changed.nodes.end(),[](const auto& n){return n.id==8;});
        *node=*historyNode;node->id=10;
        std::erase_if(changed.edges,[](const auto& edge){return edge.to==10;});
        changed.edges.push_back({7,10,0,0});
        std::erase_if(changed.configuration_bindings,[](const auto& binding){return binding.to==10;});
        load_view_draft(app,changed,"live typed contract draft");app.apply();
        auto* internalPanel=qobject_cast<GraphPanel*>(internal->widget());auto* externalPanel=qobject_cast<GraphPanel*>(external->widget());
        check(internalPanel && externalPanel,"rank change must retain universal renderer choices");
        internalPanel->set_display_mode("surface3d");externalPanel->set_display_mode("surface3d");
        check(internalPanel->active_spectrogram() && externalPanel->active_spectrogram(),"matrix explicitly selects surface mode");
        check(priorInternal.isNull() && priorExternal.isNull(),"replacement retained incompatible renderer");
        check(!internalPanel->active_spectrogram()->updatesEnabled() && externalPanel->active_spectrogram()->updatesEnabled(),"replacement lost hidden/exposed renderer state");
        check(external->isFloating() && internal->property("numericEndpoint").toMap()["direction"]=="input","contract change lost host/binding");
        internal->toggleView(true);internal->raise();
        check(wait([&]{return internal->property("renderExposed").toBool() && internalPanel->active_spectrogram()->updatesEnabled();}),"reopen replacement does not resume");
}

static void scalar_rank_and_ports(gui_fixture::App& app, auto& snapshot) {
    using namespace gui_fixture;
        auto* historyPort=app.open(8,1,0);auto* countPort=app.open(12,1,0);
        auto* historyPanel=qobject_cast<GraphPanel*>(historyPort->widget());auto* countPanel=qobject_cast<GraphPanel*>(countPort->widget());
        check(historyPanel && countPanel,"all numeric ranks use universal GraphPanel");
        const auto documentBeforeModes=app.editor->document();const auto revisionBeforeModes=app.presenter().client().snapshot().graph_revision;
        historyPanel->set_display_mode("surface3d");countPanel->set_display_mode("image2d");
        check(historyPanel->active_spectrogram() && countPanel->metrics_view() && !countPanel->image_view(),"matrix and scalar graph families enforce rank");
        countPanel->set_display_mode("points");
        check(countPanel->metrics_view() && !countPanel->active_chart(),"scalar graph cannot select a vector renderer");
        check(app.editor->document()==documentBeforeModes && app.presenter().client().snapshot().graph_revision==revisionBeforeModes,"renderer mode edited DSP schema or applied graph");
        check(historyPort->property("numericEndpoint").toMap()["node"].toInt()==8 && countPort->property("numericEndpoint").toMap()["node"].toInt()==12,"independent sink endpoints collide");
        auto countContext=context(app.graph,12,domain::PortDirection::Input,0);auto& presenter=app.presenter();
        emit presenter.numeric_frame_ready(scalar(countContext,{snapshot.graph_revision,snapshot.epoch},1,9007199254740993ULL));
        check(wait([&]{return countPanel->metrics_view()->exact_value(0)=="9007199254740993";}),"count port exact UInt64 caption");
        auto* metricNode=app.application->_ctx.data_flow_graph->delegateModel<AudioNodeModel>(12);
        check(metricNode && metricNode->add_scalar_input(),"add independent scalar metric port");
        check(metricNode->edit_scalar_input_name(1,"Second metric"),"name scalar metric port");app.apply();
        snapshot=app.presenter().client().snapshot();domain::NumericProcessingGraph metricGraph;
        check(app.editor->numeric_draft(metricGraph),"read metrics topology");
        auto secondContext=context(metricGraph,12,domain::PortDirection::Input,1);
        auto firstContext=context(metricGraph,12,domain::PortDirection::Input,0);
        emit presenter.numeric_frame_ready(scalar(firstContext,{snapshot.graph_revision,snapshot.epoch},2,9007199254740993ULL));
        emit presenter.numeric_frame_ready(scalar(secondContext,{snapshot.graph_revision,snapshot.epoch},2,18446744073709551615ULL));
        check(wait([&]{return countPanel->metrics_view()->exact_value(1)=="18446744073709551615" && countPanel->metrics_view()->exact_value(0)=="9007199254740993";}),"multiple exact scalar inputs are independently bound");
        check(!countPanel->scalar_inputs().isEmpty(),"graph sidebar metric input controls");
        const auto metricRevision=app.presenter().client().snapshot().graph_revision;
        metricNode=app.application->_ctx.data_flow_graph->delegateModel<AudioNodeModel>(12);
        check(metricNode && metricNode->set_scalar_input_enabled(1,false),"hide metric input");
        check(wait([&]{return countPanel->metrics_view()->input_count()==1;}),"hidden metric remains displayed");
        check(app.presenter().client().snapshot().graph_revision==metricRevision,"hiding metric reconfigures DSP");
        check(metricNode->set_scalar_input_enabled(1,true),"show metric input");
        check(wait([&]{return countPanel->metrics_view()->input_count()==2;}),"reenabled metric missing");
        metricNode=app.application->_ctx.data_flow_graph->delegateModel<AudioNodeModel>(12);
        check(metricNode && metricNode->remove_scalar_input(1),"remove metric port");app.apply();
}

static void terminal_and_latest_preview(gui_fixture::App& app,WorkspaceDock* internal,WorkspaceDock* external,WorkspaceDock* mel,const auto& snapshot,const auto& ctx,const auto& deliver) {
    using namespace gui_fixture;auto& presenter=app.presenter();
        pipeline::DeliveredNumericFrame marker;marker.context=ctx;marker.delivery={snapshot.graph_revision-1,snapshot.epoch};marker.status=signal_contracts::Status::unavailable;
        emit presenter.numeric_frame_ready(marker);QCoreApplication::processEvents();check(chart(external)->peak_value()==.75f,"stale marker cleared current view");
        // A live display must not replay a capture backlog at the GUI cadence.
        auto* pollingTimer=app.window->findChild<QTimer*>("numericRenderTimer");
        check(pollingTimer,"numeric polling fallback missing");pollingTimer->stop();
        for(std::uint64_t i=4;i<=100;++i)deliver(i,float(i)/100);
        check(wait([&]{return chart(external)->peak_value()==1.f;},150),"vector display replayed stale FIFO instead of latest preview");
        marker.delivery.graph_revision=snapshot.graph_revision;emit presenter.numeric_frame_ready(marker);
        check(wait([&]{return chart(internal)->value_count()==0 && chart(external)->value_count()==0;}),"unavailable clears all instances");
        pollingTimer->start();
        check(chart(mel)->peak_value()==3.5f,"unavailable leaked to another endpoint");
}
static void delivery_lifecycle(gui_fixture::App& app, WorkspaceDock* internal, WorkspaceDock* external, WorkspaceDock* mel, auto& snapshot, const auto& ctx, const auto& other, const auto& deliver) {
    using namespace gui_fixture;
        // Delivery to one endpoint must not reproject every visible graph.
        check(mel->property("renderExposed").toBool(),"isolation oracle requires exposed Mel");
        QElapsedTimer settled;settled.start();while(settled.elapsed()<80){QCoreApplication::processEvents();QThread::msleep(1);}
        auto& dataCounter=core::perf::counters[std::size_t(core::perf::Stage::ChartData)];
        const auto priorUpdates=dataCounter.count.load();core::perf::enabled=true;auto& presenter=app.presenter();
        emit presenter.numeric_frame_ready(floats(other,{snapshot.graph_revision,snapshot.epoch},2,3.5f));
        const bool changedEndpoint=wait([&]{return chart(mel)->peak_value()==3.5f;});core::perf::enabled=false;
        check(changedEndpoint,"isolated endpoint was not projected");
        check(dataCounter.count.load()-priorUpdates==1,"unrelated endpoint triggered chart reprojection");
        auto* savedRenderer=internal->widget();internal->toggleView(false);QCoreApplication::processEvents();deliver(2,.5f);
        check(wait([&]{return chart(external)->peak_value()==.5f;}),"external delivery while internal hidden");
        check(chart(internal)->peak_value()==.25f,"hidden renderer performed projection");internal->toggleView(true);internal->raise();
        const bool reopened=wait([&]{return chart(internal)->peak_value()==.5f;});
        if(!reopened)std::fprintf(stderr,"reopen diagnostic: closed=%d visible=%d exposed=%d enabled=%d count=%zu peak=%g endpoint=%d\n",internal->isClosed(),internal->isVisible(),internal->property("renderExposed").toBool(),chart(internal)->updatesEnabled(),std::size_t(chart(internal)->value_count()),double(chart(internal)->peak_value()),!internal->property("numericEndpoint").toMap().isEmpty());
        check(reopened,"reopen gets latest delivery");check(internal->widget()==savedRenderer,"reopen recreated renderer");
        app.apply();snapshot=app.presenter().client().snapshot();deliver(3,.75f);
        check(wait([&]{return chart(internal)->peak_value()==.75f && chart(external)->peak_value()==.75f;}),"compatible Apply fanout");
        terminal_and_latest_preview(app,internal,external,mel,snapshot,ctx,deliver);
}

int main(){
    try {
        using namespace gui_fixture;App app(numeric_test_graph(),{},{},true);app.apply();auto& presenter=app.presenter();
        auto* internal=app.open(10,2);auto* external=app.open(10,0);auto* mel=app.open(11,2);
        check(internal!=external && external->isFloating(),"independent representations");
        auto snapshot=app.presenter().client().snapshot();auto ctx=context(app.graph,10,domain::PortDirection::Input);auto other=context(app.graph,11,domain::PortDirection::Input);
        const auto deliver=[&](std::uint64_t sequence,float value){emit presenter.numeric_frame_ready(floats(ctx,{snapshot.graph_revision,snapshot.epoch},sequence,value));};
        deliver(1,.25f);emit presenter.numeric_frame_ready(floats(other,{snapshot.graph_revision,snapshot.epoch},1,3.f));
        check(wait([&]{return chart(internal)->peak_value()==.25f && chart(external)->peak_value()==.25f && chart(mel)->peak_value()==3.f;}),"endpoint fanout and independent nodes");
delivery_lifecycle(app,internal,external,mel,snapshot,ctx,other,deliver);
        scalar_rank_and_ports(app,snapshot);
        replace_live_contract(app,internal,external);
        restore_and_delete_endpoint(app,internal,external);
        live_demand_lifecycle(app);
                std::puts("Canonical GUI endpoint fanout, hidden/reopen, compatible Apply, unavailable and live renderer lifecycle PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"GUI views: %s\n",error.what());return 1;}
}
