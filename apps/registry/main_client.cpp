#include <QCoreApplication>

#include "aggregator/aggregator_client.h"

/// @brief Структура конфигурации клиента агрегации
struct ClientConfiguration {
    QHostAddress server_address = QHostAddress::LocalHost;
    uint16_t server_port = aggregator_component::DEFAULT_PORT;
    QUuid component_uuid = QUuid::createUuid();
    ComponentType component_type = ComponentType::core;
    uint16_t component_server_port = 0;
    int32_t connection_timeout_ms = aggregator_component::CLIENT_RECONNECT_TIMEOUT_MS;

    // Параметры переподключения
    bool infinite_reconnect = false;
    int32_t reconnect_tries = aggregator_component::RECONNECT_TRIES;
    int32_t reconnect_interval_ms = aggregator_component::CLIENT_RECONNECT_TIMEOUT_MS;

    // Параметр ожидания сервера
    bool wait_for_server = false;
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
/// @param config Конфигурация клиента для заполнения
/// @return Результат парсинга
CommandLineParseResult parse_command_line(QCommandLineParser &parser, ClientConfiguration *config) {
    using Status = CommandLineParseResult::Status;

    // Настройка парсера
    parser.setApplicationDescription(
            "Aggregator Client - connects to aggregator server for component management\n\nUsage: aggregator_client_app run [options]\n\nThe 'run' command starts the client and connects to the aggregator server.");

    // Добавление стандартных опций
    const QCommandLineOption help_option = parser.addHelpOption();
    const QCommandLineOption version_option = parser.addVersionOption();

    // Позиционный аргумент для команды запуска
    parser.addPositionalArgument("run", "Start the aggregator client and connect to server");

    // Опция для адреса сервера
    const QCommandLineOption server_address_option(
            QStringList() << "s" << "server",
            "Aggregator server address to connect to (default: 127.0.0.1)",
            "address",
            config->server_address.toString()
    );
    parser.addOption(server_address_option);

    // Опция для порта сервера
    const QCommandLineOption server_port_option(
            QStringList() << "p" << "port",
            "Aggregator server port to connect to (default: 8212)",
            "port",
            QString::number(config->server_port)
    );
    parser.addOption(server_port_option);

    // Опция для UUID компонента
    const QCommandLineOption uuid_option(
            QStringList() << "u" << "uuid",
            "Component UUID (auto-generated if not specified)",
            "uuid"
    );
    parser.addOption(uuid_option);

    // Опция для типа компонента
    const QCommandLineOption type_option(
            QStringList() << "t" << "type",
            "Component type: core, data_source, data_processor, data_sink (default: core)",
            "type",
            "core"
    );
    parser.addOption(type_option);

    // Опция для порта компонента (теперь используем -c)
    const QCommandLineOption component_port_option(
            QStringList() << "c" << "component-port",
            "Component server port for data connections (0 for dynamic allocation, default: 0)",
            "port",
            "0"
    );
    parser.addOption(component_port_option);

    // Опция для бесконечного переподключения
    const QCommandLineOption infinite_reconnect_option(
            QStringList() << "infinite-reconnect",
            "Enable infinite reconnection attempts (default: disabled)"
    );
    parser.addOption(infinite_reconnect_option);

    // Опция для количества попыток переподключения
    const QCommandLineOption reconnect_tries_option(
            QStringList() << "reconnect-tries",
            "Number of reconnection attempts when infinite reconnect is disabled (default: 3)",
            "tries",
            QString::number(config->reconnect_tries)
    );
    parser.addOption(reconnect_tries_option);

    // Опция для интервала между попытками переподключения
    const QCommandLineOption reconnect_interval_option(
            QStringList() << "reconnect-interval",
            "Interval between reconnection attempts in milliseconds (default: 200)",
            "interval_ms",
            QString::number(config->reconnect_interval_ms)
    );
    parser.addOption(reconnect_interval_option);

    // Опция для ожидания сервера
    const QCommandLineOption wait_for_server_option(
            QStringList() << "w" << "wait-for-server",
            "Wait for server to become available before first connection (default: disabled)"
    );
    parser.addOption(wait_for_server_option);

    // Парсинг аргументов
    if (not parser.parse(QCoreApplication::arguments())) {
        return {Status::Error, parser.errorText()};
    }

    if (parser.isSet(version_option)) {
        return {Status::VersionRequested};
    }

    if (parser.isSet(help_option)) {
        return {Status::HelpRequested};
    }

    // Проверка позиционного аргумента
    const QStringList positional_args = parser.positionalArguments();
    if (positional_args.isEmpty()) {
        return {Status::Error, "Error: command 'run' is required"};
    }

    const QString command = positional_args.first().toLower();
    if (command != "run") {
        return {Status::Error, QString("Error: unknown command '%1'. Use 'run' to start the client").arg(command)};
    }

    // Обработка адреса сервера
    if (parser.isSet(server_address_option)) {
        const QString address_str = parser.value(server_address_option);
        config->server_address = QHostAddress(address_str);
        if (config->server_address.isNull()) {
            return {Status::Error, QString("Invalid server address: %1").arg(address_str)};
        }
    }

    // Обработка порта сервера
    if (parser.isSet(server_port_option)) {
        bool ok = false;
        const uint16_t port = static_cast<uint16_t>(parser.value(server_port_option).toUInt(&ok));
        if (not ok or port == 0) {
            return {Status::Error, QString("Invalid server port: %1").arg(parser.value(server_port_option))};
        }
        config->server_port = port;
    }

    // Обработка UUID компонента
    if (parser.isSet(uuid_option)) {
        const QString uuid_str = parser.value(uuid_option);
        config->component_uuid = QUuid::fromString(uuid_str);
        if (config->component_uuid.isNull()) {
            return {Status::Error, QString("Invalid UUID: %1").arg(uuid_str)};
        }
    }

    // Обработка типа компонента
    if (parser.isSet(type_option)) {
        const QString type_str = parser.value(type_option).toLower();
        if (type_str == "core") {
            config->component_type = ComponentType::core;
        } else if (type_str == "data_source") {
            config->component_type = ComponentType::data_source;
        } else if (type_str == "data_processor") {
            config->component_type = ComponentType::data_processor;
        } else if (type_str == "data_sink") {
            config->component_type = ComponentType::data_sink;
        } else {
            return {Status::Error, QString("Invalid component type: %1").arg(type_str)};
        }
    }

    // Обработка порта компонента
    if (parser.isSet(component_port_option)) {
        bool ok = false;
        const uint16_t port = static_cast<uint16_t>(parser.value(component_port_option).toUInt(&ok));
        if (not ok) {
            return {Status::Error, QString("Invalid component port: %1").arg(parser.value(component_port_option))};
        }
        config->component_server_port = port;
    }

    // Обработка бесконечного переподключения
    config->infinite_reconnect = parser.isSet(infinite_reconnect_option);

    // Обработка количества попыток переподключения
    if (parser.isSet(reconnect_tries_option)) {
        bool ok = false;
        const int32_t tries = parser.value(reconnect_tries_option).toInt(&ok);
        if (not ok or tries < 0 or tries > 100) {
            return {Status::Error, QString("Invalid reconnect tries: %1 (must be between 0 and 100)").arg(
                    parser.value(reconnect_tries_option))};
        }
        config->reconnect_tries = tries;
    }

    // Обработка интервала переподключения
    if (parser.isSet(reconnect_interval_option)) {
        bool ok = false;
        const int32_t interval = parser.value(reconnect_interval_option).toInt(&ok);
        if (not ok or interval < 100 or interval > 60000) {
            return {Status::Error, QString("Invalid reconnect interval: %1 (must be between 100 and 60000 ms)").arg(
                    parser.value(reconnect_interval_option))};
        }
        config->reconnect_interval_ms = interval;
    }

    // Обработка ожидания сервера
    config->wait_for_server = parser.isSet(wait_for_server_option);

    return {Status::Ok};
}

/// @brief Точка входа в приложение клиента агрегации
/// @param argc Количество аргументов командной строки
/// @param argv Массив аргументов командной строки
/// @return Код завершения приложения
int32_t main(int32_t argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("aggregator-client");
    QCoreApplication::setApplicationVersion("1.0.0");

    // Инициализация конфигурации
    ClientConfiguration config;

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
            return 0;
        case Status::HelpRequested:
            parser.showHelp();
            return 0;
    }

    // Логирование конфигурации
    qInfo("Starting aggregator client...");
    qInfo("Server: %s:%d", qPrintable(config.server_address.toString()), config.server_port);
    qInfo("Component UUID: %s", qPrintable(config.component_uuid.toString()));
    qInfo("Component type: %d", static_cast<int32_t>(config.component_type));
    qInfo("Component port: %d", config.component_server_port);
    qInfo("Connection timeout: %d ms", config.connection_timeout_ms);

    // Логирование параметров переподключения
    if (config.infinite_reconnect) {
        qInfo("Reconnection: infinite attempts, interval %d ms", config.reconnect_interval_ms);
    } else {
        qInfo("Reconnection: %d attempts, interval %d ms", config.reconnect_tries, config.reconnect_interval_ms);
    }

    if (config.wait_for_server) {
        qInfo("Wait for server: enabled (will retry until first successful connection)");
    } else {
        qInfo("Wait for server: disabled");
    }

    // Отключаем логирование tcp_socket.bdsp для чистого вывода
    QLoggingCategory::setFilterRules("tcp_socket.bdsp.debug=false");

    // Создание конфигурации клиента
    AggregatorClientConfig client_config;
    client_config.uuid = config.component_uuid;
    client_config.type = config.component_type;
    client_config.component_server_port = config.component_server_port;

    // Создание клиента агрегации
    AggregatorClient client(client_config);
    QObject::connect(&client,&AggregatorClient::status_changed,&app,[&app](core::RuntimeState state,const QString &error) {
        if (state==core::RuntimeState::Failed) { qCritical().noquote()<<error; QTimer::singleShot(0,&app,[&app]{app.exit(1);}); }
    });

    // Настройка параметров переподключения
    client.set_reconnect_mode(config.infinite_reconnect, config.reconnect_tries, config.reconnect_interval_ms);

    // Логика подключения с учетом ожидания сервера
    if (config.wait_for_server) {
        qInfo("Waiting for aggregator server %s:%d to become available...",
              qPrintable(config.server_address.toString()), config.server_port);
        // Попытки подключения до первого успешного соединения
        client.connect_to_aggregator_server_with_waiting(config.server_address, config.server_port,
                                                    config.connection_timeout_ms);
    } else {
        // Обычное подключение без ожидания
        qInfo("Connecting to aggregator server %s:%d...",
              qPrintable(config.server_address.toString()), config.server_port);

        if (not client.connect_to_aggregator_server(config.server_address, config.server_port,
                                                    config.connection_timeout_ms)) {
            qCritical("Failed to connect to aggregator server");
            return 1;
        }
    }

    return QCoreApplication::exec();
}