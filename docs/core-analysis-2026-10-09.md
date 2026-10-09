# Анализ ядра обработки данных

Дата: 9 октября 2026 года, Europe/Moscow.

Ниже сохранён исходный отчёт до исправлений. Текущий implementation и evidence
приёмки находятся в [отчёте исправлений](core-fixes-final-2026-10-09.md).

Дополнение после нового требования пользователя: [непрерывное ядро, настраиваемая
частота анализа и переносимость](core-continuous-architecture-review-2026-10-09.md).
Пользователь исключил публичные Start/Stop: ядро работает от запуска до shutdown.
Поэтому рекомендации ниже об исправлении Stop/restart описывают прежний контракт
и не являются целевой архитектурой. Отмена подписок и остальные дефекты обработки
остаются актуальными. Внешние правки runtime после первого анализа также сдвинули
часть приведённых ниже ссылок на строки; новые anchors и хэши указаны в дополнении.

Ядро уже имеет пригодную для переиспользования границу: переносимые контракты,
domain/application, подготовленный исполнитель и DSP отделены от Qt, захвата и
сокетов. Основные обнаруженные проблемы относятся к сохранению смысла контрактов,
непрерывности Window, состоянию History и отмене асинхронной доставки. Они могут
проявляться как ошибки графиков, хотя возникают раньше renderer.

Результат трёх параллельных ревью и координаторской сверки: четыре семейства
дефектов обработки и один дефект публичного SDK helper воспроизведены адресными
пробами; три дефекта доставки установлены трассировкой текущих исходников.
Исправления не выполнены. Полная сборка текущего дерева и интеграционная,
native graphics или аппаратная приёмка этим анализом не подтверждаются.

## Границы анализа

Проверены `domain`, `signal_contracts`, portable CONTROL mapper,
`dataflow_runtime`, `dsp_kernels`, application lifecycle/control и desktop
engine/host/delivery/server. Координатор дополнительно рассмотрел SDK source
manifest, embedded profile, persistence и DATA codec. Последние компоненты не
получили такого же полного независимого покрытия, как три основных направления.
UI, Activity, редактор и renderer не являются scope этого ревью.

Основа архитектурных решений — [закреплённая редакция Clean Architecture и DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md),
[карта проекта](architecture-map.md) и [numeric runtime API](numeric-runtime-api.md).
Stable kind names, port IDs, конфигурационные ключи и wire symbols сохраняются.
Новые паттерны и второй исполнитель не требуются.

## Цепочка исполнения

1. Document/CONTROL адаптер формирует owning `NumericProcessingGraph`.
2. `validate_contract` проверяет Numeric/Structured contract; resolver проверяет
   зарегистрированные параметры, defaults и port rules.
3. Domain admission проверяет topology, реальные capabilities и resource ledger.
   Dtype edges понижаются до явных внутренних Converter nodes.
4. `PreparedGraph` создаёт immutable план, kernels/gathers и буферы mutable
   `RuntimeContext`. `GraphExecutor::apply` проверяет creator/base identity и
   переносит совместимое последнее рабочее состояние.
5. Processing проверяет входной frame и watermark, затем синхронно исполняет
   подготовленные дочерние операции. History хранится в кольце `[кадр][полоса]`;
   отдельные callbacks передают compact row и filled count.
6. Borrowed views живут только в синхронном вызове. Адаптеры сохраняют bounded
   owning frames/rows для GUI, сети и других асинхронных потребителей.

В `NumericDesktopHost` Qt engine/router/server живут в worker `QThread`.
Физический source использует bounded capture buffer и отдельный `std::thread`;
TestSignal использует `std::jthread`. Эти потоки вызывают DSP под engine mutex.
Скрытие GUI не останавливает local DSP. Worker event loop обслуживает delivery,
transport и WLED; его задержка влияет на эти выходы.

## Воспроизведённые дефекты ядра

### P2 SpectrumMerge допускает неверный выходной контракт

Источник: `domain/src/processing_contracts.cpp:439`, особенно Merge branch на
строке 448 и общие проверки на строках 454–457;
`libs/dataflow_runtime/src/runtime.cpp:489` и обработка координат на строках 957–962.

Merge разрешает stereo SpectrumMagnitude → mono SpectrumMagnitude, но не
проверяет сохранение quantity, unit и frequency coordinates. Схема может
объявить Quantity::MelEnergy и frequency step 1/1 вместо входного 48000/512;
Prepare и processing успешны, callback публикует прежние спектральные отсчёты
с новым, неверным смыслом. Это способно искажать подписи графиков и данные сети.

Другой вариант той же дыры: Regular input → ExplicitFloat64LE output. Resolver,
capability и Prepare принимают сочетание, но корректный первый frame получает
`incompatible_contract`: Merge не синтезирует explicit coordinates из Regular.

Наблюдение probe:

```text
merge explicit=0 resolve=0 capability=1
prepare=0
merge output quantity=3 step=1/1 status=0
process=0
merge explicit=1 resolve=0 capability=1
prepare=0
process=3
```

Исправление: выводить ожидаемый output из input, меняя только разрешённый
channel mapping; любые изменения coordinate representation поддерживать явно
либо отвергать при Prepare. Oracle: проверка quantity/unit/origin/step/mode по
отдельности и успешный первый frame каждого admitted сочетания.

### P2 Disabled History пропускает ненулевые строки

Источник: `libs/dataflow_runtime/src/runtime.cpp:794`, вызовы на строках 838 и 906.

`history_route` при seed копирует исходную маску, а silence применяет только к
последней строке. Snapshot с несколькими строками у disabled NetworkInput
поэтому передаёт предыдущие supplied rows ненулевыми. Последующие batch updates
также обновляют маску только одной строки, независимо от accepted ordinal delta.

Воспроизведение: `external_history_ownership` fixture, disabled NetworkInput,
snapshot с двумя строками и ordinal=100,
`NetworkInput → Converter UInt64→Int64 → SpectrogramView`.

```text
disabled source converted row0=111
disabled source converted row1=0
```

Исправление: учитывать ordinal/rebase и маскировать все фактически изменившиеся
строки в пределах delta/capacity. Не очищать всю совместимую историю при каждом
кадре. Oracle: initial snapshot, delta=2, delta>capacity, wrapped oldest,
disabled Gate/Switch и сохранение активной соседней ветки; сравнивать все rows.

### P2 Window склеивает данные через unavailable интервал

Источник: `libs/dataflow_runtime/src/runtime.cpp:826`, unavailable branch на
строке 889 и rolling Window на строках 967–999.

Очистка PCM/progress/hop при unavailable ограничена clocks от InputSelector.
Прямой `NetworkInput → Window` сохраняет samples до unavailable; восстановление
с теми же producer epoch/generation продолжает старое окно.

Воспроизведение: PCM Float32 48 kHz, Window=512, hop=800; после прогрева подать
512 единиц, unavailable sequence=0, затем 288 двоек после пропущенного интервала.
Выход содержит 224 старые единицы и 288 новых двоек без discontinuity.

```text
prepare=0
gap window first=1 last=2 discontinuity=0
```

Исправление: единое правило continuity для любых Source clocks, очищающее
rolling samples и hop accumulation при gap и выставляющее discontinuity.
Oracle: direct Source и Selector дают только samples после gap с правдивым
timestamp/discontinuity; ранее накопленная совместимая History сохраняется.

### P2 Reset History блокирует downstream Converter

Источник: `libs/dataflow_runtime/src/runtime.cpp:192` и строка 920.

`reset_history` обнуляет родительский ordinal, но Converter хранит предыдущий
ordinal и отвергает новый меньший ordinal даже при объявленной discontinuity.
После шести строк в `Spectrogram → Converter → SpectrogramView`, reset и нового
кадра processing возвращает `invalid_argument`. Родитель уже принял новую строку.
Ожидание восстановления зависит от старого ordinal и может быть очень долгим.

```text
after reset_history parent process status=2
```

Исправление: явная identity/rebase History producer и переустановка основания
Converter при runtime reset/replacement. Oracle: reset при ordinal≫capacity,
следующий frame доступен сразу, ring/count/ordinal согласованы. Compatible
rewiring между разными History producers — дополнительная гипотеза той же
области; отдельно не воспроизведена.

### P3 History row helper возвращает success для усечённой строки

Источник: `libs/signal_contracts/src/data_frame.cpp:268`, проверки 294–299,
публикация 312–320. Текущий canonical runtime этот helper не вызывает.

History заявляет три Float32 элемента в row, storage содержит четыре байта.
`history_row` проверяет только первый элемент каждой plane и возвращает ok;
полученный row не проходит `validate_view`. Checked load оставшихся элементов
отказывает, поэтому прямой OOB этим probe не установлен.

```text
malformed history row returned=0 donor_validation=4 result_validation=4
```

Исправление: проверять полный extent выбранной строки, stride/layout и
feature coordinate spans до публикации. Oracle: усечённый payload/coordinates
и неверный stride отвергаются атомарно; wrapped rows и masked silence проходят.

## Дефекты доставки по трассировке исходников

Эти сценарии подтверждены чтением конкретных control/callback путей; динамическое
воспроизведение, sanitizer run и transport run для них не выполнялись.

### P1 Отписка не отменяет скопированный callback

Источник: `adapters/desktop/runtime/src/numeric_engine.cpp:544`, копирование
taps на строках 554/575, вызов 564/581, unsubscribe 667;
`numeric_control_server.cpp:21` и destructor 25.

Engine копирует callbacks под lock и вызывает вне lock. Первый listener может
удалить владельца следующего listener, который отписывается в destructor.
Локальная копия callback остаётся и вызывает raw захваченный `Impl*` после
уничтожения владельца. У server tap используется именно такой raw capture.

Исправление: live subscription token/generation или cancellation state,
проверяемый перед каждым dispatch; предусмотреть reentrant замены handlers.
Oracle: первый listener удаляет owner второго, destructor отписывается,
второй callback не вызывается; frame и history, проверка отсутствия UAF.

### P2 Stop не отменяет остаток текущего batch

Источник: `numeric_engine.cpp:311`, `:563`, `:580`;
`application/src/processing_control.cpp:267`.

Первый callback выполняет Stop. Очереди очищаются, но локальный `next` остаётся.
Перед следующим callback проверяются только epoch/graph_revision, которые Stop
не меняет. Следующий listener получает старый frame после успешного Stop.
Stop→Start внутри первого listener обходит и простую проверку Running.

Исправление: monotonic run/delivery generation, изменяемая при успешном Stop,
и проверка этой generation перед каждым callback. Oracle: Stop и Stop→Start из
первого listener запрещают последующие pre-Stop deliveries для frame/history.

### P2 Server пропускает быстрый local Stop и Start

Источник: `numeric_control_server.cpp:28`, `:65`, `:82`, `:175`.

Server отменяет DATA по sampled переходу Running→Stopped либо reply сетевого
CONTROL Stop. Local Stop→Start между pump оставляет Running и ту же graph
revision/epoch; server сохраняет прежние outbound/inbound/queries и codec.
Старые DATA могут передаваться после restart с допустимым envelope.

Исправление: авторитетная monotonic stop/delivery generation из engine вместо
вывода события из snapshot. Oracle: backpressure удерживает старые DATA,
local Stop→Start проходит без pump между командами, возобновление транспорта
не отправляет и не исполняет pre-Stop data/queries.

## Архитектурные риски и вопросы спецификации

- Engine mutex одновременно защищает DSP и холодные Prepare/JSON/control операции.
  `with_control_service` также охватывает server transport poll/codec. Риск
  задержек capture реален по устройству критической секции; измеренного drop
  или latency bound нет. Измерять lock duration, ingress backlog/drops и cadence,
  затем выносить cold preparation/codec/I/O вне lock с проверкой base при Apply.
- Публичный NumericEngine смешивает thread-safe feed/query и owner-thread control
  с QTimer/socket ресурсами. Нормальный host dispatch соблюдает Qt affinity,
  raw `local_engine()` допускает обход. Execution contract нужно закрепить.
- `numeric_engine.cpp:189` публикует analyzer Running по run intent, хотя lifecycle
  может быть Empty. Intent и фактическую готовность/исполнение следует передавать
  отдельно, чтобы диагностика не называла неподготовленный граф работающим.
- Transform/WLED/Reduce записывают processing error в общий result, но не всегда
  в downstream status (`runtime.cpp:1039`, `:1051`, `:1055`). Callback получает
  generic unavailable вместо причины. Это source-confirmed diagnostic defect;
  адресный пример для проверки — Gain overflow.
- Regular и Explicit Hertz Mel используют разные построения треугольных фильтров
  (`dsp.cpp:103` и `runtime.cpp:1014`). Эквивалентные координаты могут менять
  численный результат. Нужен независимый mathematical oracle и решение,
  допустимо ли это различие. Название MelEnergy и использование magnitude вместо
  power также требуют явной DSP спецификации, а не автоматического изменения.
- Общий runtime State смешивает Window continuity, History masks/ordinals,
  producer identity, kernel state и diagnostics. Разделение небольших внутренних
  компонентов уменьшит риск неполного reset, сохранив одного GraphExecutor.
- Capability должна подтверждать реализуемость operation для корректных frames
  полного контракта, включая coordinates. Проверка только dtype/shape недостаточна.
- Старые shape-only domain модели не должны становиться authority новых нод.
  Canonical authority — NumericProcessingGraph и его resolver/admission.

## Порядок дальнейшей работы

1. Исправить operation contract для Merge, continuity Window, batch History masks
   и History rebase; добавить адресные regression oracles к canonical tests.
2. Исправить subscription cancellation и общий Stop generation через engine и
   server; отдельно воспроизвести reentrancy и backpressure сценарии.
3. После исправлений выделить внутренние continuity/History routing компоненты,
   унифицировать operation output rules и error propagation. Измерить control
   latency перед изменением threading. Закрепить Mel specification.
4. Единственный владелец выполняет canonical release/sdk build и проверки;
   независимое ревью принимает готовые изменения в явно указанном scope.

Это порядок предложенных блоков, а не статус выполненного рефакторинга.
Не требуется переписывать UI одновременно, менять stable IDs, восстанавливать
legacy или вводить второй executor. Публичные контракты и portable source SDK
должны оставаться границей переиспользования.

## Доказательства и ограничения

Свежей отдельной компиляцией source files снимка контрактного reviewer в TEMP прошли:
`numeric_contract_tests`, `numeric_processing_contracts_tests`,
`numeric_frame_gather_tests`, все exit=0. Найденные входы эти tests не покрывают.

Merge/helper probe собирал runtime/domain/signal_contracts source files снимка
контрактного reviewer
с существующими DSP/application archives. Три executor probes собирали текущий
runtime.cpp с существующими release dependency archives. Их результат
подтверждает конкретные дефекты этих source paths, но не freshness всех
dependencies, полный host/SDK gate или native/hardware поведение.

Временные материалы контрактного reviewer:
`C:/Users/Koba/AppData/Local/Temp/qt-core-contract-review-20261009/`
с `probe.cpp`, `probe.log`, `current-tests.log`, `source-hashes.json`.
Материалы executor reviewer:
`C:/Users/Koba/AppData/Local/Temp/qt-core-review-executor/`
с `probe.cpp`, `gap.cpp`, `rebase.cpp` и исполняемыми probes.
TEMP материалы могут быть очищены системой; сценарии и ожидаемые oracles
сохранены выше для повторения и будущих regression tests.

Source hashes из контрактного probe:

| Source | SHA256 |
| --- | --- |
| domain/src/processing_contracts.cpp | D557034057B4EB76C2C9AF21D0E3BCB3A1552269D4AC3E3B4092CCE9D426A3F6 |
| domain/src/graph_definition.cpp | 34F0FCAAF3F8D3B2FDA04AB34F4FB3513165C3D37B7235E675040A14506DC3E5 |
| libs/signal_contracts/src/data_frame.cpp | 571D0F12EBF15F1CB12F1194A60662A68AB40C5EBE11BDADDCEF326876677EA8 |
| libs/dataflow_runtime/src/runtime.cpp | 08EFB29055B5F00201C063EDDA84168FC62EB52DB312A95CAD8082ED155FD0A0 |

Координатор сверил ключевые findings с текущими исходниками и прочитал сохранённые
контрактные probe/test logs. Наличие старых SDK binaries не принималось как
доказательство текущего дерева: runtime/domain sources новее этих binaries.
Финальная сверка обнаружила внешнее изменение `domain/src/processing_contracts.cpp`
и `domain/src/graph_definition.cpp` после контрактного probe. Runtime и
`data_frame.cpp` совпадают с hashes выше. Новые domain hashes:
`F585DCB7CA7533616B42AB628207D1D16559362ED6D9EE9ECD293B5DFD27649A` и
`61A6165BAE675A49F10C7D98207C1CB69B4490FF00A30BDDBD1B32AD35C9C511`.
Координаторская source сверка подтверждает сохранение дыры Merge в этих
редакциях; test PASS относится к снимку до внешних изменений.

Группа ревью не изменяла исходники, пользовательские configs/output, shared
build directories, ветки и коммиты; координатор добавил только этот отчёт.
