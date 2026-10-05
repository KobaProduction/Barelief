#include "visualization/ui_text.h"
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
#include <QUndoCommand>
#include <QScrollBar>
#include <QJsonArray>
#include <QWheelEvent>
#include <QDoubleSpinBox>
#include <QToolButton>
#include <QMenu>
#include <QInputDialog>
#include <QUuid>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionIdUtils.hpp>
#include "pipeline/test_signal.h"
#include <map>
#include <queue>
#include <algorithm>
#include <cmath>
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
    std::function<void()> on_interaction;
    std::function<void()> synchronize_positions;

  protected:
    void mouseReleaseEvent(QMouseEvent *event) override {
        GraphicsView::mouseReleaseEvent(event);
        if (synchronize_positions) synchronize_positions();
        if (on_interaction) on_interaction();
    }
    void wheelEvent(QWheelEvent *event) override {
        const double delta=event->pixelDelta().isNull() ? event->angleDelta().y()/120.0 : event->pixelDelta().y()/40.0;
        if (delta==0) { event->ignore(); return; }
        const QPointF anchor=mapToScene(event->position().toPoint());
        const double next=std::clamp(getScale()*std::exp(std::clamp(delta,-10.0,10.0)*0.10),0.2,3.0);
        setupScale(next);
        const auto moved=mapToScene(event->position().toPoint());
        translate(moved.x()-anchor.x(),moved.y()-anchor.y());
        event->accept(); if (on_interaction) on_interaction();
    }
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

static QJsonObject processing_document(const QJsonObject &document) {
    QJsonArray nodes, connections;
    std::vector<QJsonObject> sorted;
    for (const auto value : document.value("nodes").toArray()) {
        auto node = value.toObject(); node.remove("position"); sorted.push_back(node);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.value("id").toInt() < b.value("id").toInt(); });
    for (const auto &node : sorted) nodes.append(node);
    sorted.clear();
    for (const auto value : document.value("connections").toArray()) sorted.push_back(value.toObject());
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        return QJsonDocument(a).toJson(QJsonDocument::Compact) < QJsonDocument(b).toJson(QJsonDocument::Compact);
    });
    for (const auto &edge : sorted) connections.append(edge);
    return {{"nodes",nodes},{"connections",connections}};
}
class ArrangeCommand final : public QUndoCommand {
    DataFlowGraphModel &_graph;
    std::map<QtNodes::NodeId,QPointF> _before, _after;
    void set(const auto &positions) {
        for (const auto &[id, position] : positions)
            if (_graph.nodeExists(id)) _graph.setNodeData(id, QtNodes::NodeRole::Position, position);
    }
  public:
    ArrangeCommand(DataFlowGraphModel &graph, std::map<QtNodes::NodeId,QPointF> positions, const QString &label = "Arrange pipeline")
        : _graph(graph), _after(std::move(positions)) {
        setText(label);
        for (const auto &[id, position] : _after) _before[id] = graph.nodeData(id, QtNodes::NodeRole::Position).toPointF();
    }
    void redo() override { set(_after); }
    void undo() override { set(_before); }
};
class TestSourceCommand final : public QUndoCommand {
    DataFlowGraphModel &_graph;
    QJsonObject _before, _after;
    std::vector<QtNodes::ConnectionId> _connections;
    void replace(const QJsonObject &node) {
        const auto id = node.value("id").toInt();
        if (_graph.nodeExists(id)) _graph.deleteNode(id);
        _graph.loadNode(node);
        for (const auto &connection : _connections)
            if (_graph.connectionPossible(connection)) _graph.addConnection(connection);
    }
public:
    TestSourceCommand(DataFlowGraphModel &graph, QJsonObject node, const QJsonArray &connections)
        : _graph(graph), _before(node), _after(std::move(node)) {
        setText("Use silent test source");
        auto params = pipeline::TestSignalSettings{}.json(); params["model-name"] = "TestSignal";
        _after["internal-data"] = params;
        const auto id = _before.value("id").toInt();
        for (const auto &value : connections) {
            const auto connection = QtNodes::fromJson(value.toObject());
            if (connection.outNodeId == id || connection.inNodeId == id) _connections.push_back(connection);
        }
    }
    void redo() override { replace(_after); }
    void undo() override { replace(_before); }
};

SchemeEditorWidget::SchemeEditorWidget(GuiPresenter &application, QWidget *parent)
    : QWidget(parent), _application(application),
      _scene(new DataFlowGraphicsScene(*application.state().data_flow_graph, this)),
      _view(new ProfiledGraphicsView(this)) {
    setObjectName("pipelineEditor");
    setStyle__();
    _scene->setGroupingEnabled(false);
    _view->setScene(_scene);
    _view->setScaleRange(0.2,3.0);
    _view->setTransformationAnchor(QGraphicsView::NoAnchor);
    _view->setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    auto *layout = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    auto *apply_button = ui::make<QPushButton>("Apply", this);
    apply_button->setObjectName("applyPipeline");
    auto *presets=new QToolButton(this); presets->setObjectName("pipelinePresets");
    ui::bind(presets,"text","Presets"); presets->setPopupMode(QToolButton::InstantPopup);
    auto *preset_menu=new QMenu(presets); preset_menu->setObjectName("pipelinePresetsMenu"); presets->setMenu(preset_menu);
    auto action=[preset_menu](const char *text,const char *id) {
        auto *item=preset_menu->addAction(ui::text(text)); item->setObjectName(id); ui::bind(item,"text",text); return item;
    };
    auto *reset=action("Default","defaultPipeline");
    auto *load_active=action("Active graph","loadActivePipeline");
    auto *test_source=action("Test source","useTestSource");
    preset_menu->addSeparator();
    auto *user_presets=preset_menu->addMenu(ui::text("My presets")); ui::bind(user_presets,"title","My presets");
    auto *save=action("Save preset…","savePipelinePreset");
    auto *delete_presets=preset_menu->addMenu(ui::text("Delete preset")); ui::bind(delete_presets,"title","Delete preset");
    auto refresh_presets=[this,user_presets,delete_presets] {
        user_presets->clear(); delete_presets->clear();
        for (const auto &value : _application.state().file_configs.ui.pipeline_presets) {
            const auto preset=value.toObject(); const auto id=preset.value("id").toString();
            if (id.isEmpty() || !preset.value("graph").isObject()) continue;
            const auto name=preset.value("name").toString();
            auto *load=user_presets->addAction(name);
            connect(load,&QAction::triggered,this,[this,id] { load_preset(id); });
            auto *remove=delete_presets->addAction(name);
            connect(remove,&QAction::triggered,this,[this,id] { remove_preset(id); });
        }
        user_presets->setEnabled(!user_presets->isEmpty()); delete_presets->setEnabled(!delete_presets->isEmpty());
    };
    refresh_presets(); connect(preset_menu,&QMenu::aboutToShow,this,refresh_presets);
    connect(save,&QAction::triggered,this,[this] {
        bool ok=false; const auto name=QInputDialog::getText(this,ui::text("Save preset"),ui::text("Preset name"),QLineEdit::Normal,QString(),&ok);
        if (ok && !name.trimmed().isEmpty()) save_preset(name);
    });
    bar->addWidget(apply_button); bar->addWidget(presets);
    connect(test_source,&QAction::triggered,this,[this] {
        auto &graph = *_application.state().data_flow_graph;
        const auto document = graph.save();
        for (const auto &value : document.value("nodes").toArray()) {
            const auto node = value.toObject();
            if (node.value("internal-data").toObject().value("model-name") == "AudioSource") {
                _scene->undoStack().push(new TestSourceCommand(graph,node,document.value("connections").toArray()));
                return;
            }
        }
        const auto nodes=document.value("nodes").toArray();
        if (std::any_of(nodes.begin(),nodes.end(),[](const QJsonValue &value) {
            return value.toObject().value("internal-data").toObject().value("model-name")=="TestSignal";
        })) return;
        set_status(ui::text("No audio source in this draft"));
    });
    auto *execution=bar;
    for (const auto &[label,component] : {std::pair{"Capture","Capture"},std::pair{"Analyzer","Analyzer"}}) {
        auto *enabled=ui::make<QCheckBox>(label,this);
        const bool capture=QString(component)=="Capture";
        enabled->setObjectName(capture ? "pipelineCapture" : "pipelineAnalyzer");
        enabled->setChecked(capture ? application.state().file_configs.core.run_loopback : application.state().file_configs.core.run_analyser);
        execution->addWidget(enabled);
        connect(enabled,&QCheckBox::toggled,this,[this,capture](bool value) {
            if (capture) {
                _application.state().file_configs.core.run_loopback=value;
                if (value) _application.start_loopback(); else _application.stop_loopback();
            } else {
                _application.state().file_configs.core.run_analyser=value;
                if (value) _application.start_analyzer(); else _application.stop_analyzer();
            }
            if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
        });
        connect(&application,&GuiPresenter::component_status_changed,enabled,[enabled,component](const QString &name,int state,const QString &error) {
            if (name!=component) return;
            const QSignalBlocker blocker(enabled);
            enabled->setChecked(state==1 || state==2 || state==3);
            enabled->setToolTip(error);
        });
    }
    execution->addStretch();
    auto *zoom=new QDoubleSpinBox(this); zoom->setObjectName("pipelineZoom");
    zoom->setDecimals(0); zoom->setRange(20,300); zoom->setSuffix(" %"); zoom->setSingleStep(10); zoom->setValue(100);
    execution->addWidget(ui::make<QLabel>("Zoom",this)); execution->addWidget(zoom);
    connect(zoom,&QDoubleSpinBox::valueChanged,this,[this](double percent) {
        const auto center=_view->mapToScene(_view->viewport()->rect().center());
        _view->setupScale(percent/100); _view->centerOn(center);
    });
    connect(_view,&GraphicsView::scaleChanged,zoom,[zoom](double value) {
        const QSignalBlocker blocker(zoom); zoom->setValue(value*100);
    });
    auto *fit=ui::make<QPushButton>("Center all blocks",this); fit->setObjectName("fitPipeline");
    execution->addWidget(fit);
    connect(fit,&QPushButton::clicked,this,[this] {
        const auto bounds=_scene->itemsBoundingRect().adjusted(-40,-40,40,40);
        if (!bounds.isEmpty()) {
            const double scale=std::clamp(std::min(_view->viewport()->width()/bounds.width(),_view->viewport()->height()/bounds.height()),0.2,3.0);
            _view->setupScale(scale); _view->centerOn(bounds.center());
            if (!_loading) _save_timer.start();
        }
    });
    layout->addLayout(bar);
    layout->addWidget(_view, 1);
    _status = new QLabel(this);
    _status->setObjectName("pipelineValidation");
    _status->setWordWrap(true);
    _status->hide();
    // Keep validation messages on the existing control instead of consuming another row.
    connect(this,&SchemeEditorWidget::status_message,apply_button,&QPushButton::setToolTip);
    _apply_timer.setSingleShot(true);
    _apply_timer.setInterval(350);
    connect(&_apply_timer, &QTimer::timeout, this, [this] {
        if (_application.state().file_configs.ui.pipeline_auto_apply)
            apply();
        else if (_application.state().file_configs.ui.autosave_enabled)
            emit _application.on_config_updated();
    });
    connect(apply_button, &QPushButton::clicked, this, &SchemeEditorWidget::apply);
    connect(load_active, &QAction::triggered, this, &SchemeEditorWidget::load_active_pipeline);
    _last_processing_document = processing_document(application.state().data_flow_graph->save());
    _save_timer.setSingleShot(true); _save_timer.setInterval(350);
    connect(&_save_timer, &QTimer::timeout, this, [this] {
        save_view();
        if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
    });
    auto &model = *application.state().data_flow_graph;
    for (const auto id : model.allNodeIds()) _known_nodes.insert(id);
    connect(_scene, &DataFlowGraphicsScene::modified, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodeUpdated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodePositionUpdated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodeCreated, this, [this](QtNodes::NodeId id) {
        const bool new_node = !_known_nodes.contains(id);
        _known_nodes.insert(id);
        if (_loading || !new_node) return;
        QTimer::singleShot(0, this, [this,id] { avoid_overlap(id); changed(); });
    });
    connect(&model, &DataFlowGraphModel::nodeDeleted, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::connectionCreated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::connectionDeleted, this, &SchemeEditorWidget::changed);
    connect(&_scene->undoStack(), &QUndoStack::indexChanged, this, &SchemeEditorWidget::changed);
    auto *arrange = ui::make<QPushButton>("Arrange", this);
    arrange->setObjectName("arrangePipeline"); bar->insertWidget(2,arrange);
    connect(arrange, &QPushButton::clicked, this, &SchemeEditorWidget::arrange_nodes);
    connect(_view, &GraphicsView::scaleChanged, this, [this] { if (!_loading) _save_timer.start(); });
    for (auto *scroll : {_view->horizontalScrollBar(), _view->verticalScrollBar()})
        connect(scroll, &QScrollBar::sliderMoved, this, [this] { if (!_loading) _save_timer.start(); });
    static_cast<ProfiledGraphicsView *>(_view)->on_interaction = [this] { if (!_loading) _save_timer.start(); };
    static_cast<ProfiledGraphicsView *>(_view)->synchronize_positions = [this] {
        if (_loading) return;
        auto &graph = *_application.state().data_flow_graph;
        std::map<QtNodes::NodeId,QPointF> moved;
        for (const auto id : graph.allNodeIds()) {
            auto *item = _scene->nodeGraphicsObject(id);
            if (item && item->pos() != graph.nodeData(id, QtNodes::NodeRole::Position).toPointF())
                moved[id] = item->pos();
        }
        if (!moved.empty()) _scene->undoStack().push(new ArrangeCommand(graph, std::move(moved), ui::text("Move nodes")));
    };
    connect(reset, &QAction::triggered, this, [this] {
        auto &graph = *_application.state().data_flow_graph;
        _loading = true;
        const auto ids = graph.allNodeIds();
        for (auto id : ids)
            graph.deleteNode(id);
        graph.load(pipeline::GraphProgram::defaults(_application.state().file_configs.core.spectrum_channel,
                                                    _application.state().file_configs.core.spectrum_merge,
                                                    _application.state().file_configs.core.capture_source));
        _loading = false;
        arrange_nodes(); _view->centerOn(_scene->itemsBoundingRect().center());
        if (_application.state().file_configs.ui.pipeline_auto_apply) apply(); else changed();
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
                set_status(accepted ? ui::text("Pipeline applied")
                                          : ui::text("Not applied: ") + error + ui::text(". Last valid pipeline is still running."));
            });
    connect(&application, &GuiPresenter::saved_graph_warning_changed, this,
            [this](const QString &warning) { if (!warning.isEmpty()) set_status(warning); });
    QString error;
    auto draft = application.state().data_flow_graph->save();
    draft["version"] = 1;
    set_status(pipeline::GraphProgram::compile(draft, error)
                         ? ui::text("Pipeline ready. Right-click to add nodes; drag ports to connect.")
                         : ui::text("Draft not active: ") + error);
    if (application.is_remote())
        set_status("Editor shows the saved local draft, which may differ from the remote active pipeline. Load "
                         "active pipeline replaces this draft without changing the host.");
    QTimer::singleShot(0, this, [this] {
        _loading = true;
        const auto saved = _application.state().file_configs.ui.editor_view;
        if (_application.state().file_configs.scene_graph.value("layout_pending").toBool()) {
            _loading = false; arrange_nodes(); _loading = true;
        }
        if (saved.value("version").toInt() == 1) {
            const auto scale = saved.value("scale").toDouble(1);
            const auto x = saved.value("center_x").toDouble(), y = saved.value("center_y").toDouble();
            if (std::isfinite(scale) && std::isfinite(x) && std::isfinite(y)) {
                _view->setupScale(scale); _view->centerOn(x,y);
            } else _view->centerOn(_scene->itemsBoundingRect().center());
        } else _view->centerOn(_scene->itemsBoundingRect().center());
        _loading = false;
    });
}
void SchemeEditorWidget::set_status(const QString &text) {
    _status->setText(text); emit status_message(text);
}
bool SchemeEditorWidget::save_preset(const QString &name) {
    const auto trimmed=name.trimmed(); if (trimmed.isEmpty()) return false;
    flush_presentation();
    auto graph=_application.state().data_flow_graph->save(); graph["version"]=1;
    auto &presets=_application.state().file_configs.ui.pipeline_presets;
    presets.append(QJsonObject{{"version",1},{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"name",trimmed},{"graph",graph},{"view",_application.state().file_configs.ui.editor_view}});
    if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
    return true;
}
bool SchemeEditorWidget::remove_preset(const QString &id) {
    auto &presets=_application.state().file_configs.ui.pipeline_presets;
    for (int i=0;i<presets.size();++i) if (presets[i].toObject().value("id").toString()==id) {
        presets.removeAt(i);
        if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
        return true;
    }
    return false;
}
void SchemeEditorWidget::load_document(const QJsonObject &document) {
    _save_timer.stop(); _apply_timer.stop(); _loading=true;
    auto &graph=*_application.state().data_flow_graph;
    const QSignalBlocker graph_blocker(&graph),scene_blocker(_scene);
    const auto ids=graph.allNodeIds(); for (auto id : ids) graph.deleteNode(id);
    graph.load(pipeline::canonical_graph_document(document)); _scene->onModelReset(); _scene->undoStack().clear();
    _known_nodes.clear(); for (auto id : graph.allNodeIds()) _known_nodes.insert(id);
    _loading=false; changed();
}
bool SchemeEditorWidget::load_preset(const QString &id) {
    if (_application.graph_request_pending()) return false;
    for (const auto &value : _application.state().file_configs.ui.pipeline_presets) {
        const auto preset=value.toObject();
        if (preset.value("id").toString()!=id || preset.value("version").toInt()!=1) continue;
        const auto document=preset.value("graph").toObject();
        if (!document.value("nodes").isArray() || !document.value("connections").isArray()) return false;
        QSet<int> ids;
        for (const auto &value : document.value("nodes").toArray()) {
            const auto node=value.toObject(); const int node_id=node.value("id").toInt(-1);
            if (node_id<0 || ids.contains(node_id) || pipeline::node_definition(node.value("internal-data").toObject().value("model-name").toString()).name.isEmpty()) return false;
            ids.insert(node_id);
        }
        load_document(document);
        const auto view=preset.value("view").toObject();
        const double scale=view.value("scale").toDouble(1),x=view.value("center_x").toDouble(),y=view.value("center_y").toDouble();
        if (std::isfinite(scale) && std::isfinite(x) && std::isfinite(y)) { _view->setupScale(std::clamp(scale,0.2,3.0)); _view->centerOn(x,y); }
        return true;
    }
    return false;
}
void SchemeEditorWidget::apply() {
    _application.state().editor_draft_modified = true;
    auto document = _application.state().data_flow_graph->save();
    document["version"] = 1;
    if (_application.state().file_configs.ui.autosave_enabled)
        emit _application.on_config_updated();
    set_status(ui::text("Applying pipeline…"));
    _apply_request = _application.apply_pipeline_graph(document);
}

void SchemeEditorWidget::load_active_pipeline() {
    _save_timer.stop();
    _apply_timer.stop();
    if (_application.graph_request_pending()) {
        set_status(ui::text("Wait for the pending graph command to finish before loading the active pipeline."));
        return;
    }
    const auto document = _application.client().snapshot().graph;
    QString error;
    if (!pipeline::GraphProgram::compile(document, error)) {
        set_status(ui::text("Active pipeline unavailable: ") + error);
        return;
    }
    _apply_timer.stop();
    _apply_request = 0;
    auto &graph = *_application.state().data_flow_graph;
    _loading = true;
    const QSignalBlocker graph_blocker(&graph), scene_blocker(_scene);
    const auto ids = graph.allNodeIds();
    for (auto id : ids)
        graph.deleteNode(id);
    graph.load(pipeline::canonical_graph_document(document));
    // Graph signals are blocked, so recreate graphics explicitly without treating it as an edit.
    _scene->onModelReset();
    _scene->undoStack().clear();
    _application.state().editor_draft_modified = false;
    _last_processing_document = processing_document(graph.save());
    _view->centerOn(_scene->itemsBoundingRect().center());
    _loading = false;
    set_status(ui::text("Active graph loaded. Save settings to keep this draft."));
}

SchemeEditorWidget::~SchemeEditorWidget() {
    _loading = true; _save_timer.stop(); _apply_timer.stop();
    disconnect(_application.state().data_flow_graph.get(),nullptr,this,nullptr);
    disconnect(_scene,nullptr,this,nullptr);
    disconnect(&_scene->undoStack(),nullptr,this,nullptr);
    disconnect(&_application,nullptr,this,nullptr);
    disconnect(_view,nullptr,this,nullptr);
    for (auto *scroll : {_view->horizontalScrollBar(),_view->verticalScrollBar()}) disconnect(scroll,nullptr,this,nullptr);
    static_cast<ProfiledGraphicsView *>(_view)->on_interaction = {};
    static_cast<ProfiledGraphicsView *>(_view)->synchronize_positions = {};
}
void SchemeEditorWidget::changed() {
    if (_loading) return;
    _application.state().editor_draft_modified = true;
    _save_timer.start();
    const auto processing = processing_document(_application.state().data_flow_graph->save());
    if (processing != _last_processing_document) {
        _last_processing_document = processing;
        set_status(ui::text("Draft changed; the last valid pipeline continues running"));
        _apply_timer.start();
    }
}
void SchemeEditorWidget::save_view() {
    const auto center = _view->mapToScene(_view->viewport()->rect().center());
    _application.state().file_configs.ui.editor_view = {{"version",1},{"scale",_view->getScale()},
        {"center_x",center.x()},{"center_y",center.y()}};
}
void SchemeEditorWidget::flush_presentation() {
    auto &sync = static_cast<ProfiledGraphicsView *>(_view)->synchronize_positions;
    if (sync) sync();
    _save_timer.stop(); save_view();
    if (_application.state().file_configs.ui.autosave_enabled && _application.state().editor_draft_modified)
        emit _application.on_config_updated();
}
void SchemeEditorWidget::arrange_nodes() {
    auto &graph = *_application.state().data_flow_graph;
    std::map<QtNodes::NodeId,int> incoming, layer;
    std::map<QtNodes::NodeId,std::vector<QtNodes::NodeId>> next;
    for (const auto id : graph.allNodeIds()) { incoming[id] = 0; layer[id] = 0; }
    for (const auto value : graph.save().value("connections").toArray()) {
        const auto edge = value.toObject();
        const auto from = QtNodes::NodeId(edge.value("outNodeId").toInt()), to = QtNodes::NodeId(edge.value("inNodeId").toInt());
        if (incoming.contains(from) && incoming.contains(to)) { ++incoming[to]; next[from].push_back(to); }
    }
    std::queue<QtNodes::NodeId> queue;
    for (const auto &[id,count] : incoming) if (!count) queue.push(id);
    while (!queue.empty()) {
        const auto id = queue.front(); queue.pop();
        for (auto to : next[id]) { layer[to] = std::max(layer[to],layer[id]+1); if (--incoming[to] == 0) queue.push(to); }
    }
    // Cyclic drafts are still arrangable; placement does not validate/apply DSP.
    std::map<int,std::vector<QtNodes::NodeId>> columns;
    for (const auto &[id,column] : layer) columns[column].push_back(id);
    std::map<QtNodes::NodeId,QPointF> positions;
    double x = 0;
    for (const auto &[column,ids] : columns) {
        double y = 0, width = 0;
        for (const auto id : ids) if (auto *item = _scene->nodeGraphicsObject(id)) {
            const auto bounds = item->boundingRect();
            positions[id] = QPointF(x - bounds.left(), y - bounds.top());
            width = std::max(width,bounds.width()); y += bounds.height() + 40;
        }
        x += width + 40;
    }
    if (!positions.empty()) _scene->undoStack().push(new ArrangeCommand(graph,std::move(positions)));
}
void SchemeEditorWidget::avoid_overlap(QtNodes::NodeId id) {
    auto &graph = *_application.state().data_flow_graph;
    auto *item = _scene->nodeGraphicsObject(id);
    if (!item) return;
    auto position = graph.nodeData(id,QtNodes::NodeRole::Position).toPointF();
    for (int attempt = 0; attempt < 512; ++attempt) {
        const auto rect = item->boundingRect().translated(position).adjusted(-20,-20,20,20);
        bool overlap = false;
        for (const auto other : graph.allNodeIds()) if (other != id)
            if (auto *existing = _scene->nodeGraphicsObject(other))
                if (rect.intersects(existing->sceneBoundingRect())) { overlap = true; break; }
        if (!overlap) break;
        position += QPointF(40,40);
    }
    graph.setNodeData(id,QtNodes::NodeRole::Position,position);
}
