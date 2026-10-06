# Модуль Aggregator

## Описание

Центральный компонент системы для отслеживания доступности и состояния всех компонентов архитектуры конвейера обработки данных. Использует протокол BDSP для надежной передачи пакетов через TCP. Включает сервер агрегации, клиент с поддержкой событийной системы и утилиты для работы с компонентами.

## Компоненты

### 1. Библиотека aggregator-library

Статическая библиотека, содержащая:
- Типы и структуры данных для работы с агрегатором (`include/aggregator/abstract/types.h`)
- Реализацию сервера агрегации (`AggregatorServer`)
- Клиент агрегации с поддержкой событийной системы (`AggregatorClient`)
- Утилиты для работы с компонентами (форматирование, генерация токенов)

### 2. Исполняемый файл aggregator_server_app

Сервер агрегации, который:
- Слушает на статическом порту 8212 (или пользовательском через `-p`)
- Регистрирует компоненты системы с проверкой уникальности UUID
- Восстанавливает сессии при переподключении через CRC16 токены
- Отслеживает доступность через TCP-соединение (разрыв = недоступность)
- Управляет состояниями компонентов (unknown, offline, disconnected, registered, inactive, idle, active)
- Автоматически переводит компоненты в состояние `offline` при отключении на 60 секунд
- Публикует события изменения состояния компонентов через BDSP
- Отправляет информацию о новых компонентах всем подключенным клиентам

**Использование:**
```bash
aggregator_server_app -s                    # Запуск на порту 8212
aggregator_server_app --start -p 9000       # Запуск на порту 9000
aggregator_server_app -h                    # Справка
```

### 3. Исполняемый файл aggregator_client_app

Тестовое приложение для проверки работы сервера:
- Генерирует или берет из конфигурации уникальный UUID v4
- Подключается к серверу агрегации
- Регистрируется как компонент с типом, портом и токеном сессии
- Получает уведомления об изменении состояния других компонентов
- Автоматически переподключается при разрыве соединения с настраиваемым количеством попыток

**Использование:**
```bash
aggregator_client_app                       # Подключение к localhost:8212
aggregator_client_app -p 9000               # Подключение к localhost:9000
aggregator_client_app -a 192.168.1.1 -p 8212  # Подключение к другому хосту
aggregator_client_app -h                    # Справка
```

## Архитектура

### Типы данных (`include/aggregator/abstract/types.h`)

#### Типы компонентов (`ComponentType`)
```cpp
enum class ComponentType : uint8_t {
    unknown = 0,
    data_source = 1,      // Источник данных (захват аудио, генератор)
    data_processor = 2,   // Обработчик данных (FFT, нормализация)
    data_sink = 3,        // Получатель данных (визуализация, запись)
    core = 4              // Ядро приложения (управление)
};
```

#### Состояния компонентов (`ComponentState`)
Жизненный цикл состояний:
```
unknown -> registered -> inactive -> idle -> active
                    \-> disconnected -> offline (при длительном отключении)
```

- `unknown` (0) — неизвестное состояние (заглушка на случай ошибок)
- `offline` (1) — компонент отключился и длительное время не восстанавливает подключение (более минуты)
- `disconnected` (2) — компонент отключен, если простой достигнет минуты - переходит в offline
- `registered` (3) — клиент зарегистрирован, но пока ничего не делает
- `inactive` (4) — клиент сообщил о себе данные и возможности, но пока не готов участвовать в цепочках данных
- `idle` (5) — готов к работе, но активных задач нет, простаивает
- `active` (6) — занят работой

#### Типы пакетов (`PacketType`)
- `incorrect_packet_id` (0) — неверный идентификатор пакета
- `error` (1) — пакет с ошибками
- `component_registration` (2) — регистрация компонента
- `component_info_update` (3) — полная информация о компоненте для всех клиентов
- `component_state_change` (4) — изменение состояния компонента

#### Структуры данных

**`registration_request_packet_t`** — запрос регистрации компонента
```cpp
struct registration_request_packet_t {
    uuid_bytes_t uuid;                          // UUID компонента
    ComponentType component_type;         // Тип компонента
    uint16_t component_server_port;       // Порт компонента
    session_token_t session_token;        // CRC16 токен сессии
};
```

**`component_update_full_packet_t`** — полная информация о компоненте
```cpp
struct component_update_full_packet_t {
    uuid_bytes_t uuid;                          // UUID компонента
    ComponentType type;                   // Тип компонента
    QIPv6Address address;                 // IPv6 адрес компонента
    uint16_t component_server_port;       // Порт компонента
    ComponentState state;                 // Состояние компонента
};
```

**`component_update_state_packet_t`** — изменение состояния
```cpp
struct component_update_state_packet_t {
    uuid_bytes_t uuid;                          // UUID компонента
    ComponentState new_state;             // Новое состояние
};
```

**`component_info_t`** — информация о компоненте (для клиента)
```cpp
struct component_info_t {
    ComponentType type;
    ComponentState state;
    uint16_t component_server_port;
    QHostAddress address;
};
```

**`registered_component_t`** — зарегистрированный компонент (на сервере)
```cpp
struct registered_component_t {
    TcpBDSPSocket *socket;                // TCP сокет компонента
    session_token_t session_token;        // CRC16 токен сессии
    component_info_t info;                // Информация о компоненте
};
```

### Сервер агрегатора (`AggregatorServer`)

Основной класс сервера агрегации:

```cpp
class AggregatorServer final : public TcpBDSPServer
```

**Ключевые методы:**
- `start(port, registration_timeout_ms)` — запуск сервера на указанном порту
- `stop()` — остановка сервера

**Функции:**
- Регистрация компонентов с проверкой уникальности UUID
- Восстановление сессий при переподключении через CRC16 токены
- Управление состояниями компонентов
- Автоматический перевод в состояние `offline` при отключении на 60 секунд
- Публикация событий изменения состояния компонентов через BDSP
- Отправка информации о новых компонентах всем подключенным клиентам
- Каждое TCP соединение обрабатывается отдельным `TcpBDSPSocket` (из `core`)

**Внутренние структуры:**
- `_sockets_uuid_map` — маппинг сокетов на UUID компонентов
- `_registered_components_map` — маппинг UUID на информацию о компонентах

### Клиент агрегации (`AggregatorClient`)

Основной класс для подключения к серверу агрегации:

```cpp
class AggregatorClient final : public QObject
```

**Конфигурация:**
```cpp
struct AggregatorClientConfig {
    QUuid uuid;                           // UUID компонента
    ComponentType type;                   // Тип компонента
    uint16_t component_server_port;       // Порт компонента
};
```

**Ключевые методы:**
- `connect_to_aggregator_server(address, port, timeout_ms)` — подключение к серверу
- `connect_to_aggregator_server_with_waiting(address, port, timeout_ms)` — бесконечное подключение до установления первого соединения
- `is_connected()` — проверка подключения к серверу
- `is_registered()` — проверка регистрации как компонента
- `set_reconnect_mode(infinite_mode, tries, reconnect_interval_ms)` — настройка режима переподключения

**Сигналы:**
- `on_client_disconnected()` — клиент отключился от сервера
- `on_client_connected()` — клиент подключился к серверу
- `on_client_registered()` — клиент зарегистрирован как компонент
- `on_update_component_state(uuid, new_state)` — изменение состояния компонента
- `on_update_component_info(uuid, info)` — обновление информации о компоненте

**Ключевые особенности:**
- Генерирует Qt сигналы для всех событий агрегатора
- Кэширует состояния компонентов для отслеживания изменений
- Автоматическое переподключение при разрыве соединения
- Поддержка бесконечного режима переподключения

## Конфигурация

Все параметры определены в `include/aggregator/abstract/types.h`:

```cpp
namespace aggregator_component {
    static constexpr uint16_t DEFAULT_PORT = 8212;
    static constexpr uint16_t REGISTRATION_TIMEOUT_MS = 1000;
    static constexpr int32_t CLIENT_RECONNECT_TIMEOUT_MS = 200;
    static constexpr int32_t RECONNECT_TRIES = 3;
    static constexpr uint32_t COMPONENT_OFFLINE_TIMEOUT_MS = 60000; // 1 минута
}
```

## Использование

### Базовое использование клиента

```cpp
// Создание конфигурации клиента
AggregatorClientConfig config;
config.uuid = QUuid::createUuid();
config.type = ComponentType::data_processor;
config.component_server_port = 8080;

// Создание клиента
AggregatorClient* client = new AggregatorClient(config);

// Подключение к событиям
connect(client, &AggregatorClient::on_client_registered,
        this, &MyClass::on_client_registered);
connect(client, &AggregatorClient::on_update_component_state,
        this, &MyClass::on_update_component_state);

// Подключение к серверу
client->connect_to_aggregator_server();
```

### Подключение с ожиданием

```cpp
// Бесконечное подключение до установления первого соединения
client->connect_to_aggregator_server_with_waiting(
    QHostAddress::LocalHost,
    aggregator_component::DEFAULT_PORT
);
```

### Настройка переподключения

```cpp
// Бесконечный режим переподключения
client->set_reconnect_mode(true);

// Ограниченное количество попыток
client->set_reconnect_mode(false, 5, 500);
```

### Проверка состояния

```cpp
if (client->is_connected()) {
    qInfo("Connected to aggregator server");
}

if (client->is_registered()) {
    qInfo("Registered as component");
}
```

## Протокол обмена

Все пакеты передаются через BDSP (Binary Data Stream Protocol) с кодированием COBS и Zero Padding Elimination.

### Регистрация компонента

1. Клиент отправляет `registration_request_packet_t` с типом `component_registration`
2. Сервер проверяет уникальность UUID и токена сессии
3. Сервер отправляет `component_update_full_packet_t` всем клиентам с информацией о новом компоненте
4. Клиент получает сигнал `on_client_registered()`

### Изменение состояния

1. Компонент изменяет свое состояние
2. Сервер отправляет `component_update_state_packet_t` всем клиентам
3. Клиенты получают сигнал `on_update_component_state(uuid, new_state)`

### Восстановление сессии

При переподключении компонента:
1. Клиент отправляет `registration_request_packet_t` с тем же UUID и токеном сессии
2. Сервер распознает переподключение и восстанавливает сессию
3. Состояние компонента переходит из `disconnected` в `registered`

## Утилиты

### Форматирование и преобразование

Файл `src/utils.cpp` содержит утилиты:

- `component_state_to_string(state)` — преобразование состояния в строку
- `component_type_to_string(type)` — преобразование типа в строку
- `generate_session_token(uuid, type, port)` — генерация CRC16 токена сессии
- `format_component_info(uuid, type, state)` — форматирование информации о компоненте
- `format_component_state_change(uuid, new_state)` — форматирование изменения состояния

## Сборка

Модуль собирается автоматически при сборке проекта через CMake:

```bash
cmake -B cmake-build-debug -S .
cmake --build cmake-build-debug --target aggregator_server_app -- -j16
cmake --build cmake-build-debug --target aggregator_client_app -- -j16
```

## Запуск

### Сервер агрегации

```bash
./cmake-build-debug/bin/aggregator_server_app run
```

### Клиент агрегации

```bash
./cmake-build-debug/bin/aggregator_client_app run
```

Можно запустить несколько экземпляров клиента для проверки работы с множественными подключениями.

## Логирование

Модуль использует Qt логирование с категориями:

- `aggregator_server` — события сервера агрегации
- `aggregator_client` — события клиента агрегации

Для включения отладочного вывода:
```cpp
QLoggingCategory::setFilterRules("aggregator_server.debug=true");
QLoggingCategory::setFilterRules("aggregator_client.debug=true");
```

## Архитектурный контекст

Согласованная роль агрегатора, обнаружение устройств и целевая передача данных
между независимыми ядрами описаны в [архитектуре аудиосистемы](../../docs/architecture.md).
Агрегатор является реестром; аудиоданные передаются напрямую между ядрами.
Описания возможностей передаются отдельным расширением реестра; существующий
формат регистрации сохранён. Типизированные потоки используют прямые соединения
между ядрами, описанные в [протоколе версии 2](../../docs/network-stream-v2.md).

## Асинхронное подключение

`connect_to_aggregator_server()` возвращает принятие запроса, а не результат
соединения. Подключение и регистрация завершаются через сигналы в event loop.
`status_changed` использует общий `core::RuntimeState`: Starting, Running,
Recovering, Failed и Stopped. Running означает подтверждённую регистрацию.
`disconnect_from_aggregator_server()` отменяет подключение и все повторные попытки.

При регистрации клиент получает существующий реестр через обычные пакеты
component_info_update. Переподключение использует растущую задержку до 30 секунд.
Подключение и регистрация имеют отдельные тайм-ауты. Сервер возвращает текст
ошибки UTF-8 в существующем PacketType::error перед закрытием соединения; отказ
регистрации требует нового явного запроса подключения. Форматы обычных пакетов
не изменены. `aggregator_network` проверяет эти сценарии на локальных сокетах.
