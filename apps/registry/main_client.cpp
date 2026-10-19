#include <QCoreApplication>
#include <QCommandLineParser>
#include <limits>

#include "aggregator/aggregator_client.h"

struct ClientConfiguration {
    QHostAddress server_address = QHostAddress::LocalHost;
    uint16_t server_port = aggregator_component::DEFAULT_PORT;
    QUuid component_uuid = QUuid::createUuid();
    ComponentType component_type = ComponentType::core;
    uint16_t component_server_port = 0;
    int32_t connection_timeout_ms = aggregator_component::CLIENT_RECONNECT_TIMEOUT_MS;
    bool infinite_reconnect = false;
    int32_t reconnect_tries = aggregator_component::RECONNECT_TRIES;
    int32_t reconnect_interval_ms = aggregator_component::CLIENT_RECONNECT_TIMEOUT_MS;
    bool wait_for_server = false;
};

struct CommandLineParseResult {
    enum class Status { Ok, Error, VersionRequested, HelpRequested };
    Status status_code = Status::Ok;
    QString error_string;
};

namespace {
using ParseStatus=CommandLineParseResult::Status;

CommandLineParseResult invalid(const QString& message) { return {ParseStatus::Error,message}; }

void configure_parser(QCommandLineParser& parser,const ClientConfiguration& config) {
    parser.setApplicationDescription(
        "Aggregator Client - connects to aggregator server for component management\n\n"
        "Usage: aggregator_client_app run [options]\n\n"
        "The 'run' command starts the client and connects to the aggregator server.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("run","Start the aggregator client and connect to server");
    parser.addOption(QCommandLineOption{QStringList{"s","server"},"Aggregator server address to connect to (default: 127.0.0.1)",
                      "address",config.server_address.toString()});
    parser.addOption(QCommandLineOption{QStringList{"p","port"},"Aggregator server port to connect to (default: 8212)",
                      "port",QString::number(config.server_port)});
    parser.addOption(QCommandLineOption{QStringList{"u","uuid"},"Component UUID (auto-generated if not specified)","uuid"});
    parser.addOption(QCommandLineOption{QStringList{"t","type"},"Component type: core, data_source, data_processor, data_sink (default: core)",
                      "type","core"});
    parser.addOption(QCommandLineOption{QStringList{"c","component-port"},
                      "Component server port for data connections (0 for dynamic allocation, default: 0)","port","0"});
    parser.addOption(QCommandLineOption{QStringList{"infinite-reconnect"},"Enable infinite reconnection attempts (default: disabled)"});
    parser.addOption(QCommandLineOption{QStringList{"reconnect-tries"},
                      "Number of reconnection attempts when infinite reconnect is disabled (default: 3)",
                      "tries",QString::number(config.reconnect_tries)});
    parser.addOption(QCommandLineOption{QStringList{"reconnect-interval"},
                      "Interval between reconnection attempts in milliseconds (default: 200)",
                      "interval_ms",QString::number(config.reconnect_interval_ms)});
    parser.addOption(QCommandLineOption{QStringList{"w","wait-for-server"},
                      "Wait for server to become available before first connection (default: disabled)"});
}

CommandLineParseResult check_command(const QCommandLineParser& parser) {
    const auto arguments=parser.positionalArguments();
    if(arguments.isEmpty())return invalid("Error: command 'run' is required");
    const auto command=arguments.first().toLower();
    if(command!="run")
        return invalid(QString("Error: unknown command '%1'. Use 'run' to start the client").arg(command));
    return {};
}

CommandLineParseResult read_server_options(const QCommandLineParser& parser,ClientConfiguration& config) {
    if(parser.isSet("server")) {
        const auto value=parser.value("server");
        config.server_address=QHostAddress(value);
        if(config.server_address.isNull())return invalid(QString("Invalid server address: %1").arg(value));
    }
    if(parser.isSet("port")) {
        bool ok=false;
        const auto value=parser.value("port");
        const auto port=value.toUInt(&ok);
        if(!ok || port==0 || port>std::numeric_limits<uint16_t>::max())
            return invalid(QString("Invalid server port: %1").arg(value));
        config.server_port=static_cast<uint16_t>(port);
    }
    return {};
}

CommandLineParseResult read_component_uuid(const QCommandLineParser& parser,ClientConfiguration& config) {
    if(parser.isSet("uuid")) {
        const auto value=parser.value("uuid");
        config.component_uuid=QUuid::fromString(value);
        if(config.component_uuid.isNull())return invalid(QString("Invalid UUID: %1").arg(value));
    }
    return {};
}
CommandLineParseResult read_component_type(const QCommandLineParser& parser,ClientConfiguration& config) {
    if(parser.isSet("type")) {
        const auto value=parser.value("type").toLower();
        if(value=="core")config.component_type=ComponentType::core;
        else if(value=="data_source")config.component_type=ComponentType::data_source;
        else if(value=="data_processor")config.component_type=ComponentType::data_processor;
        else if(value=="data_sink")config.component_type=ComponentType::data_sink;
        else return invalid(QString("Invalid component type: %1").arg(value));
    }
    return {};
}
CommandLineParseResult read_component_port(const QCommandLineParser& parser,ClientConfiguration& config) {
    if(parser.isSet("component-port")) {
        bool ok=false;
        const auto value=parser.value("component-port");
        const auto port=value.toUInt(&ok);
        if(!ok || port>std::numeric_limits<uint16_t>::max())
            return invalid(QString("Invalid component port: %1").arg(value));
        config.component_server_port=static_cast<uint16_t>(port);
    }
    return {};
}

CommandLineParseResult read_reconnect_tries(const QCommandLineParser& parser,ClientConfiguration& config) {
    if(parser.isSet("reconnect-tries")) {
        bool ok=false;
        const auto value=parser.value("reconnect-tries");
        const auto tries=value.toInt(&ok);
        if(!ok || tries<0 || tries>100)
            return invalid(QString("Invalid reconnect tries: %1 (must be between 0 and 100)").arg(value));
        config.reconnect_tries=tries;
    }
    return {};
}
CommandLineParseResult read_reconnect_interval(const QCommandLineParser& parser,ClientConfiguration& config) {
    if(parser.isSet("reconnect-interval")) {
        bool ok=false;
        const auto value=parser.value("reconnect-interval");
        const auto interval=value.toInt(&ok);
        if(!ok || interval<100 || interval>60000)
            return invalid(QString("Invalid reconnect interval: %1 (must be between 100 and 60000 ms)").arg(value));
        config.reconnect_interval_ms=interval;
    }
    return {};
}

CommandLineParseResult read_component_options(const QCommandLineParser& parser,ClientConfiguration& config) {
    auto result=read_component_uuid(parser,config);
    if(result.status_code!=ParseStatus::Ok)return result;
    result=read_component_type(parser,config);
    return result.status_code==ParseStatus::Ok?read_component_port(parser,config):result;
}
CommandLineParseResult read_reconnect_options(const QCommandLineParser& parser,ClientConfiguration& config) {
    config.infinite_reconnect=parser.isSet("infinite-reconnect");
    auto result=read_reconnect_tries(parser,config);
    if(result.status_code!=ParseStatus::Ok)return result;
    result=read_reconnect_interval(parser,config);
    if(result.status_code!=ParseStatus::Ok)return result;
    config.wait_for_server=parser.isSet("wait-for-server");
    return {};
}

CommandLineParseResult parse_command_line(QCommandLineParser& parser,ClientConfiguration& config) {
    configure_parser(parser,config);
    if(!parser.parse(QCoreApplication::arguments()))return invalid(parser.errorText());
    if(parser.isSet("version"))return {ParseStatus::VersionRequested};
    if(parser.isSet("help"))return {ParseStatus::HelpRequested};
    const auto command=check_command(parser);
    if(command.status_code!=ParseStatus::Ok)return command;
    const auto server=read_server_options(parser,config);
    if(server.status_code!=ParseStatus::Ok)return server;
    const auto component=read_component_options(parser,config);
    return component.status_code==ParseStatus::Ok?read_reconnect_options(parser,config):component;
}

void log_startup(const ClientConfiguration& config) {
    qInfo("Starting aggregator client...");
    qInfo("Server: %s:%d",qPrintable(config.server_address.toString()),config.server_port);
    qInfo("Component UUID: %s",qPrintable(config.component_uuid.toString()));
    qInfo("Component type: %d",static_cast<int32_t>(config.component_type));
    qInfo("Component port: %d",config.component_server_port);
    qInfo("Connection timeout: %d ms",config.connection_timeout_ms);
    if(config.infinite_reconnect)
        qInfo("Reconnection: infinite attempts, interval %d ms",config.reconnect_interval_ms);
    else
        qInfo("Reconnection: %d attempts, interval %d ms",config.reconnect_tries,config.reconnect_interval_ms);
    qInfo("Wait for server: %s",config.wait_for_server
          ? "enabled (will retry until first successful connection)" : "disabled");
}

bool connect_client(AggregatorClient& client,const ClientConfiguration& config) {
    client.set_reconnect_mode(config.infinite_reconnect,config.reconnect_tries,config.reconnect_interval_ms);
    if(config.wait_for_server) {
        qInfo("Waiting for aggregator server %s:%d to become available...",
              qPrintable(config.server_address.toString()),config.server_port);
        client.connect_to_aggregator_server_with_waiting(
            config.server_address,config.server_port,config.connection_timeout_ms);
        return true;
    }
    qInfo("Connecting to aggregator server %s:%d...",
          qPrintable(config.server_address.toString()),config.server_port);
    if(client.connect_to_aggregator_server(
           config.server_address,config.server_port,config.connection_timeout_ms))return true;
    qCritical("Failed to connect to aggregator server");
    return false;
}

int run_client(QCoreApplication& app,const ClientConfiguration& config) {
    log_startup(config);
    QLoggingCategory::setFilterRules("tcp_socket.bdsp.debug=false");
    AggregatorClientConfig client_config;
    client_config.uuid=config.component_uuid;
    client_config.type=config.component_type;
    client_config.component_server_port=config.component_server_port;
    AggregatorClient client(client_config);
    QObject::connect(&client,&AggregatorClient::status_changed,&app,[&app](core::RuntimeState state,const QString& error) {
        if(state==core::RuntimeState::Failed) {
            qCritical().noquote()<<error;
            QTimer::singleShot(0,&app,[&app]{app.exit(1);});
        }
    });
    if(!connect_client(client,config))return 1;
    return QCoreApplication::exec();
}
}

int32_t main(int32_t argc,char* argv[]) {
    QCoreApplication app(argc,argv);
    QCoreApplication::setApplicationName("aggregator-client");
    QCoreApplication::setApplicationVersion("1.0.0");
    ClientConfiguration config;
    QCommandLineParser parser;
    const auto result=parse_command_line(parser,config);
    switch(result.status_code) {
    case ParseStatus::Ok:break;
    case ParseStatus::Error:
        qInfo("Error: %s",qPrintable(result.error_string));
        qInfo("Use -h or --help for help");
        return 1;
    case ParseStatus::VersionRequested:parser.showVersion();return 0;
    case ParseStatus::HelpRequested:parser.showHelp();return 0;
    }
    return run_client(app,config);
}
