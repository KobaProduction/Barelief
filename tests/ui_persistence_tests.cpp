#include "numeric_gui_fixture.h"
#include "menu_row_font_fixture.h"
#include <core_app/settings_widget.h>
#include <visualization/ui_text.h>
#include <QComboBox>
#include <QFile>
#include <QMenuBar>
#include <QToolBar>
#include <QEventLoop>
#include <cstdio>

namespace {
class MenuPaintCounter final : public QObject {
  public:
    int paints = 0;
  protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Paint) ++paints;
        return false;
    }
};
}

int main() {
    try {
        const QJsonObject futurePreference{{"retained", 37}};
        gui_fixture::App fixture(numeric_test_graph(), {{"future_save_status_preference", futurePreference}});
        const auto menuFont = menu_row_fixture_font();
        fixture.window->setFont(menuFont);
        set_menu_row_fixture_font(fixture.window->menuBar(), menuFont);
        auto &app = *fixture.application;
        auto *settings = fixture.window->findChild<SettingsWidget *>();
        gui_fixture::check(settings != nullptr, "settings fixture");
        auto *saveStatus = fixture.window->findChild<QLabel *>("configurationSaveStatus");
        gui_fixture::check(saveStatus != nullptr, "persistent configuration save status");
        auto *menuBar = fixture.window->menuBar();
        auto *navigation = fixture.window->findChild<QWidget *>("navigationControls");
        gui_fixture::check(navigation && saveStatus->parentWidget() == menuBar,
                           "save status belongs to the menu row");
        gui_fixture::check(!fixture.window->findChild<QToolBar *>("configurationSaveToolbar"),
                           "save status does not add a toolbar row");
        const auto checkMenuRow = [&] {
            const bool result = gui_fixture::wait([&] {
                const QRect statusRect(saveStatus->mapTo(menuBar, QPoint{}), saveStatus->size());
                const QRect navigationRect(navigation->mapTo(menuBar, QPoint{}), navigation->size());
                int menuRight = 0;
                for (auto *action : menuBar->actions())
                    if (action->isVisible()) menuRight = qMax(menuRight, menuBar->actionGeometry(action).right());
                return saveStatus->isVisible() && statusRect.width() > 0 &&
                       menuBar->rect().contains(statusRect) && statusRect.left() > menuRight &&
                       statusRect.right() < navigationRect.left() &&
                       qAbs(statusRect.center().y() - navigationRect.center().y()) <= 2;
            });
            if (!result) {
                qWarning()<<"Menu status layout"<<menuBar->geometry()<<"status"<<saveStatus->geometry()
                          <<"navigation"<<navigation->geometry()<<"parent"<<navigation->parentWidget()
                          <<"visible"<<saveStatus->isVisible()<<"corner"<<menuBar->cornerWidget(Qt::TopRightCorner);
                for (auto *action : menuBar->actions()) qWarning()<<action->text()<<menuBar->actionGeometry(action);
            }
            return result;
        };
        gui_fixture::check(checkMenuRow(), "save status shares menu/action/navigation horizontal row");
        MenuPaintCounter menuPaints;
        menuBar->installEventFilter(&menuPaints);
        const auto drainMenuWork = [] {
            // Drain the corner's zero-delay follow-up and resulting layout/paint
            // work without waiting for unrelated periodic application timers.
            for (int pass = 0; pass < 8; ++pass) {
                QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
                QCoreApplication::processEvents(QEventLoop::AllEvents);
                QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
            }
        };
        drainMenuWork();
        menuBar->update();
        drainMenuWork();
        gui_fixture::check(menuPaints.paints > 0, "menu repaint observer sees a requested paint");
        const auto paintsBeforeUnchangedLayout = menuPaints.paints;
        const auto navigationHint = navigation->sizeHint();
        for (int request = 0; request < 20; ++request) {
            QEvent unchangedLayout(QEvent::LayoutRequest);
            QCoreApplication::sendEvent(navigation, &unchangedLayout);
        }
        drainMenuWork();
        gui_fixture::check(navigation->sizeHint() == navigationHint,
                           "idle corner layout requests preserve their sizing input");
        gui_fixture::check(menuPaints.paints == paintsBeforeUnchangedLayout,
                           "unchanged corner layout requests do not repaint the menu raster");
        const auto initialWindowSize = fixture.window->size();
        fixture.window->resize(800, initialWindowSize.height());
        ui::Language::instance().set("en");
        gui_fixture::check(checkMenuRow(), "menu status avoids actions/navigation after resize and translation");
        fixture.window->resize(initialWindowSize);
        ui::Language::instance().set("ru");
        // An oversized font deliberately exhausts the existing menu/navigation
        // gap. The status must yield that space, retaining its semantic text.
        auto oversizedFont = menuFont;
        oversizedFont.setPointSize(40);
        fixture.window->setFont(oversizedFont);
        set_menu_row_fixture_font(menuBar, oversizedFont);
        gui_fixture::check(gui_fixture::wait([&] { return saveStatus->width() == 0; }),
                           "save status yields an exhausted menu gap without overlapping controls");
        gui_fixture::check(!saveStatus->text().isEmpty() && saveStatus->accessibleName() == saveStatus->text(),
                           "elided save status retains full accessible state");
        fixture.window->setFont(menuFont);
        set_menu_row_fixture_font(menuBar, menuFont);
        gui_fixture::check(checkMenuRow(), "save status reappears when menu space returns");
        const auto statusIs = [saveStatus](MainWindow::ConfigurationSaveState state) {
            return saveStatus->property("saveState").toInt() == int(state);
        };
        const auto path = QString::fromLocal8Bit(fixture.path);
        desktop::NumericProjectDocument before;
        domain::NumericProcessingGraph saved;
        gui_fixture::check(bool(desktop::load_numeric_project(path, before, saved)), "read baseline project");
        std::optional<domain::NumericProcessingGraph> appliedBefore;
        gui_fixture::check(bool(desktop::read_numeric_applied(before, appliedBefore)), "read applied baseline");
        auto changed = saved; changed.nodes.back().enabled = !changed.nodes.back().enabled;
        gui_fixture::check(fixture.editor->load_numeric_draft(changed), "unsaved valid draft edit");
        emit fixture.editor->numeric_draft_changed();
        gui_fixture::check(app._ctx.editor_draft_modified, "draft remains dirty before UI preferences edit");
        gui_fixture::check(statusIs(MainWindow::ConfigurationSaveState::Unsaved), "draft edit shows unsaved configuration");
        app._ctx.file_configs.ui.autosave_enabled = true;
        auto *theme = settings->findChild<QComboBox *>("interfaceTheme");
        gui_fixture::check(theme != nullptr, "theme selector");
        theme->setCurrentIndex(1); // Dark, routed as application preferences only.
        // The preferences commit is synchronous. Do not also run the independent
        // editor debounce/autosave timer while measuring this command's delta.
        desktop::NumericProjectDocument after;
        domain::NumericProcessingGraph persisted;
        gui_fixture::check(bool(desktop::load_numeric_project(path, after, persisted)), "read preferences-only commit");
        if(gui_fixture::encoded(persisted) != gui_fixture::encoded(saved)) {
            qWarning()<<"Preferences draft diagnostic dirty/pending"<<app._ctx.editor_draft_modified<<app.presenter().graph_request_pending();
            qWarning()<<"Stored baseline"<<gui_fixture::encoded(saved);
            qWarning()<<"Stored after preferences"<<gui_fixture::encoded(persisted);
        }
        gui_fixture::check(gui_fixture::encoded(persisted) == gui_fixture::encoded(saved), "preferences edit does not save unsaved draft");
        gui_fixture::check(app._ctx.editor_draft_modified, "preferences save does not clear project dirty state");
        gui_fixture::check(statusIs(MainWindow::ConfigurationSaveState::Unsaved), "preferences-only commit does not claim draft was saved");
        QJsonObject preferences;
        gui_fixture::check(bool(desktop::read_numeric_application_settings(after, preferences)), "read persisted preferences");
        gui_fixture::check(preferences.value("ui").toObject().value("theme") == "dark", "theme persisted independently");
        std::optional<domain::NumericProcessingGraph> appliedAfter;
        gui_fixture::check(bool(desktop::read_numeric_applied(after, appliedAfter)), "read applied after preferences edit");
        gui_fixture::check(appliedBefore.has_value() == appliedAfter.has_value(), "preferences preserve applied absence");
        if (appliedBefore)
            gui_fixture::check(gui_fixture::encoded(*appliedBefore) == gui_fixture::encoded(*appliedAfter), "preferences do not stage Apply");

        app._ctx.file_configs.ui.autosave_enabled = false;
        fixture.window->resize(1000, 700); QCoreApplication::processEvents();
        gui_fixture::check(fixture.editor->save_preset("Layout boundary"), "capture layout preset");
        auto preset = app._ctx.file_configs.ui.pipeline_presets.last().toObject();
        auto presentation = preset.value("presentation").toObject();
        auto workspace = presentation.value("workspace").toObject();
        gui_fixture::check(!workspace.contains("geometry"), "new presets exclude main window geometry");
        const auto session = fixture.window->saveGeometry();
        QWidget foreignGeometry; foreignGeometry.resize(800, 600); foreignGeometry.move(210, 140);
        workspace["geometry"] = QString::fromLatin1(foreignGeometry.saveGeometry().toBase64());
        presentation["workspace"] = workspace;
        emit fixture.editor->preset_presentation_requested(presentation); QCoreApplication::processEvents();
        gui_fixture::check(fixture.window->saveGeometry() == session, "old preset geometry ignored during apply");
        gui_fixture::check(app._ctx.file_configs.ui.workspace.value("geometry").toString() == QString::fromLatin1(session.toBase64()),
                           "session geometry retained in workspace settings");
        auto *saveAction = fixture.window->findChild<QAction *>("saveSettings");
        gui_fixture::check(saveAction != nullptr, "manual save action");
        saveAction->trigger();
        gui_fixture::check(statusIs(MainWindow::ConfigurationSaveState::Saved), "manual full save reports disk commit");
        QFile baselineFile(path);
        gui_fixture::check(baselineFile.open(QIODevice::ReadOnly), "read saved bytes");
        const auto savedBytes = baselineFile.readAll(); baselineFile.close();
        fixture.window->resize(1010, 710);
        gui_fixture::check(gui_fixture::wait([&] { return statusIs(MainWindow::ConfigurationSaveState::Unsaved); }),
                           "workspace edit shows unsaved with autosave off");
        theme->setCurrentIndex(2); // Light remains an unsaved preference when autosave is off.
        gui_fixture::check(statusIs(MainWindow::ConfigurationSaveState::Unsaved), "preferences edit shows unsaved with autosave off");
        app.save_configuration_as({});
        gui_fixture::check(statusIs(MainWindow::ConfigurationSaveState::Unsaved), "cancelled Save As preserves dirty status");
        // Missing directories are created by the writer. A regular file in the
        // parent path reliably rejects disk commit without platform permissions.
        const auto blockedParent = fixture.directory.filePath("blocked-parent");
        QFile blocker(blockedParent);
        gui_fixture::check(blocker.open(QIODevice::WriteOnly) && blocker.write("block") == 5,
                           "create isolated save failure fixture");
        blocker.close();
        fixture.window->resize(1020, 720); // Queue a workspace notification behind the failed save.
        app.save_configuration_as(blockedParent + "/failed.json");
        gui_fixture::check(statusIs(MainWindow::ConfigurationSaveState::Error), "failed disk commit reports save error");
        QElapsedTimer deferredEdits; deferredEdits.start();
        gui_fixture::wait([&] { return deferredEdits.elapsed() >= 650; });
        gui_fixture::check(statusIs(MainWindow::ConfigurationSaveState::Error), "queued unsaved workspace notification retains save error");
        gui_fixture::check(baselineFile.open(QIODevice::ReadOnly) && baselineFile.readAll() == savedBytes,
                           "failed Save As preserves original file");
        baselineFile.close();
        app.save_configuration_as({});
        gui_fixture::check(statusIs(MainWindow::ConfigurationSaveState::Error), "cancelled retry retains save error");
        saveAction->trigger();
        gui_fixture::check(statusIs(MainWindow::ConfigurationSaveState::Saved), "successful retry clears save error");
        gui_fixture::check(bool(desktop::load_numeric_project(path, after, persisted)), "read successful retry");
        gui_fixture::check(bool(desktop::read_numeric_application_settings(after, preferences)), "read retry preferences");
        gui_fixture::check(preferences.value("ui").toObject().value("theme") == "light", "retry persists latest unsaved preference");
        gui_fixture::check(preferences.value("ui").toObject().value("future_save_status_preference").toObject() == futurePreference,
                           "save status bookkeeping preserves unknown source preferences");
        std::puts("UI preferences/project/preset boundaries PASS"); return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "UI persistence FAIL: %s\n", error.what()); return 1;
    }
}
