#include <visualization/quick/quick_ui.h>
#include <visualization/quick2d/vector_plot_item.h>
#include <qqml.h>
#include <visualization/ui_text.h>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QQmlContext>
#include <QQmlError>
#include <QApplication>
#include <QDebug>
#include <QResource>
#include <QEvent>
#include <QSurfaceFormat>
static void initialize_visualization_resources(){Q_INIT_RESOURCE(visualization_qml);}
namespace ui::quick {
namespace {
QVariantMap current_palette() {
    const auto p=Theme::instance().palette();QVariantMap result;
    const std::pair<const char*,QPalette::ColorRole> roles[]{
        {"window",QPalette::Window},{"base",QPalette::Base},{"text",QPalette::Text},
        {"windowText",QPalette::WindowText},{"button",QPalette::Button},
        {"buttonText",QPalette::ButtonText},{"highlight",QPalette::Highlight},
        {"highlightedText",QPalette::HighlightedText},{"mid",QPalette::Mid}};
    for(const auto& [name,role]:roles){
        result.insert(QString::fromLatin1(name),p.color(QPalette::Active,role));
        result.insert(QStringLiteral("inactive_")+QString::fromLatin1(name),p.color(QPalette::Inactive,role));
        result.insert(QStringLiteral("disabled_")+QString::fromLatin1(name),p.color(QPalette::Disabled,role));
    }
    return result;
}

void refresh_clear_color(QQuickWidget* widget,const UiBridge* bridge) {
    const auto color=bridge->palette().value("window").value<QColor>();
    if(widget->property("numericQuickClearColor").value<QColor>()==color)return;
    widget->setProperty("numericQuickClearColor",color);
    widget->setClearColor(color);
}

void record_qml_errors(QQuickWidget* widget,QQuickWidget::Status status) {
    if(status!=QQuickWidget::Error)return;
    QStringList errors;
    const auto errors_from_widget=widget->errors();
    for(const auto& error:errors_from_widget){
        errors.append(error.toString());
        qWarning().noquote()<<error.toString();
    }
    widget->setProperty("qmlErrors",errors);
}
}
UiBridge::UiBridge(QObject* parent):QObject(parent),palette_(current_palette()) {
    connect(&Theme::instance(),&Theme::changed,this,[this]{schedule_refresh();});
    connect(&Language::instance(),&Language::changed,this,[this]{schedule_refresh(true);});
    qApp->installEventFilter(this);
}
void UiBridge::schedule_refresh(bool languageChanged){
    language_changed_|=languageChanged;
    if(refresh_queued_)return;
    refresh_queued_=true;
    // Theme installs the actual application palette before this queued read.
    // Coalescing prevents widget palette notifications from re-entering QML.
    QMetaObject::invokeMethod(this,[this]{
        refresh_queued_=false;
        auto next=current_palette();
        const bool paletteChanged=next!=palette_;
        const bool languageChanged=language_changed_;language_changed_=false;
        if(!paletteChanged && !languageChanged)return;
        palette_=std::move(next);++revision_;emit changed();
    },Qt::QueuedConnection);
}
bool UiBridge::eventFilter(QObject* watched,QEvent* event){
    if(watched==qApp && event->type()==QEvent::ApplicationPaletteChange)schedule_refresh();
    return QObject::eventFilter(watched,event);
}
QString UiBridge::text(const QString& source) const {return ui::text(source.toUtf8().constData());}
void configure(){
    static const bool configured=[](){
        initialize_visualization_resources();
        qmlRegisterType<VectorPlotItem>("Numeric.Graphs",1,0,"VectorPlot");
        qmlRegisterType<VectorGeometryItem>("Numeric.Graphs",1,0,"VectorGeometry");
        // Qt's painted FBO enables multisampling only when the shared context
        // requests samples. Set this before QApplication/Quick contexts exist.
        auto format=QSurfaceFormat::defaultFormat();
        if(format.samples()<4)format.setSamples(4);
        // Application deadlines pace graph updates. Avoid an additional
        // refresh-dependent wait in the Widgets/Quick composition path.
        // This is a platform request; the driver may still impose pacing.
        format.setSwapInterval(0);
        QSurfaceFormat::setDefaultFormat(format);
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
        QQuickStyle::setStyle("Fusion");
        return true;
    }();
    Q_UNUSED(configured);
}
UiBridge* install(QQuickWidget* widget){
    auto* bridge=new UiBridge(widget);
    widget->rootContext()->setContextProperty("quickUi",bridge);
    widget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    const auto refresh=[widget,bridge]{refresh_clear_color(widget,bridge);};
    refresh();QObject::connect(bridge,&UiBridge::changed,widget,refresh);
    QObject::connect(widget,&QQuickWidget::statusChanged,widget,[widget](QQuickWidget::Status status){
        record_qml_errors(widget,status);
    });
    return bridge;
}
void initialize(QQuickWidget* widget,QObject* model,const QUrl& source){
    install(widget);widget->rootContext()->setContextProperty("graphModel",model);widget->setSource(source);
}
}
