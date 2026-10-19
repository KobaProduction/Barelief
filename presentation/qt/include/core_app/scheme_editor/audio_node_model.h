#pragma once
#include <QtNodes/NodeDelegateModel>
#include <QPointer>
#include <QLabel>
#include <QJsonArray>
#include "pipeline/engine_models.h"
#include <domain/processing_contracts.h>
#include <optional>
class NodePreviewCard;
class NodeParameterModel;
class QMenu;
class QVBoxLayout;
class QFormLayout;
class QLineEdit;
class AudioNodeModel final : public QtNodes::NodeDelegateModel {
    Q_OBJECT
public:
    explicit AudioNodeModel(QString kind);
    QString name() const override { return _kind; }
    QString caption() const override;
    QString operation_summary() const;
    QString stream_name(unsigned) const;
    unsigned grouped_channels(QtNodes::PortType) const;
    std::optional<domain::ConfigurationRole> configuration_role(QtNodes::PortType,QtNodes::PortIndex) const;
    QtNodes::PortIndex configuration_port(QtNodes::PortType,domain::ConfigurationRole) const;
    unsigned int nPorts(QtNodes::PortType type) const override;
    QtNodes::NodeDataType dataType(QtNodes::PortType type, QtNodes::PortIndex) const override;
    bool portCaptionVisible(QtNodes::PortType, QtNodes::PortIndex) const override { return true; }
    QString portCaption(QtNodes::PortType, QtNodes::PortIndex) const override;
    std::shared_ptr<QtNodes::NodeData> outData(QtNodes::PortIndex) override { return {}; }
    void setInData(std::shared_ptr<QtNodes::NodeData>, QtNodes::PortIndex) override {}
    QWidget *embeddedWidget() override;
    QJsonObject save() const override;
    void load(const QJsonObject &json) override;
    struct DiagnosticPreview {
        domain::PortDirection direction=domain::PortDirection::Output;
        domain::PortId port=0;
        bool available=false;
        QString count_source="Values", exact_value, description, failure;
        std::int64_t timestamp_ticks=0;
        std::uint64_t count=0,epoch=0,revision=0,producer_epoch=0,generation=0,sequence=0;
        bool sequence_seen=false,sequence_reset=false;
    };
    bool preview(const pipeline::DeliveredNumericFrame&);
    bool preview(const pipeline::DeliveredNumericHistoryRow&);
    bool preview_history_snapshot(const pipeline::DeliveredNumericFrame&,std::uint64_t ordinal,std::uint64_t filled_count);
    void clear_preview();
    QString diagnostic_failure(domain::PortDirection,domain::PortId) const;
    void set_preview_activity(const QString& status_key);
    void set_processing_activity(bool measured,bool active,double calls_hz);
    bool processing_measured() const {return _activity_measured;}
    bool processing_active() const {return _activity_active;}
    QWidget *parameter_widget(QWidget *parent = nullptr);
    NodeParameterModel* parameter_model() const { return _parameters; }
    void set_admission_limits(std::optional<domain::ProcessingAdmissionLimits>);
    QWidget *preview_widget(QWidget *parent = nullptr,bool compact=false);
    const domain::ConfiguredProcessingNode* numeric_node() const { return _numeric ? &*_numeric : nullptr; }
    bool numeric_mode() const { return _numeric.has_value() || !_numeric_load_error.isEmpty(); }
    QString numeric_load_error() const { return _numeric_load_error; }
    void set_numeric_node(const domain::ConfiguredProcessingNode&);
    void set_numeric_edit_handler(std::function<bool(const domain::ConfiguredProcessingNode&)> handler) { _numeric_edit=std::move(handler); }
    QJsonObject presentation() const { return _presentation; }
    static bool valid_presentation(const QJsonObject&);
    void set_presentation(const QJsonObject&);
    bool edit_display_name(const QString&);
    bool dc_outputs_shown() const { return _presentation.value("show_dc_outputs").toBool(); }
    bool edit_dc_outputs_shown(bool);
    void set_dc_connections_visible(bool);
    QWidget* scalar_inputs_widget(QWidget* parent=nullptr,bool inline_form=false);
    bool add_scalar_input();
    bool remove_scalar_input(domain::PortId);
    bool edit_scalar_input_name(domain::PortId,const QString&);
    bool set_scalar_input_enabled(domain::PortId,bool);
    void set_scalar_topology_handler(std::function<bool(const domain::ConfiguredProcessingNode&,const QJsonObject&)> handler){_scalar_topology_edit=std::move(handler);}
    bool parameter_on_block(const QString&) const;
    bool set_parameter_on_block(const QString&,bool);
    domain::PortId port_id(QtNodes::PortType,QtNodes::PortIndex) const;
    QtNodes::PortIndex port_index(QtNodes::PortType,domain::PortId) const;
    domain::PortId selected_input() const;
    bool select_input(domain::PortId);
    void populate_selector_menu(QMenu*);
    std::vector<QtNodes::PortIndex> visual_port_order(QtNodes::PortType) const;
    void set_presentation_edit_handler(std::function<bool(const QJsonObject&)> handler) { _presentation_edit=std::move(handler); }
    QJsonObject adapter() const { return _adapter; }
    bool set_adapter(const QJsonObject&);
    bool edit_adapter(const QJsonObject&);
    void set_capture_device_provider(std::function<QJsonArray(int,QString*)> provider) { _capture_device_provider=std::move(provider);emit capture_devices_changed(); }
    void set_adapter_edit_handler(std::function<bool(const QJsonObject&)> handler) { _adapter_edit=std::move(handler); }
signals:
    void activity_changed();
    void local_source_output_changed(bool enabled);
    void parameter_values_changed();
    void numeric_edit_rejected(const QString&);
    void parameter_structure_changed();
    void presentation_changed();
    void adapter_changed();
    void capture_devices_changed();
    void diagnostics_changed();
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    NodeParameterModel* _parameters=nullptr;
    QString _kind;
    QPointer<QWidget> _widget;
    QPointer<QLabel> _processing_activity;
    bool _activity_measured=false,_activity_active=false;
    double _activity_hz=0;
    QList<QPointer<NodePreviewCard>> _previews;
    std::vector<DiagnosticPreview> _diagnostics;
    QString _preview_activity;
    QString _diagnostic_error_key;
    std::optional<domain::ConfiguredProcessingNode> _numeric;
    std::optional<domain::ProcessingAdmissionLimits> _admission_limits;
    QString _numeric_load_error, _rejected_numeric_snapshot;
    std::function<bool(const domain::ConfiguredProcessingNode&)> _numeric_edit;
    QJsonObject _presentation;
    bool _dc_connections_visible=false;
    unsigned visible_numeric_ports(QtNodes::PortType,const domain::ConfiguredProcessingNode&) const;
    void update_dc_visibility(bool shown,bool connected);
    void publish_dc_port_visibility(unsigned before,unsigned after,bool shown,bool connected);
    std::function<bool(const QJsonObject&)> _presentation_edit;
    std::function<bool(const domain::ConfiguredProcessingNode&,const QJsonObject&)> _scalar_topology_edit;
    QJsonObject _adapter;
    std::function<bool(const QJsonObject&)> _adapter_edit;
    std::function<QJsonArray(int,QString*)> _capture_device_provider;
    using PortLabelEdits=std::vector<std::pair<QString,QLineEdit*>>;
    void edit_port_contracts(QWidget*);
    void edit_port_labels(QWidget*);
    std::pair<QString,QLineEdit*> port_label_editor(QWidget*,QtNodes::PortType,domain::PortId) const;
    PortLabelEdits append_port_label_editors(QFormLayout*) const;
    void apply_port_labels(const PortLabelEdits&);
    QWidget* numeric_inspector(QWidget*);
    QWidget* numeric_parameter_widget(QWidget* parent,bool inline_form=false);
    QWidget* selector_parameter_widget(QWidget* parent,QWidget* form);
    QWidget* scalar_parameter_widget(QWidget* parent,QWidget* form,bool inline_form);
    bool can_add_configured_input() const;
    bool add_configured_input();
    bool remove_configured_input(domain::PortId);
    void append_stream_menu(QMenu*);
    void append_input_menu(QMenu*);
    std::vector<unsigned> stream_ids() const;
    std::vector<unsigned> selector_stream_ids() const;
    void edit_stream_name(unsigned,const QString&);
    void append_stream_name(QVBoxLayout*,unsigned);
    void rebuild_stream_names(QWidget*);
    QWidget* stream_names_widget(QWidget*);
    void rebuild_scalar_inputs(QWidget*,bool);
    void bind_scalar_input_updates(QWidget*,bool);
    void append_scalar_input(QVBoxLayout*,domain::PortId,const QJsonObject&,bool);
    void append_scalar_input_action(QVBoxLayout*,bool);
    void replace_port_side(QtNodes::PortType,const domain::ConfiguredProcessingNode&);
    void publish_numeric_change(const std::optional<domain::ConfiguredProcessingNode>&,bool);
    bool edit_numeric(domain::ConfiguredProcessingNode);
    QVBoxLayout* create_embedded_body();
    void add_embedded_summary(QVBoxLayout*);
    void initialize_embedded_activity();
    void update_widget_style(QWidget *widget);
    void update_previews();
    void update_diagnostic_error();
    bool publish_preview(DiagnosticPreview);
    const domain::ProcessingPort* preview_port(const pipeline::DeliveredNumericFrame&) const;
    bool read_preview_value(const signal_contracts::DataFrameView&,const signal_contracts::DataContract&,DiagnosticPreview&) const;
    QString preview_failure(signal_contracts::Status) const;
    QString field_summary(const QString&,bool) const;
    QString routing_summary() const;
    QString processing_summary() const;
    QString descriptive_port_caption(QtNodes::PortType,QtNodes::PortIndex) const;
    QString fallback_port_caption(QtNodes::PortType,QtNodes::PortIndex) const;
    QString music_port_caption(QtNodes::PortType,QtNodes::PortIndex) const;
    QString numeric_port_caption(QtNodes::PortType,QtNodes::PortIndex) const;
    QString numeric_output_caption(QtNodes::PortType,QtNodes::PortIndex) const;
    QString array_multiply_caption(QtNodes::PortType,QtNodes::PortIndex) const;
    void bind_parameter_model();
    void reject_numeric_snapshot(const QString& snapshot,const QString& error);
    void load_numeric_snapshot(const QString& snapshot);
    DiagnosticPreview preview_for_port(const domain::ProcessingPort& port) const;
    void schedule_widget_size_update();
    void refresh_widget_size();
    QString fourier_port_caption(QtNodes::PortType,QtNodes::PortIndex) const;
    QString routing_port_caption(QtNodes::PortType,QtNodes::PortIndex) const;
    void update_node_style();
    void refresh_processing_activity_label(bool measured,bool active);
};
