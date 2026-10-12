#include <visualization/quick2d/plot_model.h>
#include <visualization/quick/quick_ui.h>
#include <visualization/ui_text.h>
#include <visualization/render_preparation_policy.h>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QEvent>
#include <QResizeEvent>
#include <QQuickWindow>
#include <algorithm>
#include <cmath>

void QuickPlotModel::publishVector(QVariantMap state,QPolygonF points) {
    // Same viewport budget as the projection adapter; malformed external
    // snapshots clear the series instead of retaining stale geometry.
    if(points.size()>4098)points.clear();
    const auto finite=[](const QPointF& p){return std::isfinite(p.x())&&std::isfinite(p.y());};
    if(!std::all_of(points.cbegin(),points.cend(),finite))
        points.erase(std::remove_if(points.begin(),points.end(),[&](const QPointF& p){return !finite(p);}),points.end());
    state.remove(QStringLiteral("points"));
    vector_points_=std::move(points);typed_vector_=true;
    publishState(std::move(state));
}

class QuickPlotWidget::ImageProvider final : public QQuickImageProvider {
public:
    ImageProvider():QQuickImageProvider(QQuickImageProvider::Image){}
    QImage pixels;
    QImage requestImage(const QString&,QSize* size,const QSize&) override {
        if(size)*size=pixels.size();return pixels;
    }
};
QuickPlotWidget::QuickPlotWidget(QWidget* parent):ui::quick::SceneHost(presentation::RenderPreparationPolicy::plot_host,parent),model_(new QuickPlotModel(this)),provider_(new ImageProvider) {
    // afterRendering runs on the scene renderer thread; sampling never requests
    // a repaint and therefore cannot manufacture frames for a static graph.
    connect(quickWindow(),&QQuickWindow::afterRendering,this,[this]{rendered_frames_.fetch_add(1,std::memory_order_relaxed);},Qt::DirectConnection);
    connect(quickWindow(),&QQuickWindow::beforeSynchronizing,this,[this]{
        synchronized_token_.store(submission_token_.load(std::memory_order_acquire),std::memory_order_release);
    },Qt::DirectConnection);
    connect(quickWindow(),&QQuickWindow::afterRendering,this,[this]{
        const auto token=synchronized_token_.load(std::memory_order_acquire);
        if(token && reported_token_.exchange(token,std::memory_order_acq_rel)!=token)
            QMetaObject::invokeMethod(this,[this,token]{frame_completed(token);},Qt::QueuedConnection);
    },Qt::DirectConnection);
    fps_clock_.start();fps_timer_.setInterval(500);
    connect(&fps_timer_,&QTimer::timeout,this,[this]{
        const auto frames=rendered_frames_.load(std::memory_order_relaxed);const auto elapsed=fps_clock_.nsecsElapsed();
        fps_clock_.restart();rendered_fps_=elapsed>0?qreal(frames-sampled_frames_)*1e9/qreal(elapsed):0;sampled_frames_=frames;
    });
    fps_timer_.start();
    engine()->addImageProvider(QStringLiteral("numeric"),provider_);
    rootContext()->setContextProperty("graphModel",model_);
    setSource(QUrl(QStringLiteral("qrc:/plots2d/Plot2D.qml")));
    connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{refresh_presentation();});
    connect(&ui::Theme::instance(),&ui::Theme::changed,this,[this]{refresh_presentation();});
}
QuickPlotWidget::~QuickPlotWidget() {
    // Dispose bindings while their model and shared UI context are alive.
    fps_timer_.stop();
    disconnect(quickWindow(),nullptr,this,nullptr);
    destroyScene();
}
void QuickPlotWidget::set_render_visible(bool visible) {
    if(rendering_enabled_!=visible){
        rendered_fps_=0;sampled_frames_=rendered_frames_.load(std::memory_order_relaxed);fps_clock_.restart();
        if(visible)fps_timer_.start();else fps_timer_.stop();
    }
    rendering_enabled_=visible;setSceneEnabled(visible);
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
    QWidget::changeEvent(event);
    if(event->type()==QEvent::PaletteChange)refresh_presentation();
}
void QuickPlotWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);refresh_presentation();
}
