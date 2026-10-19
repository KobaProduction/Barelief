#pragma once
#include <algorithm>
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
#include <barelief/ui_text.h>
#include <barelief/abstract/abs.h>
#include <barelief/spectrograms/spectrogram_graph.h>
#include <QFile>
#include <QJsonDocument>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QAction>
#include <QStatusBar>
#include <QComboBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QThread>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <memory>

inline bool measure_shipped_gui(CoreApplication& app,MainWindow* window,const char* label) {
    using namespace pipeline;using namespace signal_contracts;
    auto& presenter=app.presenter();
    auto wait=[](const auto& predicate){QElapsedTimer timer;timer.start();while(timer.elapsed()<6000){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(2);}return predicate();};
    const auto before=presenter.client().snapshot().graph_revision;presenter.apply_pipeline_graph({});
    if(!wait([&]{return presenter.client().snapshot().graph_revision>before && !presenter.graph_request_pending() && presenter.client().snapshot().statuses.value("analyzer").state==State::Running;}))return false;
    if(!presenter.applied_numeric_graph())return false;const auto expected=*presenter.applied_numeric_graph();
    const auto revision=presenter.client().snapshot().graph_revision,epoch=presenter.client().snapshot().epoch;
    struct Stage {EndpointIdentity endpoint;Rank rank;DType dtype;std::size_t width;unsigned count=0;std::uint64_t first=0,last=0;bool positive=false;std::uint64_t producer_epoch=0,generation=0;unsigned received=0,unavailable=0,exhausted=0;};
    std::vector<Stage> stages;std::vector<EndpointIdentity> endpoints;
    for(const auto& node:expected.nodes){
        const bool view=node.kind=="AudioView"||node.kind=="FFTView"||node.kind=="MelView"||node.kind=="SpectrogramView"||node.kind=="ScalarView";
        if(node.kind=="SampleClock"||node.kind=="PcmResampler"||(node.kind=="AudioSource" && node.id!=1))continue;
        const auto& ports=view?node.inputs:node.outputs;
        for(const auto& port:ports){if(!port.contract)continue;const auto contract=port.contract.view();const auto* numeric=std::get_if<NumericContract>(&contract);if(!numeric)continue;
            EndpointIdentity endpoint{expected.graph_id,node.id,view?domain::PortDirection::Input:domain::PortDirection::Output,port.id};
            stages.push_back({endpoint,numeric->shape.rank,numeric->dtype,numeric->shape.extents[numeric->shape.rank==Rank::Matrix?1:0].maximum});endpoints.push_back(endpoint);
        }
    }
    if(stages.empty())return false;QObject witness;bool bad=false;std::uint64_t ordinal=0;
    const auto observe=[&](const DeliveredNumericFrame& frame,bool compact){
        if(!frame.context||frame.delivery.graph_revision!=revision||frame.delivery.epoch!=epoch)return;
        auto stage=std::find_if(stages.begin(),stages.end(),[&](const auto& item){return item.endpoint==frame.context->endpoint;});if(stage==stages.end())return;
        if((stage->rank==Rank::Matrix)!=compact)return;
        ++stage->received;
        if(!frame.frame || !frame.frame.view().stamp.available){++stage->unavailable;if(frame.status==Status::exhausted)++stage->exhausted;return;}
        const auto fail=[&](const char* reason){
            if(!bad)std::fprintf(stderr,"GUI %s stage%d:%u invalid: %s producer=%llu/%llu sequence=%llu prior=%llu/%llu/%llu\n",label,int(stage->endpoint.node),unsigned(stage->endpoint.port),reason,
                (unsigned long long)frame.frame.view().stamp.producer_epoch,(unsigned long long)frame.frame.view().stamp.generation,(unsigned long long)frame.frame.view().stamp.sequence,
                (unsigned long long)stage->producer_epoch,(unsigned long long)stage->generation,(unsigned long long)stage->last);
            bad=true;
        };
        const auto view=frame.frame.view();const auto* numeric=std::get_if<NumericView>(&view.payload);if(!numeric||numeric->contract->dtype!=stage->dtype){fail("payload or dtype mismatch");return;}
        double energy=0;
        if(stage->dtype==DType::UInt64){std::uint64_t value=0;if(load_element(*numeric,{0,0,0},value)!=Status::ok){fail("UInt64 read");return;}energy=double(value);}
        else if(stage->dtype==DType::UInt32){std::uint32_t value=0;if(load_element(*numeric,{0,0,0},value)!=Status::ok){fail("UInt32 read");return;}energy=double(value);}
        else if(stage->dtype==DType::Float64){double value=0;if(load_element(*numeric,{0,0,0},value)!=Status::ok || !std::isfinite(value)){fail("Float64 read or finiteness");return;}energy=value*value;}
        else if(stage->dtype==DType::Float32){if(numeric->planes.size()!=1 || numeric->actual[0]==0 || numeric->actual[0]>stage->width || (stage->rank==Rank::Matrix && numeric->actual[0]!=stage->width)){fail("Float32 geometry");return;}for(std::size_t i=0;i<numeric->actual[0];++i){float value=0;if(load_element(*numeric,{0,i,0},value)!=Status::ok||!std::isfinite(value)){fail("Float32 read or finiteness");return;}energy+=double(value)*value;}}
        else {fail("unsupported primitive dtype");return;}
        // Sequence is producer-local. A source/generation transition can restart
        // it within the same applied graph; require five progressing frames
        // from the new producer instead of comparing unrelated sequences.
        if(stage->count && (view.stamp.producer_epoch!=stage->producer_epoch || view.stamp.generation!=stage->generation)){
            stage->count=0;stage->positive=false;
        }
        if(stage->count && view.stamp.sequence<stage->last){fail("sequence regressed within producer generation");return;}
        stage->producer_epoch=view.stamp.producer_epoch;stage->generation=view.stamp.generation;
        if(!stage->count)stage->first=view.stamp.sequence;stage->last=view.stamp.sequence;++stage->count;stage->positive|=energy>1e-12;
    };
    QObject::connect(&presenter,&GuiPresenter::numeric_frame_ready,&witness,[&](const auto& frame){observe(frame,false);});
    QObject::connect(&presenter,&GuiPresenter::numeric_history_ready,&witness,[&](const auto& row){if(row.event!=NumericHistoryEvent::Row || row.compact.delivery.graph_revision!=revision || row.compact.delivery.epoch!=epoch)return;if(row.capacity!=400 || row.filled_count!=std::min(row.accepted_row_ordinal,std::uint64_t{400})){if(!bad)std::fprintf(stderr,"GUI %s History invalid: node=%d capacity=%llu filled=%llu ordinal=%llu\n",label,row.compact.context?int(row.compact.context->endpoint.node):-1,(unsigned long long)row.capacity,(unsigned long long)row.filled_count,(unsigned long long)row.accepted_row_ordinal);bad=true;}ordinal=std::max(ordinal,row.accepted_row_ordinal);observe(row.compact,true);});
    struct RawWindowObservation {std::mutex mutex;unsigned available=0,unavailable=0,exhausted=0;std::uint64_t first=0,last=0;};
    auto rawWindow=std::make_shared<RawWindowObservation>();std::uint64_t rawWindowTap=0;
    if(std::any_of(stages.begin(),stages.end(),[](const auto& stage){return stage.endpoint.node==102 && stage.endpoint.direction==domain::PortDirection::Output && stage.endpoint.port==0;}))
        rawWindowTap=app.pipeline().subscribe_numeric_tap({{expected.graph_id,102,domain::PortDirection::Output,0}},[rawWindow,revision,epoch](const DeliveredNumericFrame& frame){
            if(frame.delivery.graph_revision!=revision || frame.delivery.epoch!=epoch)return;std::lock_guard lock(rawWindow->mutex);
            if(!frame.frame || !frame.frame.view().stamp.available){++rawWindow->unavailable;if(frame.status==Status::exhausted)++rawWindow->exhausted;return;}
            if(!rawWindow->available)rawWindow->first=frame.frame.view().stamp.sequence;rawWindow->last=frame.frame.view().stamp.sequence;++rawWindow->available;
        },{});
    struct TapGuard {NumericEngine& engine;std::uint64_t token;~TapGuard(){if(token)engine.unsubscribe_numeric_tap(token);}} tapGuard{app.pipeline(),rawWindowTap};
    presenter.set_editor_numeric_endpoints(std::move(endpoints));
    const bool has_history=std::any_of(expected.nodes.begin(),expected.nodes.end(),[](const auto& n){return n.kind=="Spectrogram";});
    const auto complete=[&]{for(const auto& stage:stages)if(stage.count<5 || stage.last<=stage.first || (stage.rank!=Rank::Scalar && !stage.positive))return false;return !has_history || ordinal>=5;};
    const bool delivered=wait([&]{return bad||complete();});presenter.set_editor_numeric_endpoints({});
    bool good=delivered&&!bad&&complete();
    std::printf("GUI %s own engine epoch=%llu revision=%llu History=%llu",label,(unsigned long long)epoch,(unsigned long long)revision,(unsigned long long)ordinal);
    for(const auto& stage:stages)std::printf(" stage%d:%u=%u/%llu(r%u,u%u,x%u,p%d)",int(stage.endpoint.node),unsigned(stage.endpoint.port),stage.count,(unsigned long long)stage.last,stage.received,stage.unavailable,stage.exhausted,int(stage.positive));
    if(rawWindowTap){std::lock_guard lock(rawWindow->mutex);std::printf(" raw102=%u/%llu..%llu(u%u,x%u)",rawWindow->available,(unsigned long long)rawWindow->first,(unsigned long long)rawWindow->last,rawWindow->unavailable,rawWindow->exhausted);}
    auto* validation=window->findChild<QLabel*>("pipelineValidation");good&=validation&&validation->toolTip().isEmpty();std::printf(" %s\n",good?"PASS":"FAIL");return good;
}

inline bool verify_startup_selector(CoreApplication& app,MainWindow* window,bool unavailable) {
    using namespace pipeline;using namespace signal_contracts;
    auto& presenter=app.presenter();
    auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
    if(!model)return false;
    auto* source=model?model->delegateModel<AudioNodeModel>(1):nullptr;
    auto* selector=model?model->delegateModel<AudioNodeModel>(200):nullptr;
    std::unique_ptr<QWidget> sourceDiagnostics(source?source->preview_widget():nullptr);
    auto* card=sourceDiagnostics.get();
    auto* values=card?card->findChild<QLabel*>("previewValues"):nullptr;
    auto* first=card?card->findChild<QLabel*>("previewExactValue"):nullptr;
    if(!selector||!card||!values||!first||window->findChild<QPushButton*>("pipelineRun")||window->findChild<QPushButton*>("pipelineStop")||window->findChild<QCheckBox*>("pipelineAutoApply"))return false;
    auto* selectorBody=selector->embeddedWidget();
    auto* editor=window->findChild<SchemeEditorWidget*>();
    if(!editor||selector->selected_input()!=0||selectorBody->findChild<QWidget*>("pipelineDataStatus"))return false;
    if(selector->numeric_node()->inputs.size()!=6 || domain::data_outputs(*selector->numeric_node()).size()!=2 || selector->grouped_channels(QtNodes::PortType::In)!=2)return false;
    auto* inspectorScene=editor->findChild<QtNodes::DataFlowGraphicsScene*>();if(!inspectorScene)return false;
    emit inspectorScene->nodeSelected(1);
    auto click_input=[&](domain::PortId id){
        for(auto* view:editor->findChildren<QGraphicsView*>()){
            auto* scene=dynamic_cast<QtNodes::BasicGraphicsScene*>(view->scene());if(!scene)continue;
            auto* node=scene->nodeGraphicsObject(200);auto* geometry=dynamic_cast<EditorNodeGeometry*>(&scene->nodeGeometry());
            if(!node || !geometry)continue;const auto port=selector->port_index(QtNodes::PortType::In,id*2);if(port==QtNodes::InvalidPortIndex)return false;
            geometry->recomputeSize(200);view->centerOn(node->mapToScene(geometry->selectorButtonRect(200,port).center()));
            // Growing a selector rebuilds its embedded form asynchronously.
            // Layout delivery can change node height and hence every pin centre;
            // derive the click from the final geometry after that delivery.
            QCoreApplication::processEvents();geometry->recomputeSize(200);
            const auto point=node->mapToScene(geometry->selectorButtonRect(200,port).center());
            view->centerOn(point);const auto local=view->mapFromScene(point);const QPointF global=view->viewport()->mapToGlobal(local);
            QMouseEvent press(QEvent::MouseButtonPress,QPointF(local),global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease,QPointF(local),global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
            QApplication::sendEvent(view->viewport(),&press);QApplication::sendEvent(view->viewport(),&release);
            if(selector->selected_input()!=id)std::fprintf(stderr,"selector click miss input=%u actual=%u local=%d,%d viewport=%dx%d items=%lld\n",unsigned(id),unsigned(selector->selected_input()),local.x(),local.y(),view->viewport()->width(),view->viewport()->height(),static_cast<long long>(view->items(local).size()));
            return selector->selected_input()==id;
        }
        return false;
    };
    std::unique_ptr<QWidget> selectorDiagnostics(selector->preview_widget());
    const auto liveCard=[](QWidget* widget,std::size_t outputs){
        const auto* count=widget?widget->findChild<QLabel*>("previewValues"):nullptr;
        const auto* first=widget?widget->findChild<QLabel*>("previewExactValue"):nullptr;
        if(!widget || !count || !first || !widget->property("previewActivity").toString().isEmpty())return false;
        const auto counts=count->text().split('\n'),values=first->text().split('\n');
        if(counts.size()!=qsizetype(outputs) || values.size()!=qsizetype(outputs))return false;
        for(qsizetype lane=0;lane<counts.size();++lane){
            bool countOk=false,valueOk=false;const auto n=counts[lane].toULongLong(&countOk);const auto v=values[lane].toDouble(&valueOk);
            if(!countOk || n==0 || !valueOk || !std::isfinite(v))return false;
        }
        return true;
    };
    QObject witness;bool bad=false;int selected=0;std::uint64_t revision=0,epoch=0,ordinal=0,history_sequence=0;
    std::uint64_t transition_after=presenter.client().snapshot().graph_revision;
    unsigned generator_frames=0,capture_frames=0,selected_frames=0,unavailable_frames=0,fft_frames=0;
    auto* fftDock=window->workspaceDock("fftDock");if(!fftDock)return false;
    // The startup workspace need not expose every representation. Rendering
    // assertions require an explicitly opened FFT dock; raising a closed ADS
    // dock leaves the renderer correctly suspended.
    fftDock->toggleView(true);fftDock->raise();
    int phase=0;
    struct Diagnostic {std::function<void()> print;~Diagnostic(){print();}} diagnostic{[&]{
        const auto snapshot=presenter.client().snapshot();
        domain::NumericProcessingGraph selectorDraft;QString selectorDraftError;
        const bool validDraft=editor->numeric_draft(selectorDraft,&selectorDraftError);
        std::fprintf(stderr,"selector draft group=%u valid=%d error=%s undo=%d fft_frames=%u\n",unsigned(selector->selected_input()),int(validDraft),qPrintable(selectorDraftError),editor->history().count(),fft_frames);
        if(const auto active=presenter.applied_numeric_graph())for(const auto& node:active->nodes)if(node.id==200)
            for(const auto& p:std::get<domain::TransformProcessingParameters>(node.parameters).values)if(p.key=="selected_input")
                std::fprintf(stderr,"selector applied group=%llu\n",(unsigned long long)std::visit([](auto value){return std::uint64_t(value);},std::get<NumericLiteral>(p.value)));
        std::fprintf(stderr,"selector diagnostic phase=%d selected=%d bad=%d unavailable=%d rev=%llu current=%llu state=%d gen=%u cap=%u chosen=%u unavailable_frames=%u ordinal=%llu seq=%llu cardActivity=%s values=%s first=%s apply=%s\n",phase,selected,int(bad),int(unavailable),static_cast<unsigned long long>(revision),static_cast<unsigned long long>(snapshot.graph_revision),int(snapshot.statuses.value("analyzer").state),generator_frames,capture_frames,selected_frames,unavailable_frames,static_cast<unsigned long long>(ordinal),static_cast<unsigned long long>(history_sequence),qPrintable(card->property("previewActivity").toString()),qPrintable(values->text()),qPrintable(first->text()),qPrintable(presenter.apply_status()));
    }};
    auto wait=[](const auto& predicate){QElapsedTimer timer;timer.start();while(timer.elapsed()<6000){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(2);}return predicate();};
    const auto observe=[&](const DeliveredNumericFrame& frame){
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
        if(endpoint.node!=1&&endpoint.node!=13&&endpoint.node!=200)return;
        const auto view=frame.frame.view();
        if(!view.stamp.available){if(endpoint.node==200)++unavailable_frames;return;}
        const auto* numeric=std::get_if<NumericView>(&view.payload);
        if(!numeric||numeric->contract->semantic!=Semantic::PCM||numeric->contract->dtype!=DType::Float32||numeric->planes.size()!=1||numeric->actual[0]==0||numeric->actual[0]>numeric->contract->shape.extents[0].maximum){bad=true;return;}
        double energy=0;for(std::size_t p=0;p<1;++p)for(std::size_t i=0;i<numeric->actual[0];++i){float x=0;if(load_element(*numeric,{p,i,0},x)!=Status::ok||!std::isfinite(x)){bad=true;return;}energy+=double(x)*x;}
        if(endpoint.node==1)++generator_frames;
        if(endpoint.node==13)++capture_frames;
        if(endpoint.node==200){++selected_frames;bad|=selected==0?energy<=0.002:(unavailable?energy!=0:energy<=0||energy>=0.002);}
    };
    QObject::connect(&presenter,&GuiPresenter::numeric_frame_ready,&witness,[&](const auto& frame){if(frame.context && frame.context->endpoint.node==10)observe(frame);});
    const auto observeHistory=[&](const DeliveredNumericHistoryRow& row){
        const auto current=presenter.client().snapshot();
        if(row.event!=NumericHistoryEvent::Row||!row.compact.context||row.compact.context->endpoint.node!=7||row.compact.context->endpoint.direction!=PortDirection::Output||row.compact.delivery.graph_revision!=current.graph_revision||row.compact.delivery.epoch!=current.epoch||
           (revision?row.compact.delivery.graph_revision!=revision:row.compact.delivery.graph_revision<=transition_after))return;
        bad|=row.accepted_row_ordinal<=ordinal||row.filled_count!=std::min(row.accepted_row_ordinal,std::uint64_t{400})||row.capacity!=400;
        const auto sequence=row.compact.frame.view().stamp.sequence;bad|=sequence<=history_sequence;
        ordinal=row.accepted_row_ordinal;history_sequence=sequence;
    };
    // Raw processing witnesses do not depend on which GUI dock/inspector is open.
    const auto rawTap=app.pipeline().subscribe_numeric_tap({{"main",1,PortDirection::Output,0},{"main",13,PortDirection::Output,0},{"main",200,PortDirection::Output,0},{"main",7,PortDirection::Output,0}},[&](DeliveredNumericFrame frame){
        QMetaObject::invokeMethod(&witness,[&,frame]{observe(frame);},Qt::QueuedConnection);
    },[&](DeliveredNumericHistoryRow row){QMetaObject::invokeMethod(&witness,[&,row]{observeHistory(row);},Qt::QueuedConnection);});
    struct RawTapGuard{NumericEngine& engine;std::uint64_t token;~RawTapGuard(){engine.unsubscribe_numeric_tap(token);}} rawGuard{app.pipeline(),rawTap};
    if(!wait([&]{return !presenter.graph_request_pending()&&presenter.client().snapshot().statuses.value("analyzer").state==State::Running;}))return false;
    auto subscribe=[&]{revision=presenter.client().snapshot().graph_revision;epoch=presenter.client().snapshot().epoch;};
    subscribe();
    phase=1;
    if(!wait([&]{return bad||(selected_frames>=5&&generator_frames>=5&&ordinal>=5&&(!unavailable?capture_frames>=5:true)&&card->property("previewActivity").toString().isEmpty()&&liveCard(card,source->numeric_node()->outputs.size()));})||bad)return false;
    const auto original_edges=model->numeric_graph().edges;
    const auto source_before=generator_frames;
    int transition=0;
    for(int choice:{1,0,1,0}) {
        phase=choice==1?2:3;
        const auto prior_fft=fft_frames;const auto prior_revision=revision,prior_ordinal=ordinal;const auto prior_generator=generator_frames,prior_capture=capture_frames;
        transition_after=prior_revision;revision=0;selected=choice;selected_frames=0;unavailable_frames=0;if(transition==1)editor->history().undo();else if(transition==2)editor->history().redo();else if(!click_input(domain::PortId(choice)))return false;
        ++transition;
        if(!wait([&]{
            if(presenter.client().snapshot().graph_revision<=prior_revision || presenter.graph_request_pending())return false;
            const auto active=presenter.applied_numeric_graph();if(!active)return false;
            for(const auto& node:active->nodes)if(node.id==200)
                for(const auto& parameter:std::get<domain::TransformProcessingParameters>(node.parameters).values)if(parameter.key=="selected_input")
                    return std::visit([choice](auto value){return value==choice;},std::get<NumericLiteral>(parameter.value));
            return false;
        }))return false;
        phase=choice==1?21:31;
        if(selector->selected_input()!=choice || !selector->operation_summary().contains(selector->stream_name(unsigned(choice))))return false;
        subscribe();
        const auto switched=model->numeric_graph();
        if(!std::is_permutation(original_edges.begin(),original_edges.end(),switched.edges.begin(),switched.edges.end()))return false;
        if(unavailable&&choice==1){if(!wait([&]{return bad||(selected_frames>=5&&ordinal>prior_ordinal&&generator_frames>prior_generator+5);})||bad)return false;const auto capture=presenter.client().snapshot().statuses.value("capture");
            if(capture.state!=State::Failed || !capture.error.contains("Injected loopback unavailable"))return false;
        }
        else if(!wait([&]{return bad||(selected_frames>=5&&fft_frames>prior_fft&&ordinal>prior_ordinal&&generator_frames>prior_generator+5&&(unavailable||capture_frames>prior_capture+5));})||bad)return false;
        if(presenter.client().snapshot().statuses.value("analyzer").state!=State::Running)return false;
        if(!unavailable || choice==0) {
            phase=choice==1?22:32;
            auto* panel=qobject_cast<GraphPanel*>(fftDock->widget());auto* chart=panel?panel->active_chart():nullptr;
            if(!chart || !wait([&]{return chart->value_count()>0 && chart->peak_value()>0;})){
                std::fprintf(stderr,"selector FFT projection closed=%d hidden=%d exposed=%d count=%zu peak=%g mode=%s\n",int(fftDock->isClosed()),int(fftDock->isHidden()),int(fftDock->property("renderExposed").toBool()),chart?chart->value_count():0,chart?double(chart->peak_value()):0,panel?qPrintable(panel->display_mode()):"missing");return false;
            }
            auto* selectorCard=selectorDiagnostics.get();
            auto* selectorValues=selectorCard->findChild<QLabel*>("previewValues");
            auto* selectorFirst=selectorCard->findChild<QLabel*>("previewExactValue");
            phase=choice==1?23:33;emit inspectorScene->nodeSelected(200);
            if(!wait([&]{return liveCard(selectorCard,selector->numeric_node()->outputs.size());})){
                std::fprintf(stderr,"selector card activity=%s values=%s first=%s lanes=%zu\n",qPrintable(selectorCard->property("previewActivity").toString()),qPrintable(selectorValues->text()),qPrintable(selectorFirst->text()),selector->numeric_node()->outputs.size());return false;
            }
        }
    }
    if(generator_frames<=source_before+10)return false;
    if(!unavailable) {
        phase=4;
        emit inspectorScene->nodeSelected(13);
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
        if(!wait([&]{return liveCard(captureCard,capture->numeric_node()->outputs.size())&&ordinal>prior_ordinal;}))return false;
        std::puts("captureDevice automatic live Apply/current values/History recovery PASS");
    }
    phase=7;
    const auto prior_revision=revision,prior_ordinal=ordinal;
    transition_after=prior_revision;revision=0;
    auto expanded=*selector->numeric_node();if(!numeric_editor::set_input_count(expanded,4))return false;
    phase=71;if(!model->edit_node(200,expanded))return false;
    phase=72;
    model->addConnection({1,0,200,6});model->addConnection({1,source->port_index(QtNodes::PortType::Out,domain::sample_rate_output),200,7});QCoreApplication::processEvents();
    const auto configInputs=unsigned(selector->configuration_port(QtNodes::PortType::In,domain::ConfigurationRole::SampleClock)!=QtNodes::InvalidPortIndex)+unsigned(selector->configuration_port(QtNodes::PortType::In,domain::ConfigurationRole::FrequencyGrid)!=QtNodes::InvalidPortIndex);
    if(selector->numeric_node()->inputs.size()!=8 || selector->nPorts(QtNodes::PortType::In)!=8+configInputs || !model->connectionExists({1,0,200,6}) || !model->connectionExists({1,source->port_index(QtNodes::PortType::Out,domain::sample_rate_output),200,7})){
        std::fprintf(stderr,"selector growth inputs=%zu ports=%u config=%u edge6=%d edge7=%d possible6=%d possible7=%d\n",selector->numeric_node()->inputs.size(),selector->nPorts(QtNodes::PortType::In),configInputs,int(model->connectionExists({1,0,200,6})),int(model->connectionExists({1,source->port_index(QtNodes::PortType::Out,domain::sample_rate_output),200,7})),int(model->connectionPossible({1,0,200,6})),int(model->connectionPossible({1,source->port_index(QtNodes::PortType::Out,domain::sample_rate_output),200,7})));return false;
    }
    phase=73;if(!click_input(3))return false;selected=0;selected_frames=0;
    phase=74;
    if(!wait([&]{
        if(presenter.client().snapshot().graph_revision<=prior_revision || presenter.graph_request_pending())return false;
        const auto active=presenter.applied_numeric_graph();if(!active)return false;
        for(const auto& node:active->nodes)if(node.id==200)
            for(const auto& parameter:std::get<domain::TransformProcessingParameters>(node.parameters).values)if(parameter.key=="selected_input")
                return std::visit([](auto value){return value==3;},std::get<NumericLiteral>(parameter.value));
        return false;
    }))return false;
    phase=75;subscribe();if(!wait([&]{return bad || selected_frames>=5 && ordinal>prior_ordinal;})||bad)return false;
    // A newer edit made with a graph request in flight must win reconciliation.
    const auto before_pending=presenter.client().snapshot().graph_revision;
    phase=76;presenter.apply_pipeline_graph({});selector->select_input(0);
    if(!wait([&]{const auto active=presenter.applied_numeric_graph();if(!active||presenter.graph_request_pending()||presenter.client().snapshot().graph_revision<=before_pending)return false;
        for(const auto& node:active->nodes)if(node.id==200){domain::ResolvedProcessingNode resolved;domain::ProcessingParameterView value;
            return bool(domain::resolve_processing_node(node,numeric_editor::limits(),resolved))&&domain::resolve_processing_parameter(resolved,"selected_input",value)==Status::ok&&std::visit([](auto x){return x==0;},std::get<NumericLiteral>(value));}return false;
    }))return false;
    phase=77;subscribe();
    auto* surfaceDock=window->workspaceDock("spectrogramDock");if(!surfaceDock)return false;surfaceDock->toggleView(true);surfaceDock->raise();
    auto* surface=surfaceDock->findChild<SpectrogramGraph*>();if(!surface)return false;
    auto* surfacePanel=qobject_cast<GraphPanel*>(surfaceDock->widget());
    std::fprintf(stderr,"selector surface mode=%s active=%d hidden=%d exposed=%d frames=%zu bands=%zu revision=%llu endpoint=%d\n",
        surfacePanel?qPrintable(surfacePanel->display_mode()):"none",int(surfacePanel && surfacePanel->active_spectrogram()==surface),int(surfaceDock->isHidden()),int(surfaceDock->property("renderExposed").toBool()),
        surface->projected_history_frames(),surface->projected_history_bands(),static_cast<unsigned long long>(surface->history_projection_revision()),int(surfaceDock->property("numericEndpoint").isValid()));
    if(!surfacePanel || surfacePanel->active_spectrogram()!=surface || surfacePanel->value_layout().isEmpty())return false;
    const auto currentGraph=presenter.applied_numeric_graph();if(!currentGraph)return false;
    const auto rowNode=std::find_if(currentGraph->nodes.begin(),currentGraph->nodes.end(),[](const auto& node){return node.kind=="ArrayConcat";});if(rowNode==currentGraph->nodes.end())return false;
    const auto expectedBands=std::get<NumericContract>(rowNode->outputs[0].contract.view()).shape.extents[0].maximum;
    phase=78;if(!wait([&]{return surface->projected_history_frames()>0 && surface->projected_history_bands()==expectedBands && surface->history_projection_revision()>0;}))return false;
    float peak=0;
    for(std::size_t frame=0;frame<surface->projected_history_frames();++frame)for(std::size_t band=0;band<expectedBands;++band){float value=0;if(!surface->projected_history_sample(frame,band,value)||!std::isfinite(value))return false;peak=std::max(peak,value);}
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
    std::unique_ptr<QWidget> form(source->parameter_widget());auto* amplitude=form->findChild<QLineEdit*>("oscillator0amplitude");if(!amplitude)return false;
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
