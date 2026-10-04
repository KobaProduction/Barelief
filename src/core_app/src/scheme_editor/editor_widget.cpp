#include "core/performance.h"
#include "core_app/scheme_editor/editor_widget.h"

#include <QtNodes/ConnectionStyle>
#include <QtNodes/DataFlowGraphModel>
#include <QtNodes/DataFlowGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtNodes/NodeDelegateModelRegistry>

#include <QtWidgets/QMessageBox>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>
#include <QtCore/QDir>
#include <QtCore/QDebug>
#include <QtGui/QKeyEvent>

#include "core_app/gui_presenter.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QUndoStack>
#include "core_app/scheme_editor/sink_data_model.h"
#include "core_app/scheme_editor/source_data_model.h"
#include "core_app/scheme_editor/processor_data_model.h"

using QtNodes::ConnectionStyle;
using QtNodes::DataFlowGraphicsScene;
using QtNodes::DataFlowGraphModel;
using QtNodes::GraphicsView;
using QtNodes::NodeDelegateModelRegistry;

class ProfiledGraphicsView final : public GraphicsView {
  public:
    using GraphicsView::GraphicsView;

  protected:
    void paintEvent(QPaintEvent *event) override {
        core::perf::Scope trace(core::perf::Stage::EditorPaint);
        GraphicsView::paintEvent(event);
    }
};

static void setStyle__() {
    ConnectionStyle::setConnectionStyle(
        R"(
  {
    "ConnectionStyle": {
      "ConstructionColor": "gray",
      "NormalColor": "black",
      "SelectedColor": "gray",
      "SelectedHaloColor": "deepskyblue",
      "HoveredColor": "deepskyblue",

      "LineWidth": 3.0,
      "ConstructionLineWidth": 2.0,
      "PointDiameter": 10.0,

      "UseDataDefinedColors": true
    }
  }
  )");
}

SchemeEditorWidget::SchemeEditorWidget(GuiPresenter &application, QWidget *parent)
    : QWidget(parent), _application(application),
      _scene(new DataFlowGraphicsScene(*application.state().data_flow_graph, this)),
      _view(new ProfiledGraphicsView(this)) {
    setObjectName("pipelineEditor");
    setStyle__();
    _scene->setGroupingEnabled(false);
    _view->setScene(_scene);
    _view->setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    auto *layout = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    auto *apply_button = new QPushButton("Apply", this);
    apply_button->setObjectName("applyPipeline");
    auto *reset = new QPushButton("Default pipeline", this);
    reset->setObjectName("defaultPipeline");
    auto *load_active = new QPushButton("Load active pipeline", this);
    load_active->setObjectName("loadActivePipeline");
    load_active->setToolTip(
        "Replace the editor draft with the engine active pipeline. Does not apply or save changes.");
    auto *live = new QCheckBox("Apply valid changes automatically", this);
    live->setChecked(true);
    bar->addWidget(apply_button);
    bar->addWidget(reset);
    bar->addWidget(load_active);
    bar->addWidget(live);
    bar->addStretch();
    layout->addLayout(bar);
    layout->addWidget(_view, 1);
    _status = new QLabel(this);
    _status->setObjectName("pipelineValidation");
    _status->setWordWrap(true);
    layout->addWidget(_status);
    _apply_timer.setSingleShot(true);
    _apply_timer.setInterval(350);
    connect(&_apply_timer, &QTimer::timeout, this, [this, live] {
        if (live->isChecked())
            apply();
        else if (_application.state().file_configs.ui.autosave_enabled)
            emit _application.on_config_updated();
    });
    connect(apply_button, &QPushButton::clicked, this, &SchemeEditorWidget::apply);
    connect(load_active, &QPushButton::clicked, this, &SchemeEditorWidget::load_active_pipeline);
    auto changed = [this, live] {
        _application.state().editor_draft_modified = true;
        _status->setText("Draft changed; the last valid pipeline continues running");
        _apply_timer.start();
    };
    connect(_scene, &DataFlowGraphicsScene::modified, this, changed);
    connect(application.state().data_flow_graph.get(), &DataFlowGraphModel::nodeUpdated, this, changed);
    connect(reset, &QPushButton::clicked, this, [this] {
        auto &graph = *_application.state().data_flow_graph;
        const auto ids = graph.allNodeIds();
        for (auto id : ids)
            graph.deleteNode(id);
        graph.load(pipeline::GraphProgram::defaults(_application.state().file_configs.core.spectrum_channel,
                                                    _application.state().file_configs.core.spectrum_merge,
                                                    _application.state().file_configs.core.capture_source));
        _view->centerScene();
        apply();
    });
    connect(&_application, &GuiPresenter::pipeline_preview, this, [this](const auto &previews) {
        if (!isVisible() || (_preview_clock.isValid() && _preview_clock.elapsed() < 100))
            return;
        _preview_clock.start();
        for (auto it = previews.begin(); it != previews.end(); ++it)
            if (auto *model = _application.state().data_flow_graph->delegateModel<AudioNodeModel>(it.key()))
                model->preview(it.value());
    });
    connect(&application, &GuiPresenter::graph_apply_finished, this,
            [this](quint64 request, bool accepted, const QString &error) {
                if (request != _apply_request)
                    return;
                _apply_request = 0;
                _status->setText(accepted ? "Pipeline applied"
                                          : "Not applied: " + error + ". Last valid pipeline is still running.");
            });
    connect(&application, &GuiPresenter::saved_graph_warning_changed, this,
            [this](const QString &warning) { if (!warning.isEmpty()) _status->setText(warning); });
    QString error;
    auto draft = application.state().data_flow_graph->save();
    draft["version"] = 1;
    _status->setText(pipeline::GraphProgram::compile(draft, error)
                         ? "Pipeline ready. Right-click to add nodes; drag ports to connect."
                         : "Draft not active: " + error);
    if (application.is_remote())
        _status->setText("Editor shows the saved local draft, which may differ from the remote active pipeline. Load "
                         "active pipeline replaces this draft without changing the host.");
    QTimer::singleShot(0, _view, &GraphicsView::centerScene);
}
void SchemeEditorWidget::apply() {
    _application.state().editor_draft_modified = true;
    auto document = _application.state().data_flow_graph->save();
    document["version"] = 1;
    if (_application.state().file_configs.ui.autosave_enabled)
        emit _application.on_config_updated();
    _status->setText("Applying pipeline…");
    _apply_request = _application.apply_pipeline_graph(document);
}

void SchemeEditorWidget::load_active_pipeline() {
    _apply_timer.stop();
    if (_application.graph_request_pending()) {
        _status->setText("Wait for the pending graph command to finish before loading the active pipeline.");
        return;
    }
    const auto document = _application.client().snapshot().graph;
    QString error;
    if (!pipeline::GraphProgram::compile(document, error)) {
        _status->setText("Active pipeline unavailable: " + error);
        return;
    }
    _apply_timer.stop();
    _apply_request = 0;
    auto &graph = *_application.state().data_flow_graph;
    const QSignalBlocker graph_blocker(&graph), scene_blocker(_scene);
    const auto ids = graph.allNodeIds();
    for (auto id : ids)
        graph.deleteNode(id);
    graph.load(document);
    // Graph signals are blocked, so recreate graphics explicitly without treating it as an edit.
    _scene->onModelReset();
    _scene->undoStack().clear();
    _application.state().editor_draft_modified = false;
    _view->centerScene();
    _status->setText("Active pipeline loaded into the editor draft. The engine and saved draft are unchanged; edit or "
                     "save to keep this draft.");
}
