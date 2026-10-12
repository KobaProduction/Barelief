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
    QString _numeric_load_error, _rejected_numeric_snapshot;
    std::function<bool(const domain::ConfiguredProcessingNode&)> _numeric_edit;
    QJsonObject _presentation;
    std::function<bool(const QJsonObject&)> _presentation_edit;
    std::function<bool(const domain::ConfiguredProcessingNode&,const QJsonObject&)> _scalar_topology_edit;
    QJsonObject _adapter;
    std::function<bool(const QJsonObject&)> _adapter_edit;
    std::function<QJsonArray(int,QString*)> _capture_device_provider;
    QWidget* numeric_parameter_widget(QWidget* parent,bool inline_form=false);
    bool edit_numeric(domain::ConfiguredProcessingNode);
    void update_widget_style(QWidget *widget);
    void update_previews();
    bool publish_preview(DiagnosticPreview);
    void update_node_style();
};
