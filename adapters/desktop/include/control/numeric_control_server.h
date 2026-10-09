#pragma once
#include <desktop/qt_byte_transport.h>
#include <QObject>
#include <QHostAddress>
#include <memory>
namespace pipeline {
class NumericEngine;
// One application service/executor, independent bounded CONTROL/DATA peers.
class NumericControlServer final : public QObject {
public:
    explicit NumericControlServer(NumericEngine&,QObject* parent=nullptr);
    ~NumericControlServer() override;
    bool listen(QHostAddress address,quint16 port,bool allow_mutations=true);
    bool open_serial(const desktop::SerialEndpoint&,bool allow_mutations=true);
    void close();
    quint16 port() const;
    QString error_string() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
