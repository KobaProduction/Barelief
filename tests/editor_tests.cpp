#include "synthetic_engine_provider.h"
#include "core_app/gui_presenter.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "visualization/ui_text.h"
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QComboBox>
#include <QTreeWidget>
#include <QLineEdit>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QLayout>
#include <QMenu>
#include <QClipboard>
#include <QMimeData>
#include <QSplitter>
#include <QSplitterHandle>
#include <QContextMenuEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDataStream>
#include <QTcpServer>
#include "core/network/tcp_packet_socket.h"
#include <QUndoStack>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <stdexcept>
#include <limits>
#include <cmath>
#include <QDir>
#include <QScrollArea>
#include <QCheckBox>
#include <QStyle>
#include <QStyleOptionButton>
static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static void move_mouse(QWidget *widget, QPoint point) {
    QMouseEvent event(QEvent::MouseMove,QPointF(point),QPointF(widget->mapToGlobal(point)),
                      Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(widget,&event);
}
static void drag(QWidget *widget, QPoint from, QPoint to) {
    QTest::mousePress(widget,Qt::LeftButton,Qt::NoModifier,from);
    move_mouse(widget,to); QTest::mouseRelease(widget,Qt::LeftButton,Qt::NoModifier,to);
}
static void click_checkbox(QCheckBox *checkbox) {
    require(checkbox,"Missing checkbox");
    QStyleOptionButton option; option.initFrom(checkbox); option.text=checkbox->text();
    const auto indicator=checkbox->style()->subElementRect(QStyle::SE_CheckBoxIndicator,&option,checkbox);
    QTest::mouseClick(checkbox,Qt::LeftButton,Qt::NoModifier,indicator.center());
}
static void node_output_controls() {
    for(const auto &definition:pipeline::node_definitions()) {
        AudioNodeModel model(definition.name);
        require(model.save().value("enabled")==true,"Node output default is not enabled");
        std::unique_ptr<QWidget> controls(model.parameter_widget());
        require(controls->findChild<QCheckBox *>("enabled"),"Generated node output control missing");
    }
    AudioNodeModel model("WLED");
    auto parameters=model.save(); parameters["extension"]=QJsonArray{1,2,3}; model.load(parameters);
    std::unique_ptr<QWidget> inspector(model.parameter_widget());
    std::unique_ptr<QWidget> embedded(model.embeddedWidget());
    auto *toggle=inspector->findChild<QCheckBox *>("enabled");
    auto *embedded_toggle=embedded->findChild<QCheckBox *>("enabled");
    inspector->show(); QTest::qWait(10);
    model.preview({2048,0});
    require(embedded->findChild<QLabel *>("previewValues")->text()=="2048" &&
            embedded->findChild<QLabel *>("previewPeak")->text()=="0","Zero peak was treated as unavailable");
    const auto unchanged=model.save();
    model.preview({2048,std::numeric_limits<float>::quiet_NaN()});
    require(model.save()==unchanged && embedded->findChild<QLabel *>("previewPeak")->text()==QStringLiteral("—"),
            "Nonfinite preview polluted parameters or rendered a numeric value");
    click_checkbox(toggle);
    require(!model.save().value("enabled").toBool() && !embedded_toggle->isChecked(),"Output toggle did not synchronize controls");
    require(embedded->findChild<QLabel *>("previewStatus")->text()==ui::text("Output disabled") &&
            embedded->findChild<QLabel *>("previewValues")->text()==QStringLiteral("—"),"Muted output retained available telemetry");
    auto expected=parameters; expected["enabled"]=false;
    require(model.save()==expected,"Output toggle changed unrelated parameters");
    AudioNodeModel restored("WLED"); restored.load(model.save());
    std::unique_ptr<QWidget> restored_controls(restored.parameter_widget());
    require(!restored_controls->findChild<QCheckBox *>("enabled")->isChecked() && restored.save()==expected,
            "Saved output gate was not restored");
    click_checkbox(toggle);
    require(model.save()==parameters && embedded_toggle->isChecked(),"Reenabled output did not restore exact configuration");
    require(embedded->findChild<QLabel *>("previewStatus")->text()==ui::text("Waiting for data"),"Reenabled output reused stale telemetry");
    AudioNodeModel function("WindowFunction");
    std::unique_ptr<QWidget> functions(function.parameter_widget());
    auto *window=functions->findChild<QComboBox *>("function");
    require(window && window->count()==4,"Window function metadata did not expose supported algorithms");
    window->setCurrentIndex(window->findData("hamming"));
    require(function.save().value("function")=="hamming","Window function selection did not persist");
    qInfo("Editor: node output toggles, read-only telemetry and window parameter checks passed");
}
static void selection_and_theme(SchemeEditorWidget &editor, QtNodes::DataFlowGraphModel &graph,
                                QtNodes::DataFlowGraphicsScene &scene) {
    auto *view=editor.findChild<QtNodes::GraphicsView *>();
    const auto previous_theme=ui::Theme::instance().code();
    const auto previous_center=view->mapToScene(view->viewport()->rect().center());
    QtNodes::NodeId mel=QtNodes::InvalidNodeId, wled=QtNodes::InvalidNodeId;
    for (auto id:graph.allNodeIds()) {
        const auto name=graph.delegateModel<AudioNodeModel>(id)->name();
        if(name=="MelView") mel=id;
        if(name=="WLED") wled=id;
    }
    require(mel!=QtNodes::InvalidNodeId && wled!=QtNodes::InvalidNodeId,"Missing selection fixtures");
    auto *model=graph.delegateModel<AudioNodeModel>(wled);
    auto parameters=model->save(); parameters["extension"]=QJsonObject{{"preserved",true}};
    model->load(parameters);
    for(const auto &theme:QStringList{"light","dark","light"}) {
        require(ui::Theme::instance().set(theme),"Cannot switch editor theme");
        QTest::qWait(20);
        for(int repetition=0;repetition<8;++repetition) for(auto id:{mel,wled}) {
            const auto bounds=scene.nodeGraphicsObject(id)->sceneBoundingRect();
            view->centerOn(bounds.center());
            QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,
                              view->mapFromScene(bounds.topLeft()+QPointF(45,18)));
            QApplication::processEvents();
            if(id==wled) {
                auto *inspector=editor.findChild<QScrollArea *>("pipelineInspector");
                auto *spin=inspector->findChild<QDoubleSpinBox *>("gain");
                require(spin && spin->value()==parameters.value("gain").toDouble(),"WLED selection lost numeric gain");
                auto *line=spin->findChild<QLineEdit *>();
                require(line && !line->text().contains("null") && line->text()==spin->text(),"WLED gain displays null");
                spin->setFocus(); QTest::keyClick(spin,Qt::Key_Tab);
                model->preview({16,std::numeric_limits<float>::quiet_NaN()});
                require(model->save()==parameters,"Selection or preview polluted WLED parameters");
                require(scene.nodeGraphicsObject(wled)->opacity()==1,"Node body remains translucent");
                require(model->embeddedWidget()->palette().color(QPalette::Window)==QApplication::palette().color(QPalette::Window),
                        "Embedded controls retained the previous theme");
            }
        }
        // Render an empty portion after the first paint and a theme change: the
        // cached QtNodes background brush used to survive and make a white grid.
        view->centerOn(90000,90000); QApplication::processEvents();
        const auto image=view->viewport()->grab().toImage();
        const auto background=QApplication::palette().color(QPalette::Window);
        int grid_pixels=0,total=0;
        for(int y=10;y<image.height()-10;++y) for(int x=10;x<image.width()-10;++x) {
            const auto color=image.pixelColor(x,y); ++total;
            const int distance=std::max({std::abs(color.red()-background.red()),std::abs(color.green()-background.green()),
                                       std::abs(color.blue()-background.blue())});
            require(distance<=30,"Editor grid has excessive contrast or a stale background");
            if(distance>0) ++grid_pixels;
        }
        require(total>0 && grid_pixels<total/8,"Editor grid is too dense");
        const auto screenshots=qEnvironmentVariable("QT_VIS_EDITOR_SCREENSHOT_DIR");
        if(!screenshots.isEmpty()) { QDir().mkpath(screenshots); image.save(screenshots+"/editor-grid-"+theme+".png"); }
    }
    for(const auto value:graph.save().value("nodes").toArray())
        if(value.toObject().value("id").toInt()==int(wled))
            require(value.toObject().value("internal-data").toObject()==parameters,"Saved WLED parameters changed after selection");
    ui::Theme::instance().set(previous_theme.isEmpty()?"system":previous_theme);
    view->centerOn(previous_center);
    qInfo("Editor: repeated Mel/WLED selection, saved parameters and light/dark grid checks passed");
}
// Real BDSP transport, but no runtime or audio endpoint: verify preset command
// ordering and start intent without opening capture on the test computer.
class PresetEnginePeer final : public QTcpServer {
public:
    QJsonObject active=pipeline::GraphProgram::defaults();
    std::vector<pipeline::EngineCommand::Kind> commands;
    quint64 revision=1;
protected:
    void incomingConnection(qintptr descriptor) override {
        auto *peer=new TcpBDSPSocket(this);
        require(peer->setSocketDescriptor(descriptor),"Cannot accept preset test peer");
        connect(peer,&TcpBDSPSocket::packetReceived,this,[this,peer](quint8 packet,QByteArray bytes) {
            require(packet==5,"Unexpected preset test packet");
            QDataStream input(bytes); input.setVersion(QDataStream::Qt_6_0);
            input.setByteOrder(QDataStream::LittleEndian); input.setFloatingPointPrecision(QDataStream::SinglePrecision);
            quint16 version,port; quint64 id,expected; quint8 kind; qint32 value; quint32 subscriptions;
            QString interface_address,destination; QByteArray graph;
            input>>version>>id>>kind>>expected>>value>>subscriptions>>interface_address>>destination>>port>>graph;
            require(version==1 && input.status()==QDataStream::Ok,"Invalid preset command");
            commands.push_back(pipeline::EngineCommand::Kind(kind));
            if(kind==pipeline::EngineCommand::ApplyGraph) {active=QJsonDocument::fromJson(graph).object();++revision;}
            QByteArray reply; QDataStream output(&reply,QIODevice::WriteOnly);
            output.setVersion(QDataStream::Qt_6_0); output.setByteOrder(QDataStream::LittleEndian);
            output.setFloatingPointPrecision(QDataStream::SinglePrecision);
            output<<quint16(1)<<id<<true<<QString{}<<quint64(123)<<revision<<revision<<quint64(0)<<quint64(0)
                <<QJsonDocument(active).toJson(QJsonDocument::Compact)<<quint32(0)<<false;
            require(peer->send_packet(6,reply),"Cannot send preset ACK");
        });
    }
};
static void held_key_pan_checks(bool overlay_only=false,bool clipboard_only=false) {
    require(qApp->platformName()=="offscreen","Pan-only checks require QT_QPA_PLATFORM=offscreen");
    PresetEnginePeer peer; require(peer.listen(QHostAddress::LocalHost,0),"Cannot listen for pan test peer");
    GuiState state; state.file_configs.ui.autosave_enabled=false; state.file_configs.ui.pipeline_auto_apply=false;
    state.file_configs.scene_graph=peer.active; state.file_configs.pipeline_graph=peer.active;
    state.delegate_model_registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    for(const auto &definition:pipeline::node_definitions())
        state.delegate_model_registry->registerModel([kind=definition.name] {return std::make_unique<AudioNodeModel>(kind);},definition.category);
    state.data_flow_graph=std::make_shared<QtNodes::DataFlowGraphModel>(state.delegate_model_registry);
    state.data_flow_graph->load(peer.active);
    GuiPresenter presenter(state, test_fixture::synthetic_engine_provider(), "127.0.0.1",peer.serverPort()); presenter.initialize();
    // Establish the initial remote document before measuring selection effects.
    // A fixed delay can let the initial snapshot/re-layout arrive during selection.
    QElapsedTimer handshake;handshake.start();
    while(!presenter.client().snapshot().epoch && handshake.elapsed()<1000) QTest::qWait(5);
    require(presenter.client().snapshot().epoch!=0,"Pan peer handshake did not complete");
    SchemeEditorWidget editor(presenter); editor.resize(1000,700); editor.show();
    auto *view=editor.findChild<QtNodes::GraphicsView *>();
    QApplication::setActiveWindow(&editor); view->setFocus(); QTest::qWait(20);
    require(view->hasFocus(),"Offscreen view did not receive Qt focus");
    auto center=[&] {return view->mapToScene(view->viewport()->rect().center());};
    if(clipboard_only) {
        auto *scene=editor.findChild<QtNodes::DataFlowGraphicsScene *>();
        auto &graph=*state.data_flow_graph;
        auto *model=graph.delegateModel<AudioNodeModel>(1);
        auto parameters=model->save(); parameters["clipboard-extension"]=73; model->load(parameters);
        scene->clearSelection(); scene->nodeGraphicsObject(1)->setSelected(true); view->setFocus();
        const int undo=scene->undoStack().count(); const auto count=graph.allNodeIds().size();
        QTest::keyClick(view,Qt::Key_C,Qt::ControlModifier);
        const auto *mime=QApplication::clipboard()->mimeData();
        require(mime && mime->hasFormat("application/qt-nodes-graph"),"Ctrl+C did not copy graph MIME");
        auto copied=QJsonDocument::fromJson(mime->data("application/qt-nodes-graph")).object();
        require(copied.value("nodes").toArray().size()==1 && copied.value("connections").toArray().isEmpty(),
                "Copy retained incident edges to unselected nodes");
        require(scene->undoStack().count()==undo,"Copy polluted undo stack");
        const QPoint pointer(85,95); const auto target=view->mapToScene(pointer);
        QMouseEvent move(QEvent::MouseMove,QPointF(pointer),QPointF(view->viewport()->mapToGlobal(pointer)),
                         Qt::NoButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(view->viewport(),&move);
        QTest::keyClick(view,Qt::Key_V,Qt::ControlModifier);
        require(graph.allNodeIds().size()==count+1 && scene->undoStack().count()==undo+1,"Ctrl+V did not paste with one undo");
        QtNodes::NodeId pasted=QtNodes::InvalidNodeId;
        for(auto *item:scene->selectedItems()) if(auto *node=qgraphicsitem_cast<QtNodes::NodeGraphicsObject *>(item)) pasted=node->nodeId();
        require(pasted!=QtNodes::InvalidNodeId && graph.delegateModel<AudioNodeModel>(pasted)->save()==parameters,
                "Paste lost node parameters or unknown fields");
        require(QLineF(graph.nodeData(pasted,QtNodes::NodeRole::Position).toPointF(),target).length()<1,
                "Paste used desktop cursor instead of viewport pointer");
        scene->undoStack().undo(); require(graph.allNodeIds().size()==count,"Paste undo failed");
        scene->undoStack().redo(); require(graph.allNodeIds().size()==count+1,"Paste redo failed");
        scene->undoStack().undo();
        QLineEdit field(&editor); field.show(); field.setText("native text"); field.selectAll(); field.setFocus();
        QTest::keyClick(&field,Qt::Key_C,Qt::ControlModifier); field.clear();
        QTest::keyClick(&field,Qt::Key_V,Qt::ControlModifier);
        require(field.text()=="native text" && graph.allNodeIds().size()==count,"Native text copy/paste was intercepted");
        field.hide(); view->setFocus();
        QtNodes::NodeId graph_id=QtNodes::InvalidNodeId;
        for(auto id:graph.allNodeIds()) if(pipeline::node_definition(graph.delegateModel<AudioNodeModel>(id)->name()).category=="views") {graph_id=id;break;}
        require(graph_id!=QtNodes::InvalidNodeId,"Missing graph activation fixture");
        QSignalSpy focus(&editor,&SchemeEditorWidget::graph_view_requested), opened(&editor,&SchemeEditorWidget::graph_open_requested);
        const auto bounds=scene->nodeGraphicsObject(graph_id)->sceneBoundingRect(); view->centerOn(bounds.center());
        const auto point=view->mapFromScene(bounds.topLeft()+QPointF(45,18));
        QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,point);
        require(focus.count()==1 && opened.isEmpty() && view->hasFocus(),"Single graph click did not preserve editor focus");
        QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::AltModifier,point);
        require(opened.isEmpty() && focus.count()==1,"Alt click unexpectedly opened or activated a graph");
        for(int placement=0;placement<3;++placement) {
            bool menu_seen=false;
            QTimer::singleShot(0,[&] {
                auto *menu=qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if(!menu) return;
                menu_seen=true;
                if(auto *action=menu->findChild<QAction *>(QStringLiteral("openGraphPlacement%1").arg(placement))) action->trigger();
                menu->close();
            });
            QContextMenuEvent context(QContextMenuEvent::Mouse,point,view->viewport()->mapToGlobal(point));
            QApplication::sendEvent(view->viewport(),&context);
            require(menu_seen && opened.count()==placement+1 && opened.last()[0].toInt()==int(graph_id) &&
                    opened.last()[1].toInt()==placement,"Viewport right-click did not open placement menu");
        }
        require(editor.findChild<QMenu *>("pipelinePresetsMenu")->title()==ui::text("Presets"),"Presets menu title empty");
        require(std::count(peer.commands.begin(),peer.commands.end(),pipeline::EngineCommand::ApplyGraph)==0,"Clipboard checks dispatched DSP");
        qInfo("Editor offscreen clipboard and graph click checks passed"); return;
    }
    if(overlay_only) {
        auto *scene=editor.findChild<QtNodes::DataFlowGraphicsScene *>();
        auto *inspector=editor.findChild<QScrollArea *>("pipelineInspector");
        auto *library=editor.findChild<QTreeWidget *>("pipelineNodeLibrary");
        const auto size=view->viewport()->size();
        const auto origin=center();
        auto *splitter=editor.findChild<QSplitter *>("pipelineSplitter");
        require(splitter && splitter->count()==3 && inspector->parentWidget()==splitter && inspector->isVisible() &&
                inspector->findChild<QLabel *>("pipelineInspectorPlaceholder"),"Inspector is not a permanent third pane");
        const auto graph_before=state.data_flow_graph->save();
        for(auto id:{0,1,8}) {
            scene->clearSelection(); scene->nodeGraphicsObject(id)->setSelected(true);
            emit scene->nodeSelected(id); QApplication::processEvents();
            require(inspector->isVisible() && view->viewport()->size()==size && QLineF(origin,center()).length()<1,
                    "Inspector selection resized viewport or moved the camera");
            scene->clearSelection(); QApplication::processEvents();
            require(inspector->isVisible() && inspector->findChild<QLabel *>("pipelineInspectorPlaceholder") &&
                    view->viewport()->size()==size && QLineF(origin,center()).length()<1,
                    "Clearing selection moved the camera");
        }
        auto *handle=splitter->handle(2); const auto before_collapse=center();
        QTest::mouseClick(handle,Qt::LeftButton); QApplication::processEvents();
        require(splitter->sizes()[2]==0 && QLineF(before_collapse,center()).length()<4/view->getScale(),"Handle click did not collapse or preserve camera");
        emit scene->nodeSelected(8); QApplication::processEvents();
        require(splitter->sizes()[2]==0,"Node selection reopened collapsed inspector");
        QTest::mouseClick(handle,Qt::LeftButton); QApplication::processEvents();
        require(splitter->sizes()[2]>0,"Divider click did not restore collapsed inspector");
        const QPoint handle_point=handle->rect().center();
        QTest::mousePress(handle,Qt::LeftButton,Qt::NoModifier,handle_point);
        move_mouse(handle,handle_point+QPoint(35,0));
        QTest::mouseRelease(handle,Qt::LeftButton,Qt::NoModifier,handle_point+QPoint(35,0));
        require(splitter->sizes()[2]>0,"Handle drag toggled collapse");
        scene->clearSelection();
        require(state.data_flow_graph->save()==graph_before,"Inspector changed graph contents");
        QSignalSpy graph_requested(&editor,&SchemeEditorWidget::graph_view_requested);
        int view_nodes=0;
        for(auto id:state.data_flow_graph->allNodeIds()) {
            const auto *model=state.data_flow_graph->delegateModel<AudioNodeModel>(id);
            const bool is_view=pipeline::node_definition(model->name()).category=="views";
            bool marker=false;
            for(auto *child:scene->nodeGraphicsObject(id)->childItems())
                if(child->toolTip()==ui::text("Double-click to open graph")) marker=true;
            require(marker==is_view,"Graph indicator is missing or attached to a processing node");
            const int before=graph_requested.count(); emit scene->nodeDoubleClicked(id);
            require(graph_requested.count()==before+(is_view?1:0),"Node activation emitted incorrect graph request");
            if(is_view) { ++view_nodes; require(graph_requested.last()[0].toInt()==int(id),"Graph request used the wrong node ID"); }
        }
        require(view_nodes>0,"Activation fixture has no view nodes");
        const auto theme_before=ui::Theme::instance().code();
        for(const auto &theme:QStringList{"dark","light"}) {
            ui::Theme::instance().set(theme); QApplication::processEvents();
            require(library->viewport()->backgroundRole()==QPalette::Window &&
                    library->viewport()->palette().color(QPalette::Window)==QApplication::palette().color(QPalette::Window),
                    "Node library did not inherit central theme");
            std::unique_ptr<QMenu> menu(scene->createSceneMenu({90000,90000}));
            auto *tree=menu->findChild<QTreeWidget *>(); require(tree,"Context node tree missing");
            const QMap<QString,const char *> category_labels{{"inputs","Inputs"},{"transforms","Transforms"},
                {"views","Graphs"},{"outputs","Outputs"}};
            require(tree->topLevelItemCount()==4,"Context categories changed");
            for(int i=0;i<tree->topLevelItemCount();++i) {
                auto *category=tree->topLevelItem(i); const auto stable=category->data(0,Qt::UserRole).toString();
                require(category_labels.contains(stable) && category->text(0)==ui::text(category_labels.value(stable)),
                        "Context category was not localized without changing its stable key");
                for(int j=0;j<category->childCount();++j)
                    require(!pipeline::node_definition(category->child(j)->text(0)).name.isEmpty(),"Context model ID was translated");
            }
        }
        ui::Theme::instance().set(theme_before.isEmpty()?"system":theme_before);
        auto *bar=editor.layout()->itemAt(0)->layout();
        auto *arrange=editor.findChild<QPushButton *>("arrangePipeline");
        auto *fit=editor.findChild<QPushButton *>("fitPipeline");
        auto *zoom=editor.findChild<QDoubleSpinBox *>("pipelineZoom");
        require(bar->indexOf(arrange)+1==bar->indexOf(fit) && bar->indexOf(fit)<bar->indexOf(zoom),"Toolbar navigation order is wrong");
        require(!arrange->text().contains("Alt+A") && !fit->text().contains("Alt+F") &&
                arrange->toolTip().contains("Alt+A") && fit->toolTip().contains("Alt+F"),"Navigation hints do not show shortcuts");
        require(editor.arrange_action()==view->findChild<QAction *>("arrangePipelineShortcut") &&
                editor.center_action()==view->findChild<QAction *>("fitPipelineShortcut"),"Menu action API did not expose the scoped actions");
        view->setFocus();
        const int undo=scene->undoStack().count(); QTest::keyClick(view,Qt::Key_A,Qt::AltModifier);
        require(scene->undoStack().count()==undo+1,"Scoped Alt+A did not arrange");
        view->centerOn(90000,90000); QTest::keyClick(view,Qt::Key_F,Qt::AltModifier);
        require(QLineF(center(),QPointF(90000,90000)).length()>100,"Scoped Alt+F did not center");
        emit scene->nodeSelected(8); QApplication::processEvents();
        auto *gain=inspector->findChild<QDoubleSpinBox *>("gain"); require(gain,"Missing inspector input");
        gain->setFocus(); const int typing_undo=scene->undoStack().count();
        QTest::keyClick(gain,Qt::Key_A,Qt::AltModifier);
        require(scene->undoStack().count()==typing_undo,"Arrange intercepted inspector input");
        fit->setFocus(); const auto outside=center(); QTest::keyClick(fit,Qt::Key_F,Qt::AltModifier);
        require(QLineF(outside,center()).length()<1,"Center shortcut fired outside canvas");
        editor.resize(470,330); QApplication::processEvents();
        require(splitter->count()==3 && splitter->widget(2)==inspector,"Resize detached inspector pane");
        require(std::count(peer.commands.begin(),peer.commands.end(),pipeline::EngineCommand::ApplyGraph)==0,"Overlay UI dispatched a DSP graph");
        qInfo("Editor offscreen permanent inspector and scoped shortcut checks passed"); return;
    }
    auto key=[&](QEvent::Type type,int value,quint32 scan=0,bool repeat=false) {
        QKeyEvent event(type,value,Qt::NoModifier,scan,0,0,{},repeat);
        QApplication::sendEvent(view,&event);
    };
    const auto graph_before=state.data_flow_graph->save();
    const auto start=center();
    key(QEvent::KeyPress,Qt::Key_W);
    require(QLineF(start,center()).length()<2,"Key press still moves in chunks");
    QTest::qWait(112); const auto axis=center();
    const double axis_pixels=QLineF(start,axis).length()*view->getScale();
    require(axis.y()<start.y() && axis_pixels>15 && axis_pixels<80,"Held key did not pan at bounded speed");
    for(int i=0;i<12;++i) key(QEvent::KeyPress,Qt::Key_W,0,true);
    key(QEvent::KeyRelease,Qt::Key_W,0,true);
    require(QLineF(axis,center()).length()<2,"Autorepeat added chunks");
    QTest::qWait(32); require(center().y()<axis.y()-3,"Autorepeat release stopped a held key");
    key(QEvent::KeyRelease,Qt::Key_W); const auto released=center();
    QTest::qWait(48); require(QLineF(released,center()).length()<2,"Release did not stop pan");
    const auto diagonal_start=center();
    key(QEvent::KeyPress,Qt::Key_W); key(QEvent::KeyPress,Qt::Key_D); QTest::qWait(112);
    key(QEvent::KeyRelease,Qt::Key_W); key(QEvent::KeyRelease,Qt::Key_D);
    const auto diagonal=center()-diagonal_start;
    const double ratio=std::hypot(diagonal.x(),diagonal.y())/QLineF(start,axis).length();
    require(diagonal.x()>10 && diagonal.y()<-10 && ratio>0.65 && ratio<1.3,"Diagonal pan is not normalized");
#ifdef Q_OS_WIN
    const auto physical_start=center(); key(QEvent::KeyPress,0x0426,0x11); QTest::qWait(48);
    key(QEvent::KeyRelease,0x0426,0x11);
    require(center().y()<physical_start.y()-5,"Physical W scan failed with a Cyrillic key");
#endif
    key(QEvent::KeyPress,Qt::Key_D); QFocusEvent focus_out(QEvent::FocusOut);
    QApplication::sendEvent(view,&focus_out); const auto unfocused=center();
    QTest::qWait(48); require(QLineF(unfocused,center()).length()<2,"Focus loss did not stop pan");
    key(QEvent::KeyPress,Qt::Key_D); editor.hide(); const auto hidden=center();
    QTest::qWait(48); require(QLineF(hidden,center()).length()<2,"Hide did not stop pan");
    editor.show(); QApplication::setActiveWindow(&editor); view->setFocus(); QTest::qWait(20);
    QLineEdit text(&editor); text.show(); text.setFocus();
    const auto typing=center(); key(QEvent::KeyPress,Qt::Key_D); QTest::qWait(48);
    require(QLineF(typing,center()).length()<2,"Text focus did not guard WASD");
    require(state.data_flow_graph->save()==graph_before,"Panning changed graph parameters or coordinates");
    require(std::count(peer.commands.begin(),peer.commands.end(),pipeline::EngineCommand::ApplyGraph)==0,
            "Panning dispatched a DSP graph command");
    qInfo("Editor offscreen held-key pan checks passed");
}
static void standard_presets() {
    PresetEnginePeer peer; require(peer.listen(QHostAddress::LocalHost,0),"Cannot listen for preset test peer");
    GuiState state;
    state.file_configs.core.run_loopback=false; state.file_configs.core.run_analyser=false;
    state.file_configs.ui.autosave_enabled=false;
    state.file_configs.scene_graph=pipeline::GraphProgram::defaults();
    state.file_configs.pipeline_graph=state.file_configs.scene_graph;
    state.delegate_model_registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    for(const auto &definition:pipeline::node_definitions())
        state.delegate_model_registry->registerModel([kind=definition.name] {return std::make_unique<AudioNodeModel>(kind);});
    state.data_flow_graph=std::make_shared<QtNodes::DataFlowGraphModel>(state.delegate_model_registry);
    auto &graph=*state.data_flow_graph; graph.load(state.file_configs.scene_graph);
    GuiPresenter presenter(state, test_fixture::synthetic_engine_provider(), "127.0.0.1",peer.serverPort()); presenter.initialize(); QTest::qWait(100);
    require(presenter.client().snapshot().epoch!=0,"Preset peer handshake did not complete");
    SchemeEditorWidget editor(presenter); editor.resize(1500,900); editor.show(); QTest::qWait(60);
    auto *scene=editor.findChild<QtNodes::DataFlowGraphicsScene *>();
    require(!editor.findChild<QCheckBox *>("pipelineCapture") && !editor.findChild<QCheckBox *>("pipelineAnalyzer"),
            "Execution toggles remain in the editor toolbar");
    const auto defaults=pipeline::GraphProgram::defaults();
    auto check_layout=[&] {
        require(graph.allNodeIds().size()==size_t(defaults.value("nodes").toArray().size()),"Standard preset lost nodes");
        require(graph.save().value("connections").toArray().size()==defaults.value("connections").toArray().size(),"Standard preset lost connections");
        for(auto a:graph.allNodeIds()) for(auto b:graph.allNodeIds()) if(a<b)
            require(!scene->nodeGraphicsObject(a)->sceneBoundingRect().intersects(scene->nodeGraphicsObject(b)->sceneBoundingRect()),
                    "Standard preset node rectangles intersect");
        for(const auto &value:graph.save().value("connections").toArray()) {
            const auto edge=value.toObject();
            require(scene->nodeGraphicsObject(edge.value("outNodeId").toInt())->sceneBoundingRect().right()<
                    scene->nodeGraphicsObject(edge.value("inNodeId").toInt())->sceneBoundingRect().left(),
                    "Standard preset connection does not progress left to right");
        }
        QString error; require(bool(pipeline::GraphProgram::compile(editor.document(),error)),"Standard preset does not compile");
    };
    require(graph.delegateModel<AudioNodeModel>(0)->name()=="AudioSource","First standard document uses a generator");
    check_layout();
    selection_and_theme(editor,graph,*scene);
    int acknowledgements=0;
    QObject::connect(&presenter,&GuiPresenter::graph_apply_finished,&editor,[&](quint64,bool accepted,const QString &) {
        require(accepted,"Standard preset Apply rejected"); ++acknowledgements;
    });
    editor.findChild<QAction *>("defaultPipeline")->trigger(); QTest::qWait(500);
    check_layout();
    require(acknowledgements==1,"Explicit Default did not Apply the same processing graph exactly once");
    auto start_count=[&](auto kind) {return std::count(peer.commands.begin(),peer.commands.end(),kind);};
    require(start_count(pipeline::EngineCommand::StartAnalyzer)==1 && start_count(pipeline::EngineCommand::StartCapture)==1,
        "Default did not start Analyzer/Capture after successful ACK");
    const auto apply_at=std::find(peer.commands.begin(),peer.commands.end(),pipeline::EngineCommand::ApplyGraph);
    require(apply_at<std::find(peer.commands.begin(),peer.commands.end(),pipeline::EngineCommand::StartAnalyzer) &&
        apply_at<std::find(peer.commands.begin(),peer.commands.end(),pipeline::EngineCommand::StartCapture),"Default started before Apply");
    require(graph.delegateModel<AudioNodeModel>(0)->name()=="AudioSource","Default preset kept the test generator");
    graph.setNodeData(0,QtNodes::NodeRole::Position,QPointF(-999,777));
    editor.findChild<QAction *>("loadActivePipeline")->trigger(); QTest::qWait(60);
    check_layout();
    require(graph.nodeData(0,QtNodes::NodeRole::Position).toPointF()!=QPointF(-999,777),"Active preset did not replace draft positions");
    require(acknowledgements==1,"Loading Active unexpectedly applied a graph");
    state.file_configs.ui.pipeline_auto_apply=false;
    const auto before_test_source=graph.nodeData(0,QtNodes::NodeRole::Position).toPointF();
    editor.findChild<QAction *>("useTestSource")->trigger(); QTest::qWait(30);
    require(graph.delegateModel<AudioNodeModel>(0)->name()=="TestSignal" &&
        graph.nodeData(0,QtNodes::NodeRole::Position).toPointF()==before_test_source &&
        graph.save().value("connections").toArray().size()==defaults.value("connections").toArray().size(),
        "Test source preset changed positions or lost routes");
    QString test_source_error;
    require(bool(pipeline::GraphProgram::compile(editor.document(),test_source_error)),"Standard test source preset does not compile");
    editor.findChild<QAction *>("defaultPipeline")->trigger(); QTest::qWait(30); check_layout();
    require(graph.delegateModel<AudioNodeModel>(0)->name()=="AudioSource","Default did not replace the test source with real capture");
    graph.setNodeData(0,QtNodes::NodeRole::Position,QPointF(13,27));
    require(editor.save_preset("custom"),"Cannot save standard graph user coordinates");
    const auto custom=state.file_configs.ui.pipeline_presets.last().toObject().value("id").toString();
    editor.findChild<QAction *>("defaultPipeline")->trigger(); QTest::qWait(400); check_layout();
    require(acknowledgements==1,"Manual mode Default silently applied the draft");
    require(editor.load_preset(custom),"Cannot restore custom coordinates"); QTest::qWait(30);
    require(graph.nodeData(0,QtNodes::NodeRole::Position).toPointF()==QPointF(13,27),"Custom preset coordinates were automatically arranged");
    editor.findChild<QAction *>("defaultPipeline")->trigger(); QTest::qWait(30); check_layout();
    graph.delegateModel<AudioNodeModel>(8)->set_parameter("gain",2);
    QTest::mouseClick(editor.findChild<QPushButton *>("applyPipeline"),Qt::LeftButton); QTest::qWait(200);
    require(acknowledgements==2,"Manual Default Apply was not acknowledged");
    require(start_count(pipeline::EngineCommand::StartAnalyzer)==2 && start_count(pipeline::EngineCommand::StartCapture)==2,
        "Manual Default lost its start intent before Apply");
    // A source checkbox is an explicit start request; coordinate changes retain
    // it in manual mode, while subsequent ordinary Applies preserve Stop.
    auto *source_model=graph.delegateModel<AudioNodeModel>(0);
    source_model->set_parameter("enabled",false);
    std::unique_ptr<QWidget> source_controls(source_model->parameter_widget());
    source_controls->show(); QTest::qWait(10);
    QSignalSpy requested(&editor,&SchemeEditorWidget::source_start_requested);
    click_checkbox(source_controls->findChild<QCheckBox *>("enabled"));
    require(source_model->save().value("enabled")==true,"Source checkbox mouse click missed its indicator");
    require(requested.count()==1,"Source checkbox did not emit an explicit start request");
    graph.setNodeData(0,QtNodes::NodeRole::Position,QPointF(20,30)); QTest::qWait(400);
    require(requested.count()==1 && acknowledgements==2 && start_count(pipeline::EngineCommand::StartCapture)==2,
            "Manual source enable sent commands before Apply or failed to request Start");
    QTest::mouseClick(editor.findChild<QPushButton *>("applyPipeline"),Qt::LeftButton); QTest::qWait(200);
    require(acknowledgements==3 && start_count(pipeline::EngineCommand::StartCapture)==3 &&
            start_count(pipeline::EngineCommand::StartAnalyzer)==3,"Source checkbox did not start after Apply ACK");
    graph.setNodeData(0,QtNodes::NodeRole::Position,QPointF(25,35));
    QTest::mouseClick(editor.findChild<QPushButton *>("applyPipeline"),Qt::LeftButton); QTest::qWait(200);
    require(acknowledgements==4 && start_count(pipeline::EngineCommand::StartCapture)==3,
            "Ordinary coordinate Apply repeated source Start");
    click_checkbox(source_controls->findChild<QCheckBox *>("enabled"));
    click_checkbox(source_controls->findChild<QCheckBox *>("enabled"));
    editor.cancel_start_intent(); // MainWindow explicit Stop takes precedence.
    QTest::mouseClick(editor.findChild<QPushButton *>("applyPipeline"),Qt::LeftButton); QTest::qWait(200);
    require(acknowledgements==5 && start_count(pipeline::EngineCommand::StartCapture)==3,
            "Explicit Stop cancellation did not suppress pending source Start");
    qInfo("Editor: first/default/active/manual/custom standard preset checks passed");
}
int main(int argc, char **argv) {
    QApplication app(argc,argv);
    try {
        if(app.arguments().contains("--pan-only")) { held_key_pan_checks(); return 0; }
        if(app.arguments().contains("--overlay-only")) { held_key_pan_checks(true); return 0; }
        if(app.arguments().contains("--inspector-only")) { held_key_pan_checks(true); return 0; }
        if(app.arguments().contains("--clipboard-only")) { held_key_pan_checks(false,true); return 0; }
        node_output_controls();
        standard_presets();
        GuiState state;
        state.file_configs.core.run_loopback=false; state.file_configs.core.run_analyser=false;
        state.file_configs.ui.autosave_enabled=false;
        state.file_configs.ui.editor_view={{"version",1},{"scale",0.75},{"center_x",800},{"center_y",600},{"extension",42}};
        state.delegate_model_registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
        for (const auto &definition : pipeline::node_definitions())
            state.delegate_model_registry->registerModel([kind=definition.name] { return std::make_unique<AudioNodeModel>(kind); });
        state.data_flow_graph=std::make_shared<QtNodes::DataFlowGraphModel>(state.delegate_model_registry);
        QJsonObject source{{"id",0},{"position",QJsonObject{{"x",0},{"y",0}}},{"internal-data",QJsonObject{{"model-name","TestSignal"}}}};
        QJsonObject sink{{"id",1},{"position",QJsonObject{{"x",500},{"y",0}}},{"internal-data",QJsonObject{{"model-name","AudioView"}}}};
        QJsonObject document{{"version",1},{"nodes",QJsonArray{source,sink}},{"connections",QJsonArray{}}};
        document["extension"]=43;
        auto extended=source; extended["node-extension"]=44; document["nodes"]=QJsonArray{extended,sink};
        state.file_configs.scene_graph=document; state.file_configs.pipeline_graph=document;
        auto &graph=*state.data_flow_graph; graph.load(document);
        GuiPresenter presenter(state, test_fixture::synthetic_engine_provider()); presenter.initialize();
        // Enabled local sources now start on valid bootstrap Apply. Keep this
        // interaction fixture stopped so its explicit preview injections test
        // delivery/invalidation instead of racing the 100 ms live-preview gate.
        presenter.stop_analyzer();
        presenter.stop_loopback();
        QTest::qWait(60);
        state.file_configs.ui.pipeline_presets.append(QJsonObject{{"version",1},{"id","race"},{"name","race"},
            {"graph",document},{"view",QJsonObject{{"version",1},{"scale",0.75},{"center_x",1200},{"center_y",900}}}});
        QMainWindow window;
        auto *dock=new QDockWidget("Editor",&window);
        auto *editor=new SchemeEditorWidget(presenter,dock); dock->setWidget(editor);
        require(editor->load_preset("race"),"Cannot replace document before startup callback");
        require(editor->remove_preset("race"),"Cannot remove fixture preset");
        window.addDockWidget(Qt::BottomDockWidgetArea,dock);
        window.resize(1300,900); window.show(); QTest::qWait(70);
        window.move(100,100);
        auto *view=editor->findChild<QtNodes::GraphicsView *>();
        auto *scene=editor->findChild<QtNodes::DataFlowGraphicsScene *>();
        auto center=[&] { return view->mapToScene(view->viewport()->rect().center()); };
        auto near=[&](QPointF point) { return QLineF(center(),point).length()<4/view->getScale(); };
        require(near({1200,900}),"Stale startup callback overwrote replacement camera");
        emit presenter.pipeline_preview({{0,{16,0.5f}}});
        require(graph.delegateModel<AudioNodeModel>(0)->last_preview().values==16,"Current document preview was not delivered");
        require(editor->document().value("extension")==43,"Document replacement lost graph extensions");
        bool extension=false;
        for(const auto &value:editor->document().value("nodes").toArray())
            if(value.toObject().value("id").toInt()==0) extension=value.toObject().value("node-extension")==44;
        require(extension,"Document save lost node extensions");
        require(editor->findChild<QTreeWidget *>("pipelineNodeLibrary")->topLevelItemCount()==4,"Node library does not have four categories");
        {
            AudioNodeModel network("NetworkInput");
            network.load({{"model-name","NetworkInput"},{"data_type","audio"},{"extension",45}});
            require(network.dataType(QtNodes::PortType::Out,0).id=="audio","Network output has no concrete type");
            std::unique_ptr<QWidget> parameters(network.parameter_widget());
            auto *type=parameters->findChild<QComboBox *>("data_type");
            require(type && type->count()==7 && parameters->findChild<QLineEdit *>("provider_uuid"),"Network registry inspector missing typed bindings");
            type->setCurrentIndex(type->findData("history"));
            require(network.dataType(QtNodes::PortType::Out,0).id=="history" && network.save().value("extension")==45,
                    "Parameter edit lost concrete type or unknown fields");
        }
        const auto old_center=center();
        for (int i=0;i<12;++i) {
            dock->hide(); view->resize(0,0); window.resize(160,100); QTest::qWait(1);
            window.resize(1300+i*10,900-i*8); dock->show(); QTest::qWait(2);
            dock->setFloating(i%2==0); QTest::qWait(2);
            require(near(old_center),"Hide/show/strong resize/docking lost camera");
        }
        dock->setFloating(false); window.resize(1300,900); QTest::qWait(30);
        qInfo("Editor: camera lifecycle passed");
        const auto pan_start=view->viewport()->rect().center();
        const auto before_pan=center();
        drag(view->viewport(),pan_start,pan_start+QPoint(70,35));
        const auto after_pan=center();
        require(QLineF(before_pan,after_pan).length()>40,"Real blank-canvas mouse pan failed");
        dock->hide(); view->resize(0,0); dock->show(); QTest::qWait(20);
        require(near(after_pan),"Show lost mouse pan position");
        const auto cursor=view->viewport()->rect().center()+QPoint(80,30);
        const auto anchor=view->mapToScene(cursor);
        QWheelEvent wheel(cursor,view->viewport()->mapToGlobal(cursor),{},QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(view->viewport(),&wheel);
        require(QLineF(anchor,view->mapToScene(cursor)).length()<3/view->getScale(),"Wheel anchor moved");
        require(view->transform().dx()==0 && view->transform().dy()==0,"Zoom accumulated unsaved transform translation");
        editor->flush_presentation();
        require(state.file_configs.ui.editor_view.value("extension")==42,"Saving camera discarded extension fields");
        require(editor->save_preset("A"),"Cannot save preset A");
        const auto a=state.file_configs.ui.pipeline_presets.last().toObject().value("id").toString();
        const auto a_center=center(); const auto a_scale=view->getScale();
        editor->findChild<QDoubleSpinBox *>("pipelineZoom")->setValue(120);
        view->centerOn(-1000,700); editor->flush_presentation();
        graph.setNodeData(1,QtNodes::NodeRole::Position,QPointF(650,70));
        require(editor->save_preset("B"),"Cannot save preset B");
        const auto b=state.file_configs.ui.pipeline_presets.last().toObject().value("id").toString();
        for (int i=0;i<20;++i) {
            require(editor->load_preset(b) && editor->load_preset(a),"Preset replacement failed");
            QTest::qWait(1);
            require(near(a_center) && std::abs(view->getScale()-a_scale)<1e-6,"Preset camera restore drifted");
            require(graph.nodeData(1,QtNodes::NodeRole::Position).toPointF()==QPointF(500,0),"Delayed callback moved preset node");
            require(scene->undoStack().count()==0,"Replacement retained old undo history");
        }
        qInfo("Editor: preset replacement passed");
        QTest::mouseClick(editor->findChild<QPushButton *>("fitPipeline"),Qt::LeftButton);
        QRectF bounds;
        for (auto id:graph.allNodeIds()) bounds=bounds.united(scene->nodeGraphicsObject(id)->sceneBoundingRect());
        require(near(bounds.center()),"Fit misses real node bounds");
        const auto original=graph.nodeData(1,QtNodes::NodeRole::Position).toPointF();
        const auto press=view->mapFromScene(scene->nodeGraphicsObject(1)->sceneBoundingRect().topLeft()+QPointF(40,18));
        drag(view->viewport(),press,press+QPoint(40,25));
        const auto moved=graph.nodeData(1,QtNodes::NodeRole::Position).toPointF();
        require(moved!=original,"Mouse drag was not saved in model");
        scene->undoStack().undo(); require(graph.nodeData(1,QtNodes::NodeRole::Position).toPointF()==original,"Mouse drag undo failed");
        scene->undoStack().redo(); require(graph.nodeData(1,QtNodes::NodeRole::Position).toPointF()==moved,"Mouse drag redo failed");
        const auto count=scene->undoStack().count();
        editor->arrange_nodes(); require(scene->undoStack().count()==count+1,"Arrange is not one undo command");
        scene->undoStack().undo();
        qInfo("Editor: node drag/arrange passed");
        auto port=[&](int id,QtNodes::PortType type) {
            auto *item=scene->nodeGraphicsObject(id);
            return view->mapFromScene(scene->nodeGeometry().portScenePosition(id,type,0,item->sceneTransform()));
        };
        int applied=0;
        QObject::connect(&presenter,&GuiPresenter::graph_apply_finished,&window,[&](quint64,bool,const QString &) { ++applied; });
        QTest::qWait(400); const auto baseline=applied;
        const auto out=port(0,QtNodes::PortType::Out),in=port(1,QtNodes::PortType::In);
        require(view->viewport()->rect().contains(out) && view->viewport()->rect().contains(in),"Test ports outside viewport");
        drag(view->viewport(),out,in);
        qInfo("Editor: connection created");
        require(graph.save().value("connections").toArray().size()==1,"Real mouse connect failed");
        QTest::qWait(450); require(applied>baseline && state.editor_draft_modified,"Mouse connect did not auto apply draft");
        const auto before_disconnect=applied;
        drag(view->viewport(),in,in+QPoint(0,100));
        qInfo("Editor: connection disconnected");
        require(graph.save().value("connections").toArray().isEmpty(),"Real mouse disconnect failed");
        QTest::qWait(450); require(applied>before_disconnect,"Mouse disconnect did not auto apply");
        scene->undoStack().undo(); require(graph.save().value("connections").toArray().size()==1,"Disconnect undo failed");
        scene->undoStack().redo(); require(graph.save().value("connections").toArray().isEmpty(),"Disconnect redo failed");
        scene->clearSelection();
        QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->mapFromScene(scene->nodeGraphicsObject(1)->sceneBoundingRect().topLeft()+QPointF(40,18)));
        require(scene->nodeGraphicsObject(1)->isSelected(),"Real mouse click did not select removed node");
        // Use the same public Delete action as the context menu. Offscreen windows
        // cannot reliably activate native WidgetShortcut keyboard focus after docking.
        view->deleteSelectionAction()->trigger();
        require(!graph.nodeExists(1),"Mouse selection/Delete did not remove node");
        scene->undoStack().undo(); require(graph.nodeExists(1),"Remove undo did not restore node");
        scene->undoStack().redo(); require(!graph.nodeExists(1),"Remove redo failed");
        qInfo("Editor: disconnect/remove undo/redo passed");
        auto parameters=graph.delegateModel<AudioNodeModel>(0)->save(); parameters["tones"]=1;
        graph.delegateModel<AudioNodeModel>(0)->load(parameters); emit graph.nodeUpdated(0);
        QTest::mouseClick(editor->findChild<QPushButton *>("applyPipeline"),Qt::LeftButton);
        QTest::qWait(450);
        auto *status=editor->findChild<QLabel *>("pipelineValidation");
        require(status->isVisible() && !status->toolTip().isEmpty(),"Invalid apply has no permanent visible status");
        require(status->toolTip().contains("1") || status->toolTip().contains("5") || status->toolTip().contains("tones") || status->toolTip().contains(QString::fromUtf8("Не применено")),"Invalid apply did not expose rejection");
        emit presenter.pipeline_preview({{0,{16,0.5f}}});
        require(graph.delegateModel<AudioNodeModel>(0)->last_preview().values==0,"Old active graph preview leaked into invalid draft");
        auto unknown=editor->document();
        auto unknown_nodes=unknown.value("nodes").toArray();
        const QJsonObject opaque{{"id",99},{"position",QJsonObject{{"x",900},{"y",700}}},
            {"internal-data",QJsonObject{{"model-name","FutureProcessor"},{"private-setting",46}}}};
        unknown_nodes.append(opaque); unknown["nodes"]=unknown_nodes;
        const QJsonObject unknown_edge{{"outNodeId",99},{"outPortIndex",2},{"inNodeId",0},{"inPortIndex",3},{"extension",47}};
        unknown["connections"]=QJsonArray{unknown_edge};
        const auto before_switch=applied;
        emit presenter.selected_device_changed(unknown,{{"version",1},{"scale",1},{"center_x",222},{"center_y",333}});
        QTest::qWait(400); require(applied==before_switch,"Device replacement auto-applied unsupported draft");
        require(near({222,333}),"Device replacement did not restore camera");
        const auto roundtrip=editor->document();
        require(roundtrip.value("nodes").toArray().contains(opaque) && roundtrip.value("connections").toArray().contains(unknown_edge),
                "Unsupported node or attached connection lost on roundtrip");
        graph.setNodeData(0,QtNodes::NodeRole::Position,QPointF(70,80));
        QTest::qWait(400); require(applied==before_switch,"Unsupported draft editing auto-applied a partial graph");
        const QJsonObject invalid_edge{{"outNodeId",0},{"outPortIndex",5},{"inNodeId",1},{"inPortIndex",6},{"extension",48}};
        auto invalid_document=document; invalid_document["connections"]=QJsonArray{invalid_edge};
        emit presenter.selected_device_changed(invalid_document,{{"version",1},{"scale",1},{"center_x",222},{"center_y",333}});
        QTest::qWait(400);
        require(editor->document().value("connections").toArray().contains(invalid_edge) && applied==before_switch,
            "Unrenderable known-node connection lost or partially applied");
        state.file_configs.ui.autosave_enabled=true;
        int saves=0; QObject::connect(&presenter,&GuiPresenter::on_config_updated,&window,[&] { ++saves; });
        graph.setNodeData(0,QtNodes::NodeRole::Position,QPointF(1234,567));
        view->centerOn(456,789); const auto closing_center=center();
        delete dock;
        require(saves>0,"Close did not flush pending presentation save");
        require(QLineF(closing_center,QPointF(state.file_configs.ui.editor_view.value("center_x").toDouble(),
            state.file_configs.ui.editor_view.value("center_y").toDouble())).length()<5,"Close saved wrong view center");
        window.close();
        qInfo("Editor native mouse, view-state, replacement, auto-apply and rejection checks passed");
    } catch(const std::exception &error) { qCritical("Editor regression: %s",error.what()); return 1; }
    return 0;
}
