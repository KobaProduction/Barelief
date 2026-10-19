#include "core_app/workspace/ui_command_registry.h"
#include "visualization/ui_text.h"
#include <QApplication>
#include <QWidget>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QAbstractSpinBox>
#include <QDialog>
#include <QTimer>
#include <QEvent>
#include <QMenu>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
namespace presentation {
namespace {
bool in_menu(QWidget *widget) {
    for (auto *parent=widget; parent; parent=parent->parentWidget())
        if (qobject_cast<QMenu *>(parent)) return true;
    return false;
}
bool owned_by(const QObject *object, const QObject *owner) {
    for (auto *parent=object; parent; parent=parent->parent())
        if (parent==owner) return true;
    return false;
}
bool owns_local_history(QWidget *widget) {
    for (auto *parent=widget; parent; parent=parent->parentWidget())
        if (qobject_cast<QLineEdit *>(parent) || qobject_cast<QTextEdit *>(parent)
            || qobject_cast<QPlainTextEdit *>(parent) || qobject_cast<QAbstractSpinBox *>(parent)
            || qobject_cast<QDialog *>(parent)) return true;
    return false;
}
bool embedded_local_history(QWidget *focus) {
    for (auto *widget=focus; widget; widget=widget->parentWidget()) {
        auto *view=qobject_cast<QGraphicsView *>(widget);
        if (!view || !view->scene()) continue;
        for (auto *item=view->scene()->focusItem(); item; item=item->parentItem())
            if (auto *proxy=qgraphicsitem_cast<QGraphicsProxyWidget *>(item))
                return proxy->widget() && owns_local_history(proxy->widget()->focusWidget());
    }
    return false;
}
}
UiCommandRegistry::UiCommandRegistry(QWidget *window) : QObject(window), _window(window) {
    setObjectName("uiCommandRegistry");
    qApp->installEventFilter(this);
    connect(qApp,&QApplication::focusChanged,this,
        [this](QWidget *before,QWidget *after) { track_focus(before,after); });
    connect(qApp,&QGuiApplication::focusWindowChanged,this,[this] { refresh(); });
}
void UiCommandRegistry::track_focus(QWidget *before,QWidget *after) {
    if (after && in_menu(after)) {
        if (before && !in_menu(before)) _popup_origin=before;
        else if (!_popup_origin) _popup_origin=_last_focus;
    } else if (after) {
        _last_focus=after;
        if (!_context_menu) _popup_origin.clear();
    }
    refresh();
}
QAction *UiCommandRegistry::register_command(UiCommandDescriptor descriptor, QAction *existing) {
    if (_commands.contains(descriptor.id)) return action(descriptor.id);
    auto *item = existing ? existing : new QAction(this);
    configure_action(item,descriptor);
    const auto id = descriptor.id;
    if (existing && !descriptor.handler) {
        descriptor.handler = [guard=QPointer<QAction>(existing)] { if (guard) guard->trigger(); };
    }
    _commands.insert(id,{std::move(descriptor),item});
    if (!existing) connect(item,&QAction::triggered,this,[this,id] { execute(id); });
    refresh(); return item;
}
void UiCommandRegistry::configure_action(QAction *item,const UiCommandDescriptor& descriptor) {
    item->setObjectName(descriptor.id);
    ui::bind(item,"text",descriptor.title_key);
    item->setShortcut(descriptor.shortcut);
    item->setShortcutContext(Qt::ApplicationShortcut);
    item->setShortcutVisibleInContextMenu(true);
    _window->addAction(item);
}
QAction *UiCommandRegistry::action(const QString &id) const { const auto it = _commands.constFind(id); return it == _commands.cend() ? nullptr : it->action.data(); }
void UiCommandRegistry::set_editor(QWidget *editor) { _editor=editor; refresh(); }
void UiCommandRegistry::set_workspace_guard(std::function<bool()> guard) { _workspace_guard=std::move(guard); refresh(); }
bool UiCommandRegistry::context_available(UiCommandContext context) const {
    bool owned_popup=false;
    if (!workspace_available(owned_popup)) return false;
    if (context == UiCommandContext::Workspace) return true;
    return editor_context_available(owned_popup);
}
bool UiCommandRegistry::workspace_available(bool& owned_popup) const {
    if (!_window || QApplication::activeModalWidget()) return false;
    auto *active_popup=QApplication::activePopupWidget();
    auto *popup=qobject_cast<QMenu *>(active_popup);
    if (active_popup && (!popup || !owned_by(popup,_window))) return false;
    if (!popup) popup=qobject_cast<QMenu *>(_context_menu.data());
    owned_popup=popup && owned_by(popup,_window);
    if (_workspace_guard && !_workspace_guard() && !owned_popup) return false;
    return true;
}
bool UiCommandRegistry::editor_context_available(bool owned_popup) const {
    // A menu may contain its own editor (the node catalog filter, for example).
    // Its live text focus takes precedence over the canvas that opened it.
    if (owns_local_history(QApplication::focusWidget()) || embedded_local_history(QApplication::focusWidget())) return false;
    auto *focus=owned_popup && _popup_origin ? _popup_origin.data() : QApplication::focusWidget();
    if (!_editor || !focus || !(focus==_editor || _editor->isAncestorOf(focus))) return false;
    return !owns_local_history(focus) && !embedded_local_history(focus);
}
bool UiCommandRegistry::can_execute(const QString &id) const {
    const auto it=_commands.constFind(id);
    return it!=_commands.cend() && context_available(it->descriptor.context) && (!it->descriptor.available || it->descriptor.available());
}
bool UiCommandRegistry::execute(const QString &id) {
    if (!can_execute(id)) return false;
    const auto handler=_commands.value(id).descriptor.handler;
    if (handler) handler();
    return true;
}
void UiCommandRegistry::refresh() { for (auto it=_commands.begin(); it!=_commands.end(); ++it) if (it->action) it->action->setEnabled(can_execute(it.key())); }
bool UiCommandRegistry::eventFilter(QObject *object, QEvent *event) {
    // Proxy-widget focus can change while QApplication still reports the same
    // graphics view. Refresh before Qt resolves the next shortcut against actions.
    if (event->type()==QEvent::ShortcutOverride) refresh();
    if (event->type()==QEvent::FocusIn || event->type()==QEvent::FocusOut) schedule_refresh();
    if (event->type()==QEvent::Show && qobject_cast<QMenu *>(object)) record_menu_show(object);
    if (event->type()==QEvent::Hide && qobject_cast<QMenu *>(object) && _context_menu) record_menu_hide();
    if (event->type()==QEvent::WindowActivate || event->type()==QEvent::WindowDeactivate || event->type()==QEvent::Show)
        schedule_refresh();
    return QObject::eventFilter(object,event);
}
void UiCommandRegistry::schedule_refresh() {
    QTimer::singleShot(0,this,[this] { refresh(); });
}
void UiCommandRegistry::record_menu_show(QObject *object) {
    auto *menu=qobject_cast<QWidget *>(object);
    // Popup menus are top-level windows even when QObject-owned by the host.
    if (_window && owned_by(menu,_window)) _context_menu=menu;
    auto *focus=QApplication::focusWidget();
    if (focus && !in_menu(focus)) _popup_origin=focus;
    else if (!_popup_origin) _popup_origin=_last_focus;
}
void UiCommandRegistry::record_menu_hide() {
    // Keep origin through QAction dispatch, then return to live focus.
    QTimer::singleShot(0,this,[this,menu=QPointer<QWidget>(_context_menu)] {
        if (_context_menu==menu && !QApplication::activePopupWidget()) {
            _context_menu.clear();
            _popup_origin.clear();
            refresh();
        }
    });
}
}
