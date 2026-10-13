# Контракты данных

[Переносимое ядро](../../docs/architecture/portable-core.md) · [Runtime API](../../docs/reference/runtime-api.md)

C++20 target `signal_contracts` задаёт данные и владение памятью без Qt, JSON,
устройств и транспорта. Installed target — `PortableDSP::signal_contracts`.

| Публичный заголовок | Назначение |
| --- | --- |
| [data_contract.h](include/signal_contracts/data_contract.h) | Primitive dtype, scalar/vector/matrix, structured contract, clock, координаты и представление памяти |
| [data_frame.h](include/signal_contracts/data_frame.h) | Borrowed `DataFrameView`, `NumericView`, mutable views, владеющий `OwnedDataFrame` и `retain_frame` |
| [buffer_pool.h](include/signal_contracts/buffer_pool.h) | Ограниченный caller-owned pool и reference-counted handles |
| [numeric_frame_gather.h](include/signal_contracts/numeric_frame_gather.h) | Подготовленная проекция числового кадра или отдельной строки матрицы |
| [numeric_policy.h](include/signal_contracts/numeric_policy.h) | Правила преобразования точности, округления и overflow |
| [oscillator.h](include/signal_contracts/oscillator.h) | Параметры независимых осцилляторов |

`semantic` и `history_element` — описательные metadata. Они не выбирают
алгоритм, допустимость операции или канальность. `same_contract` сравнивает
полную metadata identity; `same_physical_contract` игнорирует только эти два
описательных тега. Часы, координаты, normalization и storage остаются частью
физического контракта. Совместимость операции дополнительно проверяет domain.

`NumericView::ring` явно отличает кольцевое хранение от обычной Matrix.
Строки имеют логический порядок от старой к новой, физический порядок задаётся
ring descriptor. `matrix_row` выбирается в запросе gather явно. Одна timestamp
не восстанавливает времена прежних строк: используются объявленные координаты.

Borrowed views действуют в пределах жизни предоставленной памяти; runtime
callbacks ограничивают их жизнь возвратом callback. Для асинхронной доставки
используется `retain_frame` в заранее подготовленные payload/metadata pools.
Pool storage и slots живут дольше всех handles. Публикация буфера неизменяема;
совместный доступ к одному handle object синхронизирует владелец. Исчерпание pool
возвращает статус без heap fallback; отказ retain не изменяет destination.

Базовые `FrameView`/`HistoryView` находятся в [frame.h](include/signal_contracts/frame.h).
Числовой граф использует `DataFrameView` и primitive contracts; базовые структуры
не задают второй набор правил допуска схемы. Сборка/подключение package описаны
в [SDK guide](../../docs/reference/embedded-sdk.md).
