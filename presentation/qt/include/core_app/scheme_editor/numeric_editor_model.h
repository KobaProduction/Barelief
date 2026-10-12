#pragma once
#include <domain/processing_contracts.h>
#include <QtNodes/DataFlowGraphModel>
#include <QUndoStack>
#include <QJsonArray>
#include <map>
#include <tuple>
class QTreeWidget;
#include "core_app/scheme_editor/numeric_node_operations.h"
namespace numeric_editor {
void populate_catalog(QTreeWidget*);
void filter_catalog(QTreeWidget*,const QString&);
}

class NumericEditorGraphModel final : public QtNodes::DataFlowGraphModel {
    Q_OBJECT
public:
    explicit NumericEditorGraphModel(std::shared_ptr<QtNodes::NodeDelegateModelRegistry>);
    bool connectionPossible(QtNodes::ConnectionId) const override;
    void addConnection(QtNodes::ConnectionId) override;
    bool deleteConnection(QtNodes::ConnectionId) override;
    QJsonObject saveConnection(const QtNodes::ConnectionId&) const override;
    void loadConnection(const QJsonObject&) override;
    QJsonObject connectionUndoState(QtNodes::ConnectionId) const override;
    bool undoConnection(QtNodes::ConnectionId,const QJsonObject&) override;
    std::uint8_t connection_policy(QtNodes::ConnectionId) const;
    QString connection_status(QtNodes::ConnectionId) const;
    int connection_visual_state(QtNodes::ConnectionId) const; // exact, safe conversion, risk, failure
    QString connection_failure(QtNodes::ConnectionId) const;
    QString connection_effects(QtNodes::ConnectionId) const;
    bool edit_connection_policy(QtNodes::ConnectionId,std::uint8_t);
    void loadNode(const QJsonObject&) override;
    QtNodes::NodeId addNode(QString) override;
    QJsonObject save() const override;
    void load(const QJsonObject&) override;
    std::optional<domain::ConfigurationBinding> configuration_binding(QtNodes::ConnectionId) const;
    QtNodes::ConnectionId configuration_connection(const domain::ConfigurationBinding&) const;
    domain::Edge canonical_edge(QtNodes::ConnectionId) const;
    QtNodes::ConnectionId connection_id(const domain::Edge&) const;
    domain::NumericProcessingGraph numeric_graph() const;
    bool set_numeric_graph(const domain::NumericProcessingGraph&,QString* error=nullptr,const QJsonObject& node_adapters={});
    bool prepare_numeric_import(const domain::NumericProcessingGraph&,const QJsonObject& node_adapters,domain::NumericProcessingGraph&,QString* error=nullptr) const;
    void set_undo_stack(QUndoStack* stack) { _undo=stack; }
    bool edit_execution_interval(signal_contracts::Ratio);
    bool edit_node(QtNodes::NodeId,const domain::ConfiguredProcessingNode&);
    bool edit_presentation(QtNodes::NodeId,const QJsonObject&);
    bool edit_adapter(QtNodes::NodeId,const QJsonObject&);
    bool has_numeric_nodes() const;
    bool atomic_edit_in_progress() const {return _atomic_edit_in_progress;}
    bool export_numeric_graph(domain::NumericProcessingGraph&,QString* error=nullptr) const;
    void set_node_validator(std::function<domain::ProcessingDiagnostic(const domain::ConfiguredProcessingNode&)> validator) { _node_validator=std::move(validator); }
    void set_capture_device_provider(std::function<QJsonArray(int,QString*)> provider);
    bool synchronize_native_sources(QString* error=nullptr);
    QString node_error(const domain::ConfiguredProcessingNode&) const;
signals:
    void edit_rejected(const QString&);
    void connection_diagnostics_changed();
    void atomic_edit_finished();
private:
    std::function<domain::ProcessingDiagnostic(const domain::ConfiguredProcessingNode&)> _node_validator;
    void bind_node(QtNodes::NodeId);
    void replace_node(QtNodes::NodeId,const domain::ConfiguredProcessingNode&);
    bool resolve_native_sources(domain::NumericProcessingGraph&,QString* error,const QJsonObject* imported_adapters=nullptr) const;
    bool propagate_clock(domain::NumericProcessingGraph&,domain::NodeId) const;
    bool adapt_configuration(QtNodes::ConnectionId,domain::NumericProcessingGraph&) const;
    bool adapt_connection_clock(QtNodes::ConnectionId,domain::NumericProcessingGraph&) const;
    void replace_clock_graph(const domain::NumericProcessingGraph&);
    std::function<QJsonArray(int,QString*)> _capture_device_provider;
    QUndoStack* _undo=nullptr;
    std::string _graph_id="main";
    signal_contracts::Ratio _execution_interval{1,120};
    domain::ProcessingVersions _versions;
    std::map<std::tuple<QtNodes::NodeId,QtNodes::PortIndex,QtNodes::NodeId,QtNodes::PortIndex>,std::uint8_t> _edge_policies;
    bool _allow_connection_clock_adaptation=true;
    bool _atomic_edit_in_progress=false;
};
