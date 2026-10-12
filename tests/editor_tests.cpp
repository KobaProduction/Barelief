#include "synthetic_engine_provider.h"
#include "core_app/gui_presenter.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/node_parameter_model.h"
#include "core_app/scheme_editor/numeric_node_operations.h"
#include "core_app/scheme_editor/editor_geometry.h"
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
#include <QTableWidget>
#include <QDoubleSpinBox>
#include <QTransform>
#include <QGraphicsView>
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
static std::shared_ptr<QtNodes::NodeDelegateModelRegistry> inventory_registry(){
    auto registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    for(const auto& kind:numeric_editor::catalog())registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);});
    return registry;
}
static void catalogue_acceptance_checks(){
    using E=NodeParameterModel::Editor;
    // Product inventory, deliberately independent of catalog() and descriptors.
    struct Spec {const char* kind;unsigned inputs,outputs;const char* control;E editor;};
    const std::array<Spec,30> specs{{
        {"AudioSource",0,1,"captureDevice",E::Choice},{"SignalGenerator",0,1,"oscillatorCount",E::Integer},
        {"SampleClock",0,0,"clockSampleRate",E::Integer},{"PcmResampler",1,1,"output_rate",E::Literal},
        {"Window",1,1,"size",E::Literal},{"WindowFunction",1,1,"function",E::Choice},
        {"StereoFFT",1,1,nullptr,E::Caption},{"BarkWeighting",1,1,nullptr,E::Caption},
        {"Mel",1,1,"bands",E::Literal},{"SpectrumGain",1,1,"gain",E::Literal},
        {"Mixer",2,1,"mixerMode",E::Choice},{"InputSelector",2,1,"numericSelectorChannels",E::Integer},
        {"Switch",1,2,"numericRouteChannels",E::Integer},{"Gate",1,1,"numericGate",E::Choice},
        {"Spectrogram",1,2,"numericCapacity",E::Literal},{"IndexExtract",1,1,"index",E::Literal},
        {"ScalarReduce",1,1,"numericReduction",E::Choice},{"Converter",1,1,"numericRounding",E::Choice},
        {"AudioView",1,0,nullptr,E::Caption},{"WindowView",1,0,nullptr,E::Caption},
        {"WindowedView",1,0,nullptr,E::Caption},{"FFTView",1,0,nullptr,E::Caption},
        {"MelView",1,0,nullptr,E::Caption},{"SpectrogramView",1,0,nullptr,E::Caption},
        {"ScalarView",1,0,nullptr,E::Caption},{"NetworkInput",0,1,"adapterHost",E::Text},
        {"NetworkOutput",1,0,nullptr,E::Caption},{"WLEDEncode",1,1,"gain",E::Literal},
        {"WLEDOutput",1,0,"destination",E::Text},{"ChannelComparison",2,1,"window_samples",E::Literal}
    }};
    QStringList expected;for(const auto& spec:specs)expected.append(spec.kind);expected.sort();auto actual=numeric_editor::catalog();actual.sort();
    require(actual==expected,"Product catalogue differs from the explicit 30-kind inventory");
    for(const auto& spec:specs){
        AudioNodeModel node(spec.kind);const auto* dto=node.numeric_node();auto* parameters=node.parameter_model();
        require(dto->inputs.size()==spec.inputs && dto->outputs.size()==spec.outputs,qPrintable(QString("Wrong mono port roles for ")+spec.kind));
        if(spec.control){const auto* field=parameters->field(spec.control);require(field && field->editor==spec.editor,qPrintable(QString("Missing operation/adapter editor for ")+spec.kind));}
        for(auto direction:{domain::PortDirection::Input,domain::PortDirection::Output}){
            const auto& ports=direction==domain::PortDirection::Input?dto->inputs:dto->outputs;
            for(const auto& port:ports){const auto prefix=QString(direction==domain::PortDirection::Input?"input%1":"output%1").arg(port.id);
                const auto* caption=parameters->field(prefix+"Caption");require(caption && caption->editor==E::Caption && !caption->pinnable && caption->section==NodeParameterModel::Section::Contract,"Every data port needs a read-only contract role");
                for(const auto* unsupported:{"Channels","Unit","Layout","Semantic","Meaning"})require(!parameters->field(prefix+unsupported),"Descriptive contract controls must not be registered");
                for(int axis=0;axis<2;++axis)if(const auto* extent=parameters->field(prefix+QString("Extent%1").arg(axis)))require(extent->literal_type==1 && std::holds_alternative<std::uint64_t>(std::get<signal_contracts::NumericLiteral>(extent->read())),"Dimensional editor must use exact UInt64");
            }
        }
        std::unique_ptr<QWidget> form(node.parameter_widget());
        const auto saved=node.save();node.set_processing_activity(true,true,120);require(node.save()==saved,"Catalogue diagnostics mutate a configured node");
        if(numeric_editor::is_graph_view(spec.kind))require(!parameters->field("input0Dtype") && !parameters->field("input0Extent0"),"View must derive its contract from upstream");
        if(QString(spec.kind)=="AudioSource")require(!parameters->field("sample_rate") && parameters->field("source") && parameters->field("captureSavedFormat") && parameters->field("captureDeviceError") && parameters->field("refreshCaptureDevices"),"Capture operation and adapter responsibilities are not separated");
        if(QString(spec.kind)=="NetworkInput")for(const auto* key:{"adapterProviderUuid","adapterStreamId","adapterHost","adapterPort","adapterDeviceIdentity","adapterGraphId","adapterNodeId","adapterPortIndex","adapterDirection"})require(parameters->field(key),"Network adapter route editor absent");
        if(QString(spec.kind)=="Spectrogram")require(node.portCaption(QtNodes::PortType::Out,0).startsWith(ui::text("History")) && node.portCaption(QtNodes::PortType::Out,1).startsWith(ui::text("Filled frames")),"History/count outputs have ambiguous roles");
        for(const auto& language:{QString("ru"),QString("en")})for(const auto& theme:{QString("light"),QString("dark")}){
            ui::Language::instance().set(language);ui::Theme::instance().set(theme);QCoreApplication::processEvents();
            auto* bypass=form->findChild<QCheckBox*>("numericBypass");require(bypass && bypass->text()==ui::text("Bypass"),"Catalogue form fails live translated bypass");
            for(auto* control:form->findChildren<QWidget*>())for(auto group:{QPalette::Active,QPalette::Inactive,QPalette::Disabled})
                require(control->palette().color(group,QPalette::WindowText)==QApplication::palette().color(group,QPalette::WindowText),"Catalogue control overrides centralized theme text colors");
            form->setEnabled(false);require(!bypass->isEnabled(),"Catalogue disabled controls ignore their inspector");form->setEnabled(true);
            require(node.save()==saved,"Catalogue localization/theme mutates processing configuration");
        }
    }
    AudioNodeModel generator("SignalGenerator");std::unique_ptr<QWidget> form(generator.parameter_widget());auto* table=form->findChild<QTableWidget*>("oscillatorTable");require(table,"Generator table absent");
    const auto saved=generator.save();
    for(const auto& language:{QString("ru"),QString("en"),QString("ru")}){
        ui::Language::instance().set(language);QCoreApplication::processEvents();
        const char* labels[]{"Enabled","Frequency, Hz","Amplitude","Phase, radians","Attack, seconds","Release, seconds"};
        for(int column=0;column<6;++column)require(table->horizontalHeaderItem(column+1)->text()==ui::text(labels[column]),"Open oscillator headers fail live language update");
        require(generator.save()==saved,"Language switching changes oscillator parameters");
    }
    ui::Language::instance().set("en");
}
static void oscillator_bank_acceptance_checks(){
    NumericEditorGraphModel graph(inventory_registry());QUndoStack undo;graph.set_undo_stack(&undo);
    const auto id=graph.addNode("SignalGenerator");auto* node=graph.delegateModel<AudioNodeModel>(id);
    auto configured=*node->numeric_node();auto& tones=std::get<domain::SignalGeneratorProcessingParameters>(configured.parameters).oscillators;
    tones.resize(3);tones[0].id=7;tones[0].frequency_hz=440.123456789;tones[1].id=19;tones[1].frequency_hz=880;tones[2].id=41;tones[2].frequency_hz=1320;
    require(graph.edit_node(id,configured),"Exact oscillator fixture rejected");
    std::unique_ptr<QWidget> form(node->parameter_widget());auto* model=node->parameter_model();
    auto* edit=form->findChild<QLineEdit*>("oscillator0frequency_hz");require(edit,"Exact oscillator text editor missing");
    const auto before=node->save();const int index=undo.index();
    require(edit->text().toDouble()==440.123456789,"Oscillator display loses Float64 precision");
    QMetaObject::invokeMethod(edit,"editingFinished",Qt::DirectConnection);
    require(node->save()==before && undo.index()==index,"Untouched oscillator creates lossy undo command");
    edit->setText("440.123456789");edit->setModified(true);emit edit->textEdited(edit->text());
    QMetaObject::invokeMethod(edit,"editingFinished",Qt::DirectConnection);
    require(node->save()==before && undo.index()==index,"Exact no-op oscillator edit creates undo command");
    edit->setText("987.654321987");edit->setModified(true);emit edit->textEdited(edit->text());
    model->setBypassed(true);ui::Language::instance().set("ru");QCoreApplication::processEvents();
    require(edit->text()=="987.654321987" && model->editState("oscillator0frequency_hz").dirty,"Bypass/language overwrites oscillator draft");
    ui::Language::instance().set("en");
    const auto invalidBefore=node->save();const int invalidIndex=undo.index();
    edit->setText("-1");edit->setModified(true);emit edit->textEdited(edit->text());
    QMetaObject::invokeMethod(edit,"editingFinished",Qt::DirectConnection);
    require(node->save()==invalidBefore && undo.index()==invalidIndex && !model->editState("oscillator0frequency_hz").error.isEmpty(),"Invalid oscillator frequency mutates model or lacks error");
    require(form->findChild<QLabel*>("oscillator0frequency_hz_error")->text().size()>0,"Oscillator validation is not visible");
    model->setDraftInput("oscillator2amplitude","0.123456789",3);
    const auto original=*node->numeric_node();const int removeIndex=undo.index();
    auto* remove=form->findChild<QPushButton*>("oscillator1Remove");require(remove,"Per-row remove action missing");remove->click();
    const auto& remaining=std::get<domain::SignalGeneratorProcessingParameters>(node->numeric_node()->parameters).oscillators;
    require(remaining.size()==2 && remaining[0].id==7 && remaining[1].id==41 && undo.index()==removeIndex+1,"Middle removal renumbers stable oscillator IDs");
    require(model->editState("oscillator1amplitude").input=="0.123456789" && !model->editState("oscillator2amplitude").dirty,"Draft does not follow oscillator ID after middle removal");
    QCoreApplication::processEvents();
    require(form->findChild<QLabel*>("oscillator1Id")->text()=="41","Oscillator row identity not displayed");
    undo.undo();QCoreApplication::processEvents();
    require(domain::same_processing_parameters(original.parameters,node->numeric_node()->parameters),"Undo does not restore exact oscillator bank and IDs");
    require(model->editState("oscillator2amplitude").input=="0.123456789" && !model->editState("oscillator1amplitude").dirty,"Undo does not remap surviving oscillator draft by ID");
}
static void dimensional_editor_acceptance_checks(){
    // Size writers accept one exact representation. Wrong-tag edits must never
    // round-trip via a floating point intermediate or modify the configured DTO.
    for(const auto& kind:numeric_editor::catalog()){
        AudioNodeModel node(kind);auto* model=node.parameter_model();std::unique_ptr<QWidget> form(node.parameter_widget());
        QStringList sizes;
        for(const auto& field:model->fields())if(field.editor==NodeParameterModel::Editor::Literal && field.literal_type==1)sizes.append(field.key);
        for(const auto& key:sizes){
            const auto saved=node.save();const auto value=std::get<signal_contracts::NumericLiteral>(model->field(key)->read());
            const auto text=QString::number(std::get<std::uint64_t>(value));
            auto* type=form->findChild<QComboBox*>(key+"_type");auto* edit=form->findChild<QLineEdit*>(key);
            require(type && edit && type->isHidden() && type->currentIndex()==1,"Size type chooser must be fixed to UInt64");
            for(int unsupported:{0,2,3})require(!model->editLiteral(key,text,unsupported) && node.save()==saved,"Size editor accepted unsupported literal representation");
        }
    }
    AudioNodeModel history("Spectrogram");require(history.parameter_model()->editLiteral("numericCapacity","17",1),"UInt64 History capacity edit rejected");
    require(std::get<domain::HistoryProcessingParameters>(history.numeric_node()->parameters).capacity==17,"Capacity writer does not retain UInt64 size");
    struct Size {const char* kind;const char* key;int type;};
    for(const auto& size:std::array<Size,5>{{{"Window","size",0},{"Mel","bands",0},{"ChannelComparison","window_samples",0},{"IndexExtract","index",0},{"Spectrogram","numericCapacity",1}}}){
        AudioNodeModel node(size.kind);auto* model=node.parameter_model();const auto* field=model->field(size.key);
        require(field && field->literal_type==size.type,"Operation size editor lacks its registered exact writer type");
        const auto saved=node.save();for(int unsupported:{2,3})require(!model->editLiteral(size.key,"512",unsupported) && node.save()==saved,"Operation size accepted a floating point literal");
    }
}
static void grouped_layout_acceptance_checks(){
    const auto registry=inventory_registry();NumericEditorGraphModel graph(registry);QUndoStack undo;graph.set_undo_stack(&undo);
    QtNodes::DataFlowGraphicsScene scene(graph);scene.setNodeGeometry(std::make_unique<EditorNodeGeometry>(graph,[]{return QJsonObject{};}));
    QGraphicsView canvas(&scene);canvas.resize(1000,700);
    const auto source=graph.addNode("AudioSource"),selector=graph.addNode("InputSelector"),router=graph.addNode("Switch");
    const QString longName=QString::fromUtf8("Длинное пользовательское имя потока — генераторы и источник звука для анализа частот");
    for(unsigned channels:{1u,2u,4u,8u}){
        auto* capture=graph.delegateModel<AudioNodeModel>(source);auto physical=*capture->numeric_node();const auto mono=physical.outputs.front();physical.outputs.clear();
        for(unsigned lane=0;lane<channels;++lane){auto port=mono;port.id=lane;physical.outputs.push_back(port);}capture->set_numeric_node(physical);
        capture->set_presentation(QJsonObject{{"display_name",longName}});
        for(auto id:{selector,router}){
            auto* node=graph.delegateModel<AudioNodeModel>(id);auto dto=*node->numeric_node();
            require(id==selector?numeric_editor::set_selector_channels(dto,channels):numeric_editor::set_route_channels(dto,channels),"Configure grouped layout channels");
            require(id==selector?numeric_editor::set_input_count(dto,3):numeric_editor::set_output_count(dto,3),"Configure three grouped streams");node->set_numeric_node(dto);
            auto presentation=node->presentation();presentation["display_name"]=longName;presentation["stream_names"]=QJsonArray{longName,longName+" 2",longName+" 3"};node->set_presentation(presentation);
        }
        for(const auto& language:{QString("ru"),QString("en")})for(const auto& theme:{QString("light"),QString("dark")}){
            ui::Language::instance().set(language);ui::Theme::instance().set(theme);QCoreApplication::processEvents();
            auto& geometry=static_cast<EditorNodeGeometry&>(scene.nodeGeometry());
            for(auto id:{source,selector,router}){
                auto* node=graph.delegateModel<AudioNodeModel>(id);auto* body=node->embeddedWidget();geometry.recomputeSize(id);
                require(geometry.boundingRect(id).contains(QRectF(geometry.widgetPosition(id),body->size())),"Multichannel card clips embedded controls");
                // DefaultNodePainter uses captionRect only for its width, then
                // draws at a separately computed text baseline. Its metric
                // rectangle's origin is not a node-local painted position.
                auto captionFont=QApplication::font();
                const bool nickname= !graph.nodeData(id,QtNodes::NodeRole::Label).toString().isEmpty();
                captionFont.setBold(!nickname);captionFont.setItalic(nickname);
                const QFontMetricsF captionMetrics(captionFont);
                const auto captionText=graph.nodeData(id,QtNodes::NodeRole::Caption).toString();
                const auto textBounds=captionMetrics.boundingRect(captionText);
                const auto baseline=geometry.captionPosition(id);
                const double captionCenter=baseline.x()+geometry.captionRect(id).width()/2.;
                const QRectF paintedCaption(captionCenter-textBounds.width()/2.,baseline.y()-captionMetrics.ascent(),
                                            std::max(textBounds.width(),captionMetrics.horizontalAdvance(captionText)),captionMetrics.height());
                require(geometry.boundingRect(id).contains(paintedCaption),"Long user node name clips its painted header bounds");
                for(auto side:{QtNodes::PortType::In,QtNodes::PortType::Out}){
                    const auto numeric=side==QtNodes::PortType::In?node->numeric_node()->inputs.size():node->numeric_node()->outputs.size();
                    for(unsigned port=0;port<numeric;++port){
                        const auto pin=geometry.portPosition(id,side,port);const auto label=geometry.portTextPosition(id,side,port);const auto text=node->portCaption(side,port);QFontMetricsF metrics(geometry.label_font());
                        for(const auto& line:text.split('\n'))require(label.x()>=0 && label.x()+metrics.horizontalAdvance(line)<=geometry.size(id).width(),"Long RU/EN port caption clips node bounds");
                        require(geometry.checkPortHit(id,side,pin)==port,"Multichannel pin hit loses actual port index");
                        if(node->grouped_channels(side) && port>0){const double spacing=pin.y()-geometry.portPosition(id,side,port-1).y();require(std::abs(spacing-geometry.pitch()-(port%channels==0?24.:0.))<1e-6,"Ports are not compact within a stream and separated between streams");}
                        for(double zoom:{.25,1.,2.}){
                            canvas.setTransform(QTransform::fromScale(zoom,zoom));const auto local=canvas.viewportTransform().inverted().map(QPointF(canvas.mapFromScene(pin)));
                            require(geometry.checkPortHit(id,side,local)==port,"Zoom changes multichannel pin hit routing");
                        }
                        if(id==selector && side==QtNodes::PortType::In){const auto button=geometry.selectorButtonRect(id,port);require(geometry.selectorButtonHit(id,button.center())==port,"Grouped selector label does not select its physical channel");}
                    }
                }
            }
        }
    }
    ui::Language::instance().set("en");ui::Theme::instance().set("dark");
    // Editing C with live data wires must refuse atomically instead of
    // reinterpreting stable port 1 as a different stream/channel.
    NumericEditorGraphModel connected(registry);QUndoStack history;connected.set_undo_stack(&history);
    const auto producer=connected.addNode("AudioSource"),route=connected.addNode("InputSelector");
    connected.addConnection({producer,0,route,1});require(connected.connectionExists({producer,0,route,1}),"Connected selector fixture absent");
    const auto saved=connected.save();const int checkpoint=history.index();auto changed=*connected.delegateModel<AudioNodeModel>(route)->numeric_node();
    require(numeric_editor::set_selector_channels(changed,2),"Candidate grouped C rejected");
    require(!connected.edit_node(route,changed) && connected.save()==saved && history.index()==checkpoint && connected.connectionExists({producer,0,route,1}),"Connected C change reinterprets links or creates an Undo command");
    auto* routeModel=connected.delegateModel<AudioNodeModel>(route);auto presentation=routeModel->presentation();presentation["stream_names"]=QJsonArray{"Generator",longName};routeModel->set_presentation(presentation);
    const int before=history.index();require(routeModel->select_input(1),"Named stream selection failed");require(routeModel->operation_summary().contains(longName),"Selected stream name absent from card summary");
    history.undo();require(routeModel->selected_input()==0 && routeModel->operation_summary().contains("Generator"),"Undo leaves stale selected stream name");history.redo();require(history.index()==before+1 && routeModel->operation_summary().contains(longName),"Redo loses selected stream name");
    NumericEditorGraphModel restored(registry);restored.load(connected.save());auto* restoredRoute=restored.delegateModel<AudioNodeModel>(route);require(restoredRoute && restoredRoute->selected_input()==1 && restoredRoute->operation_summary().contains(longName),"Document reload loses selected stream name");
}
static void bypass_controls_checks(){
    for(const auto& kind:numeric_editor::catalog()){
        AudioNodeModel model(kind);
        std::unique_ptr<QWidget> body(model.embeddedWidget()),inspector(model.parameter_widget());
        auto* inlineBypass=body->findChild<QCheckBox*>("numericBypass");auto* inspectorBypass=inspector->findChild<QCheckBox*>("numericBypass");
        require(inlineBypass && inspectorBypass && model.numeric_node()->enabled && !inlineBypass->isChecked() && !inspectorBypass->isChecked(),"Every new node must expose unchecked bypass with processing enabled");
        auto* form=body->findChild<QWidget*>("numericNodeParameters");
        auto* inspectorForm=inspector->objectName()=="numericNodeParameters"?inspector.get():inspector->findChild<QWidget*>("numericNodeParameters");
        require(form && inspectorForm && form->layout()->itemAt(0)->widget()==inlineBypass && inspectorForm->layout()->itemAt(0)->widget()==inspectorBypass,"Bypass must stay available at the top of both parameter forms");
        if(kind=="ScalarView"){
            const auto ordered=[](QWidget* parameters){auto* wrapper=parameters->parentWidget();return wrapper && wrapper->layout() && wrapper->layout()->count()==2 && wrapper->layout()->itemAt(0)->widget()==parameters && wrapper->layout()->itemAt(1)->widget() && wrapper->layout()->itemAt(1)->widget()->objectName()=="scalarInputsEditor";};
            require(!body->findChild<QWidget*>("scalarInputsEditor") && ordered(inspectorForm),"Scalar wrappers must place the bypass/parameter form before metric input controls");
        }
        require(!model.parameter_model()->field("numericEnabled") && !model.parameter_model()->field("numericBypass") && !inspector->findChild<QAction*>("showOnBlock_numericEnabled") && !inspector->findChild<QAction*>("showOnBlock_numericBypass"),"Bypass must not be a pinnable processing parameter");
        require(!model.set_parameter_on_block("numericBypass",false) && !model.parameter_model()->setOnBlock("numericEnabled",true),"Presentation pinning cannot hide or duplicate bypass");
        auto* activity=body->findChild<QLabel*>("pipelineNodeActivity");require(activity,"Block processing activity indicator absent");
        const auto configured=model.save();model.set_processing_activity(true,true,120.);
        require(activity->text()==ui::text("Activity: processing") && activity->toolTip().contains("120.0"),"Activity indicator ignores measured node telemetry");
        const auto statusText=activity->text();
        for(int i=0;i<100;++i)model.set_processing_activity(true,true,100.+i);
        require(activity->text()==statusText && model.save()==configured,"Execution counter refresh changes status/processing parameters");
        model.set_processing_activity(true,false,0.);require(activity->text()==ui::text("Activity: no processing in interval"),"Inactive interval retains active node indicator");
        model.set_processing_activity(false,false,120.);require(activity->toolTip().contains(QStringLiteral("—")),"Unmeasured activity displays configured target as measured rate");
        const auto presentation=model.presentation();
        inlineBypass->setChecked(true);
        require(!model.numeric_node()->enabled && inspectorBypass->isChecked() && model.presentation()==presentation,"Bypass must invert only the execution mode and synchronize forms");
        inspectorBypass->setChecked(false);
        require(model.numeric_node()->enabled && !inlineBypass->isChecked(),"Inspector must restore processing and synchronize inline bypass");
        model.set_numeric_edit_handler([](const auto&){return false;});inlineBypass->setChecked(true);
        require(model.numeric_node()->enabled && !inlineBypass->isChecked() && !inspectorBypass->isChecked(),"Rejected bypass edit must restore both controls");
    }
}
static void selector_repair_and_names_checks(){
    AudioNodeModel broken("InputSelector");auto malformed=*broken.numeric_node();malformed.outputs.clear();broken.set_numeric_node(malformed);
    const auto before=broken.save();std::unique_ptr<QWidget> brokenBody(broken.embeddedWidget()),brokenInspector(broken.parameter_widget());
    require(std::get<int>(broken.parameter_model()->field("numericInputs")->read())==0,"Empty selector outputs must show unavailable stream geometry safely");
    require(!broken.select_input(99) && broken.save()==before,"Rejected malformed selector selection must be atomic");
    AudioNodeModel mono("InputSelector");auto sparse=*mono.numeric_node();require(numeric_editor::set_input_count(sparse,3) && numeric_editor::remove_input(sparse,1),"Sparse mono selector fixture rejected");mono.set_numeric_node(sparse);
    std::unique_ptr<QWidget> inspector(mono.parameter_widget());auto* retained=inspector->findChild<QLineEdit*>("streamName2");
    require(retained && !inspector->findChild<QLineEdit*>("streamName1"),"Mono stream name controls must follow retained stable port IDs");
    retained->setText("Retained stream");QMetaObject::invokeMethod(retained,"editingFinished",Qt::DirectConnection);
    require(mono.stream_name(2)=="Retained stream" && mono.portCaption(QtNodes::PortType::In,mono.port_index(QtNodes::PortType::In,2)).contains("Retained stream"),"Retained stream rename must update its actual port caption");
}
static void canonical_parameter_checks(){
    AudioNodeModel selector("InputSelector");require(selector.parameter_model()->field("numericSelectorChannels"),"Selector channels field filtered by suffix");
    auto grouped=*selector.numeric_node();require(numeric_editor::set_selector_channels(grouped,2) && numeric_editor::set_input_count(grouped,3),"Grouped selector geometry rejected");selector.set_numeric_node(grouped);require(selector.grouped_channels(QtNodes::PortType::In)==2 && selector.numeric_node()->inputs.size()==6,"Selector requires three groups of two ports");require(selector.portCaption(QtNodes::PortType::In,0).contains(ui::text("Channel %1").arg(1)) && selector.portCaption(QtNodes::PortType::In,1).contains(ui::text("Channel %1").arg(2)),"Selector channel captions lose positions");
    AudioNodeModel router("Switch");auto route=*router.numeric_node();require(numeric_editor::set_route_channels(route,2) && numeric_editor::set_output_count(route,3),"Grouped output switch rejected");router.set_numeric_node(route);require(router.numeric_node()->inputs.size()==2 && router.numeric_node()->outputs.size()==6 && router.grouped_channels(QtNodes::PortType::Out)==2,"Output switch must preserve C channels across S groups");
    AudioNodeModel clock("SampleClock");const auto clockPin=clock.configuration_port(QtNodes::PortType::Out,domain::ConfigurationRole::SampleClock);require(clockPin!=QtNodes::InvalidPortIndex && clock.configuration_role(QtNodes::PortType::Out,clockPin)==domain::ConfigurationRole::SampleClock,"Configuration clock port not exposed");
    AudioNodeModel window("WindowFunction");auto* function=window.parameter_model()->field("function");require(function && function->editor==NodeParameterModel::Editor::Choice && function->options.size()==4,"Window function requires an enumerated editor");require(window.parameter_model()->edit("function",1),"Window choice edit rejected");require(std::get<int>(window.parameter_model()->field("function")->read())==1,"Window choice does not roundtrip canonical string parameter");
    AudioNodeModel history("Spectrogram");auto* capacity=history.parameter_model()->field("numericCapacity");require(capacity && capacity->literal_type==1,"History capacity must retain UInt64 representation");
    AudioNodeModel generator("SignalGenerator");require(generator.parameter_model()->edit("generatorBank1",false),"Generator bank action rejected");require(std::get<domain::SignalGeneratorProcessingParameters>(generator.numeric_node()->parameters).oscillators.size()==8,"Multitone bank not reflected in ordinary parameters");
    std::unique_ptr<QWidget> panel(generator.parameter_widget());require(panel->findChild<QWidget*>("oscillatorTable"),"Generator bank table absent");
    AudioNodeModel capture("AudioSource");int generation=0;capture.set_capture_device_provider([&](int,QString*){QJsonArray devices;for(int i=0;i<generation;++i)devices.append(QJsonObject{{"id",QString::number(i)},{"name",QString("Device %1").arg(i)}});return devices;});generation=2;require(capture.parameter_model()->edit("refreshCaptureDevices",false),"Capture refresh rejected");require(capture.parameter_model()->field("captureDevice")->options.size()==3,"Capture refresh retains old choices");
    AudioNodeModel mixer("Mixer");require(mixer.parameter_model()->field("mixerInputCount")->minimum==1,"Mixer cannot use one independent input");require(mixer.parameter_model()->field("mixerShare0"),"Normalized proportion readout absent");
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
        ui::Language::instance().set("en");catalogue_acceptance_checks();oscillator_bank_acceptance_checks();dimensional_editor_acceptance_checks();grouped_layout_acceptance_checks();bypass_controls_checks();canonical_parameter_checks();selector_repair_and_names_checks();preview_model_checks();
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
        require(capture->text()==ui::text("Audio capture: receiving") && validation->toolTip()==admissionWarning,"Capture recovery overwrites admission warning");
        ui::Language::instance().set("en");require(capture->text()=="Audio capture: receiving","Live capture recovery fails EN refresh");
        emit presenter.component_status_changed("capture",int(pipeline::State::Stopped),{});
        require(capture->isVisibleTo(editor.toolbar()) && capture->text()==ui::text("Audio capture: inactive") && !capture->toolTip().isEmpty() && !capture->toolTip().contains("fixture:") && capture->textInteractionFlags()==Qt::NoTextInteraction && validation->toolTip()==admissionWarning && editor.document()==beforeCapture && editor.history().index()==beforeCaptureUndo,"Inactive capture status loses independence/visibility or mutates draft/history/admission warning");

        auto* library=editor.findChild<QTreeWidget*>("pipelineNodeLibrary");require(library && library->topLevelItemCount()==3,"Sidebar categories differ from shared roles");
        auto* scene=editor.findChild<QtNodes::DataFlowGraphicsScene*>();std::unique_ptr<QMenu> menu(scene->createSceneMenu({9000,9000}));
        auto* context=menu->findChild<QTreeWidget*>("numericContextCatalog");require(context && context->topLevelItemCount()==3,"Context catalog missing shared roles");
        auto kinds=[](QTreeWidget* tree){QStringList result;std::function<void(QTreeWidgetItem*)> visit=[&](auto* item){const auto kind=item->data(0,Qt::UserRole).toString();if(!kind.isEmpty())result.append(kind);for(int i=0;i<item->childCount();++i)visit(item->child(i));};for(int i=0;i<tree->topLevelItemCount();++i)visit(tree->topLevelItem(i));result.sort();return result;};
        auto expectedKinds=numeric_editor::catalog();expectedKinds.sort();require(kinds(context)==kinds(library) && kinds(library)==expectedKinds,"Sidebar/context must expose every catalog leaf in the shared hierarchy");
        {
            const auto source=graph->addNode("AudioSource"),sink=graph->addNode("Converter");auto node=*graph->delegateModel<AudioNodeModel>(sink)->numeric_node();numeric_editor::set_dtype(node,domain::PortDirection::Input,0,signal_contracts::DType::UInt16);graph->edit_node(sink,node);
            const QtNodes::ConnectionId edge{source,0,sink,0};graph->addConnection(edge);scene->connectionGraphicsObject(edge)->setSelected(true);
            auto* policy=editor.findChild<QComboBox*>("connectionConversionPolicy");require(policy && policy->currentIndex()==0 && editor.findChild<QLabel*>("connectionRisk"),"Selecting a dtype edge must expose its actual policy in the existing inspector");
            policy->setCurrentIndex(3);require(graph->connection_policy(edge)==3,"Inspector policy must edit the processing graph");editor.history().undo();require(graph->connection_policy(edge)==0 && policy->currentIndex()==0,"Inspector policy undo must refresh both representations");
            graph->deleteNode(sink);graph->deleteNode(source);
        }
        {
            QJsonArray devices{QJsonObject{{"id","default-mono"},{"is_default",true},{"native_sample_rate",48000},{"native_channels",1}},
                QJsonObject{{"id","saved-three"},{"is_default",false},{"native_sample_rate",44100},{"native_channels",3}}};
            graph->set_capture_device_provider([&](int,QString*){return devices;});
            domain::NumericProcessingGraph pinned;pinned.graph_id="main";
            auto source=numeric_editor::default_node("AudioSource");source.id=1;
            auto view=numeric_editor::default_node("AudioView");view.id=2;view.inputs[0].contract=source.outputs[0].contract;
            pinned.nodes={source,view};pinned.edges={{1,2,0,0}};QString error;
            require(desktop::resolve_numeric_source_format(pinned,1,44100,3,&error),"Prepare saved three-output contract");
            pinned.edges[0].output_port=2;
            const QJsonObject adapters{{"1",QJsonObject{{"device_identity","saved-three"}}}};
            require(editor.load_numeric_draft(pinned,&error,adapters),"Atomic widget pinned graph and adapter import");
            domain::NumericProcessingGraph applied;
            require(editor.numeric_draft(applied,&error) && applied.edges==pinned.edges && applied.nodes[0].outputs.size()==3 &&
                graph->delegateModel<AudioNodeModel>(1)->adapter().value("device_identity")=="saved-three",
                "Widget load rewired pinned output three to default mono");
            const auto before=editor.document();const auto undoIndex=editor.history().index(),undoCount=editor.history().count();
            devices[1]=QJsonObject{{"id","saved-three"},{"is_default",false},{"native_sample_rate",7999},{"native_channels",3}};
            require(!editor.load_numeric_draft(pinned,&error,adapters) && editor.document()==before &&
                editor.history().index()==undoIndex && editor.history().count()==undoCount &&
                graph->delegateModel<AudioNodeModel>(1)->adapter().value("device_identity")=="saved-three",
                "Rejected widget pinned import changed graph, bindings or Undo");
            graph->set_capture_device_provider({});
        }
        auto converter=numeric_editor::default_node("Converter");converter.id=3;
        std::get<domain::ConverterProcessingParameters>(converter.parameters).policy.nan_replacement=std::uint64_t{9007199254740993ULL};
        domain::NumericProcessingGraph draft;draft.graph_id="main";draft.nodes.push_back(converter);QString error;
        require(editor.load_numeric_draft(draft,&error),"Widget canonical load failed");domain::NumericProcessingGraph exported;
        require(editor.numeric_draft(exported,&error) && exported.nodes.size()==1 && exported.nodes[0].id==3,"Widget export lost owning node/id");
        require(!editor.toolbar()->findChild<QLineEdit*>("pipelineAnalysisRate") && editor.toolbar()->findChild<QLabel*>("pipelineProcessingRate"),"Toolbar must show measured rate, with editable target in settings only");
        auto* captureStatus=editor.toolbar()->findChild<QLabel*>("pipelineCaptureStatus");require(captureStatus && !captureStatus->toolTip().isEmpty() && captureStatus->textInteractionFlags()==Qt::NoTextInteraction,"Capture must be a descriptive noninteractive status");
        std::unique_ptr<QWidget> executionSettings(editor.execution_settings_widget());
        auto* rate=executionSettings->findChild<QLineEdit*>("pipelineAnalysisRate");require(rate,"Execution settings missing rational rate editor");
        const auto interval=graph->numeric_graph().execution_interval;const auto rateUndo=editor.history().index();
        rate->setText("24000/1001");emit rate->editingFinished();
        require(graph->numeric_graph().execution_interval.numerator==1001 && graph->numeric_graph().execution_interval.denominator==24000 && editor.history().index()==rateUndo+1,"Rational analysis rate loses exact numerator/denominator or undo");
        editor.history().undo();require(graph->numeric_graph().execution_interval==interval,"Rate settings undo fails");
        rate->setText("120/0");emit rate->editingFinished();require(graph->numeric_graph().execution_interval==interval && editor.history().index()==rateUndo,"Malformed settings rate changes graph");
        auto sixty=draft;sixty.execution_interval={1,60};require(editor.load_numeric_draft(sixty) && rate->text()=="60","Loading a graph leaves existing rate settings on previous target");
        require(editor.load_numeric_draft(draft) && rate->text()=="120","Restoring graph does not synchronize persistent rate settings");

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
        auto* inspector=editor.findChild<QWidget*>("pipelineInspector");
        auto* preview=inspector->findChild<QWidget*>("pipelineDataStatus");auto* form=inspector->findChild<QWidget*>("numericNodeParameters");
        require(preview && form && preview->parentWidget()->layout()->indexOf(preview)<preview->parentWidget()->layout()->indexOf(form) &&
            preview->parentWidget()->layout()->indexOf(form)<preview->parentWidget()->layout()->indexOf(renameBlock),"Inspector data availability and parameters must precede rename/options");
        auto* textEditor=editor.findChild<QLineEdit*>("numericNan");require(textEditor,"Exact numeric text editor absent");
        const auto clipboardNodeCount=graph->allNodeIds().size();textEditor->setFocus();textEditor->selectAll();
        QTest::keyClick(textEditor,Qt::Key_C,Qt::ControlModifier);
        require(QApplication::clipboard()->text()=="9007199254740993" && graph->allNodeIds().size()==clipboardNodeCount,"Copy inside a parameter field copied graph nodes instead of text");
        const auto renameUndo=editor.history().index();
        QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Exact converter");dialog->accept();}});
        renameBlock->click();require(converterModel->caption()=="Exact converter" && converterModel->name()=="Converter" && editor.history().index()==renameUndo+1,"Inspector rename did not update user caption with one undo command");
        editor.history().undo();require(converterModel->caption()!="Exact converter","Inspector rename undo did not restore default");editor.history().redo();
        require(editor.save_preset("exact snapshot"),"Canonical preset save failed");const auto preset=state.file_configs.ui.pipeline_presets.last().toObject();
        rate->setText("90");emit rate->editingFinished();
        graph->deleteNode(3);require(editor.load_preset(preset["id"].toString()),"Canonical preset reload failed");
        require(rate->text()=="120" && graph->numeric_graph().execution_interval==interval,"Preset replacement leaves stale persistent analysis target");
        require(editor.numeric_draft(exported,&error),qPrintable(QString("Preset export failed: ")+error));
        const auto& replacement=std::get<domain::ConverterProcessingParameters>(exported.nodes[0].parameters).policy.nan_replacement;
        require(std::holds_alternative<std::uint64_t>(replacement),"Preset changes UInt64 literal tag");
        require(std::get<std::uint64_t>(replacement)==9007199254740993ULL,qPrintable(QString("Preset rounds exact UInt64 literal to ")+QString::number(std::get<std::uint64_t>(replacement))));
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
