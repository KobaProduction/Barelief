#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/resampler_comparison_dialog.h>
#include <core_app/music_analysis_dialog.h>
#include <core_app/analysis_services.h>
#include <speex_backend.h>
#include <visualization/ui_text.h>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QStatusBar>
#include <QPointer>
#include <utility>

#include "core_application_state.h"
void CoreApplication::_bind_default_graph_action() {
    connect(_main_window,&MainWindow::default_graph_requested,this,[this]{_load_default_graph();});
}
void CoreApplication::_load_default_graph() {
    auto* editor=_main_window->data_flow_editor();if(!editor)return;
    QString error;
    if(!editor->load_numeric_draft(numeric_default_graph(),&error)){_main_window->statusBar()->showMessage(error,10000);return;}
    editor->initialize_default_layout();emit editor->preset_presentation_requested({});
    _presenter->apply_pipeline_graph({});
}
void CoreApplication::_bind_analysis_tools() {
    auto* tools=_main_window->menuBar()->addMenu(ui::text("Tools"));
    tools->setObjectName("analysisToolsMenu");ui::bind(tools,"title","Tools");
    auto* compare=tools->addAction(ui::text("Resampler comparison"));
    compare->setObjectName("resamplerComparison");ui::bind(compare,"text","Resampler comparison");
    connect(compare,&QAction::triggered,this,[this]{_show_resampler_comparison();});
    auto* analyze=tools->addAction(ui::text("Frequency statistics and profiles"));
    analyze->setObjectName("musicFileAnalysis");ui::bind(analyze,"text","Frequency statistics and profiles");
    connect(analyze,&QAction::triggered,this,&CoreApplication::_show_music_analysis);
}
void CoreApplication::_show_resampler_comparison() {
    show_resampler_comparison(_main_window,[](const auto& request,const auto& cancelled){return desktop_analysis::offline::compare_resamplers(request,cancelled,&desktop_resampling::speex_backend_factory());});
}
void CoreApplication::_show_music_analysis() {
    auto targets=[this]{auto* graph=dynamic_cast<NumericEditorGraphModel*>(_presenter->state().data_flow_graph.get());return graph?core_app::frequency_profile_targets(graph->numeric_graph()):std::vector<core_app::FrequencyProfileTarget>{};};
    auto apply=[this](const core_app::FrequencyProfileTarget& target,const dsp_kernels::FrequencyProfile& profile,QString& error){return _apply_frequency_profile(target,profile,error);};
    auto* dialog=new core_app::MusicAnalysisDialog(targets,apply,desktop_analysis_services(),_main_window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->show();
}
bool CoreApplication::_apply_frequency_profile(const core_app::FrequencyProfileTarget& target,const dsp_kernels::FrequencyProfile& profile,QString& error) {
    if(_presenter->graph_request_pending()){error=ui::text("Wait for the current scheme update");return false;}
    auto* graph=dynamic_cast<NumericEditorGraphModel*>(_presenter->state().data_flow_graph.get());if(!graph)return false;
    domain::ConfiguredProcessingNode coefficients,arithmetic;
    if(!core_app::frequency_profile_edits(graph->numeric_graph(),target,profile,coefficients,arithmetic,error))return false;
    return graph->edit_nodes({coefficients,arithmetic},&error);
}
void CoreApplication::_bind_editor_actions(StartupState& state) {
    auto* editor=_main_window->data_flow_editor();if(!editor)return;
    _populate_example_menu(editor);
    connect(editor,&SchemeEditorWidget::generator_loopback_graph_requested,this,[this,editor]{_load_example(editor,numeric_startup_example());});
    _presenter->set_graph_adapter_provider([guard=QPointer<SchemeEditorWidget>(editor)]{return guard?guard->graph_adapter_metadata():QJsonObject{};});
    _restore_startup_draft(state,editor);
    connect(editor,&SchemeEditorWidget::numeric_draft_changed,this,[this]{_ctx.editor_draft_modified=true;_save_configuration(false);});
}
void CoreApplication::_populate_example_menu(SchemeEditorWidget* editor) {
    auto* menu=_main_window->findChild<QMenu*>("pipelinePresetsMenu");if(!menu)return;
    for(const auto* id:{"defaultPipeline","generatorLoopbackPreset","useTestSource"})if(auto* old=menu->findChild<QAction*>(id))old->setVisible(false);
    auto* anchor=menu->findChild<QAction*>("loadActivePipeline");
    for(auto example:numeric_example_graphs())_add_example_action(menu,anchor,editor,std::move(example));
}
void CoreApplication::_add_example_action(QMenu* menu,QAction* anchor,SchemeEditorWidget* editor,NumericExampleGraph example) {
    auto* action=new QAction(ui::text(example.title_key),menu);menu->insertAction(anchor,action);
    ui::bind(action,"text",example.title_key);action->setObjectName(QStringLiteral("example-")+QString::fromLatin1(example.id));
    connect(action,&QAction::triggered,this,[this,editor,example=std::move(example)]{_load_example(editor,example);});
}
