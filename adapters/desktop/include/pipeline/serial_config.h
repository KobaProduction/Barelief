#pragma once
#include <desktop/qt_byte_transport.h>
#include <QCommandLineParser>
#include <QJsonObject>
#include <cmath>
#include <limits>

namespace pipeline {
struct SerialBinding {
    bool configured=false;
    desktop::SerialEndpoint endpoint;
    int input_node=-1;
    QString error;
    explicit operator bool() const {return error.isEmpty();}
};
inline SerialBinding read_serial_binding(const QJsonObject &config) {
    SerialBinding result;
    const auto network=config.value("network").toObject();
    if(!network.contains("serial")) return result;
    result.configured=true;
    if(!network.value("serial").isObject()) {result.error="network.serial must be an object";return result;}
    const auto serial=network.value("serial").toObject();
    const auto port=serial.value("port_name");
    const auto baud=serial.value("baud_rate").toDouble(115200);
    const auto node=serial.value("input_node");
    if(!port.isString() || port.toString().trimmed().isEmpty() ||
       (serial.contains("baud_rate") && !serial.value("baud_rate").isDouble()) ||
       !std::isfinite(baud) || baud<=0 || baud>std::numeric_limits<qint32>::max() || std::floor(baud)!=baud ||
       !node.isDouble() || !std::isfinite(node.toDouble()) || node.toDouble()<0 ||
       node.toDouble()>=std::numeric_limits<int>::max() || std::floor(node.toDouble())!=node.toDouble()) {
        result.error="network.serial requires explicit port_name, positive integer baud_rate and NetworkInput input_node";
        return result;
    }
    result.endpoint.port_name=port.toString();result.endpoint.baud_rate=qint32(baud);
    result.input_node=node.toInt();
    return result;
}
inline void add_serial_options(QCommandLineParser &parser) {
    parser.addOption({"serial-port","Explicit UART typed-stream endpoint (requires configured NetworkInput)","name"});
    parser.addOption({"serial-baud","UART baud rate (default 115200)","baud"});
    parser.addOption({"serial-input-node","Existing NetworkInput node ID for UART stream","id"});
}
// Apply CLI overrides only to the runtime config copy. Never rewrite user JSON.
inline bool apply_serial_options(const QCommandLineParser &parser,QJsonObject &config,QString &error) {
    auto network=config.value("network").toObject();auto serial=network.value("serial").toObject();
    bool changed=false;
    for(const auto &[option,key]:{std::pair{"serial-baud","baud_rate"},std::pair{"serial-input-node","input_node"}}) {
        if(!parser.isSet(option)) continue;
        bool valid=false;const auto value=parser.value(option).toInt(&valid);
        if(!valid) {error=QStringLiteral("Invalid --%1: expected an integer").arg(option);return false;}
        serial[key]=value;changed=true;
    }
    if(parser.isSet("serial-port")) {serial["port_name"]=parser.value("serial-port");changed=true;}
    if(changed) {network["serial"]=serial;config["network"]=network;}
    const auto binding=read_serial_binding(config);error=binding.error;return bool(binding);
}
}
