#pragma once
#include <QtNodes/NodeDelegateModel>
#include <QPointer>
#include <QLabel>
#include "pipeline/graph.h"
class AudioNodeModel final : public QtNodes::NodeDelegateModel {
public:
    explicit AudioNodeModel(QString kind);
    QString name() const override { return _kind; }
    QString caption() const override { return _kind; }
    unsigned int nPorts(QtNodes::PortType type) const override;
    QtNodes::NodeDataType dataType(QtNodes::PortType type, QtNodes::PortIndex) const override;
    std::shared_ptr<QtNodes::NodeData> outData(QtNodes::PortIndex) override { return {}; }
    void setInData(std::shared_ptr<QtNodes::NodeData>, QtNodes::PortIndex) override {}
    QWidget *embeddedWidget() override;
    QJsonObject save() const override;
    void load(const QJsonObject &json) override;
    void preview(pipeline::NodePreview value);
private:
    QString _kind;
    QJsonObject _parameters;
    QPointer<QWidget> _widget;
    QPointer<QLabel> _preview;
    void sync_controls();
};
