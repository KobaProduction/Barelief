#include "synthetic_engine_provider.h"
#include "core_app/gui_presenter.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/node_parameter_model.h"
#include "visualization/ui_text.h"
#include <desktop/numeric_project_document.h>
#include <pipeline/numeric_projection.h>
#include <QtNodes/DataFlowGraphicsScene>
#include <QApplication>
#include <QPushButton>
#include <QLabel>
#include <QInputDialog>
#include <QTimer>
#include <QClipboard>
#include <QLineEdit>
#include <QCheckBox>
#include <QLineF>
#include <QVBoxLayout>
#include <QtNodes/GraphicsView>
#include <QTreeWidget>
#include <QMenu>
#include <QComboBox>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static void bypass_controls_checks(){
    for(const auto& kind:numeric_editor::catalog()){
        AudioNodeModel model(kind);
        std::unique_ptr<QWidget> body(model.embeddedWidget()),inspector(model.parameter_widget());
        auto* inlineBypass=body->findChild<QCheckBox*>("numericBypass");auto* inspectorBypass=inspector->findChild<QCheckBox*>("numericBypass");
        require(inlineBypass && inspectorBypass && model.numeric_node()->enabled && !inlineBypass->isChecked() && !inspectorBypass->isChecked(),"Every new node must expose unchecked bypass with processing enabled");
        auto* form=body->findChild<QWidget*>("numericNodeParameters");
        auto* inspectorForm=inspector->objectName()=="numericNodeParameters"?inspector.get():inspector->findChild<QWidget*>("numericNodeParameters");
        require(form && inspectorForm && form->layout()->itemAt(0)->widget()==inlineBypass && inspectorForm->layout()->itemAt(0)->widget()==inspectorBypass,"Bypass must precede parameter menus/sections in the actual block and inspector forms");
        if(kind=="ScalarView"){
            const auto ordered=[](QWidget* parameters){auto* wrapper=parameters->parentWidget();return wrapper && wrapper->layout() && wrapper->layout()->count()==2 && wrapper->layout()->itemAt(0)->widget()==parameters && wrapper->layout()->itemAt(1)->widget() && wrapper->layout()->itemAt(1)->widget()->objectName()=="scalarInputsEditor";};
            require(ordered(form) && ordered(inspectorForm),"Scalar wrappers must place the bypass/parameter form before metric input controls");
        }
        require(!model.parameter_model()->field("numericEnabled") && !model.parameter_model()->field("numericBypass") && !inspector->findChild<QAction*>("showOnBlock_numericEnabled") && !inspector->findChild<QAction*>("showOnBlock_numericBypass"),"Bypass must not be a pinnable processing parameter");
        require(!model.set_parameter_on_block("numericBypass",false) && !model.parameter_model()->setOnBlock("numericEnabled",true),"Presentation pinning cannot hide or duplicate bypass");
        const auto presentation=model.presentation();
        inlineBypass->setChecked(true);
        require(!model.numeric_node()->enabled && inspectorBypass->isChecked() && model.presentation()==presentation,"Bypass must invert only the execution mode and synchronize forms");
        inspectorBypass->setChecked(false);
        require(model.numeric_node()->enabled && !inlineBypass->isChecked(),"Inspector must restore processing and synchronize inline bypass");
        model.set_numeric_edit_handler([](const auto&){return false;});inlineBypass->setChecked(true);
        require(model.numeric_node()->enabled && !inlineBypass->isChecked() && !inspectorBypass->isChecked(),"Rejected bypass edit must restore both controls");
    }
}
struct PreviewPools {
    alignas(64) std::array<std::byte,8192> payload{},metadata{};
    std::array<signal_contracts::BufferSlot,4> ps{},ms{};
    signal_contracts::BufferPool p{payload,ps,2048},m{metadata,ms,2048};
};
static pipeline::DeliveredNumericFrame scalar_delivery(PreviewPools& pools,const domain::ConfiguredProcessingNode& node,
    pipeline::DeliveryStamp delivery,std::uint64_t value,std::uint64_t sequence) {
    using namespace signal_contracts;
    auto context=std::make_shared<pipeline::NumericDeliveryContext>();context->endpoint={"main",node.id,domain::PortDirection::Input,0};context->contract=node.inputs[0].contract;
    auto contract=std::get<NumericContract>(context->contract.view());
    alignas(64) std::array<std::byte,8> bytes{};const std::array<PlaneLayout,1> planes{{{0,{8,8}}}};
    MutableNumericView numeric{&contract,bytes,planes,{1,1},{},{}};require(store_element(numeric,{},value)==Status::ok,"Fixture UInt64 storage");
    FrameStamp stamp;stamp.sequence=sequence;stamp.producer_epoch=1;stamp.epoch=delivery.epoch;stamp.graph_revision=delivery.graph_revision;
    DataFrameView frame{stamp,numeric.view()};pipeline::DeliveredNumericFrame result;
    require(pipeline::retain_numeric_delivery(context,delivery,frame,Status::ok,numeric_editor::limits().frame,pools.p,pools.m,result)==Status::ok,"Fixture bounded retention");return result;
}
static void preview_model_checks() {
    using namespace signal_contracts;
    AudioNodeModel model("ScalarView");auto node=*model.numeric_node();node.id=12;
    require(numeric_editor::set_dtype(node,domain::PortDirection::Input,0,DType::UInt64),"UInt64 scalar contract");model.set_numeric_node(node);
    std::unique_ptr<QWidget> card(model.preview_widget());auto* exact=card->findChild<QLabel*>("previewExactValue");auto* status=card->findChild<QLabel*>("previewStatus");
    PreviewPools pools;auto frame=scalar_delivery(pools,node,{3,5},std::numeric_limits<std::uint64_t>::max(),8);const auto snapshot=model.save();
    require(model.preview(frame) && exact->text()=="18446744073709551615","Preview rounds UInt64 maximum");
    require(model.save()==snapshot,"Diagnostic changed configured parameters");
    auto stale=scalar_delivery(pools,node,{3,5},7,7);require(!model.preview(stale) && exact->text()=="18446744073709551615","Late frame replaced newer exact value");
    auto foreign=frame;auto wrong=std::make_shared<pipeline::NumericDeliveryContext>(*frame.context);wrong->endpoint.node=13;foreign.context=wrong;
    require(!model.preview(foreign) && exact->text()=="18446744073709551615","Foreign endpoint changed preview");
    auto unavailable=frame;unavailable.status=Status::unavailable;require(model.preview(unavailable) && exact->text()==QStringLiteral("—"),"Unavailable frame retained stale value");
    require(!model.preview(stale),"Unavailable discarded sequence fence");
    model.clear_preview();frame={};stale={};foreign={};unavailable={};
    require(pools.p.in_use()==0 && pools.m.in_use()==0,"Preview retains pooled frame storage");
    auto next=scalar_delivery(pools,node,{3,5},9007199254740993ULL,9);require(model.preview(next) && exact->text()=="9007199254740993","Preview loses integer above binary64 exact range");
    for(const auto& theme:{QString("light"),QString("dark")}){
        require(ui::Theme::instance().set(theme),"Theme switch refused");QApplication::processEvents();
        require(card->palette().color(QPalette::WindowText)==QApplication::palette().color(QPalette::WindowText),"Preview breaks inherited palette");
        require(exact->text()=="9007199254740993","Theme mutates diagnostic value");
    }
    require(ui::Language::instance().set("ru"),"RU language unavailable");require(status->text()==QString::fromUtf8("Входной порт 0 · Данные доступны"),"Live preview status not translated");
    require(card->findChild<QLabel*>("previewUnit")->text().contains(QString::fromUtf8("Входной порт")),"Live endpoint label not translated");
    require(ui::Language::instance().set("en"),"EN language unavailable");require(status->text()=="Input port 0 · Available","Preview fails live EN language update");
    node.enabled=false;model.set_numeric_node(node);require(status->text()=="Input port 0 · Bypass · Waiting for data" && exact->text()==QStringLiteral("—"),"Bypass transition reused stale diagnostics");
    auto bypassZero=scalar_delivery(pools,node,{3,5},0,10);
    require(model.preview(bypassZero) && status->text()=="Input port 0 · Bypass · Available" && exact->text()=="0","Bypass must preview actual typed-zero output instead of reporting a stopped node");
    auto bypassUnavailable=bypassZero;bypassUnavailable.status=Status::unavailable;
    require(model.preview(bypassUnavailable) && status->text()=="Input port 0 · Bypass · Unavailable" && exact->text()==QStringLiteral("—"),"Bypass must preserve actual unavailability diagnostics");
    bypassZero={};bypassUnavailable={};
    node.enabled=true;model.set_numeric_node(node);require(exact->text()==QStringLiteral("—"),"Reenable reused old diagnostic");
    auto tagged=std::get<NumericContract>(node.inputs[0].contract.view());tagged.semantic=Semantic::History;
    require(numeric_editor::set_contract(node,domain::PortDirection::Input,0,tagged),"Passive preview tag fixture");model.set_numeric_node(node);
    auto taggedScalar=scalar_delivery(pools,node,{3,5},42,11);
    require(model.preview(taggedScalar) && exact->text()=="42","A passive History tag must not suppress a physical scalar preview");taggedScalar={};
    next={};AudioNodeModel history("Spectrogram");auto h=*history.numeric_node();h.id=14;
    auto historyContract=std::get<NumericContract>(h.outputs[0].contract.view());historyContract.shape.extents[0].maximum=4;
    require(domain::own_processing_contract(historyContract,numeric_editor::limits().frame,2048,h.outputs[0].contract)==Status::ok,"Bounded History preview fixture contract");h.parameters=domain::HistoryProcessingParameters{4};history.set_numeric_node(h);std::unique_ptr<QWidget> historyCard(history.preview_widget());
    auto context=std::make_shared<pipeline::NumericDeliveryContext>();context->endpoint={"main",14,domain::PortDirection::Output,0};context->contract=h.outputs[0].contract;
    pipeline::DeliveredNumericHistoryRow row;row.compact.context=context;row.compact.delivery={3,5};row.event=pipeline::NumericHistoryEvent::Empty;
    row.capacity=std::get<NumericContract>(context->contract.view()).shape.extents[0].maximum;
    require(history.preview(row) && historyCard->findChild<QLabel*>("previewValues")->text()==QStringLiteral("0\n—"),"Empty history count not available zero");
    row.filled_count=row.capacity+1;require(!history.preview(row),"Invalid filled count accepted");
    row.filled_count=0;row.event=pipeline::NumericHistoryEvent::Unavailable;require(history.preview(row) && historyCard->findChild<QLabel*>("previewValues")->text()==QStringLiteral("—\n—"),"Unavailable history retained count");
    require(historyCard->findChild<QLabel*>("previewStatus")->text().split('\n').size()==2 && !historyCard->findChild<QLabel*>("previewStatus")->text().contains("Input port"),"Processor preview advertises unsupported input callback");
    const auto historyConfigured=history.save();
    require(historyContract.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE && historyContract.coordinates[0].unit==NumericUnit::Seconds,"History fixture must preserve the explicit source-clock contract");
    const auto encodeTime=[](double seconds,std::span<std::byte> bytes){
        const auto bits=std::bit_cast<std::uint64_t>(seconds);for(unsigned byte=0;byte<8;++byte)bytes[byte]=std::byte((bits>>(8*byte))&255);
    };
    auto retainedHistory=[&](bool compact,std::uint64_t sequence){
        auto contract=std::get<NumericContract>(context->contract.view());
        if(compact){contract.semantic=contract.history_element;contract.history_element=Semantic::Numeric;contract.shape={Rank::Vector,{contract.shape.axes[1],Axis::None},{contract.shape.extents[1],ExtentBounds{}}};contract.coordinates={contract.coordinates[1],AxisCoordinates{}};}
        std::array<float,200> values{};values.fill(7);if(!compact)std::fill_n(values.begin(),50,0.f);PlaneLayout plane{0,compact?std::array<std::size_t,2>{4,0}:std::array<std::size_t,2>{200,4}};
        FrameStamp stamp;stamp.producer_epoch=701;stamp.generation=17;stamp.sequence=sequence;stamp.epoch=3;stamp.graph_revision=5;stamp.timestamp_ticks=compact?64800:64000;
        std::array<std::byte,32> times{};for(std::size_t rowIndex=1;rowIndex<4;++rowIndex)encodeTime(double(77+rowIndex)/60.,std::span(times).subspan(rowIndex*8,8));
        std::array<std::byte,50*8> frequencies{};for(std::size_t band=0;band<50;++band)encodeTime(double(band+1)*100.,std::span(frequencies).subspan(band*8,8));
        CoordinateView coordinates;if(!compact)coordinates.axes[0]=times;coordinates.axes[compact?0:1]=frequencies;
        DataFrameView input{stamp,NumericView{&contract,std::as_bytes(std::span(values)),std::span(&plane,1),compact?std::array<std::size_t,2>{50,1}:std::array<std::size_t,2>{4,50},compact?RingRows{}:RingRows{4,0},coordinates}};
        pipeline::DeliveredNumericFrame result{context,{3,5},{},Status::ok};require(retain_frame(input,numeric_editor::limits().frame,pools.p,pools.m,result.frame)==Status::ok,"Cold History fixture retention");return result;
    };
    auto full=retainedHistory(false,800);const auto beforeCold=historyCard->findChild<QLabel*>("previewValues")->text();const auto retainedPayload=pools.p.in_use(),retainedMetadata=pools.m.in_use();
    const auto retainedTimes=std::get<NumericView>(full.frame.view().payload).coordinates.axes[0];
    require(retainedTimes.size()==32 && pipeline::decode_coordinate(retainedTimes.subspan(0,8))==0 && pipeline::decode_coordinate(retainedTimes.subspan(8,8))==78./60. && pipeline::decode_coordinate(retainedTimes.subspan(24,8))==80./60.,"Cold History fixture must own individual real row times after its donor expires");
    require(!history.preview_history_snapshot(full,2,3) && historyCard->findChild<QLabel*>("previewValues")->text()==beforeCold && history.save()==historyConfigured && pools.p.in_use()==retainedPayload && pools.m.in_use()==retainedMetadata,"Malformed cold ordinal changes preview/count/parameters/storage");
    require(history.preview_history_snapshot(full,80,3) && historyCard->findChild<QLabel*>("previewValues")->text()==QStringLiteral("3\n—"),"Cold History snapshot count unavailable");
    row.compact=retainedHistory(true,801);row.event=pipeline::NumericHistoryEvent::Row;row.filled_count=4;row.accepted_row_ordinal=81;
    row.has_time_coordinate=true;encodeTime(81./60.,row.time_coordinate);
    require(history.preview(row) && historyCard->findChild<QLabel*>("previewValues")->text()==QStringLiteral("4\n—"),"Compact History does not continue after cold snapshot");
    require(!history.preview_history_snapshot(full,80,3) && historyCard->findChild<QLabel*>("previewValues")->text()==QStringLiteral("4\n—"),"Late cold snapshot replaces newer compact ordinal");
    require(history.save()==historyConfigured,"Cold History preview mutates parameters");full={};row.compact={};require(pools.p.in_use()==0 && pools.m.in_use()==0,"Cold History preview retains pooled storage");
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        ui::Language::instance().set("en");bypass_controls_checks();preview_model_checks();
        GuiState state;state.file_configs.ui.autosave_enabled=false;state.file_configs.ui.pipeline_auto_apply=false;
        state.delegate_model_registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
        for(const auto& kind:numeric_editor::catalog())state.delegate_model_registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
        auto graph=std::make_shared<NumericEditorGraphModel>(state.delegate_model_registry);state.data_flow_graph=graph;
        domain::NumericProcessingGraph empty;empty.graph_id="main";require(graph->set_numeric_graph(empty),"Empty canonical fixture rejected");state.file_configs.scene_graph=graph->save();
        GuiPresenter presenter(state,test_fixture::synthetic_engine_provider(false));SchemeEditorWidget editor(presenter);editor.resize(1000,700);editor.show();QApplication::processEvents();
        QSignalSpy typedSave(&editor,&SchemeEditorWidget::numeric_draft_changed),applied(&presenter,&GuiPresenter::graph_apply_finished);
        // Presentation-only capture status: injected projected backend states,
        // never physical capture, OS input or a forged successful Apply.
        auto* capture=editor.toolbar()->findChild<QLabel*>("pipelineCaptureStatus");
        auto* validation=editor.toolbar()->findChild<QLabel*>("pipelineValidation");
        require(capture && validation,"Capture status indicator absent");
        const auto beforeCapture=editor.document();const int beforeCaptureUndo=editor.history().index();
        emit presenter.apply_status_changed("Not applied: fixture contract");const auto admissionWarning=validation->toolTip();
        emit presenter.component_status_changed("capture",int(pipeline::State::Failed),"fixture: loopback device unavailable");
        require(capture->isVisibleTo(editor.toolbar()) && capture->text()==ui::text("Capture failed") && capture->toolTip().contains("fixture: loopback device unavailable"),"Projected capture failure is not visible with backend error");
        emit presenter.apply_status_changed("Applied");
        emit presenter.component_status_changed("analyzer",int(pipeline::State::Running),{});
        require(capture->text()==ui::text("Capture failed") && validation->toolTip()==admissionWarning,"Apply or analyzer status masks capture/admission failure");
        ui::Language::instance().set("ru");
        require(capture->text()==QString::fromUtf8("Ошибка захвата") && capture->toolTip().contains("fixture: loopback device unavailable"),"Live capture failure is not translated to RU");
        emit presenter.component_status_changed("capture",int(pipeline::State::Starting),{});
        require(capture->text()==ui::text("Capture starting") && !capture->toolTip().contains("fixture:"),"Capture starting retains stale failure");
        emit presenter.component_status_changed("capture",int(pipeline::State::Running),{});
        require(capture->text()==ui::text("Capture running") && validation->toolTip()==admissionWarning,"Capture recovery overwrites admission warning");
        ui::Language::instance().set("en");require(capture->text()=="Capture running","Live capture recovery fails EN refresh");
        emit presenter.component_status_changed("capture",int(pipeline::State::Stopped),{});
        require(!capture->isVisibleTo(editor.toolbar()) && capture->toolTip().isEmpty() && editor.document()==beforeCapture && editor.history().index()==beforeCaptureUndo,"Capture status mutates draft/history or retains stopped error");

        auto* library=editor.findChild<QTreeWidget*>("pipelineNodeLibrary");require(library && library->topLevelItemCount()==3,"Sidebar categories differ from shared roles");
        auto* scene=editor.findChild<QtNodes::DataFlowGraphicsScene*>();std::unique_ptr<QMenu> menu(scene->createSceneMenu({9000,9000}));
        auto* context=menu->findChild<QTreeWidget*>("numericContextCatalog");require(context && context->topLevelItemCount()==3,"Context catalog missing shared roles");
        auto kinds=[](QTreeWidget* tree){QStringList result;std::function<void(QTreeWidgetItem*)> visit=[&](auto* item){const auto kind=item->data(0,Qt::UserRole).toString();if(!kind.isEmpty())result.append(kind);for(int i=0;i<item->childCount();++i)visit(item->child(i));};for(int i=0;i<tree->topLevelItemCount();++i)visit(tree->topLevelItem(i));result.sort();return result;};
        auto expectedKinds=numeric_editor::catalog();expectedKinds.sort();require(kinds(context)==kinds(library) && kinds(library)==expectedKinds,"Sidebar/context must expose every catalog leaf in the shared hierarchy");
        {
            const auto source=graph->addNode("TestSignal"),sink=graph->addNode("Converter");auto node=*graph->delegateModel<AudioNodeModel>(sink)->numeric_node();numeric_editor::set_dtype(node,domain::PortDirection::Input,0,signal_contracts::DType::UInt16);graph->edit_node(sink,node);
            const QtNodes::ConnectionId edge{source,0,sink,0};graph->addConnection(edge);scene->connectionGraphicsObject(edge)->setSelected(true);
            auto* policy=editor.findChild<QComboBox*>("connectionConversionPolicy");require(policy && policy->currentIndex()==0 && editor.findChild<QLabel*>("connectionRisk"),"Selecting a dtype edge must expose its actual policy in the existing inspector");
            policy->setCurrentIndex(3);require(graph->connection_policy(edge)==3,"Inspector policy must edit the processing graph");editor.history().undo();require(graph->connection_policy(edge)==0 && policy->currentIndex()==0,"Inspector policy undo must refresh both representations");
            graph->deleteNode(sink);graph->deleteNode(source);
        }
        auto converter=numeric_editor::default_node("Converter");converter.id=3;
        std::get<domain::ConverterProcessingParameters>(converter.parameters).policy.nan_replacement=std::uint64_t{9007199254740993ULL};
        domain::NumericProcessingGraph draft;draft.graph_id="main";draft.nodes.push_back(converter);QString error;
        require(editor.load_numeric_draft(draft,&error),"Widget canonical load failed");domain::NumericProcessingGraph exported;
        require(editor.numeric_draft(exported,&error) && exported.nodes.size()==1 && exported.nodes[0].id==3,"Widget export lost owning node/id");
        const auto before=editor.document();auto invalid=draft;invalid.nodes[0].kind="UnknownNumericNode";
        require(!editor.load_numeric_draft(invalid,&error) && editor.document()==before,"Rejected load changed current draft");
        graph->setNodeData(3,QtNodes::NodeRole::Position,QPointF(420,230));
        auto* converterModel=graph->delegateModel<AudioNodeModel>(3);require(converterModel->set_parameter_on_block("numericNan",true),"Preset visibility edit failed");
        auto* editorView=editor.findChild<QtNodes::GraphicsView*>();require(editorView,"Graph clipboard view missing");
        int clipboardActions=0;
        for(auto* action:editorView->actions())if(action->shortcuts().contains(QKeySequence(QKeySequence::Copy)) || action->shortcuts().contains(QKeySequence(QKeySequence::Paste))) {
            ++clipboardActions;require(action->shortcutContext()==Qt::WidgetWithChildrenShortcut && !action->autoRepeat(),"Graph clipboard shortcuts escape the focused view or repeat");
        }
        require(clipboardActions==2,"Graph Ctrl+C/V shortcuts were remapped or missing");
        scene->nodeSelected(3);auto* renameBlock=editor.findChild<QPushButton*>("pipelineRenameNode");require(renameBlock,"Ordinary processor inspector lacks rename control");
        auto* textEditor=editor.findChild<QLineEdit*>("numericNan");require(textEditor,"Exact numeric text editor absent");
        const auto clipboardNodeCount=graph->allNodeIds().size();textEditor->setFocus();textEditor->selectAll();
        QTest::keyClick(textEditor,Qt::Key_C,Qt::ControlModifier);
        require(QApplication::clipboard()->text()=="9007199254740993" && graph->allNodeIds().size()==clipboardNodeCount,"Copy inside a parameter field copied graph nodes instead of text");
        const auto renameUndo=editor.history().index();
        QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Exact converter");dialog->accept();}});
        renameBlock->click();require(converterModel->caption()=="Exact converter" && converterModel->name()=="Converter" && editor.history().index()==renameUndo+1,"Inspector rename did not update user caption with one undo command");
        editor.history().undo();require(converterModel->caption()!="Exact converter","Inspector rename undo did not restore default");editor.history().redo();
        require(editor.save_preset("exact snapshot"),"Canonical preset save failed");const auto preset=state.file_configs.ui.pipeline_presets.last().toObject();
        graph->deleteNode(3);require(editor.load_preset(preset["id"].toString()),"Canonical preset reload failed");
        require(editor.numeric_draft(exported) && std::get<std::uint64_t>(std::get<domain::ConverterProcessingParameters>(exported.nodes[0].parameters).policy.nan_replacement)==9007199254740993ULL,"Preset rounds exact UInt64 literal");
        require(graph->nodeData(3,QtNodes::NodeRole::Position).toPointF()==QPointF(420,230),"Preset loses node position");
        require(graph->delegateModel<AudioNodeModel>(3)->parameter_on_block("numericNan"),"Preset loses presentation flag");
        require(graph->delegateModel<AudioNodeModel>(3)->caption()=="Exact converter","Preset loses the user display name");
        editor.findChild<QPushButton*>("applyPipeline")->click();
        for(int i=0;i<100 && applied.isEmpty();++i)QTest::qWait(10);
        require(!applied.isEmpty() && applied.last()[1].toBool() && typedSave.count()>=1,"Canonical Apply not acknowledged by executor");
        require(!QJsonDocument(presenter.client().snapshot().graph).toJson().contains("inline_parameters"),"Presentation flags reached engine Apply snapshot");
        domain::NumericProcessingGraph actual;require(bool(desktop::decode_numeric_editor_graph(QJsonDocument(presenter.client().snapshot().graph).toJson(QJsonDocument::Compact),actual)) && actual.nodes.size()==1 && actual.nodes[0].id==3,"Apply did not reach canonical engine snapshot");
        graph->deleteNode(3);editor.flush_presentation();require(graph->has_numeric_nodes() && typedSave.count()>=2,"Empty draft drops owning save notification");
        require(editor.remove_preset(preset["id"].toString()),"Preset removal failed");
        domain::NumericProcessingGraph diagnostic;diagnostic.graph_id="main";
        auto scalar=numeric_editor::default_node("ScalarView");scalar.id=12;
        require(numeric_editor::set_dtype(scalar,domain::PortDirection::Input,0,signal_contracts::DType::UInt64),"Diagnostic UInt64 input");diagnostic.nodes.push_back(scalar);
        require(editor.load_numeric_draft(diagnostic),"Diagnostic graph load");applied.clear();editor.findChild<QPushButton*>("applyPipeline")->click();
        for(int i=0;i<100 && applied.isEmpty();++i)QTest::qWait(10);
        require(!applied.isEmpty() && applied.last()[1].toBool(),"Diagnostic graph Apply failed");
        auto* model=graph->delegateModel<AudioNodeModel>(12);std::unique_ptr<QWidget> diagnosticCard(model->preview_widget());auto* exact=diagnosticCard->findChild<QLabel*>("previewExactValue");
        require(exact,"Registered diagnostic preview value absent");
        for(int i=0;i<200 && presenter.client().snapshot().statuses.value("analyzer").state!=pipeline::State::Running;++i)QTest::qWait(10);
        require(presenter.client().snapshot().statuses.value("analyzer").state==pipeline::State::Running,"Preview fixture must run actual current graph");
        const auto snapshot=presenter.client().snapshot();PreviewPools pools;
        auto frame=scalar_delivery(pools,scalar,{snapshot.graph_revision,snapshot.epoch},9007199254740993ULL,1);
        emit presenter.numeric_frame_ready(frame);require(exact->text()=="9007199254740993","Connected presenter signal fails inline preview");
        const auto parameters=model->save();editor.hide();require(exact->text()==QStringLiteral("—"),"Hidden editor retains available stale diagnostic");
        emit presenter.numeric_frame_ready(frame);require(exact->text()==QStringLiteral("—"),"Hidden editor consumes numeric delivery");editor.show();
        emit presenter.numeric_frame_ready(frame);require(exact->text()=="9007199254740993","Reopened editor fails current delivery");
        presenter.apply_pipeline_graph(editor.document());
        for(int i=0;i<200 && (presenter.client().snapshot().graph_revision==snapshot.graph_revision || presenter.graph_request_pending());++i)QTest::qWait(10);
        QCoreApplication::processEvents();
        require(presenter.client().snapshot().graph_revision>snapshot.graph_revision && presenter.client().snapshot().statuses.value("analyzer").state==pipeline::State::Running,"Apply did not advance the continuous graph");
        emit presenter.numeric_frame_ready(frame);require(exact->text()==QStringLiteral("—"),"New graph revision accepts stale preview frame");
        auto* bypass=model->embeddedWidget()->findChild<QCheckBox*>("numericBypass");require(bypass,"Connected node bypass absent");
        const int bypassUndo=editor.history().index();bypass->setChecked(true);
        require(!model->numeric_node()->enabled && editor.history().index()==bypassUndo+1,"Bypass edit bypasses editor command/undo path");
        editor.history().undo();require(model->numeric_node()->enabled && !bypass->isChecked(),"Undo fails to restore bypass control");
        editor.history().redo();require(!model->numeric_node()->enabled && bypass->isChecked(),"Redo fails to restore bypass control");
        editor.history().undo();
        auto changed=*model->numeric_node();changed.enabled=false;require(graph->edit_node(12,changed),"Draft parameter edit failed");
        changed.enabled=true;require(graph->edit_node(12,changed),"Draft reenable failed");
        auto alternate=*model->numeric_node();numeric_editor::set_dtype(alternate,domain::PortDirection::Input,0,signal_contracts::DType::Int64);require(graph->edit_node(12,alternate),"Diverging contract edit failed");
        emit presenter.numeric_frame_ready(frame);require(exact->text()==QStringLiteral("—"),"Divergent draft displays active graph data");
        require(parameters!=model->save(),"Fixture draft edit failed to change typed snapshot");
        auto badFrame=frame;auto badContext=std::make_shared<pipeline::NumericDeliveryContext>(*frame.context);badContext->endpoint.graph_id="foreign";badFrame.context=badContext;
        emit presenter.numeric_frame_ready(badFrame);require(exact->text()==QStringLiteral("—"),"Foreign graph populates inline preview");
        std::unique_ptr<QWidget> controls(model->parameter_widget());auto* inspectorBypass=controls->findChild<QCheckBox*>("numericBypass");require(inspectorBypass,"Missing dedicated bypass control");
        ui::Language::instance().set("ru");require(inspectorBypass->text()==ui::text("Bypass") && bypass->text()==ui::text("Bypass"),"Bypass controls not live translated");ui::Language::instance().set("en");
        auto* view=editor.findChild<QtNodes::GraphicsView*>();const auto retainedPosition=graph->nodeData(12,QtNodes::NodeRole::Position).toPointF();const auto retainedScale=view->getScale();
        editor.hide();editor.show();QApplication::processEvents();require(graph->nodeData(12,QtNodes::NodeRole::Position).toPointF()==retainedPosition && view->getScale()==retainedScale,"Ordinary show resets explicit layout");
        editor.hide();graph->setNodeData(12,QtNodes::NodeRole::Position,QPointF(9000,7000));editor.initialize_default_layout();QApplication::processEvents();
        require(graph->nodeData(12,QtNodes::NodeRole::Position).toPointF()==QPointF(9000,7000),"Default layout runs before editor viewport visible");
        const auto processingBefore=editor.document().value("nodes").toArray().first().toObject().value("internal-data").toObject().value("numeric_processing");
        editor.show();QApplication::processEvents();const auto arranged=graph->nodeData(12,QtNodes::NodeRole::Position).toPointF();
        require(arranged!=QPointF(9000,7000) && editor.document().value("nodes").toArray().first().toObject().value("internal-data").toObject().value("numeric_processing")==processingBefore,"One-time default layout missing or mutates processing");
        require(QLineF(view->mapToScene(view->viewport()->rect().center()),scene->itemsBoundingRect().center()).length()<5/view->getScale(),"Default fit fails to center ready viewport");
        graph->setNodeData(12,QtNodes::NodeRole::Position,QPointF(111,222));editor.hide();editor.show();QApplication::processEvents();
        require(graph->nodeData(12,QtNodes::NodeRole::Position).toPointF()==QPointF(111,222),"Default layout repeats and destroys user positions");
        editor.hide();editor.initialize_default_layout();
        require(editor.load_numeric_draft(diagnostic),"Explicit typed replacement rejected");graph->setNodeData(12,QtNodes::NodeRole::Position,QPointF(333,444));
        editor.show();QApplication::processEvents();require(graph->nodeData(12,QtNodes::NodeRole::Position).toPointF()==QPointF(333,444),"Stale initial layout overwrites explicit typed replacement");
        qInfo("Canonical editor: catalog, atomic import, presets, Apply, exact bounded diagnostic, fences, live theme/RU/EN passed");return 0;
    }catch(const std::exception& error){qCritical("%s",error.what());return 1;}
}
