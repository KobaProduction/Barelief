#pragma once
#include "presentation/processor_discovery_port.h"
#include <memory>

namespace presentation {
class ProcessorDirectoryModel final : public QObject {
    Q_OBJECT
public:
    explicit ProcessorDirectoryModel(QString local_id, std::shared_ptr<ProcessorDiscoveryPort> discovery, QObject *parent = nullptr);
    const QVector<ProcessorEndpoint> &processors() const { return _processors; }
    const ProcessorDiscoverySnapshot &discovery() const { return _snapshot; }
    QString selected_id() const { return _selected; }
    const ProcessorEndpoint *selected() const;
    void set_selected(const QString &id);
    void add_direct(const QString &id, const QString &address, quint16 port);
    bool request_selection(const QString &id);
    bool connect_registry(const QString &, quint16);
    void disconnect_registry();
signals:
    void changed();
    void selection_requested(const presentation::ProcessorEndpoint &endpoint);
private:
    void refresh();
    void merge_registry_endpoint(ProcessorEndpoint endpoint);
    QString _local, _selected;
    std::shared_ptr<ProcessorDiscoveryPort> _discovery;
    ProcessorDiscoverySnapshot _snapshot;
    QVector<ProcessorEndpoint> _processors;
};
}
