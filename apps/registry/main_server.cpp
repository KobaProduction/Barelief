#include <QCoreApplication>

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

/// @brief Парсинг аргументов командной строки
/// @param parser Парсер командной строки
/// @param config Конфигурация сервера для заполнения
/// @return Результат парсинга
CommandLineParseResult parse_command_line(QCommandLineParser &parser, ServerConfiguration *config) {
    using Status = CommandLineParseResult::Status;

    // Настройка парсера
    parser.setApplicationDescription("Aggregator Server - central component for tracking component availability\n\nUsage: aggregator_server_app run [options]\n\nThe 'run' command starts the aggregator server.");
    
    // Добавление стандартных опций
    const QCommandLineOption help_option = parser.addHelpOption();
    const QCommandLineOption version_option = parser.addVersionOption();
    
    // Позиционный аргумент для команды запуска
    parser.addPositionalArgument("run", "Start the aggregator server");
    
    // Опция для порта сервера
    const QCommandLineOption port_option(
        QStringList() << "p" << "port",
        "Port to listen on (default: 8212)",
        "port",
        QString::number(config->port)
    );
    parser.addOption(port_option);
    
    // Опция для таймаута регистрации
    const QCommandLineOption registration_timeout_option(
        QStringList() << "t" << "registration-timeout",
        "Registration timeout in milliseconds. If a client doesn't register within this time, connection will be closed. Use 0 to disable timeout (default: 1000)",
        "timeout_ms",
        QString::number(config->registration_timeout_ms)
    );
    parser.addOption(registration_timeout_option);

    // Парсинг аргументов
    if (not parser.parse(QCoreApplication::arguments())) {
        return { Status::Error, parser.errorText() };
    }

    if (parser.isSet(version_option)) {
        return { Status::VersionRequested };
    }

    if (parser.isSet(help_option)) {
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

    // Обработка порта сервера
    if (parser.isSet(port_option)) {
        bool ok = false;
        const uint32_t parsed_port = parser.value(port_option).toUInt(&ok);
        if (not ok or parsed_port == 0 or parsed_port > 65535) {
            return { Status::Error, QString("Invalid port number: %1").arg(parser.value(port_option)) };
        }
        config->port = static_cast<uint16_t>(parsed_port);
    }

    // Обработка таймаута регистрации
    if (parser.isSet(registration_timeout_option)) {
        bool ok = false;
        const uint32_t parsed_timeout = parser.value(registration_timeout_option).toUInt(&ok);
        if (not ok or parsed_timeout > 60000) {
            return { Status::Error, QString("Invalid registration timeout: %1 (must be between 0 and 60000 ms, 0 = no timeout)").arg(parser.value(registration_timeout_option)) };
        }
        config->registration_timeout_ms = static_cast<uint32_t>(parsed_timeout);
    }

    return { Status::Ok };
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

    // Логирование конфигурации
    qInfo("Starting aggregator server...");
    qInfo("Port: %d", config.port);
    if (config.registration_timeout_ms == 0) {
        qInfo("Registration timeout: disabled (clients can connect without time limit)");
    } else {
        qInfo("Registration timeout: %d ms", config.registration_timeout_ms);
    }

    // Отключаем логирование tcp_socket.bdsp для чистого вывода
    QLoggingCategory::setFilterRules("tcp_socket.bdsp.debug=false");

    // Создание и запуск сервера агрегации
    AggregatorServer server(&app);
    if (not server.start(config.port, config.registration_timeout_ms)) {
        return 1;
    }
    int32_t result = QCoreApplication::exec();
    server.stop();
    return result;
}
