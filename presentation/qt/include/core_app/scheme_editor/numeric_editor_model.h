#pragma once
#include <domain/processing_contracts.h>
#include <QtNodes/DataFlowGraphModel>
#include <QUndoStack>
#include <QJsonArray>
#include <map>
#include <tuple>
#include <utility>
class QTreeWidget;
class AudioNodeModel;
namespace presentation { struct PlotDescriptor; }
#include "core_app/scheme_editor/numeric_node_operations.h"
namespace numeric_editor {
presentation::PlotDescriptor plot_descriptor_for_kind(const QString&);
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
    bool edit_nodes(const std::vector<domain::ConfiguredProcessingNode>&,QString* error=nullptr);
    bool edit_presentation(QtNodes::NodeId,const QJsonObject&);
    bool edit_adapter(QtNodes::NodeId,const QJsonObject&);
    bool has_numeric_nodes() const;
    bool atomic_edit_in_progress() const {return _atomic_edit_in_progress;}
    // Execution resolves hardware formats; persistence retains the editor contracts.
    bool export_numeric_graph(domain::NumericProcessingGraph&,QString* error=nullptr,bool resolve_native=true) const;
    void set_node_validator(std::function<domain::ProcessingDiagnostic(const domain::ConfiguredProcessingNode&)> validator) { _node_validator=std::move(validator); }
    void set_capture_device_provider(std::function<QJsonArray(int,QString*)> provider);
    void set_admission_limits(std::optional<domain::ProcessingAdmissionLimits>);
    const std::optional<domain::ProcessingAdmissionLimits>& admission_limits() const { return _admission_limits; }
    bool synchronize_native_sources(QString* error=nullptr);
    QString node_error(const domain::ConfiguredProcessingNode&) const;
signals:
    void edit_rejected(const QString&);
    void connection_diagnostics_changed();
    void atomic_edit_finished();
    void immediate_apply_requested();
    void persistence_topology_changed();
private:
    int compute_connection_visual_state(QtNodes::ConnectionId) const;
    bool connection_endpoints_available(QtNodes::ConnectionId) const;
    bool connection_would_cycle(QtNodes::ConnectionId) const;
    bool bind_configuration_clock(const domain::ConfiguredProcessingNode&,domain::ConfiguredProcessingNode&,
                                  const domain::ConfigurationBinding&,domain::NumericProcessingGraph&) const;
    std::pair<AudioNodeModel*,AudioNodeModel*> connection_models(QtNodes::ConnectionId) const;
    std::pair<const domain::ProcessingPort*,const domain::ProcessingPort*> data_connection_ports(QtNodes::ConnectionId) const;
    void add_adapted_connection(QtNodes::ConnectionId);
    void clear_removed_clock_binding(QtNodes::ConnectionId);
    bool edit_scalar_topology(QtNodes::NodeId,const domain::ConfiguredProcessingNode&,const QJsonObject&);
    void bind_node_handlers(QtNodes::NodeId,AudioNodeModel&);
    void sync_new_audio_source();
    void restore_data_connections(const domain::NumericProcessingGraph&);
    void restore_configuration_connections(const domain::NumericProcessingGraph&,const domain::NumericProcessingGraph&);
    bool resolve_adapter_graph(QtNodes::NodeId,const QJsonObject&,domain::NumericProcessingGraph&);
    bool load_draft_node(const domain::ConfiguredProcessingNode&,const QJsonObject&,QString*);
    bool load_draft_edges(const domain::NumericProcessingGraph&,QString*);
    bool load_draft_bindings(const domain::NumericProcessingGraph&,QString*);
    void install_draft_graph(const domain::NumericProcessingGraph&,const NumericEditorGraphModel&);
    void restore_document_nodes(const domain::NumericProcessingGraph&);
    void load_document_policies(const QJsonObject&);
    QJsonObject document_connection(QJsonObject) const;
    void save_document_connections(QJsonObject&) const;
    void save_document_metadata(QJsonObject&) const;
    void append_numeric_graph_edges(domain::NumericProcessingGraph&,const std::vector<QtNodes::NodeId>&) const;
    bool validate_export_node_diagnostics(const domain::NumericProcessingGraph&,QString*) const;
    void install_draft_nodes(const domain::NumericProcessingGraph&,const NumericEditorGraphModel&);
    void install_draft_edges(const domain::NumericProcessingGraph&);
    domain::NumericProcessingGraph graph_without_undo_edge(QtNodes::ConnectionId) const;
    bool is_parameter_connection(QtNodes::ConnectionId);
    bool validate_generator_edit(const domain::ConfiguredProcessingNode&);
    bool commit_parameter_edit(const domain::NumericProcessingGraph&,const domain::NumericProcessingGraph&);
    bool valid_node_edit_source(QtNodes::NodeId,const domain::ConfiguredProcessingNode&);
    bool evaluate_node_changes(domain::NumericProcessingGraph&,QString*);
    bool draft_preflight(const domain::NumericProcessingGraph&,QString* error=nullptr);
    void commit_graph_edit(const domain::NumericProcessingGraph&,const domain::NumericProcessingGraph&,const QString&);
    std::function<domain::ProcessingDiagnostic(const domain::ConfiguredProcessingNode&)> _node_validator;
    void bind_node(QtNodes::NodeId);
    void reveal_connected_dc(const domain::NumericProcessingGraph&);
    void refresh_connected_dc(QtNodes::NodeId);
    bool resolve_native_sources(domain::NumericProcessingGraph&,QString* error,const QJsonObject* imported_adapters=nullptr) const;
    bool resolve_native_source(domain::NumericProcessingGraph&,domain::NodeId,const QJsonObject&,QString*) const;
    bool validate_export_nodes(const domain::NumericProcessingGraph&,QString*) const;
    QString capture_adapter_error(const AudioNodeModel&,const QJsonObject&) const;
    void commit_adapter_edit(QtNodes::NodeId,const QJsonObject&,const QJsonObject&,const domain::NumericProcessingGraph&,const domain::NumericProcessingGraph&);
    bool propagate_clock(domain::NumericProcessingGraph&,domain::NodeId) const;
    bool adapt_configuration(QtNodes::ConnectionId,domain::NumericProcessingGraph&) const;
    bool adapt_connection_clock(QtNodes::ConnectionId,domain::NumericProcessingGraph&) const;
    void replace_clock_graph(const domain::NumericProcessingGraph&);
    void remove_changed_connections(const domain::NumericProcessingGraph&,const domain::NumericProcessingGraph&);
    void update_graph_nodes(const domain::NumericProcessingGraph&);
    void restore_graph_connections(const domain::NumericProcessingGraph&,const domain::NumericProcessingGraph&);
    std::function<QJsonArray(int,QString*)> _capture_device_provider;
    std::optional<domain::ProcessingAdmissionLimits> _admission_limits;
    QUndoStack* _undo=nullptr;
    std::string _graph_id="main";
    signal_contracts::Ratio _execution_interval{1,120};
    domain::ProcessingVersions _versions;
    std::map<std::tuple<QtNodes::NodeId,QtNodes::PortIndex,QtNodes::NodeId,QtNodes::PortIndex>,std::uint8_t> _edge_policies;
    mutable std::map<std::tuple<QtNodes::NodeId,QtNodes::PortIndex,QtNodes::NodeId,QtNodes::PortIndex>,int> _connection_visual_states;
    bool _allow_connection_clock_adaptation=true;
    bool _atomic_edit_in_progress=false;
};
