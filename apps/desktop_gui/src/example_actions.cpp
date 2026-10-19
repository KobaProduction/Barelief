#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/example_presentation.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <QStatusBar>

void CoreApplication::_load_example(SchemeEditorWidget* editor,NumericExampleGraph example) {
    if(_presenter->graph_request_pending())return;
    auto* model=dynamic_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get());
    if(model)example.graph.graph_id=model->numeric_graph().graph_id;
    QString error;
    if(!editor->load_numeric_draft(example.graph,&error)) {
        _main_window->statusBar()->showMessage(error,10000);
        return;
    }
    apply_example_labels(dynamic_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get()),example);
    editor->initialize_default_layout();
    emit editor->preset_presentation_requested(example_presentation(example,_presenter->device_id(),
        QString::fromStdString(example.graph.graph_id)));
    _presenter->apply_pipeline_graph({});
}
