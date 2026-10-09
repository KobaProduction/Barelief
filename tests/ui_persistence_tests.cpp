#include "numeric_gui_fixture.h"
#include <core_app/settings_widget.h>
#include <QComboBox>
#include <QFile>
#include <cstdio>

int main() {
    try {
        gui_fixture::App fixture;
        auto &app = *fixture.application;
        auto *settings = fixture.window->findChild<SettingsWidget *>();
        gui_fixture::check(settings != nullptr, "settings fixture");
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
        std::puts("UI preferences/project/preset boundaries PASS"); return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "UI persistence FAIL: %s\n", error.what()); return 1;
    }
}
