#pragma once
#include <QtNodes/NodeDelegateModel>
#include <QPointer>
#include <QLabel>
#include "pipeline/graph.h"
class NodePreviewCard;
class AudioNodeModel final : public QtNodes::NodeDelegateModel {
    Q_OBJECT
public:
    explicit AudioNodeModel(QString kind);
    QString name() const override { return _kind; }
    QString caption() const override;
    unsigned int nPorts(QtNodes::PortType type) const override;
    QtNodes::NodeDataType dataType(QtNodes::PortType type, QtNodes::PortIndex) const override;
    std::shared_ptr<QtNodes::NodeData> outData(QtNodes::PortIndex) override { return {}; }
    void setInData(std::shared_ptr<QtNodes::NodeData>, QtNodes::PortIndex) override {}
    QWidget *embeddedWidget() override;
    QJsonObject save() const override;
    void load(const QJsonObject &json) override;
    void preview(pipeline::NodePreview value);
    QWidget *parameter_widget(QWidget *parent = nullptr);
    QWidget *preview_widget(QWidget *parent = nullptr);
    void set_parameter(const QString &key, const QJsonValue &value);
    pipeline::NodePreview last_preview() const { return _last_preview; }
signals:
    void local_source_output_changed(bool enabled);
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    QString _kind;
    QJsonObject _parameters;
    QPointer<QWidget> _widget;
    QList<QPointer<NodePreviewCard>> _previews;
    QList<QPointer<QWidget>> _controls;
    pipeline::NodePreview _last_preview;
    void sync_controls();
    void update_widget_style(QWidget *widget);
    void update_previews();
    void update_node_style();
};
