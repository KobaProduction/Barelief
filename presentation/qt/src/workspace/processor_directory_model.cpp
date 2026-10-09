#include "core_app/workspace/processor_directory_model.h"
#include <QUuid>
#include <algorithm>
namespace presentation {
static QString canonical(const QString &id) {
    const QUuid uuid(id); return uuid.isNull() ? id : uuid.toString(QUuid::WithoutBraces);
}
ProcessorDirectoryModel::ProcessorDirectoryModel(QString local, std::shared_ptr<ProcessorDiscoveryPort> discovery, QObject *parent)
    : QObject(parent), _local(std::move(local)), _selected(_local), _discovery(std::move(discovery)) {
    setObjectName("processorDirectoryModel");
    ProcessorEndpoint endpoint; endpoint.id = _local; endpoint.local = true;
    _processors.append(endpoint);
    if (_discovery) connect(_discovery.get(), &ProcessorDiscoveryPort::changed, this, &ProcessorDirectoryModel::refresh);
    refresh();
}
void ProcessorDirectoryModel::refresh() {
    _snapshot = _discovery ? _discovery->snapshot() : ProcessorDiscoverySnapshot{};
    for (auto &existing : _processors) if (existing.from_registry) {
        existing.available = false; existing.error = _snapshot.error;
    }
    for (auto endpoint : _snapshot.processors) {
        if (canonical(endpoint.id) == canonical(_local)) continue;
        auto found = std::find_if(_processors.begin(), _processors.end(), [&](const auto &item) { return canonical(item.id) == canonical(endpoint.id); });
        if (found == _processors.end()) _processors.append(endpoint);
        else {
            endpoint.id = found->id;
            if (!endpoint.available) {
                endpoint.name = found->name; endpoint.address = found->address; endpoint.port = found->port;
            }
            *found = endpoint;
        }
    }
    emit changed();
}
const ProcessorEndpoint *ProcessorDirectoryModel::selected() const {
    for (const auto &item : _processors) if (canonical(item.id) == canonical(_selected)) return &item;
    return nullptr;
}
void ProcessorDirectoryModel::set_selected(const QString &id) { _selected = id; emit changed(); }
void ProcessorDirectoryModel::add_direct(const QString &id, const QString &address, quint16 port) {
    if (id.isEmpty() || address.isEmpty() || !port) return;
    for (const auto &item : _processors) if (canonical(item.id) == canonical(id)) return;
    ProcessorEndpoint item; item.id = id; item.name = id; item.address = address; item.port = port;
    _processors.append(item); emit changed();
}
bool ProcessorDirectoryModel::request_selection(const QString &id) {
    // Read the port immediately before dispatch; stale widgets cannot select a lost endpoint.
    refresh();
    for (const auto &item : _processors) if (canonical(item.id) == canonical(id)) {
        if (!item.available) return false;
        emit selection_requested(item); return true;
    }
    return false;
}
bool ProcessorDirectoryModel::connect_registry(const QString &address, quint16 port) { return _discovery && _discovery->connect_registry(address, port); }
void ProcessorDirectoryModel::disconnect_registry() { if (_discovery) _discovery->disconnect_registry(); }
}
