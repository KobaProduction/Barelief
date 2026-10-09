#include "synthetic_engine_provider.h"
#include <core_app/gui_presenter.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <visualization/ui_text.h>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QApplication>
#include <QAction>
#include <QScrollArea>
#include <QSplitter>
#include <QLineEdit>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QImage>
#include <QLineF>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <cmath>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <cstdio>
#include <stdexcept>
static void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(int argc,char** argv){
    if(qEnvironmentVariable("QT_QPA_PLATFORM")!="offscreen")return 2;
    QApplication app(argc,argv);
    try{
        GuiState state;state.file_configs.ui.autosave_enabled=false;state.file_configs.ui.pipeline_auto_apply=false;state.delegate_model_registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();for(const auto& kind:numeric_editor::catalog())state.delegate_model_registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
        auto graph=std::make_shared<NumericEditorGraphModel>(state.delegate_model_registry);state.data_flow_graph=graph;domain::NumericProcessingGraph typed;typed.graph_id="main";auto source=numeric_editor::default_node("NetworkInput"),sink=numeric_editor::default_node("NetworkOutput");source.id=1;sink.id=2;typed.nodes={source,sink};typed.edges={{1,2,0,0}};require(graph->set_numeric_graph(typed),"Canonical navigation graph rejected");state.file_configs.scene_graph=graph->save();
        GuiPresenter presenter(state,test_fixture::synthetic_engine_provider(false));SchemeEditorWidget editor(presenter);editor.resize(1100,760);editor.show();QTest::qWait(30);QSignalSpy applied(&presenter,&GuiPresenter::graph_apply_finished);
        auto* view=editor.findChild<QtNodes::GraphicsView*>();auto* scene=editor.findChild<QtNodes::DataFlowGraphicsScene*>();auto* inspector=editor.findChild<QScrollArea*>("pipelineInspector");auto* splitter=editor.findChild<QSplitter*>("pipelineSplitter");require(view && scene && inspector && splitter && splitter->count()==3,"Navigation components missing");
        auto center=[&]{return view->mapToScene(view->viewport()->rect().center());};auto near=[&](QPointF expected,double pixels=4){return QLineF(expected,center()).length()<pixels/view->getScale();};
        view->setupScale(.75);view->centerOn(1200,900);editor.flush_presentation();
        const auto selectionSize=view->viewport()->size();const auto selectionCenter=center();for(int i=0;i<5;++i){scene->clearSelection();scene->nodeGraphicsObject(i%2+1)->setSelected(true);emit scene->nodeSelected(i%2+1);QCoreApplication::processEvents();require(inspector->isVisible() && view->viewport()->size()==selectionSize && near(selectionCenter),"Selection changes inspector layout or canvas center");}
        scene->clearSelection();QCoreApplication::processEvents();require(inspector->findChild<QLabel*>("pipelineInspectorPlaceholder"),"Deselection does not restore inspector placeholder");
        auto* handle=splitter->handle(2);auto clickHandle=[&]{const auto point=handle->rect().center();for(const auto type:{QEvent::MouseButtonPress,QEvent::MouseButtonRelease}){QMouseEvent event(type,point,handle->mapToGlobal(point),Qt::LeftButton,type==QEvent::MouseButtonPress?Qt::LeftButton:Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(handle,&event);}QCoreApplication::processEvents();};const auto expandedCenter=center();clickHandle();require(splitter->sizes()[2]==0 && near(expandedCenter),"Inspector collapse loses camera");scene->nodeGraphicsObject(1)->setSelected(true);emit scene->nodeSelected(1);QCoreApplication::processEvents();require(splitter->sizes()[2]==0,"Selection reopens collapsed inspector");clickHandle();require(splitter->sizes()[2]>0 && near(expandedCenter),"Inspector does not restore width/camera");
        graph->setNodeData(1,QtNodes::NodeRole::Position,QPointF(21000,25000));graph->setNodeData(2,QtNodes::NodeRole::Position,QPointF(-2000,600));const auto beforeArrange=editor.history().count();
        // Only this fixture's offscreen Qt widget receives focus/events. No OS
        // input, cursor positioning, active-window API or desktop capture is used.
        view->setFocus(Qt::OtherFocusReason);QTest::qWait(10);require(view->hasFocus(),"Offscreen canvas Qt focus unavailable");QTest::keyClick(view,Qt::Key_A,Qt::AltModifier);QTest::qWait(10);require(editor.history().count()==beforeArrange+1,"Canvas Alt+A did not arrange once");
        require(scene->nodeGraphicsObject(1)->sceneBoundingRect().right()<scene->nodeGraphicsObject(2)->sceneBoundingRect().left(),"Arrange does not follow route direction");view->centerOn(90000,90000);QTest::keyClick(view,Qt::Key_F,Qt::AltModifier);require(QLineF(center(),QPointF(90000,90000)).length()>100,"Canvas Alt+F did not center");
        scene->clearSelection();scene->nodeGraphicsObject(1)->setSelected(true);emit scene->nodeSelected(1);QCoreApplication::processEvents();auto* text=inspector->findChild<QLineEdit*>("adapterStreamId");require(text,"Current inspector text input missing");const auto beforeTyping=editor.history().count();text->setFocus(Qt::OtherFocusReason);QTest::keyClick(text,Qt::Key_A,Qt::AltModifier);require(editor.history().count()==beforeTyping,"Arrange intercepts inspector input");
        view->setFocus(Qt::OtherFocusReason);view->centerOn(90000,90000);QElapsedTimer quiet;quiet.start();auto replies=applied.count();for(int i=0;i<300 && (quiet.elapsed()<500 || presenter.graph_request_pending());++i){QTest::qWait(10);if(applied.count()!=replies || presenter.graph_request_pending()){replies=applied.count();quiet.restart();}}require(!presenter.graph_request_pending() && quiet.elapsed()>=500,"Pending source edit before navigation");const auto graphBeforeNavigation=editor.document();const auto runtimeBeforeNavigation=presenter.client().snapshot();applied.clear();
        const QPoint cursor(view->viewport()->width()/3,view->viewport()->height()/3);const auto anchor=view->mapToScene(cursor);const auto beforeWheelScale=view->getScale();QWheelEvent wheel(cursor,view->viewport()->mapToGlobal(cursor),{},QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(view->viewport(),&wheel);std::fprintf(stderr,"Wheel before=%.5f after=%.5f anchorDelta=%.5f center=(%.3f,%.3f)\n",beforeWheelScale,view->getScale(),QLineF(anchor,view->mapToScene(cursor)).length(),center().x(),center().y());require(view->getScale()>beforeWheelScale && QLineF(anchor,view->mapToScene(cursor)).length()<4/view->getScale(),"Wheel zoom loses pointer anchor");
        const QPoint from=view->viewport()->rect().center(),to=from+QPoint(70,45);const auto beforePan=center();auto mouse=[&](QEvent::Type type,QPoint point,Qt::MouseButton button,Qt::MouseButtons buttons){QMouseEvent event(type,point,view->viewport()->mapToGlobal(point),button,buttons,Qt::NoModifier);QApplication::sendEvent(view->viewport(),&event);};mouse(QEvent::MouseButtonPress,from,Qt::MiddleButton,Qt::MiddleButton);mouse(QEvent::MouseMove,to,Qt::NoButton,Qt::MiddleButton);mouse(QEvent::MouseButtonRelease,to,Qt::MiddleButton,Qt::NoButton);require(QLineF(beforePan,center()).length()>40/view->getScale(),"Middle pan did not move camera");
        auto key=[&](QEvent::Type type,int code,quint32 scan=0,bool repeat=false){QKeyEvent event(type,code,Qt::NoModifier,scan,0,0,QString(),repeat);QApplication::sendEvent(view,&event);};
        view->setFocus(Qt::OtherFocusReason);const auto beforeHeld=center();key(QEvent::KeyPress,Qt::Key_W);require(near(beforeHeld,2),"WASD press jumps in chunks");QTest::qWait(70);const auto held=center();require(held.y()<beforeHeld.y()-5,"Held W did not pan");key(QEvent::KeyRelease,Qt::Key_W,0,true);QTest::qWait(35);require(center().y()<held.y()-3,"Autorepeat release stopped held key");key(QEvent::KeyRelease,Qt::Key_W);const auto released=center();QTest::qWait(45);require(near(released,2),"Key release fails to stop pan");
        const auto diagonalStart=center();key(QEvent::KeyPress,Qt::Key_W);key(QEvent::KeyPress,Qt::Key_D);QTest::qWait(65);key(QEvent::KeyRelease,Qt::Key_W);key(QEvent::KeyRelease,Qt::Key_D);const auto delta=center()-diagonalStart;require(delta.x()>5 && delta.y()<-5 && std::abs(delta.x()/delta.y())>.6 && std::abs(delta.x()/delta.y())<1.5,"Diagonal pan not normalized");
        const auto physicalStart=center();key(QEvent::KeyPress,0x0426,17);QTest::qWait(65);key(QEvent::KeyRelease,0x0426,17);require(center().y()<physicalStart.y()-5,"Physical W under RU layout does not pan");
        key(QEvent::KeyPress,Qt::Key_W);text->setFocus(Qt::OtherFocusReason);QTest::qWait(30);const auto textCenter=center();QTest::qWait(45);require(near(textCenter,2),"Text focus does not stop keyboard pan");view->setFocus(Qt::OtherFocusReason);key(QEvent::KeyPress,Qt::Key_W);editor.hide();QTest::qWait(25);const auto hiddenCenter=center();QTest::qWait(45);require(near(hiddenCenter,2),"Hidden editor keeps panning");editor.show();QTest::qWait(20);
        // Isolate the background oracle: bounded panning can leave nodes in view.
        QList<QGraphicsItem*> paintedItems;for(auto* item:scene->items())if(!item->parentItem() && item->isVisible()){paintedItems.append(item);item->hide();}
        for(const auto& theme:{QString("light"),QString("dark")}){ui::Theme::instance().set(theme);QCoreApplication::processEvents();view->centerOn(90000,90000);QImage pixels(view->viewport()->size(),QImage::Format_ARGB32);view->viewport()->render(&pixels);const auto base=QApplication::palette().color(QPalette::Window);int different=0;for(int y=0;y<pixels.height();++y)for(int x=0;x<pixels.width();++x){const auto color=pixels.pixelColor(x,y);if(color!=base){++different;require(std::abs(color.lightness()-base.lightness())<=30,"Grid has stale palette or excessive contrast");}}require(different<pixels.width()*pixels.height()/8,"Editor grid is too dense");}
        for(auto* item:paintedItems)item->show();
        // Offscreen hide/show leaves the fixture window inactive. Run keyboard
        // routing first, then independently verify camera restoration without activation.
        editor.flush_presentation();const auto resizeCenter=center();const auto resizeScale=view->getScale();
        for(const auto size:{QSize(800,600),QSize(1300,900),QSize(1100,760)}){editor.hide();editor.resize(size);editor.show();QTest::qWait(10);if(!near(resizeCenter) || view->getScale()!=resizeScale)std::fprintf(stderr,"Camera resize %dx%d desired=(%.3f,%.3f)/%.4f actual=(%.3f,%.3f)/%.4f viewport=%dx%d\n",size.width(),size.height(),resizeCenter.x(),resizeCenter.y(),resizeScale,center().x(),center().y(),view->getScale(),view->viewport()->width(),view->viewport()->height());require(near(resizeCenter) && view->getScale()==resizeScale,"Hide/show/resize resets camera");}
        
        require(editor.document()==graphBeforeNavigation && applied.isEmpty() && presenter.client().snapshot().graph_revision==runtimeBeforeNavigation.graph_revision,"Navigation/theme changes graph or applies DSP");
        std::puts("canonical editor navigation: inspector/scoped shortcuts, camera resize/zoom/pan/RU physical keys, stop fences and central grid themes PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
