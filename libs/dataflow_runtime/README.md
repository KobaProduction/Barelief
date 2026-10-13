# Исполнитель схемы

[Переносимое ядро](../../docs/architecture/portable-core.md) · [Runtime API](../../docs/reference/runtime-api.md)

C++20 target `dataflow_runtime` зависит от application/domain, signal contracts,
DSP kernels и WLED codec. Qt, JSON, системные таймеры, capture и сетевой I/O
подключает внешний владелец. Installed target — `PortableDSP::dataflow_runtime`.
Публичная точка входа — [dataflow_runtime/runtime.h](include/dataflow_runtime/runtime.h).

`PreparedGraph` содержит подготовленный план, `RuntimeContext` — изменяемое
состояние операций. Единственный `GraphExecutor` реализует application ports
подготовки и исполнения. Владелец сериализует обработку и Apply для каждого
executor; повторный вход в один context из callback не поддерживается.

Prepare проверяет параметры, физические контракты, capabilities и ресурсный
бюджет, строит topology и резервирует буферы/workspace. Apply активирует план
и переносит совместимое состояние; отказ сохраняет рабочую схему. `shutdown`
предназначен для teardown владельцем. Пользовательского Start/Stop нет.
Detached preparation использует owning snapshot, затем регистрацию под
сериализованным доступом; дополнительный snapshot входит в peak memory budget.

`process_numeric_ports` принимает source frame конкретного узла/порта,
`process_clock` — явный clock event. `advance_unavailable_source` продвигает
известную ветвь при пропаже данных. Callback выдаёт готовые выходы и входы sinks,
а History callback — компактную строку, заполненность и ordinal. Все views
заимствованы до возврата callback; async-адаптер приобретает bounded owner.
Внешняя ring History принимается через owning ingress с ordinal/retention owner.

`execution_interval` — сокращённая рациональная длительность 1 ms…60 s,
по умолчанию `{1,120}`. Window рассчитывает дробный hop по Fs и переносит остаток;
это не GUI FPS и не ресемплинг PCM. Число окон и callbacks на один ingress
ограничено профилем. Независимые внешние clocks не синхронизируются автоматически.

History хранит `[временной кадр][элемент]`, с полной ёмкостью и типизированными
нулями с момента подготовки. Filled count отличается от shape. Изменение только
ёмкости сохраняет новейшие совместимые строки и их время. `history` предоставляет
отдельный сериализованный borrowed snapshot. Локальная History принимает
fixed-width Vector; обычная Matrix не становится накопителем из-за подписи.

Bypass передаёт исходный кадр только при совпадении физического контракта;
несовместимый или отключённый маршрут формирует typed zeros на известных часах
либо unavailable при неизвестном состоянии. Renderer не выполняет эти операции.
`NodeStats` и `MixerInputStats` предоставляют bounded диагностику; измерительные
монотонные часы передаются в `NumericRuntimeProfile` внешним владельцем.

Prepared processing не расширяет контейнеры и не выполняет I/O. Admission
учитывает DTO, plan, payload, scratch и stack reservations; реальный allocator
overhead, stack high-water и аппаратное время измеряются отдельно.
