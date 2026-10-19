#include <QCoreApplication>
#include <QCommandLineParser>

#include "aggregator/aggregator_server.h"

/// @brief Структура конфигурации сервера агрегации
struct ServerConfiguration {
    uint16_t port = 8212;
    uint32_t registration_timeout_ms = aggregator_component::REGISTRATION_TIMEOUT_MS;
};

/// @brief Результат парсинга командной строки
struct CommandLineParseResult {
    enum class Status {
        Ok,
        Error,
        VersionRequested,
        HelpRequested
    };
    Status status_code = Status::Ok;
    QString error_string;
};

namespace {
struct ServerOptions {
    QCommandLineOption help;
    QCommandLineOption version;
    QCommandLineOption port;
    QCommandLineOption registration_timeout;
};

ServerOptions configure_parser(QCommandLineParser& parser,const ServerConfiguration& config) {
    parser.setApplicationDescription("Aggregator Server - central component for tracking component availability\n\nUsage: aggregator_server_app run [options]\n\nThe 'run' command starts the aggregator server.");
    auto help=parser.addHelpOption();
    auto version=parser.addVersionOption();
    parser.addPositionalArgument("run","Start the aggregator server");
    QCommandLineOption port(QStringList{"p","port"},"Port to listen on (default: 8212)","port",QString::number(config.port));
    QCommandLineOption timeout(QStringList{"t","registration-timeout"},"Registration timeout in milliseconds. If a client doesn't register within this time, connection will be closed. Use 0 to disable timeout (default: 1000)","timeout_ms",QString::number(config.registration_timeout_ms));
    parser.addOption(port);
    parser.addOption(timeout);
    return {help,version,port,timeout};
}

CommandLineParseResult parse_server_settings(QCommandLineParser& parser,const ServerOptions& options,ServerConfiguration& config) {
    using Status=CommandLineParseResult::Status;
    if(parser.isSet(options.port)) {
        bool valid=false;
        const auto value=parser.value(options.port).toUInt(&valid);
        if(!valid || value==0 || value>65535)return {Status::Error,QString("Invalid port number: %1").arg(parser.value(options.port))};
        config.port=static_cast<uint16_t>(value);
    }
    if(parser.isSet(options.registration_timeout)) {
        bool valid=false;
        const auto value=parser.value(options.registration_timeout).toUInt(&valid);
        if(!valid || value>60000)return {Status::Error,QString("Invalid registration timeout: %1 (must be between 0 and 60000 ms, 0 = no timeout)").arg(parser.value(options.registration_timeout))};
        config.registration_timeout_ms=value;
    }
    return {Status::Ok};
}

int run_server(QCoreApplication& app,const ServerConfiguration& config) {
    qInfo("Starting aggregator server...");
    qInfo("Port: %d",config.port);
    if(config.registration_timeout_ms==0)qInfo("Registration timeout: disabled (clients can connect without time limit)");
    else qInfo("Registration timeout: %d ms",config.registration_timeout_ms);
    QLoggingCategory::setFilterRules("tcp_socket.bdsp.debug=false");
    AggregatorServer server(&app);
    if(!server.start(config.port,config.registration_timeout_ms))return 1;
    const auto result=QCoreApplication::exec();
    server.stop();
    return result;
}
}

/// @brief Парсинг аргументов командной строки
/// @param parser Парсер командной строки
/// @param config Конфигурация сервера для заполнения
/// @return Результат парсинга
CommandLineParseResult parse_command_line(QCommandLineParser &parser, ServerConfiguration *config) {
    using Status = CommandLineParseResult::Status;
    const auto options=configure_parser(parser,*config);

    // Парсинг аргументов
    if (not parser.parse(QCoreApplication::arguments())) {
        return { Status::Error, parser.errorText() };
    }

    if (parser.isSet(options.version)) {
        return { Status::VersionRequested };
    }

    if (parser.isSet(options.help)) {
        return { Status::HelpRequested };
    }

    // Проверка позиционного аргумента
    const QStringList positional_args = parser.positionalArguments();
    if (positional_args.isEmpty()) {
        return { Status::Error, "Error: command 'run' is required" };
    }
    
    const QString command = positional_args.first().toLower();
    if (command != "run") {
        return { Status::Error, QString("Error: unknown command '%1'. Use 'run' to start the server").arg(command) };
    }

    return parse_server_settings(parser,options,*config);
}

/// @brief Точка входа в приложение сервера агрегации
/// @param argc Количество аргументов командной строки
/// @param argv Массив аргументов командной строки
/// @return Код завершения приложения
int32_t main(int32_t argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("aggregator-server");
    QCoreApplication::setApplicationVersion("1.0.0");

    // Инициализация конфигурации
    ServerConfiguration config;
    
    // Парсинг командной строки
    QCommandLineParser parser;
    using Status = CommandLineParseResult::Status;
    CommandLineParseResult parse_result = parse_command_line(parser, &config);
    
    switch (parse_result.status_code) {
    case Status::Ok:
        break;
    case Status::Error:
        qInfo("Error: %s", qPrintable(parse_result.error_string));
        qInfo("Use -h or --help for help");
        return 1;
    case Status::VersionRequested:
        parser.showVersion();
    case Status::HelpRequested:
        parser.showHelp();
    }

    return run_server(app,config);
}
