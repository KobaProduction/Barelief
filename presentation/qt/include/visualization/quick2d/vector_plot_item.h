#pragma once
#include <QQuickPaintedItem>
#include <QVariantMap>
#include <QVector>
#include <QPolygonF>
#include <QPointer>
#include <atomic>

// Qt renderer adapter. The snapshot is normalized presentation geometry;
// signal samples, contracts and processing remain outside the scene item.
class QuickPlotModel;
class VectorPlotItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QVariantMap presentation READ presentation WRITE setPresentation NOTIFY presentationChanged)
    Q_PROPERTY(QObject* sourceModel READ sourceModel WRITE setSourceModel NOTIFY sourceModelChanged)
    Q_PROPERTY(QVariantMap colors READ colors WRITE setColors NOTIFY colorsChanged)
    Q_PROPERTY(quint64 paintCount READ paintCount)
    Q_PROPERTY(quint64 consumedPresentationCount READ consumedPresentationCount)
    Q_PROPERTY(int paintEngineType READ paintEngineType)
    Q_PROPERTY(int paintSamples READ paintSamples)
    Q_PROPERTY(qreal pieRadius READ pieRadius WRITE setPieRadius NOTIFY pieRadiusChanged)
    Q_PROPERTY(qreal pieLabelWidth READ pieLabelWidth WRITE setPieLabelWidth NOTIFY pieLabelWidthChanged)
    Q_PROPERTY(QVariantList pieLabels READ pieLabels WRITE setPieLabels NOTIFY pieLabelsChanged)
public:
    explicit VectorPlotItem(QQuickItem* parent=nullptr);
    QVariantMap presentation() const {return presentation_;}
    QObject* sourceModel() const;
    void setSourceModel(QObject* model);
    QVariantMap colors() const {return colors_;}
    quint64 paintCount() const {return paint_count_.load(std::memory_order_relaxed);}
    quint64 consumedPresentationCount() const {return consumed_count_.load(std::memory_order_relaxed);}
    int paintEngineType() const {return paint_engine_type_.load(std::memory_order_relaxed);}
    int paintSamples() const {return paint_samples_.load(std::memory_order_relaxed);}
    qreal pieRadius() const {return pie_radius_;}
    qreal pieLabelWidth() const {return pie_label_width_;}
    QVariantList pieLabels() const {return pie_labels_;}
    void setPieRadius(qreal value);
    void setPieLabelWidth(qreal value);
    void setPieLabels(const QVariantList& value);
    void setPresentation(const QVariantMap& value);
    void setColors(const QVariantMap& value);
    Q_INVOKABLE void requestPaint();
    void paint(QPainter* painter) override;
signals:
    void presentationChanged();
    void sourceModelChanged();
    void colorsChanged();
    void pieRadiusChanged();
    void pieLabelWidthChanged();
    void pieLabelsChanged();
protected:
    void itemChange(ItemChange change,const ItemChangeData& data) override;
    void geometryChange(const QRectF& next,const QRectF& previous) override;
protected:
    QVariantMap presentation_,colors_;
    QPointer<QuickPlotModel> source_model_;
    QMetaObject::Connection source_changed_,source_destroyed_;
    void syncSourceModel();
    void applyPresentation(const QVariantMap& value,bool typed);
    QPolygonF points_;
    QVariant x_tick_source_,y_tick_source_;
    QVector<double> x_ticks_,y_ticks_;
    QString style_;
    QString kind_;
    double baseline_=0,line_width_=1.25,point_size_=12,bar_width_=80;
    QColor grid_color_,series_color_,base_color_,text_color_;
    QVector<QPointF> slices_;
    struct Leader {QPointF anchor;QPointF position;bool left;};
    QVector<Leader> leaders_;
    QVariantList pie_labels_;
    qreal pie_radius_=1,pie_label_width_=0;
    std::atomic<quint64> paint_count_{0},consumed_count_{0},presentation_revision_{0};
    std::atomic<int> paint_engine_type_{-1};
    std::atomic<int> paint_samples_{-1};
    quint64 consumed_revision_=0;
};

// Vector series are scene geometry, not a viewport-sized painted texture.
// The pie renderer retains the painter adapter above.
class VectorGeometryItem : public VectorPlotItem {
    Q_OBJECT
    Q_PROPERTY(quint64 geometryUpdateCount READ geometryUpdateCount)
    Q_PROPERTY(int geometryVertexCount READ geometryVertexCount)
    Q_PROPERTY(bool geometryVerticesFinite READ geometryVerticesFinite)
public:
    explicit VectorGeometryItem(QQuickItem* parent=nullptr);
    quint64 geometryUpdateCount() const {return geometry_updates_.load();}
    int geometryVertexCount() const {return geometry_vertices_.load();}
    bool geometryVerticesFinite() const {return geometry_finite_.load();}
protected:
    QSGNode* updatePaintNode(QSGNode* oldNode,UpdatePaintNodeData*) override;
private:
    std::atomic<quint64> geometry_updates_{0};
    std::atomic<int> geometry_vertices_{0};
    std::atomic<bool> geometry_finite_{true};
};
