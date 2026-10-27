#include <core_app/core_application.h>
#include <pipeline/serial_config.h>
#include <config/config_document.h>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QDebug>
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void command_line_exit(FILE* stream,const QString& message,int status) {
    const auto bytes=message.toUtf8();std::fwrite(bytes.constData(),1,static_cast<std::size_t>(bytes.size()),stream);
    std::fflush(stream);std::exit(status);
}
QString desktop_gui_config_path(const QString& explicit_path) {
    if(!explicit_path.isEmpty())return configuration::active_path(explicit_path);
    const auto directory=QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if(directory.isEmpty())return configuration::active_path();
    const auto persistent=QDir(directory).filePath("config.json");
    const auto legacy=configuration::active_path();
    // A first run may have neither a config directory nor a legacy file.
    // Create the user-writable location before either migration or save.
    if(!QDir().mkpath(directory))
        qWarning().noquote()<<"Could not create Barelief preferences directory:"<<directory;
    // Preserve portable builds on the first launch without replacing an
    // existing per-user configuration or writing to Program Files.
    if(!QFileInfo::exists(persistent) && QFileInfo::exists(legacy) && !QFile::copy(legacy,persistent))
        qWarning().noquote()<<"Could not import previous Barelief configuration:"<<legacy;
    return QFileInfo(persistent).absoluteFilePath();
}
}
void CoreApplication::_parse_command_line() {
    QCommandLineParser parser;parser.setApplicationDescription("Barelief | Signal Processing & Visualization");
    const auto help=parser.addHelpOption();const auto version=parser.addVersionOption();
    parser.addOptions({QCommandLineOption({"c","config"},"Configuration file path","file"),
        QCommandLineOption("remote-host","Connect to a separate console engine host","host"),
        QCommandLineOption("remote-port","Console engine TCP port","port","8213")});
    pipeline::add_serial_options(parser);
    // GUI process/showHelp would open a Windows QMessageBox; CLI exits stay terminal-facing.
    if(!parser.parse(arguments()))command_line_exit(stderr,parser.errorText()+"\n",2);
    if(parser.isSet(help))command_line_exit(stdout,parser.helpText(),0);
    if(parser.isSet(version))command_line_exit(stdout,applicationName()+" "+applicationVersion()+"\n",0);
    _read_command_line(parser);
}
void CoreApplication::_read_command_line(const QCommandLineParser& parser) {
    _config_manager.setConfigPath(desktop_gui_config_path(parser.value("config")));
    _remote_host=parser.value("remote-host");
    if(!_remote_host.isEmpty() && (parser.isSet("serial-port") || parser.isSet("serial-baud") || parser.isSet("serial-input-node")))command_line_exit(stderr,"Serial input belongs to the local graph; choose either --remote-host or Serial options.\n",2);
    for(const auto& option:{"serial-port","serial-baud","serial-input-node"})if(parser.isSet(option))_serial_arguments<<QStringLiteral("--")+option<<parser.value(option);
    bool ok=false;const auto value=parser.value("remote-port").toUInt(&ok);
    if(!ok || !value || value>65535)command_line_exit(stderr,"Invalid --remote-port: expected an integer from 1 to 65535.\n",2);
    _remote_port=static_cast<quint16>(value);
}
