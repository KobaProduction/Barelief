# API исполнения

[Переносимое ядро](../architecture/portable-core.md) · [Desktop runtime](../architecture/desktop-runtime.md)

Публичные контракты находятся в [runtime.h](../../libs/dataflow_runtime/include/dataflow_runtime/runtime.h),
[graph_lifecycle.h](../../application/include/application/graph_lifecycle.h) и
[processing_control.h](../../application/include/application/processing_control.h).
Этот справочник описывает назначение границ; точные сигнатуры и defaults задают заголовки.

## Модель и подготовка

`NumericProcessingGraph` владеет узлами, портами, параметрами, связями данных и
отдельными конфигурационными зависимостями. `resolve_processing_node` проверяет
операцию, `admit_processing_graph` — topology, контракты, actual capability query
и ресурсные ограничения. Наличие dtype в общем списке не разрешает любой kernel.

`NumericRuntimeProfile` задаёт admission, memory и kernel limits, поддержку
History, bounded delivery и необязательные платформенные часы измерения.
`PreparedGraph::numeric_capabilities(profile)` возвращает query фактически
доступных сочетаний; profile/context должен жить дольше использующих его query.

`PreparedExecution` объединяет immutable `PreparedPlan` и mutable
`RuntimeContext`. `GraphExecutor::prepare_numeric` выделяет память до обработки.
Для desktop `snapshot_preparation_base`, `prepare_numeric_detached` и
`register_prepared` разделяют холодную подготовку и сериализованную регистрацию.
`apply` проверяет владельца bundle/base; лишь успех потребляет bundle и
активирует план. Совместимое состояние переносится из актуального working context.

## Lifecycle и CONTROL

`GraphLifecycle` вызывает `NumericGraphPreparationPort` и `GraphRuntimePort` для
Prepare, Apply и connect/disconnect. Composition сериализует контроль, обработку
и query. Ошибка подготовки/замены сохраняет рабочий план; owner teardown использует
`shutdown_transactional`. Пользовательского Start/Stop нет.

`ProcessingControlService` добавляет session identity, CAS revisions,
единственного pending owner/token/TTL, replay и registered parameter updates.
Replies содержат borrowed graph/capabilities и требуют owning-проекции до
асинхронной отправки. `working_numeric_snapshot()` даёт общий immutable DTO-owner
для replay без удержания kernels. `ExplicitPersist` сохраняет applied graph
revision через отдельно подключаемый `ProcessingPersistencePort`.

Интервал анализа `execution_interval` по умолчанию `{1,120}` секунд, сокращённая
положительная дробь в диапазоне 1 мс–60 с. Он определяет шаг в signal timeline,
а не период GUI timer. Fs и время поступления источника сохраняют отдельный смысл.

## Обработка и владение

| Вход | API / правило |
| --- | --- |
| Обычный numeric/structured frame | `process_numeric_ports`: borrowed input потребляется синхронно |
| External History | `process_numeric_owned_ports`: owning frame, обязательный row ordinal и owner, удерживающий storage/pools |
| Локальные SampleClock/генераторы | `process_clock` |
| Недоступный источник | `advance_unavailable_source`: известный contract/clock позволяет typed silence |

`NumericRuntimeCallbacks` отдельно публикует результаты выходных портов и входов потребителей с
точной identity, contract, FrameStamp, status и DeliveryStamp. Все callback views
заимствованы только до возврата. Асинхронные очереди приобретают ограниченного
владельца; borrowed frame нельзя просто сохранить в контейнере.

Prepared processing не читает JSON, не выполняет transport I/O и не расширяет
контейнеры. Совместимый обход обычной операции передаёт вход, несовместимый
публикует типизированные нули; маршрутизаторы сохраняют свои явные правила.
Смена связи не даёт разрешения сбросить всю совместимую историю.

## History и диагностика

`NumericHistoryRowView` передаёт выбранную компактную строку, событие,
`filled_count`, capacity и `accepted_row_ordinal`. Полный `history(...)` —
сериализованный borrowed snapshot ring; удержание полного снимка выполняет адаптер.
Полная capacity существует с нулевым старым хвостом, occupancy и ordinal не
выводятся из размеров матрицы. Resize сохраняет новейшие реальные кадры и не
создаёт новые timestamps/ordinal.

`NodeStats` содержит calls, produced, errors, self total/max nanoseconds,
16 histogram bins и deadline misses. `MixerInputStats` — depth/capacity,
dropped/underrun samples. Часы измерения предоставляет platform; export и
форматирование выполняются вне processing path.

Desktop использует [EnginePort](../../presentation/ports/include/pipeline/engine_port.h):
Snapshot, ApplyGraph, Subscribe; коррелированные EngineReply и EngineSnapshot;
owning numeric frame, History row и cold snapshot callbacks.
Скрытие UI не должно становиться командой shutdown runtime.
