#pragma once
#include <QQuickWidget>
#include <QVariantMap>
#include <QImage>
#include <functional>

// Presentation snapshots contain only bounded, viewport-sized geometry and
// exact strings. Samples and their source metadata remain in the C++ façades.
class QuickPlotModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
public:
    explicit QuickPlotModel(QObject* parent=nullptr):QObject(parent){}
    QVariantMap state() const {return state_;}
    void publish(QVariantMap state) {state_=std::move(state);emit changed();}
signals:
    void changed();
private:
    QVariantMap state_;
};

class QuickPlotWidget : public QQuickWidget {
public:
    explicit QuickPlotWidget(QWidget* parent=nullptr);
    ~QuickPlotWidget() override;
    void set_render_visible(bool visible);
    QuickPlotModel* presentation_model() const {return model_;}
    QImage numeric_raster() const;
protected:
    void publish(QVariantMap state);
    void publish_image(const QImage& image);
    virtual void refresh_presentation() {}
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool rendering_enabled_=true;
private:
    QuickPlotModel* model_;
    class ImageProvider;
    ImageProvider* provider_;
    quint64 image_revision_=0;
    QString image_url_;
};
