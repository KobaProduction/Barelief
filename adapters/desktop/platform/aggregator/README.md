# Реестр компонентов Aggregator

`aggregator-library` содержит Qt-сервер и асинхронный клиент обнаружения.
`AggregatorServer` регистрирует UUID, адрес, control port, состояние компонента
и capabilities. `AggregatorClient` поддерживает cache обнаруженных компонентов,
события обновления, reconnect и разрешение UUID в актуальный core endpoint.
Потоки CONTROL/DATA устанавливаются напрямую между ядрами.

`connect_to_aggregator_server()` возвращает принятие запроса подключения.
Подтверждённая регистрация приходит через сигналы; `RuntimeState::Running`
означает именно её. `disconnect_from_aggregator_server()` отменяет соединение
и повторные попытки. Session token реестра и checksum не дают право изменять
схему найденного ядра: writable согласуется отдельно в CONTROL.

Default port реестра — 8212. Точные типы определены в
[types.h](../../include/aggregator/abstract/types.h), client API — в
[aggregator_client.h](../../include/aggregator/aggregator_client.h).
Команды запуска находятся в общем [справочнике](../../../../docs/reference/build-and-run.md).
Архитектурная роль: [обмен между ядрами](../../../../docs/architecture/embedded-and-exchange.md).
