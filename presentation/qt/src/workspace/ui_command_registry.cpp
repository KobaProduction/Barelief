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
namespace presentation {
namespace {
bool in_menu(QWidget *widget) {
    for (auto *parent=widget; parent; parent=parent->parentWidget())
        if (qobject_cast<QMenu *>(parent)) return true;
    return false;
}
}
UiCommandRegistry::UiCommandRegistry(QWidget *window) : QObject(window), _window(window) {
    setObjectName("uiCommandRegistry");
    qApp->installEventFilter(this);
    connect(qApp,&QApplication::focusChanged,this,[this](QWidget *before,QWidget *after) {
        if (after && in_menu(after)) {
            if (before && !in_menu(before)) _popup_origin=before;
            else if (!_popup_origin) _popup_origin=_last_focus;
        } else if (after) {
            _last_focus=after;
            _popup_origin.clear();
        }
        refresh();
    });
    connect(qApp,&QGuiApplication::focusWindowChanged,this,[this] { refresh(); });
}
QAction *UiCommandRegistry::register_command(UiCommandDescriptor descriptor, QAction *existing) {
    if (_commands.contains(descriptor.id)) return action(descriptor.id);
    auto *item = existing ? existing : new QAction(this);
    item->setObjectName(descriptor.id); ui::bind(item,"text",descriptor.title_key);
    item->setShortcut(descriptor.shortcut); item->setShortcutContext(Qt::ApplicationShortcut);
    item->setShortcutVisibleInContextMenu(true); _window->addAction(item);
    const auto id = descriptor.id;
    if (existing && !descriptor.handler) {
        descriptor.handler = [guard=QPointer<QAction>(existing)] { if (guard) guard->trigger(); };
    }
    _commands.insert(id,{std::move(descriptor),item});
    if (!existing) connect(item,&QAction::triggered,this,[this,id] { execute(id); });
    refresh(); return item;
}
QAction *UiCommandRegistry::action(const QString &id) const { const auto it = _commands.constFind(id); return it == _commands.cend() ? nullptr : it->action.data(); }
void UiCommandRegistry::set_editor(QWidget *editor) { _editor=editor; refresh(); }
void UiCommandRegistry::set_workspace_guard(std::function<bool()> guard) { _workspace_guard=std::move(guard); refresh(); }
bool UiCommandRegistry::context_available(UiCommandContext context) const {
    if (!_window || QApplication::activeModalWidget()) return false;
    auto *popup=qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!popup) popup=qobject_cast<QMenu *>(_context_menu.data());
    const bool owned_popup=popup && _window->isAncestorOf(popup);
    if (_workspace_guard && !_workspace_guard() && !owned_popup) return false;
    if (context == UiCommandContext::Workspace) return true;
    auto *focus=owned_popup && _popup_origin ? _popup_origin.data() : QApplication::focusWidget();
    if (!_editor || !focus || !(focus==_editor || _editor->isAncestorOf(focus))) return false;
    for (auto *widget=focus; widget; widget=widget->parentWidget()) {
        if (qobject_cast<QLineEdit *>(widget) || qobject_cast<QTextEdit *>(widget) || qobject_cast<QPlainTextEdit *>(widget)
            || qobject_cast<QAbstractSpinBox *>(widget) || qobject_cast<QDialog *>(widget)) return false;
        if (widget==_editor) break;
    }
    return true;
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
    if (event->type()==QEvent::Show && qobject_cast<QMenu *>(object)) {
        auto *menu=qobject_cast<QWidget *>(object);
        if (_window && _window->isAncestorOf(menu)) _context_menu=menu;
        auto *focus=QApplication::focusWidget();
        if (focus && !in_menu(focus)) _popup_origin=focus;
        else if (!_popup_origin) _popup_origin=_last_focus;
    }
    if (event->type()==QEvent::Hide && qobject_cast<QMenu *>(object) && _context_menu) {
        // QAction dispatch follows popup dismissal. Retain its origin until that
        // synchronous dispatch completes, then return to live focus context.
        QTimer::singleShot(0,this,[this,menu=QPointer<QWidget>(_context_menu)] {
            if (_context_menu==menu && !QApplication::activePopupWidget()) {
                _context_menu.clear(); _popup_origin.clear(); refresh();
            }
        });
    }
    if (event->type()==QEvent::WindowActivate || event->type()==QEvent::WindowDeactivate || event->type()==QEvent::Show)
        QTimer::singleShot(0,this,[this] { refresh(); });
    return QObject::eventFilter(object,event);
}
}
