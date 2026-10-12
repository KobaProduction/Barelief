#pragma once
#include <QWidget>
#include <QQuickWidget>
#include <QQuickView>
#include <QPointer>
#include <visualization/render_preparation_policy.h>

namespace ui::quick {
// QWidget layout boundary around a Quick scene. Numeric models and renderers
// never depend on whether the scene uses a native window or a texture widget.
class SceneHost : public QWidget {
    Q_OBJECT
public:
    explicit SceneHost(presentation::PlotHost kind, QWidget* parent = nullptr);
    ~SceneHost() override;
    QQuickWindow* quickWindow() const;
    QQuickItem* rootObject() const;
    QQmlEngine* engine() const;
    QQmlContext* rootContext() const;
    QQuickWidget::Status status() const;
    QList<QQmlError> errors() const;
    void setSource(const QUrl& source);
    void setFormat(const QSurfaceFormat& format);
    QSurfaceFormat format() const;
    QImage grabFramebuffer() const;
    bool nativeWindow() const { return view_ != nullptr; }
    void setSceneEnabled(bool enabled);
protected:
    // Derived scene owners call this before their callback state is destroyed.
    // Deleting a native container also joins its Quick rendering lifetime.
    void destroyScene();
    void paintEvent(QPaintEvent*) override;
private:
    QQuickWidget* widget_ = nullptr;
    QQuickView* view_ = nullptr;
    QWidget* container_ = nullptr;
};
// Read-only scene access for telemetry and own-renderer captures. Accepts both
// hosted plots and existing Quick texture views (settings and surface3D).
class SceneReference {
public:
    SceneReference(QWidget* owner = nullptr):owner_(owner){}
    explicit operator bool() const {return owner_ && (host() || texture());}
    SceneReference* operator->(){return this;}
    const SceneReference* operator->() const {return this;}
    QWidget* owner() const {return owner_;}
    QQuickWindow* quickWindow() const {return host()?host()->quickWindow():texture()->quickWindow();}
    QQuickItem* rootObject() const {return host()?host()->rootObject():texture()->rootObject();}
    QQuickWidget::Status status() const {return host()?host()->status():texture()->status();}
    QImage grabFramebuffer() const {return host()?host()->grabFramebuffer():texture()->grabFramebuffer();}
    QSize size() const {return owner_->size();}
    bool isVisible() const {return owner_ && owner_->isVisible();}
private:
    SceneHost* host() const {return qobject_cast<SceneHost*>(owner_.data());}
    QQuickWidget* texture() const {return qobject_cast<QQuickWidget*>(owner_.data());}
    QPointer<QWidget> owner_;
};
}
