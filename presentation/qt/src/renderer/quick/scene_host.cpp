#include <visualization/quick/scene_host.h>
#include <visualization/quick/quick_ui.h>
#include <QVBoxLayout>
#include <QQmlContext>
#include <QQmlEngine>
#include <QPainter>
#include <QQuickItem>

namespace ui::quick {
void SceneHost::createNativeScene(){
    view_=new QQuickView;view_->setResizeMode(QQuickView::SizeRootObjectToView);view_->setFormat(QSurfaceFormat::defaultFormat());
    container_=QWidget::createWindowContainer(view_,this);
    auto* bridge=new UiBridge(view_);rootContext()->setContextProperty("quickUi",bridge);
    const auto refresh=[this,bridge]{view_->setColor(bridge->palette().value("window").value<QColor>());};
    refresh();connect(bridge,&UiBridge::changed,this,refresh);
    connect(view_,&QQuickView::statusChanged,this,[this](auto state){
        if(state==QQuickView::Error){QStringList messages;const auto failures=errors();for(const auto& error:failures)messages.append(error.toString());setProperty("qmlErrors",messages);}
    });
}
void SceneHost::createTextureScene(){
    widget_=new QQuickWidget(this);widget_->setObjectName("plotTextureHost");install(widget_);container_=widget_;
}
SceneHost::SceneHost(presentation::PlotHost kind,QWidget* parent):QWidget(parent){
    configure();auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
    if(kind==presentation::PlotHost::NativeWindow)createNativeScene();else createTextureScene();
    layout->addWidget(container_);
}
SceneHost::~SceneHost() {
    destroyScene();
}
void SceneHost::destroyScene() {
    if(!container_)return;
    // Destroy the render scene before its model/bridge owner is torn down.
    setSource({});
    delete container_;
    container_=nullptr;view_=nullptr;widget_=nullptr;
}
QQuickWindow* SceneHost::quickWindow() const {return view_?view_:widget_->quickWindow();}
QQuickItem* SceneHost::rootObject() const {return view_?view_->rootObject():widget_->rootObject();}
QQmlEngine* SceneHost::engine() const {return view_?view_->engine():widget_->engine();}
QQmlContext* SceneHost::rootContext() const {return view_?view_->rootContext():widget_->rootContext();}
QQuickWidget::Status SceneHost::status() const {return view_?QQuickWidget::Status(view_->status()):widget_->status();}
QList<QQmlError> SceneHost::errors() const {return view_?view_->errors():widget_->errors();}
void SceneHost::setSource(const QUrl& source){
    if(view_)view_->setSource(source);else widget_->setSource(source);
    QStringList messages;const auto failures=errors();for(const auto& error:failures)messages.append(error.toString());setProperty("qmlErrors",messages);
}
void SceneHost::setFormat(const QSurfaceFormat& format){if(view_)view_->setFormat(format);else widget_->setFormat(format);}
QSurfaceFormat SceneHost::format() const {return view_?view_->format():widget_->format();}
QImage SceneHost::grabFramebuffer() const {return view_?view_->grabWindow():widget_->grabFramebuffer();}
void SceneHost::setSceneEnabled(bool enabled){
    setUpdatesEnabled(enabled);
    container_->setVisible(enabled);
    container_->setUpdatesEnabled(enabled);
}
void SceneHost::paintEvent(QPaintEvent*) {QPainter painter(this);painter.fillRect(rect(),palette().brush(QPalette::Base));}
}
