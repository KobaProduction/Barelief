#pragma once
#include <QObject>
#include <QString>
#include <QVector>

namespace presentation {
struct ProcessorEndpoint {
    QString id, name, address;
    quint16 port = 0;
    bool local = false, from_registry = false, available = true, allow_mutations = true;
    QString error;
};
enum class DiscoveryState { Disconnected, Connecting, Connected, Unavailable };
struct ProcessorDiscoverySnapshot {
    DiscoveryState state = DiscoveryState::Disconnected;
    QString error;
    QVector<ProcessorEndpoint> processors;
};
class ProcessorDiscoveryPort : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual ProcessorDiscoverySnapshot snapshot() const = 0;
    virtual bool connect_registry(const QString &address, quint16 port) = 0;
    virtual void disconnect_registry() = 0;
signals:
    void changed();
};
}
