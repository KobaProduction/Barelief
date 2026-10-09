#pragma once
#include <QQuickFramebufferObject>
#include <QPointer>
class SurfaceQuickItem : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(QObject* backend READ backend WRITE setBackend NOTIFY backendChanged)
public:
    explicit SurfaceQuickItem(QQuickItem *parent=nullptr);
    QObject *backend() const {return _backend;}
    void setBackend(QObject *value);
    Renderer *createRenderer() const override;
signals:
    void backendChanged();
private:
    QPointer<QObject> _backend;
};
