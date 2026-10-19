#pragma once
#include <QObject>
#include <QAction>
#include <QHash>
#include <QPointer>
#include <functional>
class QWidget;
class QMenu;
namespace presentation {
enum class UiCommandContext { Workspace, SchemaEditor };
struct UiCommandDescriptor {
    QString id;
    const char *title_key;
    QKeySequence shortcut;
    UiCommandContext context = UiCommandContext::Workspace;
    std::function<void()> handler;
    std::function<bool()> available;
};
class UiCommandRegistry final : public QObject {
    Q_OBJECT
public:
    explicit UiCommandRegistry(QWidget *window);
    QAction *register_command(UiCommandDescriptor descriptor, QAction *existing = nullptr);
    QAction *action(const QString &id) const;
    void set_editor(QWidget *editor);
    void set_workspace_guard(std::function<bool()> guard);
    bool can_execute(const QString &id) const;
    bool execute(const QString &id);
    void refresh();
private:
    struct Entry { UiCommandDescriptor descriptor; QPointer<QAction> action; };
    QHash<QString, Entry> _commands;
    QPointer<QWidget> _window, _editor;
    QPointer<QWidget> _last_focus, _popup_origin;
    QPointer<QWidget> _context_menu;
    std::function<bool()> _workspace_guard;
    bool context_available(UiCommandContext) const;
    bool workspace_available(bool& owned_popup) const;
    bool editor_context_available(bool owned_popup) const;
    void track_focus(QWidget *before,QWidget *after);
    void configure_action(QAction *item,const UiCommandDescriptor& descriptor);
    void record_menu_show(QObject *object);
    void record_menu_hide();
    void schedule_refresh();
    bool eventFilter(QObject *, QEvent *) override;
};
}
