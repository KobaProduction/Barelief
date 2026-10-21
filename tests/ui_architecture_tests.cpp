#include <core_app/workspace/ui_command_registry.h>
#include <core_app/workspace/processor_directory_model.h>
#include <core_app/ui/field_row.h>
#include <core_app/ui/collapsible_section.h>
#include <core_app/ui/validation_message.h>
#include <core_app/settings_widget.h>
#include <core_app/scheme_editor/node_parameter_model.h>
#include <core_app/scheme_editor/node_parameter_form.h>
#include <core_app/scheme_editor/numeric_node_operations.h>
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QTest>
#include <cstdio>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL %s\n", message); ++failures; }
}
void settle() { QCoreApplication::processEvents(); }
class FakeDiscovery final : public presentation::ProcessorDiscoveryPort {
public:
    presentation::ProcessorDiscoverySnapshot current;
    int connects = 0, disconnects = 0;
    presentation::ProcessorDiscoverySnapshot snapshot() const override { return current; }
    bool connect_registry(const QString &host, quint16 port) override {
        ++connects;
        if (host.isEmpty() || !port) return false;
        current.state = presentation::DiscoveryState::Connecting; emit changed(); return true;
    }
    void disconnect_registry() override {
        ++disconnects; current = {}; emit changed();
    }
    void publish(presentation::ProcessorDiscoverySnapshot snapshot) { current = std::move(snapshot); emit changed(); }
};
void discovery() {
    using namespace presentation;
    auto port = std::make_shared<FakeDiscovery>();
    const QString local = "11111111-1111-1111-1111-111111111111";
    const QString remote = "22222222-2222-2222-2222-222222222222";
    int selections = 0; ProcessorEndpoint selected;
    {
        ProcessorDirectoryModel model(local, port);
        QObject::connect(&model, &ProcessorDirectoryModel::selection_requested, &model,
                         [&](const ProcessorEndpoint &endpoint) { ++selections; selected = endpoint; });
        check(model.processors().size() == 1 && model.selected()->local, "local processor without registry");
        check(model.connect_registry("127.0.0.1", 8212) && port->connects == 1, "connect delegates to port");
        ProcessorEndpoint endpoint; endpoint.id = remote; endpoint.address = "127.0.0.2";
        endpoint.name = "Read only"; endpoint.port = 8213; endpoint.from_registry = true; endpoint.allow_mutations = false;
        port->publish({DiscoveryState::Connected, {}, {endpoint}});
        check(model.processors().size() == 2 && selections == 0, "discovery does not select or apply");
        check(model.request_selection("{" + remote + "}"), "canonical identity resolves read-only endpoint");
        check(selections == 1 && !selected.allow_mutations && selected.port == 8213, "selection retains typed permissions and endpoint");
        model.set_selected(remote);
        endpoint.id = "{" + remote + "}"; endpoint.address = "127.0.0.3";
        port->publish({DiscoveryState::Connected, {}, {endpoint}});
        check(model.processors().size() == 2 && model.selected()->id == remote, "refresh preserves selected identity without duplicate");
        check(model.selected()->address == "127.0.0.3", "refresh updates endpoint snapshot");
        model.disconnect_registry();
        check(port->disconnects == 1 && !model.request_selection(remote), "lost endpoint cannot be selected from stale view");
        check(model.request_selection(local), "registry loss does not remove local processing");
    }
    port->publish({DiscoveryState::Unavailable, "gone", {}}); // No callbacks into a destroyed model.
}
void modeless_commands(QWidget& host,presentation::UiCommandRegistry& registry,int& undos) {
    QDialog dialog(&host); auto *dialogLayout = new QVBoxLayout(&dialog);
    auto *dialogText = new QLineEdit(&dialog); dialogLayout->addWidget(dialogText);
    dialog.show(); dialog.activateWindow(); dialogText->setFocus(); settle(); registry.refresh();
    check(QApplication::focusWidget()==dialogText,"command fixture has actual modeless dialog text focus");
    check(!registry.execute("schemaUndo") && undos == 2, "modeless settings dialog excludes schema history");
    dialog.close(); host.close();
}
void text_command_exclusion(QWidget& host,QLineEdit* text,presentation::UiCommandRegistry& registry,QAction* action,QMenu& menu,int& undos) {
    host.activateWindow(); text->setFocus(); settle(); registry.refresh();
    check(QApplication::focusWidget()==text,"command fixture has actual text focus after menu dismissal");
    check(!action->isEnabled() && !registry.execute("schemaUndo") && undos == 2, "text editing excludes schema undo");
    menu.popup(text->mapToGlobal(QPoint(0, 0))); settle(); registry.refresh();
    check(!action->isEnabled(), "menu opened from text retains text editing context");
    menu.hide(); action->trigger(); settle();
    check(undos == 2, "text-origin menu cannot dispatch schema history");
}
void commands() {
    using namespace presentation;
    QWidget host; auto *layout = new QVBoxLayout(&host);
    auto *editor = new QWidget(&host); editor->setFocusPolicy(Qt::StrongFocus); layout->addWidget(editor);
    auto *editorLayout = new QVBoxLayout(editor); auto *text = new QLineEdit(editor); editorLayout->addWidget(text);
    UiCommandRegistry registry(&host); registry.set_editor(editor);
    bool belongs = true, available = true; int undos = 0;
    registry.set_workspace_guard([&] { return belongs; });
    auto *action = registry.register_command({"schemaUndo", "Undo", QKeySequence("Ctrl+Z"),
        UiCommandContext::SchemaEditor, [&] { ++undos; }, [&] { return available; }});
    check(registry.register_command({"schemaUndo", "Redo", {}, UiCommandContext::Workspace, {}}) == action,
          "command registration reuses canonical action");
    host.show(); host.activateWindow(); editor->setFocus(); settle(); registry.refresh();
    check(QApplication::focusWidget()==editor,"command fixture has actual editor focus");
    check(registry.can_execute("schemaUndo") && action->isEnabled(), "schema history available in editor");
    action->trigger(); check(undos == 1, "canonical action dispatches command");
    QMenu menu(&host); menu.addAction(action);
    menu.popup(editor->mapToGlobal(QPoint(0, 0))); settle(); registry.refresh();
    check(action->isEnabled(), "editor history remains available in owned menu");
    menu.hide(); action->trigger();
    check(undos == 2, "menu dismissal retains editor origin during action dispatch");
    settle();
    text_command_exclusion(host,text,registry,action,menu,undos);
    host.activateWindow(); editor->setFocus(); settle(); available = false; registry.refresh();
    check(!action->isEnabled() && !registry.execute("schemaUndo"), "history availability shared with action");
    available = true; belongs = false; registry.refresh();
    check(!registry.execute("schemaUndo"), "other workspace context cannot execute schema history");
    belongs = true;
    modeless_commands(host,registry,undos);
}
void components_and_sections() {
    QWidget parent; auto *input = new QLineEdit(&parent);
    ui::FieldRow row("Address", input, &parent);
    check(row.label()->buddy() == input, "shared row label binds editor");
    check(!input->accessibleName().isEmpty(), "shared row gives accessible name");
    row.setError("Invalid address"); row.setError({});
    ui::CollapsibleSection section("Processing", &parent); int changes = 0;
    QObject::connect(&section, &ui::CollapsibleSection::expandedChanged, &section, [&](bool) { ++changes; });
    section.setExpanded(false); check(!section.isExpanded(), "section collapsed state");
    section.setExpanded(true); check(section.isExpanded() && changes > 0, "section expansion emits state change");
    GuiState state; SettingsWidget settings(state); auto *tabs = settings.findChild<QTabWidget *>("settingsSections");
    check(tabs != nullptr, "settings sections view");
    settings.select_section(SettingsWidget::General); const auto initial = tabs->currentIndex();
    settings.select_section(SettingsWidget::WLED); check(tabs->currentIndex() == initial, "hidden WLED page cannot be opened");
    settings.select_section(SettingsWidget::Network); check(tabs->currentIndex() == initial, "hidden Network page cannot be opened");
    for (const auto &description : SettingsWidget::available_sections())
        check(description.section != SettingsWidget::WLED && description.section != SettingsWidget::Network,
              "navigation metadata excludes obsolete pages");
}
void parameters() {
    using Model = NodeParameterModel;
    auto node = numeric_editor::default_node("Converter"); node.id = 77;
    Model model; int commits = 0;
    model.setEditHandlers([&](const domain::ConfiguredProcessingNode &candidate) {
        ++commits; node = candidate; model.setNode(node, false); return true;
    }, {}, {});
    model.setNode(node, true);
    const auto *dtype = model.field("output0Dtype");
    check(dtype && dtype->section == Model::Section::Contract && model.field("output0Caption") && model.field("output0Caption")->section==Model::Section::Contract, "read-only contracts and advanced contract editors classified by explicit metadata");
    const auto *gain = model.field("numericGain"); check(gain && gain->editor == Model::Editor::Literal, "typed gain descriptor");
    check(model.editLiteral("numericGain", "18446744073709551615", 1), "exact UInt64 max accepted");
    check(std::get<std::uint64_t>(std::get<signal_contracts::NumericLiteral>(model.field("numericGain")->read())) == UINT64_MAX,
          "presentation model retains exact UInt64 without double");
    const auto accepted = commits;
    NodeParameterForm inspector(&model, false); NodeParameterForm second(&model, false);
    auto *edit = inspector.findChild<QLineEdit *>("numericGain");
    auto *other = second.findChild<QLineEdit *>("numericGain"); check(edit && other, "forms share parameter model");
    edit->setText("-1"); edit->setModified(true); emit edit->editingFinished(); settle();
    check(commits == accepted && edit->text() == "-1" && !model.editState("numericGain").error.isEmpty(),
          "invalid UInt64 text retained with error without changing model");
    check(other->text() == "-1", "edit state shared across simultaneous forms");
    edit->setText("9007199254740993"); edit->setModified(true); emit edit->editingFinished(); settle();
    check(commits == accepted + 1 && model.editState("numericGain").error.isEmpty(), "valid correction clears error and commits once");
    check(other->text() == "9007199254740993", "exact correction synchronizes forms");
    inspector.show(); edit->setFocus(); settle(); edit->selectAll(); QTest::keyClicks(edit, "-2");
    const auto beforeRebuild = commits;
    model.setNode(node, true); settle();
    edit = inspector.findChild<QLineEdit *>("numericGain");
    check(edit && edit->text() == "-2" && commits == beforeRebuild, "pending input survives structural form rebuild");
    inspector.hide();
    model.setExpanded(Model::Section::Contract, true);
    auto different = node; different.id = 78; model.setNode(different, true);
    check(!model.expanded(Model::Section::Contract), "expansion state belongs to selected node");
    model.setNode(node, true); check(model.expanded(Model::Section::Contract), "reselection restores expansion state");
    settle();
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv); discovery(); commands(); components_and_sections(); parameters();
    std::printf("UI architecture: %d failures\n", failures); return failures ? 1 : 0;
}
