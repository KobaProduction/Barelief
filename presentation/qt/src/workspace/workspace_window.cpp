#include "core_app/workspace/workspace_window.h"
#include "barelief/ui_text.h"
#include <DockAreaWidget.h>
#include <DockAreaTitleBar.h>
#include <DockComponentsFactory.h>
#include <DockAreaTabBar.h>
#include <QToolButton>
#include <QShowEvent>
#include <FloatingDockContainer.h>
#include <QDockWidget>
#include <QApplication>
#include <QSplitter>
#include <QMenu>
#include <QShortcut>
#include <QSet>
#include <QTimer>
#include <QLabel>
#include <QLayout>
#include <QStyle>
#include <map>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

static void applyNativeFrame(QWidget *window) {
#ifdef Q_OS_WIN
    const auto palette=QApplication::palette();
    const BOOL dark=palette.color(QPalette::Window).lightness()<palette.color(QPalette::WindowText).lightness();
    const HWND handle=reinterpret_cast<HWND>(window->winId());
    // Unsupported attributes are ignored by older Windows versions.
    DwmSetWindowAttribute(handle,20,&dark,sizeof(dark));
    const auto background=palette.color(QPalette::Window),foreground=palette.color(QPalette::WindowText);
    const COLORREF caption=RGB(background.red(),background.green(),background.blue());
    const COLORREF text=RGB(foreground.red(),foreground.green(),foreground.blue());
    DwmSetWindowAttribute(handle,35,&caption,sizeof(caption));
    DwmSetWindowAttribute(handle,36,&text,sizeof(text));
#else
    Q_UNUSED(window);
#endif
}

namespace {
// Extend the library's title bar through its public factory. ADS retains all
// tab, mouse, preview, pin and close handling; only product controls are added.
class WorkspaceTitleBar final : public ads::CDockAreaTitleBar {
public:
    explicit WorkspaceTitleBar(ads::CDockAreaWidget *area) : ads::CDockAreaTitleBar(area) {
        connect(area,&ads::CDockAreaWidget::currentChanged,this,[this]{refreshControls();});
        auto refreshTabs=[this] { QTimer::singleShot(0,this,[this]{refreshControls();}); };
        connect(tabBar(),&ads::CDockAreaTabBar::tabClosed,this,refreshTabs);
        connect(tabBar(),&ads::CDockAreaTabBar::tabOpened,this,refreshTabs);
        connect(tabBar(),&ads::CDockAreaTabBar::tabInserted,this,refreshTabs);
        connect(tabBar(),&ads::CDockAreaTabBar::removingTab,this,refreshTabs);
        setupControls();
    }
    void setupControls() {
        const auto buttons=findChildren<QToolButton *>();
        for(auto *button:buttons) button->setFixedSize(24,24);
        layout()->setContentsMargins(6,2,6,2); layout()->setSpacing(4);
        if(auto *tabs=findChild<QToolButton *>("tabsMenuButton")) {
            layout()->removeWidget(tabs); insertWidget(0,tabs);
        }
        _fps=new QLabel(this); _fps->setObjectName("graphRenderStatus");
        _fps->setMinimumWidth(48); _fps->setAlignment(Qt::AlignCenter);
        insertWidget(indexOf(tabBar())+1,_fps); _fps->hide();
    }
    ~WorkspaceTitleBar() override { parkControls(); }
    void showEvent(QShowEvent *event) override {
        ads::CDockAreaTitleBar::showEvent(event); refreshControls();
    }
private:
    void parkControls() {
        if(_extra && _extra->parentWidget()==this) { _extra->hide(); _extra->setParent(_owner); }
        _extra=nullptr;
    }
public:
    void refreshControls() {
        if(auto *tabs=findChild<QToolButton *>("tabsMenuButton")) tabs->setVisible(dockAreaWidget()->openedDockWidgets().size()>1);
        refreshPlacementControls();
        refreshControlOwner();
        refreshFPS();
        auto *controls=_owner ? _owner->headerControls() : nullptr;
        if(controls==_extra) return;
        parkControls(); _extra=controls;
        if(_extra) {
            auto *placement=findChild<QToolButton *>("graphPlacement");
            insertWidget(placement ? indexOf(placement) : indexOf(_fps)+1,_extra); _extra->show();
        }
    }
    void refreshPlacementControls() {
        const auto buttons=findChildren<QToolButton *>();
        for(auto *button:buttons) if(button->objectName()=="graphPlacement") {
            if(!button->property("compactControl").toBool()) {
                button->setProperty("compactControl",true); button->setProperty("menuControl",true);
                button->setAutoRaise(false); button->setFixedHeight(24);
                button->style()->unpolish(button); button->style()->polish(button); button->updateGeometry();
            }
            layout()->removeWidget(_fps); insertWidget(indexOf(button),_fps);
        }
    }
    void refreshControlOwner() {
        auto *current=qobject_cast<WorkspaceDock *>(dockAreaWidget()->currentDockWidget());
        if(current!=_owner) {
            parkControls(); disconnect(_controlChange); disconnect(_fpsChange); _owner=current;
            if(current) _controlChange=connect(current,&WorkspaceDock::headerControlsChanged,this,[this]{refreshControls();});
            if(current) _fpsChange=connect(current,&WorkspaceDock::renderFPSChanged,this,[this]{refreshFPS();});
        }
    }
private:
    QPointer<WorkspaceDock> _owner;
    QPointer<QWidget> _extra;
    QMetaObject::Connection _controlChange;
    QMetaObject::Connection _fpsChange;
    QLabel *_fps=nullptr;
    void refreshFPS() { if(_fps) { _fps->setText(_owner ? _owner->renderFPS() : QString()); _fps->setVisible(!_fps->text().isEmpty()); } }
};
class WorkspaceComponents final : public ads::CDockComponentsFactory {
public:
    ads::CDockAreaTitleBar *createDockAreaTitleBar(ads::CDockAreaWidget *area) const override { return new WorkspaceTitleBar(area); }
};
}

WorkspaceDock::WorkspaceDock(const QString &title, QWidget *parent) : ads::CDockWidget(nullptr,title) { setParent(parent); }
void WorkspaceDock::raise() {
    toggleView(true);
    setAsCurrentTab();
    if (auto *floating=floatingDockContainer()) { floating->raise(); floating->activateWindow(); }
}
void WorkspaceDock::setFloating(bool floating) {
    if (floating && !ads::CDockWidget::isFloating()) { if (isClosed()) toggleView(true); ads::CDockWidget::setFloating(); }
    // Reinsertion always supplies an explicit area through WorkspaceWindow.
}
void WorkspaceDock::setHeaderControls(QWidget *widget) {
    _headerControls=widget; emit headerControlsChanged();
    if(auto *area=dockAreaWidget()) if(auto *bar=dynamic_cast<WorkspaceTitleBar *>(area->titleBar())) bar->refreshControls();
}
void WorkspaceDock::setRenderFPS(qreal value,bool visible) {
    const auto text=visible ? QString::number(qMax(qreal(0),value),'f',0)+QStringLiteral(" FPS") : QString();
    if(_fpsText==text) return;
    _fpsText=text; emit renderFPSChanged();
}

WorkspaceWindow::WorkspaceWindow(QWidget *parent) : QMainWindow(parent) {
    configureWorkspace();
    bindWorkspaceEvents();
}
void WorkspaceWindow::configureWorkspace() {
    ads::CDockManager::setConfigFlags(ads::CDockManager::DefaultNonOpaqueConfig |
        ads::CDockManager::DragPreviewIsDynamic |
        ads::CDockManager::FocusHighlighting | ads::CDockManager::EqualSplitOnInsertion |
        ads::CDockManager::DockAreaCloseButtonClosesTab);
    // Dynamic geometry preview does not need an expensive OpenGL screenshot.
    ads::CDockManager::setConfigFlag(ads::CDockManager::DragPreviewShowsContentPixmap,false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DragPreviewHasWindowFrame,false);
    ads::CDockManager::setAutoHideConfigFlags(ads::CDockManager::DefaultAutoHideConfig);
    _workspace=new ads::CDockManager(this);
    _workspace->setStyleSheet(_workspace->styleSheet()+ui::workspace_style());
    _workspace->setComponentsFactory(new WorkspaceComponents);
    _workspace->setObjectName("graphWorkspace");
}
void WorkspaceWindow::bindWorkspaceEvents() {
    connect(_workspace,&ads::CDockManager::dockWidgetAdded,this,[](ads::CDockWidget *dock) {
        if(auto *area=dock->dockAreaWidget()) if(auto *bar=dynamic_cast<WorkspaceTitleBar *>(area->titleBar())) bar->refreshControls();
    });
    connect(_workspace,&ads::CDockManager::floatingWidgetCreated,this,[](ads::CFloatingDockContainer *floating) {
        QTimer::singleShot(0,floating,[floating]{applyNativeFrame(floating);});
    });
    connect(qApp,&QGuiApplication::paletteChanged,this,[this] {
        const auto floatingWidgets=_workspace->floatingWidgets();
        for(auto *floating:floatingWidgets) applyNativeFrame(floating);
    });
    connect(_workspace,&ads::CDockManager::focusedDockWidgetChanged,this,
        [this](ads::CDockWidget *,ads::CDockWidget *current) {
            recordFocusedDock(current);
        });
}
void WorkspaceWindow::recordFocusedDock(ads::CDockWidget *current) {
    if (auto *dock=qobject_cast<WorkspaceDock *>(current)) {
        if(auto *container=dock->dockContainer()) {
            auto *host=container->window();
            if(!_lastFocus.contains(host)) connect(host,&QObject::destroyed,this,[this,host]{_lastFocus.remove(host);});
            _lastFocus[host]=dock;
        }
        emit graphAreaActivated(dock);
    }
}
WorkspaceWindow::~WorkspaceWindow() {
    disconnect(qApp,nullptr,this,nullptr);
    disconnect(_workspace,nullptr,this,nullptr);
    for(auto it=_lastFocus.cbegin();it!=_lastFocus.cend();++it) disconnect(it.key(),nullptr,this,nullptr);
    // ADS also owns parentless inactive tabs. Destroy it while adapter state is
    // alive, before QMainWindow tears down QObject children and floating hosts.
    delete _workspace;
}
QList<WorkspaceDock *> WorkspaceWindow::workspaceDocks() const {
    QList<WorkspaceDock *> result;
    const auto panels=_workspace->dockWidgetsMap();
    for(auto *panel:panels)
        if(auto *dock=qobject_cast<WorkspaceDock *>(panel)) result.append(dock);
    return result;
}
WorkspaceDock *WorkspaceWindow::workspaceDock(const QString &id) const {
    return qobject_cast<WorkspaceDock *>(_workspace->findDockWidget(id));
}
WorkspaceDock *WorkspaceWindow::activeDock() const {
    for(auto *widget=QApplication::focusWidget();widget;widget=widget->parentWidget())
        if(auto *dock=qobject_cast<WorkspaceDock *>(widget)) if(dock->dockManager()==_workspace) return dock;
    auto *active=QApplication::activeWindow();
    auto *last=_lastFocus.value(active).data();
    if(last && last->dockContainer() && last->dockContainer()->window()==active) return last;
    return nullptr;
}
void WorkspaceWindow::activateAdjacentTab(int direction) {
    auto *dock=activeDock(); auto *area=dock ? dock->dockAreaWidget() : nullptr;
    if(!area) return;
    const auto tabs=area->openedDockWidgets();
    if(tabs.isEmpty()) return;
    const int index=tabs.indexOf(area->currentDockWidget());
    auto *next=tabs[(std::max(0,index)+direction+tabs.size())%tabs.size()];
    next->setAsCurrentTab(); _workspace->setDockWidgetFocused(next);
}
static ads::DockWidgetArea adsArea(Qt::DockWidgetArea area) {
    switch(area) {
    case Qt::RightDockWidgetArea: return ads::RightDockWidgetArea;
    case Qt::TopDockWidgetArea: return ads::TopDockWidgetArea;
    case Qt::BottomDockWidgetArea: return ads::BottomDockWidgetArea;
    case Qt::NoDockWidgetArea: return ads::CenterDockWidgetArea;
    default: return ads::LeftDockWidgetArea;
    }
}
void WorkspaceWindow::placeDock(WorkspaceDock *dock,WorkspaceDock *anchor,Qt::DockWidgetArea side) {
    _workspace->addDockWidget(adsArea(side),dock,anchor ? anchor->dockAreaWidget() : nullptr);
    emit dock->dockLocationChanged();
}
void WorkspaceWindow::moveDockToHostEdge(WorkspaceDock *dock,Qt::DockWidgetArea side) {
    auto *container=dock->dockContainer();
    if(container) container->addDockWidget(adsArea(side),dock);
    else placeDock(dock,nullptr,side);
    emit dock->dockLocationChanged();
}
void WorkspaceWindow::focusDock(WorkspaceDock *dock) { _workspace->setDockWidgetFocused(dock); }
void WorkspaceWindow::addDockWidget(Qt::DockWidgetArea area, WorkspaceDock *dock) {
    _workspace->addDockWidget(adsArea(area),dock); emit dock->dockLocationChanged();
}
void WorkspaceWindow::removeDockWidget(WorkspaceDock *dock) { _workspace->removeDockWidget(dock); }
void WorkspaceWindow::splitDockWidget(WorkspaceDock *anchor,WorkspaceDock *dock,Qt::Orientation orientation) {
    _workspace->addDockWidget(orientation==Qt::Horizontal ? ads::RightDockWidgetArea : ads::BottomDockWidgetArea,
                              dock,anchor->dockAreaWidget());
    emit dock->dockLocationChanged();
}
void WorkspaceWindow::tabifyDockWidget(WorkspaceDock *anchor,WorkspaceDock *dock) {
    if (anchor==dock) return;
    _workspace->addDockWidgetTabToArea(dock,anchor->dockAreaWidget()); emit dock->dockLocationChanged();
}
QList<WorkspaceDock *> WorkspaceWindow::tabifiedDockWidgets(WorkspaceDock *dock) const {
    QList<WorkspaceDock *> result;
    if (auto *area=dock->dockAreaWidget()) {
        const auto peers=area->dockWidgets();
        for(auto *peer:peers)
            if(peer!=dock) if(auto *panel=qobject_cast<WorkspaceDock *>(peer)) result.append(panel);
    }
    return result;
}
void WorkspaceWindow::resizeDocks(const QList<WorkspaceDock *> &docks,const QList<int> &sizes,Qt::Orientation orientation) {
    // Preserve the old QMap pointer ordering when applying nested split sizes.
    std::map<QSplitter *,QList<int>> changes;
    for(int i=0;i<docks.size() && i<sizes.size();++i) {
        auto *area=docks[i]->dockAreaWidget();
        auto *splitter=area ? qobject_cast<QSplitter *>(area->parentWidget()) : nullptr;
        if(!splitter || splitter->orientation()!=orientation) continue;
        if(!changes.contains(splitter)) changes[splitter]=splitter->sizes();
        changes[splitter][splitter->indexOf(area)]=sizes[i];
    }
    for(const auto& [splitter, splitterSizes]:changes) splitter->setSizes(splitterSizes);
}
QByteArray WorkspaceWindow::saveState(int version) const { return _workspace->saveState(version); }
bool WorkspaceWindow::restoreState(const QByteArray &state,int version) { return _workspace->restoreState(state,version); }
using LegacyDockMapping = std::map<QDockWidget *,WorkspaceDock *>;
static LegacyDockMapping createLegacyProxies(WorkspaceWindow& owner,QMainWindow& legacy,bool oldNames) {
    const QMap<QString,QString> aliases{{"spectrogramDock","surfaceDock"},{"windowSignalDock","samplesDock"},{"windowedSignalDock","windowedSamplesDock"}};
    LegacyDockMapping mapping;
    const auto docks=owner.workspaceDocks();
    for(auto *dock:docks) {
        auto *proxy=new QDockWidget(&legacy);
        proxy->setObjectName(oldNames ? aliases.value(dock->objectName(),dock->objectName()) : dock->objectName());
        legacy.addDockWidget(Qt::LeftDockWidgetArea,proxy); mapping[proxy]=dock;
    }
    return mapping;
}
static void restoreLegacyDock(WorkspaceWindow& owner,QMainWindow& legacy,
    const LegacyDockMapping& mapping,QSet<QDockWidget *>& visited,
    QDockWidget* proxy,WorkspaceDock* dock) {
    if(proxy->isFloating()) {
        auto *floating=owner.workspace()->addDockWidgetFloating(dock);
        floating->setGeometry(proxy->geometry());
    } else {
        owner.workspace()->addDockWidget(adsArea(legacy.dockWidgetArea(proxy)),dock);
        const auto peers=legacy.tabifiedDockWidgets(proxy);
        for(auto *peer:peers)if(mapping.contains(peer)) {
            owner.workspace()->addDockWidgetTabToArea(mapping.at(peer),dock->dockAreaWidget()); visited.insert(peer);
            mapping.at(peer)->toggleView(!peer->isHidden());
        }
    }
    dock->toggleView(!proxy->isHidden());
}
bool WorkspaceWindow::importLegacyState(const QByteArray &state,int version,bool oldNames) {
    // Read the previous Qt format with empty proxies, never reparent renderers
    // into the legacy layout. Keep the original serialized field for downgrade.
    QMainWindow legacy;
    const auto mapping=createLegacyProxies(*this,legacy,oldNames);
    if (!legacy.restoreState(state,version)) return false;
    QSet<QDockWidget *> visited;
    for(auto it=mapping.cbegin();it!=mapping.cend();++it) {
        auto *proxy=it->first; auto *dock=it->second;
        if(visited.contains(proxy)) continue;
        visited.insert(proxy);
        restoreLegacyDock(*this,legacy,mapping,visited,proxy,dock);
    }
    return true;
}
