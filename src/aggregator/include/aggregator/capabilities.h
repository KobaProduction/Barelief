#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
namespace aggregator_capabilities {
constexpr quint8 PacketId = 16;
inline bool valid(const QJsonObject &o) {
    if (o.value("version").toInt() != 2 || QUuid(o.value("uuid").toString()).isNull())
        return false;
    const int port = o.value("stream_port").toInt();
    if (port < 0 || port > 65535)
        return false;
    const auto streams = o.value("streams").toArray();
    if (streams.size() > 128)
        return false;
    QSet<QString> ids;
    const QStringList types = {"audio", "window", "windowed", "stereo-spectrum", "spectrum", "mel", "history"};
    for (auto v : streams) {
        auto s = v.toObject();
        auto id = s.value("id").toString();
        if (id.isEmpty() || id.toUtf8().size() > 256 || ids.contains(id) ||
            !types.contains(s.value("data_type").toString()))
            return false;
        ids.insert(id);
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact).size() <= 32768;
}
inline QByteArray encode(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
inline bool decode(const QByteArray &b, QJsonObject &o) {
    if (b.size() > 32768)
        return false;
    auto d = QJsonDocument::fromJson(b);
    if (!d.isObject() || !valid(d.object()))
        return false;
    o = d.object();
    return true;
}
} // namespace aggregator_capabilities
