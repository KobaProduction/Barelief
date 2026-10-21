#include "ui_audit_recorder.h"
#include "numeric_gui_fixture.h"
#include <core_app/workspace/ui_command_registry.h>
#include <core_app/settings_widget.h>
#include <core_app/ui/field_row.h>
#include <QtNodes/internal/UndoCommands.hpp>
#include <QtNodes/GraphicsView>
#include <QTest>
#include <QDialog>
#include <QComboBox>
#include <QLineEdit>
#include <QTabWidget>
#include <QMenuBar>
#include <QSet>

namespace ui_audit {
namespace {
QStringList require(bool condition,const QString& message) { return condition?QStringList{}:QStringList{message}; }
QJsonArray field_inventory(QWidget* page) {
    QJsonArray result;
    for(const auto children=page->findChildren<QWidget*>();auto* widget:children) if(auto* row=dynamic_cast<ui::FieldRow*>(widget)) {
        result.append(QJsonObject{{"label",row->label()->text()},{"editor",row->editor()->objectName()},
            {"tooltip",row->editor()->toolTip()},{"accessibleName",row->editor()->accessibleName()},
            {"buddy",row->label()->buddy()==row->editor()},{"enabled",row->editor()->isEnabled()}});
    }
    return result;
}
}

void audit_text_history(Recorder& recorder,SchemeEditorWidget*& editor,presentation::UiCommandRegistry*& registry,QtNodes::DataFlowGraphicsScene*& scene,NumericEditorGraphModel*& model,auto& focus_editor) {
    // The real node catalog popup field exercises focus gating and text Undo,
    // without sending any operating-system keyboard or mouse input.
    std::unique_ptr<QMenu> catalogMenu(scene->createSceneMenu(QPointF(-100000,-100000)));
    auto* text=catalogMenu->findChild<QLineEdit*>();
    if(text && text->isEnabled()) {
        focus_editor();catalogMenu->show();catalogMenu->activateWindow();
        text->setFocus(Qt::OtherFocusReason);QCoreApplication::processEvents();registry->refresh();
        const auto historyIndex=editor->history().index();const auto graphBefore=model->save();const auto textBefore=text->text();
        recorder.transition("command.text_focus","Type into real node catalog popup filter; graph Undo/Redo must remain gated",catalogMenu.get(),
            [&]{QTest::keyClicks(text,"audit");registry->refresh();},
            [&]{return require(QApplication::focusWidget()==text && !registry->can_execute("undoDataFlow")
                && !registry->can_execute("redoDataFlow") && editor->history().index()==historyIndex,
                "Text entry captured graph history or editor Undo/Redo remained executable");},"qt_input_keyboard");
        recorder.transition("command.text_undo","Ctrl+Z in focused catalog filter changes text only",catalogMenu.get(),
            [&]{QTest::keyClick(text,Qt::Key_Z,Qt::ControlModifier);},
            [&]{return require(model->save()==graphBefore && editor->history().index()==historyIndex && text->text()==textBefore,
                "Text Undo changed graph history or failed to restore text");},"qt_input_keyboard");
        recorder.transition("command.text_redo","Ctrl+Shift+Z in focused catalog filter restores local text only",catalogMenu.get(),
            [&]{QTest::keyClick(text,Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);},
            [&]{return require(model->save()==graphBefore && editor->history().index()==historyIndex && text->text()==textBefore+"audit",
                "Text Redo changed graph history or failed to restore text");},"qt_input_keyboard");
        text->setText(textBefore);
        catalogMenu->hide();
        recorder.coverage("application.text_focus_undo","partial","Qt-local typing and Ctrl+Z in actual node catalog popup filter; graph command gating/document isolation checked. Embedded node field and editor-owned popup-origin routing remain separate scenarios.");
    } else recorder.coverage("application.text_focus_undo","not_covered","No enabled QLineEdit was available in the real node catalog popup.");
    recorder.evidence("application.pipeline_routes",{{"connect","production scene ConnectCommand"},{"disconnect","production scene DisconnectCommand"},
        {"delete","scene menu deleteGraphSelection QAction"},{"undoRedo","UiCommandRegistry plus scene QUndoStack"},
        {"capture","isolated synthetic adapter; no speaker output"},{"autoApply",false}});
    recorder.coverage("application.pipeline_transitions","partial","Compatible/incompatible boundaries, disconnect/reconnect, exact document Undo/Redo and menu delete covered; no port drag/hit-testing claims.");
    recorder.coverage("application.destructive_file_routes","not_covered","Save-as/open native file dialogs and reset/preset confirmation flows require separate bounded dialog scenarios.");
    recorder.coverage("application.keyboard_shortcut_delivery","partial","Production canvas Ctrl+Z/Ctrl+Shift+Z restore exact graph documents once; catalog popup text Undo/Redo stays local. Popup-origin menu command routing remains a separate scenario.");
}

void audit_delete_commands(Recorder& recorder,SchemeEditorWidget*& editor,presentation::UiCommandRegistry*& registry,QtNodes::DataFlowGraphicsScene*& scene,NumericEditorGraphModel*& model,auto& focus_editor,auto& target,auto& replacement,auto& reconnected) {
    scene->clearSelection();
    auto* targetItem=scene->nodeGraphicsObject(target);
    gui_fixture::check(targetItem,"delete target scene item");targetItem->setSelected(true);
    std::unique_ptr<QMenu> deleteMenu(scene->createSceneMenu(targetItem->sceneBoundingRect().center()));
    auto* deleteAction=deleteMenu->findChild<QAction*>("deleteGraphSelection");
    gui_fixture::check(deleteAction,"actual delete selection action");
    recorder.transition("pipeline.delete_selection","Delete selected node through actual scene menu QAction",editor,
        [&]{deleteAction->trigger();},[&]{return require(!model->nodeExists(target) && !model->connectionExists(replacement),"Delete left selected node or incident edge");});
    const auto deleted=model->save();
    recorder.transition("pipeline.delete_undo","Undo node deletion with incident connection",editor,
        [&]{editor->history().undo();},[&]{return require(model->save()==reconnected,"Delete Undo did not restore node and edge document");});
    recorder.transition("pipeline.delete_redo","Redo node deletion",editor,
        [&]{editor->history().redo();},[&]{return require(model->save()==deleted,"Delete Redo did not restore deleted document");});
    editor->history().undo();

audit_text_history(recorder,editor,registry,scene,model,focus_editor);
}

void audit_connection_keys(Recorder& recorder,SchemeEditorWidget*& editor,presentation::UiCommandRegistry*& registry,QtNodes::DataFlowGraphicsScene*& scene,NumericEditorGraphModel*& model,QtNodes::GraphicsView*& view,auto& focus_editor,auto& target,auto& edge,auto& replacement,auto& bad,auto& baseline,auto& connected,auto& executed) {
    const auto connectedIndex=editor->history().index();
    recorder.transition("command.graph_undo_key","Deliver Ctrl+Z to the real canvas and restore the graph document",editor,
        [&]{QTest::keyClick(view,Qt::Key_Z,Qt::ControlModifier);},
        [&]{return require(model->save()==baseline && editor->history().index()==connectedIndex-1,"Canvas Ctrl+Z did not undo exactly once");},"qt_input_keyboard");
    recorder.transition("command.graph_redo_key","Deliver Ctrl+Shift+Z to the real canvas and restore the connection",editor,
        [&]{QTest::keyClick(view,Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);},
        [&]{return require(model->save()==connected && editor->history().index()==connectedIndex,"Canvas Ctrl+Shift+Z did not redo exactly once");},"qt_input_keyboard");
    recorder.transition("pipeline.disconnect","Disconnect existing edge through real scene command",editor,
        [&]{editor->history().push(new QtNodes::DisconnectCommand(scene,edge));},
        [&]{return require(!model->connectionExists(edge),"Disconnect left original connection present");});
    const auto disconnected=model->save();
    recorder.transition("pipeline.incompatible_connect","Check rank-incompatible connection to an unoccupied input at real model boundary",editor,
        [&]{executed=model->connectionPossible(bad);},
        [&]{return require(!executed && model->save()==disconnected,"Incompatible candidate was accepted or mutated the document");});
    recorder.transition("pipeline.reconnect","Reconnect view to another compatible source",editor,
        [&]{if(model->connectionPossible(replacement))editor->history().push(new QtNodes::ConnectCommand(scene,replacement));},
        [&]{return require(model->connectionExists(replacement) && !model->connectionExists(edge),"Replacement route is not exclusive");});
    const auto reconnected=model->save();
    recorder.transition("pipeline.reconnect_undo","Undo replacement and restore disconnected input",editor,
        [&]{editor->history().undo();},[&]{return require(model->save()==disconnected,"Reconnect Undo failed exact document restoration");});
    recorder.transition("pipeline.reconnect_redo","Redo compatible replacement",editor,
        [&]{editor->history().redo();},[&]{return require(model->save()==reconnected,"Reconnect Redo failed exact document restoration");});

audit_delete_commands(recorder,editor,registry,scene,model,focus_editor,target,replacement,reconnected);
}

void audit_connection_commands(Recorder& recorder,SchemeEditorWidget*& editor,presentation::UiCommandRegistry*& registry,QtNodes::DataFlowGraphicsScene*& scene,NumericEditorGraphModel*& model,QtNodes::GraphicsView*& view,auto& focus_editor) {
    // Use the production scene's public Undo commands. This verifies graph state
    // transitions without pretending to exercise port hit testing or wire dragging.
    focus_editor();
    const auto source=model->addNode("Window");
    const auto other=model->addNode("Window");
    const auto target=model->addNode("AudioView");
    const auto incompatible=model->addNode("Spectrogram");
    const QtNodes::ConnectionId edge{source,0,target,0},replacement{other,0,target,0},bad{incompatible,0,target,0};
    editor->history().clear();
    const auto baseline=model->save();
    recorder.transition("pipeline.compatible_connect","Connect compatible vector source to view using the real scene command",editor,
        [&]{if(model->connectionPossible(edge))editor->history().push(new QtNodes::ConnectCommand(scene,edge));},
        [&]{return require(model->connectionExists(edge) && editor->history().canUndo(),"Compatible connection not created as undoable operation");});
    const auto connected=model->save();
    focus_editor();
    bool executed=false;
    recorder.transition("pipeline.undo_registered","Undo connection through real UiCommandRegistry",editor,
        [&]{executed=registry->execute("undoDataFlow");},
        [&]{return require(executed && model->save()==baseline,"Registered editor Undo did not restore the document");});
    focus_editor();
    recorder.transition("pipeline.redo_registered","Redo connection through real UiCommandRegistry",editor,
        [&]{executed=registry->execute("redoDataFlow");},
        [&]{return require(executed && model->save()==connected,"Registered editor Redo did not restore the connected document");});
audit_connection_keys(recorder,editor,registry,scene,model,view,focus_editor,target,edge,replacement,bad,baseline,connected,executed);
}

void audit_navigation_routes(Recorder& recorder,gui_fixture::App& app,MainWindow*& window,SchemeEditorWidget*& editor,presentation::UiCommandRegistry*& registry,QtNodes::DataFlowGraphicsScene*& scene,NumericEditorGraphModel*& model,QtNodes::GraphicsView*& view,auto& focus_editor) {
    focus_editor();
    bool navigated=false;
    const auto navigationDocument=editor->document();
    recorder.transition("navigation.focus_flow","Focus Data Flow through registered workspace command",window,
        [&]{navigated=registry->execute("workspaceFocusFlow");QCoreApplication::processEvents();},
        [&]{auto* focus=QApplication::focusWidget();return require(navigated && !app.dock("dataFlowDock")->isHidden()
            && focus && (focus==editor || editor->isAncestorOf(focus)),"Focus Data Flow did not expose/focus the editor");});
    for(const auto ids=QStringList{"workspaceNextTab","workspacePreviousTab"};const auto& id:ids) {
        focus_editor();
        recorder.transition("navigation."+id,"Dispatch registered adjacent workspace tab navigation",window,
            [&]{navigated=registry->execute(id);QCoreApplication::processEvents();},
            [&]{return require(navigated && editor->document()==navigationDocument,"Workspace navigation failed or changed the graph document");});
    }
    for(int step=0;step<2;++step) {
        // The second workspace command remains callable while its editor is hidden.
        window->activateWindow();registry->refresh();
        const bool wasHidden=app.dock("dataFlowDock")->isHidden();
        recorder.transition(QString("navigation.toggle_flow.%1").arg(step),"Toggle Data Flow through registered workspace command",window,
            [&]{navigated=registry->execute("workspaceToggleTools");QCoreApplication::processEvents();},
            [&]{return require(navigated && app.dock("dataFlowDock")->isHidden()!=wasHidden
                && editor->document()==navigationDocument,"Workspace toggle failed or changed the graph document");});
    }
    recorder.coverage("application.navigation_routes","partial","Real registered focus, adjacent-tab and Data Flow toggle routes dispatched with document isolation checks; multi-panel ordering, detach and switcher popup remain separate scenarios.");

audit_connection_commands(recorder,editor,registry,scene,model,view,focus_editor);
}

void audit_settings_routes(Recorder& recorder,gui_fixture::App& app,MainWindow*& window,SchemeEditorWidget*& editor,presentation::UiCommandRegistry*& registry,QtNodes::DataFlowGraphicsScene*& scene,NumericEditorGraphModel*& model,QtNodes::GraphicsView*& view,auto& focus_editor) {
    auto* settings=window->findChild<SettingsWidget*>();
    auto* dialog=window->findChild<QDialog*>("settingsDialog");
    auto* tabs=settings?settings->findChild<QTabWidget*>("settingsSections"):nullptr;
    gui_fixture::check(settings && dialog && tabs,"application audit real settings routes");
    auto* language=settings->findChild<QComboBox*>("interfaceLanguage");
    gui_fixture::check(language,"language settings field");
    const auto initialLanguage=language->currentData().toString();
    for(const auto locales=QStringList{"ru","en"};const auto& locale:locales) {
        language->setCurrentIndex(language->findData(locale));
        for(const auto& section:SettingsWidget::available_sections()) {
            const QString id=section.section==SettingsWidget::General?QString("openSettings"):
                QString("openSettingsSection%1").arg(int(section.section));
            focus_editor();
            bool dispatched=false;
            recorder.transition("settings."+locale+"."+id,"Open settings section through its registered navigation command",window,
                [&]{dispatched=registry->execute(id);QCoreApplication::processEvents();},
                [&]{return require(dispatched && dialog->isVisible() && tabs->currentIndex()==int(section.section)
                    && tabs->isTabVisible(int(section.section)),"Settings command did not expose its designated visible section");});
            const auto fields=field_inventory(tabs->currentWidget());
            recorder.evidence("settings."+locale+"."+id,{{"title",tabs->tabText(tabs->currentIndex())},{"fields",fields},
                {"route",id},{"section",int(section.section)}});
            for(const auto& entry:fields) if(entry.toObject().value("label").toString().trimmed().isEmpty())
                recorder.finding("settings.label."+locale+"."+id,"warning","label",
                    "A FieldRow has an empty visible label","Every settings FieldRow has a readable label",entry.toObject());
            dialog->hide();QCoreApplication::processEvents();
        }
    }
    language->setCurrentIndex(language->findData(initialLanguage));
    recorder.coverage("application.settings_routes","covered","All advertised settings sections reached through real registry in RU and EN; labels, tooltips, accessibility names and buddy relations recorded.");
    recorder.coverage("application.settings_native_readability","not_covered","Offscreen evidence does not establish native text clipping, screen-reader usability or visual comprehension; empty tooltips alone are not defects.");

audit_navigation_routes(recorder,app,window,editor,registry,scene,model,view,focus_editor);
}

void record_registered_command(auto& id,Recorder& recorder,presentation::UiCommandRegistry*& registry,MainWindow*& window,auto& commands,auto& shortcuts) {

        auto* action=registry->action(id);
        if(!action) {
            recorder.finding("command.missing."+id,"error","runtime_contract","Registered action is missing",id);
            return;
        }
        QJsonArray menus;
        for(const auto menusInWindow=window->findChildren<QMenu*>();auto* menu:menusInWindow) if(menu->actions().contains(action))
            menus.append(QJsonObject{{"id",menu->objectName()},{"title",menu->title()}});
        const auto key=action->shortcut().toString(QKeySequence::PortableText);
        const QJsonObject item{{"id",id},{"text",action->text()},{"shortcut",key},
            {"context",int(action->shortcutContext())},{"enabled",action->isEnabled()},
            {"canExecute",registry->can_execute(id)},{"menus",menus}};
        commands.append(item);
        if(action->text().trimmed().isEmpty() || action->objectName()!=id || menus.isEmpty()
            || action->isEnabled()!=registry->can_execute(id))
            recorder.finding("command.contract."+id,"error","runtime_contract",
                "Action identity, label, menu membership or enablement is inconsistent","Stable named, labelled action shared by menu and registry",item);
        if(!key.isEmpty()) {
            if(shortcuts.contains(key)) recorder.finding("command.shortcut."+id,"review","shortcut_conflict",
                "Two registered commands advertise the same shortcut","Review whether their contexts are mutually exclusive",
                QJsonObject{{"first",shortcuts[key]},{"second",id},{"shortcut",key}});
            shortcuts.insert(key,id);
        }
    }

void audit_command_inventory(Recorder& recorder,gui_fixture::App& app,MainWindow*& window,SchemeEditorWidget*& editor,presentation::UiCommandRegistry*& registry,QtNodes::DataFlowGraphicsScene*& scene,NumericEditorGraphModel*& model,QtNodes::GraphicsView*& view,auto& focus_editor) {
    const QStringList ids{"undoDataFlow","redoDataFlow","openSettings","openSettingsSection1",
        "openSettingsSection2","openSettingsSection5","openEngineRegistry","openPipeline",
        "saveSettings","saveSettingsAs","openSettingsFile","resetWorkspace","resetCameraView",
        "arrangePipelineShortcut","fitPipelineShortcut","workspaceNextTab","workspacePreviousTab",
        "workspaceDetachTab","workspaceHidePanel","workspaceFocusFlow","workspaceToggleTools","workspaceSwitcher"};
    QJsonArray commands;
    QHash<QString,QString> shortcuts;
    for(const auto& id:ids) {record_registered_command(id,recorder,registry,window,commands,shortcuts);}
    recorder.evidence("application.commands",{{"actions",commands},{"dispatch","real UiCommandRegistry; no replacement command registry"}});
    for(const auto& binding:std::initializer_list<std::pair<QString,QKeySequence>>{
            {"undoDataFlow",QKeySequence("Ctrl+Z")},{"redoDataFlow",QKeySequence("Ctrl+Shift+Z")}}) {
        auto* action=registry->action(binding.first);
        if(!action)continue;
        if(action->shortcut()!=binding.second)
            recorder.finding("command.convention."+binding.first,"error","runtime_contract",
                "Graph history shortcut differs from the requested editor binding",
                "Editor Undo uses Ctrl+Z and Redo uses Ctrl+Shift+Z; focused text controls own their local history",
                {{"id",binding.first},{"actual",action->shortcut().toString(QKeySequence::PortableText)},
                 {"expected",binding.second.toString(QKeySequence::PortableText)},{"scope","SchemaEditor"},
                 {"menuText",action->text()}});
    }
    recorder.coverage("application.command_registry","covered","Registered IDs, text, shortcut, QAction context, enablement and menu object identity inventoried; duplicate shortcuts classified for review.");

audit_settings_routes(recorder,app,window,editor,registry,scene,model,view,focus_editor);
}

void audit_application(Recorder& recorder) {
    // This group owns QApplication through CoreApplication. Never use the user's
    // configuration, real capture, desktop input, or a second QApplication.
    gui_fixture::App app;
    auto* window=app.window;
    auto* editor=app.editor;
    auto* registry=window->findChild<presentation::UiCommandRegistry*>("uiCommandRegistry");
    auto* scene=editor->findChild<QtNodes::DataFlowGraphicsScene*>();
    auto* model=dynamic_cast<NumericEditorGraphModel*>(app.presenter().state().data_flow_graph.get());
    auto* view=editor->findChild<QtNodes::GraphicsView*>();
    gui_fixture::check(registry && scene && model && view,"application audit real registry/editor");
    const auto focus_editor=[&] {
        window->activateWindow();
        app.dock("dataFlowDock")->show();
        window->focusDock(app.dock("dataFlowDock"));
        view->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents(); registry->refresh();
    };
    focus_editor();

audit_command_inventory(recorder,app,window,editor,registry,scene,model,view,focus_editor);
}
}
