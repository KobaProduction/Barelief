#include "numeric_gui_fixture.h"
#include "menu_row_font_fixture.h"
#include <core_app/settings_widget.h>
#include <barelief/ui_text.h>
#include <QComboBox>
#include <QFile>
#include <QMenuBar>
#include <QToolBar>
#include <QEventLoop>
#include <cstdio>

namespace {
using gui_fixture::check;
QJsonObject future_preference() { return {{"retained", 37}}; }
domain::NumericProcessingGraph persistence_graph() {
    // Persistence/menu checks need a real editable source, not unrelated
    // FFT/History preparation and Qt Quick render work.
    const auto example = numeric_test_graph();
    domain::NumericProcessingGraph graph;
    graph.graph_id = "ui-persistence";
    graph.nodes.push_back(example.nodes.front());
    return graph;
}
void report_phase(const QElapsedTimer &clock, const char *phase) {
    std::fprintf(stderr, "UI persistence phase: %s (%lld ms)\n", phase,
                 static_cast<long long>(clock.elapsed()));
    std::fflush(stderr);
}
class MenuPaintCounter final : public QObject {
  public:
    int paints = 0;
  protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Paint) ++paints;
        return false;
    }
};
SettingsWidget *settings_widget(gui_fixture::App &fixture) {
    auto *settings = fixture.window->findChild<SettingsWidget *>();
    check(settings != nullptr, "settings fixture");
    return settings;
}
QComboBox *theme_selector(gui_fixture::App &fixture) {
    auto *theme = settings_widget(fixture)->findChild<QComboBox *>("interfaceTheme");
    check(theme != nullptr, "theme selector");
    return theme;
}
QLabel *save_status(gui_fixture::App &fixture) {
    auto *status = fixture.window->findChild<QLabel *>("configurationSaveStatus");
    check(status != nullptr, "persistent configuration save status");
    return status;
}
bool status_is(gui_fixture::App &fixture, MainWindow::ConfigurationSaveState state) {
    return save_status(fixture)->property("saveState").toInt() == int(state);
}
void drain_menu_work() {
    // Drain zero-delay corner follow-ups without waiting for periodic timers.
    for (int pass = 0; pass < 8; ++pass) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
    }
}
class MenuChecks {
    MainWindow &window;
    QMenuBar *menu;
    QLabel *status;
    QWidget *navigation;
    QFont font;
    int menu_right() const {
        int right = 0;
        const auto actions = menu->actions();
        for (auto *action : actions)
            if (action->isVisible()) right = qMax(right, menu->actionGeometry(action).right());
        return right;
    }
    bool row_fits() const {
        const QRect statusRect(status->mapTo(menu, QPoint{}), status->size());
        const QRect navigationRect(navigation->mapTo(menu, QPoint{}), navigation->size());
        return status->isVisible() && statusRect.width() > 0 &&
               menu->rect().contains(statusRect) && statusRect.left() > menu_right() &&
               statusRect.right() < navigationRect.left() &&
               qAbs(statusRect.center().y() - navigationRect.center().y()) <= 2;
    }
    void diagnose_row() const {
        qWarning() << "Menu status layout" << menu->geometry() << "status" << status->geometry()
                   << "navigation" << navigation->geometry() << "parent" << navigation->parentWidget()
                   << "visible" << status->isVisible() << "corner" << menu->cornerWidget(Qt::TopRightCorner);
        const auto actions = menu->actions();
        for (auto *action : actions) qWarning() << action->text() << menu->actionGeometry(action);
    }
    bool check_row() const {
        const bool result = gui_fixture::wait([this] { return row_fits(); });
        if (!result) diagnose_row();
        return result;
    }
    void set_font(const QFont &value) {
        window.setFont(value);
        set_menu_row_fixture_font(menu, value);
    }
    void request_unchanged_layout() const {
        for (int request = 0; request < 20; ++request) {
            QEvent unchangedLayout(QEvent::LayoutRequest);
            QCoreApplication::sendEvent(navigation, &unchangedLayout);
        }
        drain_menu_work();
    }
  public:
    explicit MenuChecks(gui_fixture::App &fixture)
        : window(*fixture.window), menu(window.menuBar()), status(save_status(fixture)),
          navigation(window.findChild<QWidget *>("navigationControls")), font(menu_row_fixture_font()) {
        set_font(font);
        check(navigation && status->parentWidget() == menu, "save status belongs to the menu row");
        check(!window.findChild<QToolBar *>("configurationSaveToolbar"), "save status does not add a toolbar row");
        check(check_row(), "save status shares menu/action/navigation horizontal row");
    }
    void repaint_checks() const {
        MenuPaintCounter counter;
        menu->installEventFilter(&counter);
        drain_menu_work();
        menu->update();
        drain_menu_work();
        check(counter.paints > 0, "menu repaint observer sees a requested paint");
        const auto before = counter.paints;
        const auto hint = navigation->sizeHint();
        request_unchanged_layout();
        check(navigation->sizeHint() == hint, "idle corner layout requests preserve their sizing input");
        check(counter.paints == before, "unchanged corner layout requests do not repaint the menu raster");
    }
    void resize_checks() {
        const auto initialSize = window.size();
        window.resize(800, initialSize.height());
        ui::Language::instance().set("en");
        check(check_row(), "menu status avoids actions/navigation after resize and translation");
        window.resize(initialSize);
        ui::Language::instance().set("ru");
    }
    void exhausted_gap_checks() {
        auto oversized = font;
        oversized.setPointSize(40);
        set_font(oversized);
        check(gui_fixture::wait([this] { return status->width() == 0; }),
              "save status yields an exhausted menu gap without overlapping controls");
        check(!status->text().isEmpty() && status->accessibleName() == status->text(),
              "elided save status retains full accessible state");
        set_font(font);
        check(check_row(), "save status reappears when menu space returns");
    }
};
void menu_checks(gui_fixture::App &fixture) {
    MenuChecks menu(fixture);
    menu.repaint_checks();
    menu.resize_checks();
    menu.exhausted_gap_checks();
}
QString project_path(const gui_fixture::App &fixture) { return QString::fromLocal8Bit(fixture.path); }
struct StoredProject {
    desktop::NumericProjectDocument document;
    domain::NumericProcessingGraph graph;
    std::optional<domain::NumericProcessingGraph> applied;
};
StoredProject read_project(const QString &path, const char *message, const char *appliedMessage) {
    StoredProject result;
    check(bool(desktop::load_numeric_project(path, result.document, result.graph)), message);
    check(bool(desktop::read_numeric_applied(result.document, result.applied)), appliedMessage);
    return result;
}
QJsonObject read_preferences(const desktop::NumericProjectDocument &document, const char *message) {
    QJsonObject preferences;
    check(bool(desktop::read_numeric_application_settings(document, preferences)), message);
    return preferences;
}
void edit_unsaved_draft(gui_fixture::App &fixture, const domain::NumericProcessingGraph &saved) {
    auto changed = saved;
    changed.nodes.back().enabled = !changed.nodes.back().enabled;
    check(fixture.editor->load_numeric_draft(changed), "unsaved valid draft edit");
    emit fixture.editor->numeric_draft_changed();
    check(fixture.application->_ctx.editor_draft_modified, "draft remains dirty before UI preferences edit");
    check(status_is(fixture, MainWindow::ConfigurationSaveState::Unsaved), "draft edit shows unsaved configuration");
}
void check_unchanged_graph(gui_fixture::App &fixture, const StoredProject &before, const StoredProject &after) {
    if (gui_fixture::encoded(after.graph) != gui_fixture::encoded(before.graph)) {
        qWarning() << "Preferences draft diagnostic dirty/pending" << fixture.application->_ctx.editor_draft_modified
                   << fixture.application->presenter().graph_request_pending();
        qWarning() << "Stored baseline" << gui_fixture::encoded(before.graph);
        qWarning() << "Stored after preferences" << gui_fixture::encoded(after.graph);
    }
    check(gui_fixture::encoded(after.graph) == gui_fixture::encoded(before.graph), "preferences edit does not save unsaved draft");
    check(before.applied.has_value() == after.applied.has_value(), "preferences preserve applied absence");
    if (before.applied)
        check(gui_fixture::encoded(*before.applied) == gui_fixture::encoded(*after.applied), "preferences do not stage Apply");
}
void preferences_checks(gui_fixture::App &fixture) {
    const auto path = project_path(fixture);
    const auto before = read_project(path, "read baseline project", "read applied baseline");
    edit_unsaved_draft(fixture, before.graph);
    // Explicit preferences persistence must not flush the dirty project.
    fixture.application->_ctx.file_configs.ui.autosave_enabled = false;
    theme_selector(fixture)->setCurrentIndex(1);
    emit fixture.window->settings_change_requested({ui::SettingsChangeScope::ApplicationPreferences,true});
    const auto after = read_project(path, "read preferences-only commit", "read applied after preferences edit");
    check_unchanged_graph(fixture, before, after);
    check(fixture.application->_ctx.editor_draft_modified, "preferences save does not clear project dirty state");
    check(status_is(fixture, MainWindow::ConfigurationSaveState::Unsaved), "preferences-only commit does not claim draft was saved");
    const auto preferences = read_preferences(after.document, "read persisted preferences");
    check(preferences.value("ui").toObject().value("theme") == "dark", "theme persisted independently");
}
QString foreign_window_geometry() {
    QWidget foreignGeometry;
    foreignGeometry.resize(800, 600);
    foreignGeometry.move(210, 140);
    return QString::fromLatin1(foreignGeometry.saveGeometry().toBase64());
}
QJsonObject geometry_preset(gui_fixture::App &fixture) {
    check(fixture.editor->save_preset("Layout boundary"), "capture layout preset");
    const auto preset = fixture.application->_ctx.file_configs.ui.pipeline_presets.last().toObject();
    auto presentation = preset.value("presentation").toObject();
    auto workspace = presentation.value("workspace").toObject();
    check(!workspace.contains("geometry"), "new presets exclude main window geometry");
    workspace["geometry"] = foreign_window_geometry();
    presentation["workspace"] = workspace;
    return presentation;
}
void preset_geometry_checks(gui_fixture::App &fixture) {
    auto &app = *fixture.application;
    app._ctx.file_configs.ui.autosave_enabled = false;
    fixture.window->resize(1000, 700);
    QCoreApplication::processEvents();
    const auto presentation = geometry_preset(fixture);
    const auto session = fixture.window->saveGeometry();
    emit fixture.editor->preset_presentation_requested(presentation);
    QCoreApplication::processEvents();
    check(fixture.window->saveGeometry() == session, "old preset geometry ignored during apply");
    check(app._ctx.file_configs.ui.workspace.value("geometry").toString() == QString::fromLatin1(session.toBase64()),
          "session geometry retained in workspace settings");
}
QByteArray read_saved_bytes(const QString &path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "read saved bytes");
    return file.readAll();
}
QAction *save_action(gui_fixture::App &fixture) {
    auto *action = fixture.window->findChild<QAction *>("saveSettings");
    check(action != nullptr, "manual save action");
    return action;
}
void edit_preferences_without_autosave(gui_fixture::App &fixture) {
    fixture.window->resize(1010, 710);
    check(gui_fixture::wait([&] { return status_is(fixture, MainWindow::ConfigurationSaveState::Unsaved); }),
          "workspace edit shows unsaved with autosave off");
    theme_selector(fixture)->setCurrentIndex(2);
    check(status_is(fixture, MainWindow::ConfigurationSaveState::Unsaved), "preferences edit shows unsaved with autosave off");
    fixture.application->save_configuration_as({});
    check(status_is(fixture, MainWindow::ConfigurationSaveState::Unsaved), "cancelled Save As preserves dirty status");
}
QString blocked_save_path(gui_fixture::App &fixture) {
    // A regular file as parent rejects commit without permission assumptions.
    const auto parent = fixture.directory.filePath("blocked-parent");
    QFile blocker(parent);
    check(blocker.open(QIODevice::WriteOnly) && blocker.write("block") == 5, "create isolated save failure fixture");
    return parent + "/failed.json";
}
void failed_save_checks(gui_fixture::App &fixture, const QByteArray &savedBytes) {
    const auto blockedPath = blocked_save_path(fixture);
    fixture.window->resize(1020, 720);
    fixture.application->save_configuration_as(blockedPath);
    check(status_is(fixture, MainWindow::ConfigurationSaveState::Error), "failed disk commit reports save error");
    QElapsedTimer deferredEdits;
    deferredEdits.start();
    gui_fixture::wait([&] { return deferredEdits.elapsed() >= 650; });
    check(status_is(fixture, MainWindow::ConfigurationSaveState::Error), "queued unsaved workspace notification retains save error");
    check(read_saved_bytes(project_path(fixture)) == savedBytes, "failed Save As preserves original file");
    fixture.application->save_configuration_as({});
    check(status_is(fixture, MainWindow::ConfigurationSaveState::Error), "cancelled retry retains save error");
}
void retry_save_checks(gui_fixture::App &fixture) {
    save_action(fixture)->trigger();
    check(status_is(fixture, MainWindow::ConfigurationSaveState::Saved), "successful retry clears save error");
    desktop::NumericProjectDocument after;
    domain::NumericProcessingGraph persisted;
    check(bool(desktop::load_numeric_project(project_path(fixture), after, persisted)), "read successful retry");
    const auto preferences = read_preferences(after, "read retry preferences");
    check(preferences.value("ui").toObject().value("theme") == "light", "retry persists latest unsaved preference");
    check(preferences.value("ui").toObject().value("future_save_status_preference").toObject() == future_preference(),
          "save status bookkeeping preserves unknown source preferences");
}
void save_checks(gui_fixture::App &fixture) {
    save_action(fixture)->trigger();
    check(status_is(fixture, MainWindow::ConfigurationSaveState::Saved), "manual full save reports disk commit");
    const auto savedBytes = read_saved_bytes(project_path(fixture));
    edit_preferences_without_autosave(fixture);
    failed_save_checks(fixture, savedBytes);
    retry_save_checks(fixture);
}
void run_checks() {
    QElapsedTimer clock;
    clock.start();
    gui_fixture::App fixture(persistence_graph(), {{"future_save_status_preference", future_preference()}});
    report_phase(clock, "fixture ready");
    settings_widget(fixture);
    menu_checks(fixture);
    report_phase(clock, "menu layout and repaint assertions complete");
    preferences_checks(fixture);
    report_phase(clock, "preferences-only commit assertions complete");
    preset_geometry_checks(fixture);
    save_checks(fixture);
    report_phase(clock, "save failure and retry assertions complete");
}
}
int main() {
    try {
        run_checks();
        std::puts("UI preferences/project/preset boundaries PASS");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "UI persistence FAIL: %s\n", error.what());
        return 1;
    }
}
