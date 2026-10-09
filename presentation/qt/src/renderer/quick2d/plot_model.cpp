#include <visualization/quick2d/plot_model.h>
#include <visualization/quick/quick_ui.h>
#include <visualization/ui_text.h>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QEvent>
#include <QResizeEvent>

class QuickPlotWidget::ImageProvider final : public QQuickImageProvider {
public:
    ImageProvider():QQuickImageProvider(QQuickImageProvider::Image){}
    QImage pixels;
    QImage requestImage(const QString&,QSize* size,const QSize&) override {
        if(size)*size=pixels.size();return pixels;
    }
};
QuickPlotWidget::QuickPlotWidget(QWidget* parent):QQuickWidget((ui::quick::configure(),parent)),model_(new QuickPlotModel(this)),provider_(new ImageProvider) {
    setResizeMode(QQuickWidget::SizeRootObjectToView);
    engine()->addImageProvider(QStringLiteral("numeric"),provider_);
    ui::quick::initialize(this,model_,QUrl(QStringLiteral("qrc:/plots2d/Plot2D.qml")));
    connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{refresh_presentation();});
    connect(&ui::Theme::instance(),&ui::Theme::changed,this,[this]{refresh_presentation();});
}
QuickPlotWidget::~QuickPlotWidget() {
    // Dispose bindings while their model and shared UI context are alive.
    setSource(QUrl{});
}
void QuickPlotWidget::set_render_visible(bool visible) {
    rendering_enabled_=visible;setUpdatesEnabled(visible);
    if(visible)refresh_presentation();
}
void QuickPlotWidget::publish(QVariantMap state) {
    if(!image_url_.isEmpty())state["imageSource"]=image_url_;
    model_->publish(std::move(state));
}
void QuickPlotWidget::publish_image(const QImage& image) {
    provider_->pixels=image;image_url_=QStringLiteral("image://numeric/%1").arg(++image_revision_);
}
QImage QuickPlotWidget::numeric_raster() const {return provider_->pixels;}
void QuickPlotWidget::changeEvent(QEvent* event) {
    QQuickWidget::changeEvent(event);
    if(event->type()==QEvent::PaletteChange)refresh_presentation();
}
void QuickPlotWidget::resizeEvent(QResizeEvent* event) {
    QQuickWidget::resizeEvent(event);refresh_presentation();
}
