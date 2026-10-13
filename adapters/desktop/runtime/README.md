# Настольные runtime-адаптеры

Модуль подключает переносимый `GraphExecutor` к Qt event loop, capture и
transport. `NumericEngine` управляет подготовкой/применением через общий
`GraphLifecycle` и `ProcessingControlService`; `NumericDesktopHost` размещает
endpoint и сетевые ресурсы в worker-потоке GUI. CLI использует те же адаптеры
без визуализации.

Внутренние collaborators разделяют сериализацию executor, SourceIngress,
DeliveryHub и холодное staging. Результаты переходят в async-доставку только
с ограниченным владельцем; processing views заимствованы до возврата callback.
Успешный Apply активирует обработку. Shutdown выполняет владелец приложения.

Описание границ: [desktop runtime](../../../docs/architecture/desktop-runtime.md).
Публичные контракты: [API исполнения](../../../docs/reference/runtime-api.md),
[CONTROL/DATA](../../../docs/reference/protocol.md).
