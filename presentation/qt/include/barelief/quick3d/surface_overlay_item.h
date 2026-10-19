#pragma once
#include <QQuickPaintedItem>
#include <QPainterPath>
#include <QPen>
#include <QFont>
#include <QVariantList>
#include <QPointer>
#include <QMetaObject>
#include <vector>
#include <atomic>

// GUI presentation display list decoded once per publication, painted natively.
class SurfaceOverlayItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QVariantList commands READ commands WRITE setCommands NOTIFY commandsChanged)
    Q_PROPERTY(QObject* sourceModel READ sourceModel WRITE setSourceModel NOTIFY sourceModelChanged)
    Q_PROPERTY(int paintEngineType READ paintEngineType NOTIFY paintTelemetryChanged)
public:
    explicit SurfaceOverlayItem(QQuickItem* parent=nullptr);
    QVariantList commands() const {return commands_;}
    void setCommands(const QVariantList& commands);
    QObject* sourceModel() const {return source_model_;}
    void setSourceModel(QObject* model);
    void paint(QPainter* painter) override;
    int paintEngineType() const {return paint_engine_type_.load(std::memory_order_relaxed);}
signals:
    void commandsChanged();
    void sourceModelChanged();
    void paintTelemetryChanged();
private:
    struct Command {
        QPainterPath path;
        QPen pen;
        QBrush brush;
        QFont font;
        QTransform transform;
        QRectF clip,rect,bounds;
        QString text;
        int flags=0;
        bool isText=false,baseline=false;
    };
    static QBrush decodeBrush(const QVariantMap& brush);
    static QFont decodeFont(const QVariantMap& value);
    static QPainterPath decodePath(const QString& kind,const QVariantMap& value,const QRectF& rect);
    static void appendPointPath(QPainterPath& path,const QString& kind,const QVariantMap& value);
    static void decodeGeometry(Command& command,const QVariantMap& value);
    static QRectF strokeBounds(const Command& command);
    static QTransform decodeTransform(const QVariantMap& value);
    static void completeBounds(Command& command);
    void appendCommand(std::vector<Command>& next,QRectF& damage,const QVariantList& commands,qsizetype index);
    static void paintCommand(QPainter* painter,const Command& command);

    static Command decode(const QVariantMap& command);
    QVariantList commands_;
    std::vector<Command> decoded_;
    std::atomic<int> paint_engine_type_{-1};
    QPointer<QObject> source_model_;
    QMetaObject::Connection source_changed_,source_destroyed_;
};
