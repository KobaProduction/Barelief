#pragma once

#include <DockWidget.h>
#include <DockManager.h>
#include <QMainWindow>
#include <QPointer>
#include <QHash>

// The presentation boundary owns docking. DSP and graph widgets know nothing
// about ADS, its containers, drag sessions or serialized layout format.
class WorkspaceDock final : public ads::CDockWidget {
    Q_OBJECT
public:
    WorkspaceDock(const QString &title, QWidget *parent);
    void setWidget(QWidget *widget) { ads::CDockWidget::setWidget(widget, ForceNoScrollArea); }
    void show() { if (isClosed()) toggleView(true); }
    void hide() { toggleView(false); }
    bool close() { closeDockWidget(); return isClosed(); }
    bool isHidden() const { return isClosed(); }
    bool isFloating() const { return isInFloatingContainer(); }
    bool sharesHost(const WorkspaceDock *other) const { return other && dockContainer()==other->dockContainer(); }
    void raise();
    void setFloating(bool floating);
    void setHeaderControls(QWidget *widget);
    void setRenderFPS(qreal value,bool visible);
    QString renderFPS() const { return _fpsText; }
    QWidget *headerControls() const { return _headerControls; }
    void setFeatures(DockWidgetFeatures features) {
        ads::CDockWidget::setFeatures(features | DockWidgetFocusable | DockWidgetPinnable);
    }
signals:
    void dockLocationChanged();
    void headerControlsChanged();
    void renderFPSChanged();
private:
    QPointer<QWidget> _headerControls;
    QString _fpsText;
};

class WorkspaceWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit WorkspaceWindow(QWidget *parent = nullptr);
    ~WorkspaceWindow() override;
    ads::CDockManager *workspace() const { return _workspace; }
    QList<WorkspaceDock *> workspaceDocks() const;
    WorkspaceDock *workspaceDock(const QString &id) const;
    void addDockWidget(Qt::DockWidgetArea area, WorkspaceDock *dock);
    void removeDockWidget(WorkspaceDock *dock);
    void splitDockWidget(WorkspaceDock *anchor, WorkspaceDock *dock, Qt::Orientation orientation);
    void tabifyDockWidget(WorkspaceDock *anchor, WorkspaceDock *dock);
    QList<WorkspaceDock *> tabifiedDockWidgets(WorkspaceDock *dock) const;
    void resizeDocks(const QList<WorkspaceDock *> &docks, const QList<int> &sizes, Qt::Orientation orientation);
    QByteArray saveState(int version = 0) const;
    bool restoreState(const QByteArray &state, int version = 0);
    bool importLegacyState(const QByteArray &state, int version = 0, bool oldNames = false);
    WorkspaceDock *activeDock() const;
    void activateAdjacentTab(int direction);
    void placeDock(WorkspaceDock *dock, WorkspaceDock *anchor, Qt::DockWidgetArea side);
    void moveDockToHostEdge(WorkspaceDock *dock, Qt::DockWidgetArea side);
    void focusDock(WorkspaceDock *dock);
signals:
    void graphAreaActivated(WorkspaceDock *dock);
private:
    void configureWorkspace();
    void bindWorkspaceEvents();
    void recordFocusedDock(ads::CDockWidget *current);
    ads::CDockManager *_workspace;
    QHash<QWidget *,QPointer<WorkspaceDock>> _lastFocus;
};
