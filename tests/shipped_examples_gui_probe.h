#pragma once
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/graph_panel.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/scheme_editor/editor_geometry.h>
#include <QGraphicsView>
#include <QMouseEvent>
#include <desktop/numeric_project_document.h>
#include <visualization/ui_text.h>
#include <visualization/abstract/abs.h>
#include <visualization/spectrograms/spectrogram_graph.h>
#include <QFile>
#include <QJsonDocument>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QAction>
#include <QStatusBar>
#include <QComboBox>
#include <QLineEdit>
#include <QElapsedTimer>
#include <QThread>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>

inline bool measure_shipped_gui(CoreApplication& app,MainWindow* window,const char* label) {
    using namespace pipeline;using namespace signal_contracts;
    auto& presenter=app.presenter();
    auto wait=[](const auto& predicate){QElapsedTimer timer;timer.start();while(timer.elapsed()<5000){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(2);}return predicate();};
    auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
    auto* source=model?model->delegateModel<AudioNodeModel>(1):nullptr;
    std::unique_ptr<QWidget> diagnostics(source?source->preview_widget():nullptr);
    auto* card=diagnostics.get();
    auto* values=card?card->findChild<QLabel*>("previewValues"):nullptr;
    auto* first=card?card->findChild<QLabel*>("previewExactValue"):nullptr;
    if(!card||!values||!first)return false;
    struct Stage {unsigned count=0;double first=0,last=0;bool positive=false,changed=false;};
    std::array<Stage,12> stages{};std::uint64_t scalar=0,ordinal=0;bool bad=false;
    QObject witness;std::uint64_t revision=0,epoch=0;
    domain::NumericProcessingGraph expected;
    auto observe=[&](const DeliveredNumericFrame& frame,unsigned id){
        if(!frame.context||!frame.frame||!frame.frame.view().stamp.available||frame.delivery.graph_revision!=revision||frame.delivery.epoch!=epoch)return;
        const auto view=frame.frame.view();const auto* value=std::get_if<NumericView>(&view.payload);if(!value){bad=true;return;}
        if(id==12||(id==7&&frame.context->endpoint.port==1)){std::uint64_t x=0;bad|=load_element(*value,{0,0,0},x)!=Status::ok;scalar=std::max(scalar,x);return;}
        if(value->contract->dtype!=DType::Float32){bad=true;return;}double energy=0;
        if(id==7||id==8){if(value->contract->semantic!=Semantic::MelEnergy||value->actual[0]!=50){bad=true;return;}}
        else {const auto& port=id<=6?expected.nodes[id-1].outputs[0]:expected.nodes[id-1].inputs[0];
            const auto contract=std::get<NumericContract>(port.contract.view());
            if(!same_contract(contract,*value->contract)||value->actual[0]!=contract.shape.extents[0].maximum){bad=true;return;}}
        for(std::size_t p=0;p<value->planes.size();++p)for(std::size_t i=0;i<value->actual[0];++i){float x=0;if(load_element(*value,{p,i,0},x)!=Status::ok||!std::isfinite(x)){bad=true;return;}energy+=double(x)*x;}
        auto& stage=stages[id];if(!stage.count)stage.first=energy;++stage.count;stage.last=energy;stage.positive|=energy>1e-12;stage.changed|=std::abs(energy-stage.first)>1e-10;
    };
    QObject::connect(&presenter,&GuiPresenter::numeric_frame_ready,&witness,[&](const auto& frame){
        if(!frame.context)return;const auto id=frame.context->endpoint.node;
        if(id<1||id>12||id==8||(id==7&&frame.context->endpoint.port==0))return;
        if(frame.context->endpoint.direction!=(id<=7?PortDirection::Output:PortDirection::Input))return;observe(frame,unsigned(id));
    });
    QObject::connect(&presenter,&GuiPresenter::numeric_history_ready,&witness,[&](const auto& row){
        if(row.event!=NumericHistoryEvent::Row||!row.compact.context)return;const auto endpoint=row.compact.context->endpoint;
        if(!((endpoint.node==7&&endpoint.direction==PortDirection::Output)||(endpoint.node==8&&endpoint.direction==PortDirection::Input)))return;
        if(row.compact.delivery.graph_revision!=revision||row.compact.delivery.epoch!=epoch)return;
        bad|=row.capacity!=400||row.filled_count!=std::min(row.accepted_row_ordinal,std::uint64_t{400});ordinal=std::max(ordinal,row.accepted_row_ordinal);observe(row.compact,unsigned(endpoint.node));
    });
    const auto before=presenter.client().snapshot().graph_revision;presenter.apply_pipeline_graph({});
    if(!wait([&]{return presenter.client().snapshot().graph_revision>before&&presenter.client().snapshot().statuses.value("analyzer").state==State::Running;}))return false;
    revision=presenter.client().snapshot().graph_revision;epoch=presenter.client().snapshot().epoch;
    if(!presenter.applied_numeric_graph())return false;expected=*presenter.applied_numeric_graph();
    std::vector<EndpointIdentity> endpoints;
    for(int id=1;id<=7;++id)endpoints.push_back({"main",id,PortDirection::Output,0});
    endpoints.push_back({"main",7,PortDirection::Output,1});
    for(int id=8;id<=12;++id)endpoints.push_back({"main",id,PortDirection::Input,0});
    presenter.set_editor_numeric_endpoints(std::move(endpoints));
    const bool delivered=wait([&]{if(bad)return true;if(scalar<5||ordinal<5)return false;for(unsigned id=1;id<=11;++id)if(stages[id].count<5||!stages[id].positive||!stages[id].changed)return false;return card->property("previewActivity").toString().isEmpty()&&values->text().toULongLong()>0&&first->text()!=QStringLiteral("—");});
    presenter.set_editor_numeric_endpoints({});
    bool good=delivered&&!bad&&scalar>=5&&ordinal>=5&&presenter.client().snapshot().statuses.value("analyzer").state==State::Running;
    std::printf("GUI %s own engine epoch=%llu revision=%llu History ordinal=%llu count=%llu",label,
        static_cast<unsigned long long>(epoch),static_cast<unsigned long long>(revision),static_cast<unsigned long long>(ordinal),static_cast<unsigned long long>(scalar));
    for(unsigned id=1;id<=11;++id){const auto& s=stages[id];std::printf(" stage%u=%u/%.9g/%.9g",id,s.count,s.first,s.last);good&=s.count>=5&&s.positive&&s.changed;}
    auto* validation=window->findChild<QLabel*>("pipelineValidation");good&=validation&&validation->toolTip().isEmpty();
    std::printf(" %s\n",good?"PASS":"FAIL");return good;
}

inline bool verify_startup_selector(CoreApplication& app,MainWindow* window,bool unavailable) {
    using namespace pipeline;using namespace signal_contracts;
    auto& presenter=app.presenter();
    auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
    auto* source=model?model->delegateModel<AudioNodeModel>(1):nullptr;
    auto* selector=model?model->delegateModel<AudioNodeModel>(14):nullptr;
    std::unique_ptr<QWidget> sourceDiagnostics(source?source->preview_widget():nullptr);
    auto* card=sourceDiagnostics.get();
    auto* values=card?card->findChild<QLabel*>("previewValues"):nullptr;
    auto* first=card?card->findChild<QLabel*>("previewExactValue"):nullptr;
    if(!selector||!card||!values||!first||window->findChild<QPushButton*>("pipelineRun")||window->findChild<QPushButton*>("pipelineStop")||window->findChild<QCheckBox*>("pipelineAutoApply"))return false;
    auto* selectorBody=selector->embeddedWidget();
    auto* editor=window->findChild<SchemeEditorWidget*>();
    if(!editor||selector->selected_input()!=0||selectorBody->findChild<QWidget*>("pipelineDataStatus"))return false;
    auto click_input=[&](domain::PortId id){
        for(auto* view:editor->findChildren<QGraphicsView*>()){
            auto* scene=dynamic_cast<QtNodes::BasicGraphicsScene*>(view->scene());if(!scene)continue;
            auto* node=scene->nodeGraphicsObject(14);auto* geometry=dynamic_cast<EditorNodeGeometry*>(&scene->nodeGeometry());
            if(!node || !geometry)continue;const auto port=selector->port_index(QtNodes::PortType::In,id);if(port==QtNodes::InvalidPortIndex)return false;
            geometry->recomputeSize(14);view->centerOn(node->mapToScene(geometry->selectorButtonRect(14,port).center()));
            // Growing a selector rebuilds its embedded form asynchronously.
            // Layout delivery can change node height and hence every pin centre;
            // derive the click from the final geometry after that delivery.
            QCoreApplication::processEvents();geometry->recomputeSize(14);
            const auto point=node->mapToScene(geometry->selectorButtonRect(14,port).center());
            view->centerOn(point);const auto local=view->mapFromScene(point);const QPointF global=view->viewport()->mapToGlobal(local);
            QMouseEvent press(QEvent::MouseButtonPress,QPointF(local),global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease,QPointF(local),global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
            QApplication::sendEvent(view->viewport(),&press);QApplication::sendEvent(view->viewport(),&release);
            return selector->selected_input()==id;
        }
        return false;
    };
    std::unique_ptr<QWidget> selectorDiagnostics(selector->preview_widget());
    QObject witness;bool bad=false;int selected=0;std::uint64_t revision=0,epoch=0,ordinal=0,history_sequence=0;
    std::uint64_t transition_after=presenter.client().snapshot().graph_revision;
    unsigned generator_frames=0,capture_frames=0,selected_frames=0,unavailable_frames=0,fft_frames=0;
    auto* fftDock=window->workspaceDock("fftDock");if(!fftDock)return false;fftDock->raise();
    int phase=0;
    struct Diagnostic {std::function<void()> print;~Diagnostic(){print();}} diagnostic{[&]{
        const auto snapshot=presenter.client().snapshot();
        std::fprintf(stderr,"selector diagnostic phase=%d selected=%d bad=%d unavailable=%d rev=%llu current=%llu state=%d gen=%u cap=%u chosen=%u unavailable_frames=%u ordinal=%llu seq=%llu cardActivity=%s values=%s first=%s apply=%s\n",phase,selected,int(bad),int(unavailable),static_cast<unsigned long long>(revision),static_cast<unsigned long long>(snapshot.graph_revision),int(snapshot.statuses.value("analyzer").state),generator_frames,capture_frames,selected_frames,unavailable_frames,static_cast<unsigned long long>(ordinal),static_cast<unsigned long long>(history_sequence),qPrintable(card->property("previewActivity").toString()),qPrintable(values->text()),qPrintable(first->text()),qPrintable(presenter.apply_status()));
    }};
    auto wait=[](const auto& predicate){QElapsedTimer timer;timer.start();while(timer.elapsed()<6000){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(2);}return predicate();};
    QObject::connect(&presenter,&GuiPresenter::numeric_frame_ready,&witness,[&](const DeliveredNumericFrame& frame){
        const auto current=presenter.client().snapshot();
        if(!frame.context||!frame.frame||frame.delivery.graph_revision!=current.graph_revision||frame.delivery.epoch!=current.epoch||
           (revision?frame.delivery.graph_revision!=revision:frame.delivery.graph_revision<=transition_after))return;
        const auto endpoint=frame.context->endpoint;
        if(endpoint.node==10 && endpoint.direction==PortDirection::Input && endpoint.port==0) {
            const auto view=frame.frame.view();const auto* numeric=std::get_if<NumericView>(&view.payload);
            if(view.stamp.available && numeric && numeric->actual[0]>0) {
                double energy=0;for(std::size_t i=0;i<numeric->actual[0];++i){float x=0;if(load_element(*numeric,{0,i,0},x)!=Status::ok||!std::isfinite(x)){bad=true;return;}energy+=double(x)*x;}
                if(energy>0)++fft_frames;
            }
            return;
        }
        if(endpoint.direction!=PortDirection::Output||endpoint.port!=0)return;
        if(endpoint.node!=1&&endpoint.node!=13&&endpoint.node!=14)return;
        const auto view=frame.frame.view();
        if(!view.stamp.available){if(endpoint.node==14)++unavailable_frames;return;}
        const auto* numeric=std::get_if<NumericView>(&view.payload);
        if(!numeric||numeric->contract->semantic!=Semantic::PCM||numeric->contract->dtype!=DType::Float32||numeric->planes.size()!=2||numeric->actual[0]!=256){bad=true;return;}
        double energy=0;for(std::size_t p=0;p<2;++p)for(std::size_t i=0;i<256;++i){float x=0;if(load_element(*numeric,{p,i,0},x)!=Status::ok||!std::isfinite(x)){bad=true;return;}energy+=double(x)*x;}
        if(endpoint.node==1)++generator_frames;
        if(endpoint.node==13)++capture_frames;
        if(endpoint.node==14){++selected_frames;bad|=energy<=0||(selected==0?energy<=0.002:energy>=0.002);}
    });
    QObject::connect(&presenter,&GuiPresenter::numeric_history_ready,&witness,[&](const auto& row){
        const auto current=presenter.client().snapshot();
        if(row.event!=NumericHistoryEvent::Row||!row.compact.context||row.compact.context->endpoint.node!=7||row.compact.context->endpoint.direction!=PortDirection::Output||row.compact.delivery.graph_revision!=current.graph_revision||row.compact.delivery.epoch!=current.epoch||
           (revision?row.compact.delivery.graph_revision!=revision:row.compact.delivery.graph_revision<=transition_after))return;
        bad|=row.accepted_row_ordinal<=ordinal||row.filled_count!=std::min(row.accepted_row_ordinal,std::uint64_t{400})||row.capacity!=400;
        const auto sequence=row.compact.frame.view().stamp.sequence;bad|=sequence<=history_sequence;
        ordinal=row.accepted_row_ordinal;history_sequence=sequence;
    });
    if(!wait([&]{return !presenter.graph_request_pending()&&presenter.client().snapshot().statuses.value("analyzer").state==State::Running;}))return false;
    auto subscribe=[&]{revision=presenter.client().snapshot().graph_revision;epoch=presenter.client().snapshot().epoch;};
    subscribe();
    phase=1;
    if(!wait([&]{return bad||(selected_frames>=5&&generator_frames>=5&&ordinal>=5&&(!unavailable?capture_frames>=5:true)&&card->property("previewActivity").toString().isEmpty()&&values->text().toULongLong()>0&&first->text()!=QStringLiteral("—"));})||bad)return false;
    const auto original_edges=model->numeric_graph().edges;
    const auto source_before=generator_frames;
    int transition=0;
    for(int choice:{1,0,1,0}) {
        phase=choice==1?2:3;
        const auto prior_fft=fft_frames;const auto prior_revision=revision,prior_ordinal=ordinal;const auto prior_generator=generator_frames,prior_capture=capture_frames;
        transition_after=prior_revision;revision=0;selected=choice;selected_frames=0;unavailable_frames=0;if(transition==1)editor->history().undo();else if(transition==2)editor->history().redo();else if(!click_input(domain::PortId(choice)))return false;
        ++transition;
        if(!wait([&]{return presenter.client().snapshot().graph_revision>prior_revision&&!presenter.graph_request_pending();}))return false;
        subscribe();
        const auto switched=model->numeric_graph();
        if(!std::is_permutation(original_edges.begin(),original_edges.end(),switched.edges.begin(),switched.edges.end()))return false;
        if(unavailable&&choice==1){if(!wait([&]{return bad||(unavailable_frames>0&&generator_frames>prior_generator+5);})||bad||selected_frames!=0)return false;const auto capture=presenter.client().snapshot().statuses.value("capture");
            if(capture.state!=State::Failed || !capture.error.contains("Injected loopback unavailable"))return false;
            auto* panel=qobject_cast<GraphPanel*>(fftDock->widget());auto* chart=panel?panel->active_chart():nullptr;
            if(!chart || !wait([&]{return chart->value_count()==0;}))return false;
        }
        else if(!wait([&]{return bad||(selected_frames>=5&&fft_frames>prior_fft&&ordinal>prior_ordinal&&generator_frames>prior_generator+5&&(unavailable||capture_frames>prior_capture+5));})||bad)return false;
        if(presenter.client().snapshot().statuses.value("analyzer").state!=State::Running)return false;
        if(!unavailable || choice==0) {
            auto* panel=qobject_cast<GraphPanel*>(fftDock->widget());auto* chart=panel?panel->active_chart():nullptr;
            if(!chart || !wait([&]{return chart->value_count()>0 && chart->peak_value()>0;}))return false;
            auto* selectorCard=selectorDiagnostics.get();
            auto* selectorValues=selectorCard->findChild<QLabel*>("previewValues");
            auto* selectorFirst=selectorCard->findChild<QLabel*>("previewExactValue");
            if(!wait([&]{return selectorCard->property("previewActivity").toString().isEmpty() && selectorValues->text().toULongLong()>0 && selectorFirst->text()!=QStringLiteral("—");}))return false;
        }
    }
    if(generator_frames<=source_before+10)return false;
    if(!unavailable) {
        phase=4;
        auto* capture=model->delegateModel<AudioNodeModel>(13);
        std::unique_ptr<QWidget> captureDiagnostics(capture?capture->preview_widget():nullptr);
        auto* captureCard=captureDiagnostics.get();
        auto* captureValues=captureCard?captureCard->findChild<QLabel*>("previewValues"):nullptr;
        auto* captureFirst=captureCard?captureCard->findChild<QLabel*>("previewExactValue"):nullptr;
        if(!captureCard||!captureValues||!captureFirst)return false;
        model->set_capture_device_provider([](int,QString*){return QJsonArray{QJsonObject{{"id","fixture-loopback"},{"name","Silent fixture loopback"},{"is_default",false}}};});
        std::unique_ptr<QWidget> captureForm(capture->parameter_widget());auto* device=captureForm->findChild<QComboBox*>("captureDevice");
        if(!device)return false;const auto index=device->findData("fixture-loopback");if(index<0)return false;
        const auto prior_revision=revision,prior_ordinal=ordinal;const auto prior_generator=generator_frames;
        transition_after=prior_revision;revision=0;
        device->setCurrentIndex(index);
        phase=6;
        if(!wait([&]{return presenter.client().snapshot().graph_revision>prior_revision&&!presenter.graph_request_pending();}))return false;
        subscribe();
        if(!wait([&]{return captureCard->property("previewActivity").toString().isEmpty()&&captureValues->text().toULongLong()>0&&captureFirst->text()!=QStringLiteral("—")&&ordinal>prior_ordinal;}))return false;
        std::puts("captureDevice automatic live Apply/current values/History recovery PASS");
    }
    phase=7;
    const auto prior_revision=revision,prior_ordinal=ordinal;
    transition_after=prior_revision;revision=0;
    auto expanded=*selector->numeric_node();if(!numeric_editor::set_input_count(expanded,3))return false;
    phase=71;if(!model->edit_node(14,expanded))return false;
    phase=72;
    model->addConnection({1,0,14,2});QCoreApplication::processEvents();
    if(selector->nPorts(QtNodes::PortType::In)!=3 || !model->connectionExists({1,0,14,2}))return false;
    phase=73;if(!click_input(2))return false;selected=0;selected_frames=0;
    phase=74;
    if(!wait([&]{return presenter.client().snapshot().graph_revision>prior_revision&&!presenter.graph_request_pending();}))return false;
    phase=75;subscribe();if(!wait([&]{return bad || selected_frames>=5 && ordinal>prior_ordinal;})||bad)return false;
    // A newer edit made with a graph request in flight must win reconciliation.
    const auto before_pending=presenter.client().snapshot().graph_revision;
    phase=76;presenter.apply_pipeline_graph({});selector->select_input(0);
    if(!wait([&]{const auto active=presenter.applied_numeric_graph();if(!active||presenter.graph_request_pending()||presenter.client().snapshot().graph_revision<=before_pending)return false;
        for(const auto& node:active->nodes)if(node.id==14){domain::ResolvedProcessingNode resolved;domain::ProcessingParameterView value;
            return bool(domain::resolve_processing_node(node,numeric_editor::limits(),resolved))&&domain::resolve_processing_parameter(resolved,"selected_input",value)==Status::ok&&std::visit([](auto x){return x==0;},std::get<NumericLiteral>(value));}return false;
    }))return false;
    phase=77;subscribe();
    auto* surfaceDock=window->workspaceDock("spectrogramDock");if(!surfaceDock)return false;surfaceDock->raise();
    auto* surface=surfaceDock->findChild<SpectrogramGraph*>();if(!surface)return false;
    auto* surfacePanel=qobject_cast<GraphPanel*>(surfaceDock->widget());
    std::fprintf(stderr,"selector surface mode=%s active=%d hidden=%d exposed=%d frames=%zu bands=%zu revision=%llu endpoint=%d\n",
        surfacePanel?qPrintable(surfacePanel->display_mode()):"none",int(surfacePanel && surfacePanel->active_spectrogram()==surface),int(surfaceDock->isHidden()),int(surfaceDock->property("renderExposed").toBool()),
        surface->projected_history_frames(),surface->projected_history_bands(),static_cast<unsigned long long>(surface->history_projection_revision()),int(surfaceDock->property("numericEndpoint").isValid()));
    if(!surfacePanel || surfacePanel->active_spectrogram()!=surface)return false;
    phase=78;if(!wait([&]{return surface->projected_history_frames()>0 && surface->projected_history_bands()==50 && surface->history_projection_revision()>0;}))return false;
    float peak=0;
    for(std::size_t frame=0;frame<surface->projected_history_frames();++frame)for(std::size_t band=0;band<50;++band){float value=0;if(!surface->projected_history_sample(frame,band,value)||!std::isfinite(value))return false;peak=std::max(peak,value);}
    phase=79;if(!(peak>0))return false;
    phase=80;const auto projected=surface->history_projection_revision();if(!wait([&]{return surface->history_projection_revision()>projected;}))return false;
    std::printf("app-owned History to renderer projection frames=%zu bands=%zu peak=%.9g (pre-GL)\n",surface->projected_history_frames(),surface->projected_history_bands(),peak);
    std::printf("startup continuous N selector/live undo redo/injected unavailable=%d History ordinal=%llu sequence=%llu PASS\n",int(unavailable),static_cast<unsigned long long>(ordinal),static_cast<unsigned long long>(history_sequence));
    return presenter.client().snapshot().statuses.value("analyzer").state==State::Running && !bad;
}

inline bool verify_example_autosave(CoreApplication& app,MainWindow* window,const QString& path) {
    using namespace pipeline;
    auto& presenter=app.presenter();auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
    auto* autosave=window->findChild<QCheckBox*>("enableAutosave");auto* validation=window->findChild<QLabel*>("pipelineValidation");
    if(!model||!autosave||!validation)return false;
    auto wait=[](const auto& predicate){QElapsedTimer timer;timer.start();while(timer.elapsed()<5000){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(2);}return predicate();};
    auto encoded=[](auto graph){std::sort(graph.edges.begin(),graph.edges.end(),[](const auto& a,const auto& b){return std::tie(a.from,a.to,a.output_port,a.input_port)<std::tie(b.from,b.to,b.output_port,b.input_port);});QByteArray bytes;desktop::encode_numeric_graph(graph,bytes);return bytes;};
    autosave->setChecked(false);const auto revision=presenter.client().snapshot().graph_revision;
    auto* source=model->delegateModel<AudioNodeModel>(1);if(!source)return false;
    std::unique_ptr<QWidget> form(source->parameter_widget());auto* amplitude=form->findChild<QLineEdit*>("amplitude");if(!amplitude)return false;
    amplitude->setText("0.2");amplitude->setModified(true);QMetaObject::invokeMethod(amplitude,"editingFinished",Qt::DirectConnection);
    if(!wait([&]{return presenter.client().snapshot().graph_revision>revision&&!presenter.graph_request_pending();})||QFile::exists(path)){qWarning()<<"Autosave probe: live edit or disabled persistence";return false;}
    autosave->setChecked(true);desktop::NumericProjectDocument saved;domain::NumericProcessingGraph disk;std::optional<domain::NumericProcessingGraph> applied;
    // Enabling UI autosave persists preferences, leaving the existing document's
    // draft/applied snapshots intact until a project save is requested.
    if(!wait([&]{QJsonObject settings;return desktop::load_numeric_project(path,saved,disk)&&
        desktop::read_numeric_application_settings(saved,settings)&&settings["ui"].toObject()["autosave_enabled"].toBool();})){qWarning()<<"Autosave probe: preferences save"<<window->statusBar()->currentMessage()<<QFile::exists(path);return false;}
    if(encoded(disk)!=encoded(numeric_startup_graph()) || !desktop::read_numeric_applied(saved,applied) || applied || !app._ctx.editor_draft_modified){qWarning()<<"Autosave probe: snapshot boundary"<<disk.nodes.size()<<bool(applied)<<app._ctx.editor_draft_modified;return false;}
    window->activateWindow();window->setFocus();
    auto* save=window->findChild<QAction*>("saveSettings");
    if(!wait([&]{return save&&save->isEnabled();})){qWarning()<<"Autosave probe: Save context";return false;}
    save->trigger();
    if(!wait([&]{return desktop::load_numeric_project(path,saved,disk)&&desktop::read_numeric_applied(saved,applied)&&applied&&encoded(disk)==encoded(model->numeric_graph())&&encoded(*applied)==encoded(*presenter.applied_numeric_graph());})){qWarning()<<"Autosave probe: full Save snapshots";return false;}
    const auto working_revision=presenter.client().snapshot().graph_revision;
    model->setNodeData(1,QtNodes::NodeRole::Position,QPointF(713,421));
    if(!wait([&]{desktop::NumericDraftPresentation display;return desktop::load_numeric_project(path,saved,disk)&&desktop::read_numeric_presentation(saved,display)&&display.positions["1"].toObject()["x"].toDouble()==713;})){qWarning()<<"Autosave probe: position save";return false;}
    if(presenter.client().snapshot().graph_revision!=working_revision)return false;
    auto* node=model->delegateModel<AudioNodeModel>(2);if(!node)return false;
    const auto good=*node->numeric_node();auto invalid=good;std::get<domain::TransformProcessingParameters>(invalid.parameters).values={{"size",signal_contracts::NumericLiteral{std::uint64_t{63}}}};
    node->set_numeric_node(invalid);if(!wait([&]{return !validation->toolTip().isEmpty();}))return false;
    if(presenter.client().snapshot().graph_revision!=working_revision||presenter.client().snapshot().statuses.value("analyzer").state!=State::Running)return false;
    const auto savedDraft=encoded(disk);const auto savedApplied=encoded(*applied);
    auto* theme=window->findChild<QComboBox*>("interfaceTheme");if(!theme)return false;
    theme->setCurrentIndex(1);
    if(!wait([&]{QJsonObject settings;return desktop::load_numeric_project(path,saved,disk)&&
        desktop::read_numeric_application_settings(saved,settings)&&settings["ui"].toObject()["theme"]=="dark";}))return false;
    if(encoded(disk)!=savedDraft || !desktop::read_numeric_applied(saved,applied) || !applied || encoded(*applied)!=savedApplied)return false;
    if(presenter.client().snapshot().graph_revision!=working_revision)return false;
    node->set_numeric_node(good);
    std::puts("continuous live Apply independent of autosave; position-only save and rejected draft preserve runtime PASS");return true;
}
