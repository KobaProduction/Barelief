#pragma once
#include <QQuickWidget>
#include <barelief/quick/scene_host.h>
#include <QVariantMap>
#include <QImage>
#include <QPolygonF>
#include <functional>
#include <atomic>
#include <QElapsedTimer>
#include <QTimer>
#include <QRectF>
#include <algorithm>
#include <cmath>

// Presentation snapshots contain only bounded, viewport-sized geometry and
// exact strings. Samples and their source metadata remain in the C++ façades.
class QuickPlotModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QVariantMap axes READ axes NOTIFY axesChanged)
public:
    explicit QuickPlotModel(QObject* parent=nullptr):QObject(parent){}
    // Coordinates and deltas are fractions of the visible plot, never samples.
    Q_INVOKABLE void navigate(const QString& action,double x,double y,double amount=1,int axes=3) {
        emit navigationRequested(action,x,y,amount,axes);
    }
    QVariantMap state() const {return state_;}
    QVariantMap axes() const {return axes_;}
    // Implicit sharing keeps one owned, immutable normalized geometry snapshot
    // between the GUI model and renderer. No samples cross the QML boundary.
    const QPolygonF& vectorPoints() const {return vector_points_;}
    bool hasTypedVector() const {return typed_vector_;}
    void publishVector(QVariantMap state,QPolygonF points);
    void publish(QVariantMap state) {
        typed_vector_=false;vector_points_.clear();publishState(std::move(state));
    }
signals:
    void changed();
    void axesChanged();
    void navigationRequested(const QString& action,double x,double y,double amount,int axes);
private:
    void publishState(QVariantMap state) {
        QVariantMap nextAxes;
        for(const auto* key:{"xTicks","yTicks","xGrid","yGrid","xTitle","yTitle","plotLeft"})nextAxes.insert(QString::fromLatin1(key),state.value(QString::fromLatin1(key)));
        const bool newAxes=nextAxes!=axes_;
        state_=std::move(state);axes_=std::move(nextAxes);
        if(newAxes)emit axesChanged();
        emit changed();
    }
    QVariantMap state_,axes_;
    QPolygonF vector_points_;
    bool typed_vector_=false;
};

class QuickPlotWidget : public ui::quick::SceneHost {
    Q_OBJECT
public:
    explicit QuickPlotWidget(QWidget* parent=nullptr);
    QuickPlotWidget(presentation::PlotHost kind,QWidget* parent);
    ~QuickPlotWidget() override;
    void set_render_visible(bool visible);
    QuickPlotModel* presentation_model() const {return model_;}
    QImage numeric_raster() const;
    void set_axis_density(int gridX,int gridY,int labelsX,int labelsY) {
        grid_x_=gridX;grid_y_=gridY;labels_x_=labelsX;labels_y_=labelsY;refresh_presentation();
    }
    QRectF viewport() const {return viewport_;}
    qreal rendered_fps() const {return rendering_enabled_ && isVisible()?rendered_fps_:0;}
protected:
private:
    void bindNavigation();
    void bindRenderTelemetry();
    void bindFrameRate();
    void initializeScene();
protected:

    void publish(QVariantMap state);
    void publish_image(const QImage& image);
    virtual void refresh_presentation() {}
    QRectF viewport_{0,0,1,1};
    int grid_x_=0,grid_y_=0,labels_x_=0,labels_y_=0;
    virtual void frame_completed(quint64) {}
    void mark_submission(quint64 token) {submission_token_.store(token,std::memory_order_release);}
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool rendering_enabled_=true;
private:
    QuickPlotModel* model_;
    class ImageProvider;
    ImageProvider* provider_;
    std::atomic<quint64> rendered_frames_{0};
    std::atomic<quint64> submission_token_{0},synchronized_token_{0},reported_token_{0};
    quint64 sampled_frames_=0;
    qreal rendered_fps_=0;
    QElapsedTimer fps_clock_;
    QTimer fps_timer_;
    quint64 image_revision_=0;
    QString image_url_;
};
