# Непрерывное ядро: архитектурное ревью

Ниже описано состояние до исправлений. Актуальные изменения и границы их
приёмки находятся в [отчёте исправлений](core-fixes-final-2026-10-09.md).

9 октября 2026 года, Europe/Moscow. Дополнение к [первому анализу](core-analysis-2026-10-09.md).

Следующее уточнение пользователя и результаты fresh PCM/DSP/helper probes —
[углублённое ревью PCM](core-pcm-deep-review-2026-10-09.md): bypass вместо global
Stop, execution interval по времени, шесть новых семейств дефектов. Это дополнение
уточняет предложенную здесь cadence policy, не возвращая GUI authority.

## Вывод

Текущая реализация **не соответствует новому требованию** непрерывного ядра с
настраиваемым target анализа. Публичный Start/Stop встроен в application,
GraphExecutor и CONTROL; Window использует фиксированные 60 Hz; firmware ставит
обработку в зависимость от сетевого соединения. Это изменение требований к ранее
разрешённому поведению, а не доказательство неправильной реализации старого Stop.

Переносимая основа удачна: типизированные контракты, подготовленный план,
отдельное mutable состояние, один исполнитель, ограниченная владеющая доставка,
отсутствие Qt и сетевого I/O внутри DSP. Полное переписывание не требуется.
Однако оптимальность и 60 Hz на MCU не подтверждены измерениями. Основные слабые
места — универсальное состояние узла, смешение политик доставки, управление
непрерывностью и неполная модель CPU/пиковых ресурсов.

Ревью выполнено тремя независимыми направлениями: контракты/application,
executor/DSP, desktop/embedded lifecycle и delivery; координатор сверил firmware,
SDK source manifest, бюджетирование и официальную документацию. Код не менялся.

## Принятый смысл непрерывности

После запуска процесс/устройство поддерживает processing service до shutdown.
Появление первого корректного prepared plan автоматически активирует его.
Отсутствие схемы, недоступный источник, отключённая связь и отсутствие сетевого
подписчика — состояния готовности/данных, а не причины остановки всего сервиса.
У известного отключённого маршрута остаются типизированные нули и принятые часы;
неизвестный контракт распространяет unavailable. Недоступность источника сама по
себе не разрешает придумывать реальные PCM или менять его sample rate.

Shutdown остаётся внутренней ответственностью composition root: прекратить
producers, закрыть delivery, дождаться callbacks и освободить ресурсы. Удаление
пользовательского Stop не означает удаление безопасного завершения процесса.
Не требуется busy loop: DSP исполняет работу при наличии входа и согласно
подготовленной временной политике; ожидающий scheduler остаётся действующим.

## Матрица соответствия

| Требование | Сейчас | Оценка |
|---|---|---|
| Непрерывный сервис до shutdown | run intent, Start/Stop, process guard | Не соответствует новому контракту |
| Target анализа настраивается в ядре | Window делит sample rate на 60 | Нет такой настройки |
| GUI не управляет DSP | capture исполняет GraphExecutor, GUI получает snapshots | Базовая граница верна; доставка зависит от Qt event loop |
| Transport не управляет жизнью обработки | ESP worker пропускает source tick без соединения | Нарушено |
| Смысл/геометрия данных проверяются | typed admission/frame validation | Хорошая основа; дефекты Merge и History из первого отчёта |
| MCU ресурсы предсказуемы | memory/stack admission, prepared storage | Есть bounds; нет hardware WCET и измеренной stack/heap приёмки |
| Декомпозиция позволяет переиспользование | portable libraries/application, single executor | В целом да; State и desktop engine перегружены |

## Непрерывность и частота

`application/include/application/processing_control.h:112–121` содержит Start,
Stop и run_requested; `application/src/processing_control.cpp:267` меняет run
intent. `application/src/graph_lifecycle.cpp:67–90` передаёт этот флаг в Apply и
управляет start/stop. `libs/dataflow_runtime/include/dataflow_runtime/runtime.h:92`
и `src/runtime.cpp:1150–1192` реализуют отдельный running state и отказ process
при выключенном исполнителе. Изменять нужно весь контракт согласованно, а не
добавлять `running_=true` в одном месте. Остальные stable IDs/ports/kinds/wire
symbols не следует переименовывать или перенумеровывать.

В firmware `examples/esp32_streamer/main/main.cpp:352` отсутствие TCP descriptor
приводит к ожиданию 100 ms и continue **до** source tick (`:361–368`). Ошибка
CONTROL (`:358`) и fencing (`:337–359`) также обходят обработку. Один worker
может остаться, если ограниченные CONTROL/transport jobs не препятствуют DSP;
разделение на дополнительные задачи оправдано только если blocking I/O нельзя
устранить и учтены очереди/стек. Второй GraphExecutor не нужен.

Нужно различать частоты отсчётов, блоков, анализа и отображения:

| Величина | Пример | Кто задаёт |
|---|---|---|
| Sample rate | 48 000 samples/s | Источник/устройство и контракт часов |
| Ingress block rate | 48 000 / 256 = 187,5 blocks/s | Размер блока и источник |
| Analysis target | 60 windows/s | Переносимая execution policy ядра |
| Presentation rate | GUI обновляет последнюю проекцию | Потребитель/renderer |

Window планирует окна по sample-time (`runtime.cpp:977–978`), чередуя целые hop
для не кратных 60 sample rates. При 48 kHz hop=800; при 44,1 kHz hop=735.
Большой блок 4096 samples при 48 kHz может синхронно вызвать 5–6 полных
downstream проходов. Следовательно 60 окон/s не означает равномерный wall-clock
callback каждые 16,67 ms. Ветки без Window исполняются с частотой ingress.

Desktop drain использует `QTimer::start(16)` (`numeric_engine.cpp:160`): это
номинально 62,5 вызова/s, не настройка target ядра. Qt допускает задержку таймера
и объединяет пропущенные timeout в один; даже PreciseTimer не обеспечивает
hard realtime. [Официальная документация QTimer](https://doc.qt.io/qt-6/qtimer.html).

Firmware `next=now+period` (`main.cpp:368`) привязывает каждый следующий срок
к фактическому запоздалому wakeup и накапливает drift. Требуется абсолютный
deadline/sample cursor и явная bounded политика missed deadlines. Для реального
PCM приоритет имеет аппаратный sample clock, а не искусственная генерация кадров
по wall-clock. В `embedded_support/src/profile.cpp:51/:165` и `profile.h:34`
зашито 60 для spectral throughput; эти расчёты должны использовать ту же policy.

### Минимальная portable execution policy

Одна admitted policy в execution bundle, default 60 Hz; profile/capabilities
задают разрешённые границы. Application Prepare/Apply переносит настройку,
configuration и CONTROL сохраняют явное значение. Не дублировать глобальную
политику отдельными независимыми полями каждого Window.

Для целого target H текущий fractional-hop алгоритм можно сохранить с divisor H.
Для дробных частот допустим положительный reduced rational num/den:
`sample interval = sample_rate * den / num`. Checked arithmetic выполняется до
processing. Target выше sample rate следует отвергать без отдельной явно
определённой политики interpolation/duplicate; `max(1, hop)` не выполняет такой
target честно.

Нужно различать requested/admitted/actual rate, предел emitted windows/events на
ingress, правила перегрузки и scheduling phase. При смене target сохраняются
rolling PCM и совместимая History; фаза пересчитывается на принятой sample
boundary. Текущее копирование hop/remainder/progress в `runtime.cpp:217–224`
потребует пересмотра. Timestamp остаётся реальным sample-time (`:990–1002`),
а не `frame_index / 60`; History Seconds уже использует timestamp (`:1079–1088`).
Coarse tick units могут дать одинаковые timestamps нескольких окон — нужна
объявленная точность часов, а не искусственное увеличение timestamps.

## Дополнительный дефект доставки PCM

**P1 conditional, source-confirmed; не аппаратно воспроизведён.**
`adapters/desktop/runtime/src/numeric_engine.cpp:336–351` оставляет только
последний ordinary frame каждого endpoint и помечает superseded frame как
discontinuity. Комментарий прямо включает network taps. То есть объединение
происходит до реального network backpressure.

Если между двумя drain поступают три PCM-блока по 256 samples при 48 kHz,
подписчик получает только последний блок с discontinuity. При повторении такого
режима `runtime.cpp:766–768` очищает зависимый Window на каждом вводе, а hop
равен 800. Window не накапливает достаточно samples для устойчивого FFT/Mel
выхода. Числа описывают конкретный воспроизводимый кандидат, а не гарантированное
поведение любого timer/transport/source.

Нужны отдельные политики: GUI-preview latest-value; loss-sensitive stream —
ограниченная FIFO владеющих PCM-блоков и честная сигнализация actual overflow.
При overflow нельзя сохранять видимость непрерывного потока. Проблема не
устраняется одним удалением Stop или заменой 16 на 16,67 ms.

## Декомпозиция и оптимальность

Desktop NumericEngine также объединяет lifecycle/executor, JSON projection,
source reconciliation, owning delivery, WLED и Qt timers. Разделение по владельцам:
application owns active/pending graph; source host owns producers/device teardown;
delivery adapter owns queues/subscriptions; network hosts own sessions/sockets;
composition соединяет их. Не создавать несколько authorities mutable graph.

`runtime.cpp:64–99` содержит универсальный State с FFT/Mel, Window cadence,
History/route masks, producer identity, owning external ingress и WLED diagnostic
metadata для каждого узла. Это допустимый прототип, но именно смешанные counters
и общий clear/refresh создают риск неполного reset. Полезные внутренние границы:
continuity, Window schedule/storage, owning History, routed History overlay,
prepared operation и её mutable state. Dispatcher остаётся один.

Выбирать storage нужно по prepared operation. Большой inline variant сам по себе
не экономит память: размер равен крупнейшему member. Не требуется создавать
virtual иерархию для каждой ноды. Resolved operation descriptor уменьшит
строковые переходы в hot path без изменения внешних kind names.

Оптимизации требуют измерений: Window сдвигает rolling buffer (`:985`), explicit
Hertz Mel обходится O(bands×bins) (`:1025–1032`), exact integer/Float32 reductions
используют широкую арифметику и bit search (`numeric_arithmetic.h:197`). Ring
Window и prepared Mel weights могут помочь, но менять точную арифметику на float
ради скорости без изменения declared precision policy нельзя.

WLED peak/magnitude наследуется через generic emit (`runtime.cpp:846/:880–889`)
как скрытая upstream metadata. Следует сделать её зависимость явной. Regular и
explicit Hertz Mel требуют общей математической спецификации — shape/dtype и
название MelEnergy ещё не определяют magnitude/power/normalization.

Cold Prepare, capture и часть I/O используют общий desktop mutex. Before/after
измерение control latency, capture lock wait и dropped frames необходимо перед
решением о threading. Подготовка нового плана вне active processing lock с
проверкой base revision и коротким serialized Apply — полезное направление.
Совместимые истории сохраняются; невалидный новый план не заменяет рабочий.

## MCU: переносимость и реальные пределы

SDK source manifest компилирует те же portable исходники target-toolchain;
`adapters/esp_idf/portable_sdk/CMakeLists.txt` использует C++20,
`-fno-exceptions/-fno-rtti`, выбирает ANSI/ESP-DSP backend. Это хорошая граница.
В manifest переносимая numeric_control_session находится под desktop directory:
это вопрос владения/расположения, само по себе не Qt-зависимость.

Оставшиеся ограничения:

- Admission считает память/стек, но не CPU throughput и multiplicity Window
  events (`runtime.cpp:339–423`). Нужны maximum block/minimum hop и downstream
  fanout/work counts; затем стоимость конкретного target на устройстве.
- Recursive dispatch (`:819/:892`) использует reference stack estimate
  `max_nodes*1536+4096` (`:345`), не измеренный native ABI. Bounded iterative
  event workspace возможен, но обязан сохранить все Window emissions и lifetime
  borrowed views. Один topological проход по последнему значению потеряет окна.
- Shared zero pool резервируется по `frame.max_payload_bytes` (`:556–558`),
  а не максимальной потребности данного graph. Это кандидат на экономию.
- Prepare использует STL heap allocations. При отключённых exceptions условные
  catch в `:528/:727–730` не дают recoverable OOM. Для гарантированного отказа
  без потери рабочего плана требуется bounded supplied storage/allocator,
  соответствующий peak old+new/control/codec budget.
- FFT vendor tables process-global; `backend_tables.cpp:7–17` защищает init,
  но не чужой deinit. Composition root должен владеть lifetime backend. Метка
  EspDsp не гарантирует аппаратное ускорение: radix4 идёт через ANSI (`backend_esp_dsp.cpp:12`).

ESP-IDF рекомендует измерять не только free heap, но largest free block/minimum
heap и stack high-water; рекурсия и большие automatic objects повышают stack
риск. В ESP32 high-water API возвращает bytes. Эти данные, а также `.data/.bss`,
важнее одного reference resource estimate.
[Официальные рекомендации ESP-IDF по RAM](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/performance/ram-usage.html).

Нужен bounded hardware сценарий: максимальный admitted graph/depth/block,
несколько targets, outage/reconnect и Prepare/Apply во время работы. Собирать
processing max/p95, deadline misses, windows/events counts, queue occupancy,
actual analysis rate, heap/stack high-water. Host/fake-IDF PASS не заменяет его.

## Naming, публичные границы и документация

Внешние имена Window/WindowFunction, InputSelector N→1 и Switch 1→N достаточно
точно различают ответственность. Главный naming risk — не стиль слов, а несколько
моделей с похожими именами и разными полномочиями. Старые desktop audio_pipeline,
graph и graph_document в дереве не доказывают второй действующий executor:
canonical CMake targets используют NumericEngine/numeric_project_document.
Но старые GraphDefinition/NodeParameters ещё компилируются в domain и транзитивно
экспортируются через processing_contracts.h; data_contract.h включает frame.h с
float-only FrameView, а buffer_pool.h содержит и canonical, и старые types.

Общий registry kinds/ports/parameters следует отделить от старых validators и
models, после проверки consumers сократить canonical публичную поверхность.
Portable control session логичнее разместить в portable control adapter. Это
упрощение зависимости и ownership, а не массовое переименование stable keys.
`libs/dataflow_runtime/README.md:10–25` ещё описывает legacy ingress и отсутствующий
allow_legacy_window; документ нельзя считать актуальной спецификацией.

**Wire-symbol trap:** `numeric_control_message.cpp:191–200` кодирует position в
таблице как i+1. Start=6, Stop=7, Persist=8, Release=9; удаление элементов массива
`:522–524` сдвинет оставшиеся stable symbols. Отвергать удалённые операции с
зарезервированными номерами; не сжимать таблицу. Аналогично сохранить номер
Running=4 при исключении Stopped. Это не legacy shim и не сохранение Stop.

Cadence нельзя поместить в ClockContract: domain/tick_seconds/sample_rate
описывают часы данных. WLEDOutput cadence_ms (`graph_definition.cpp:194`) также
не является глобальным target. Если policy сохраняется в graph DTO, обновить
equality (`graph_lifecycle.cpp:7`), own_graph/GraphProjection
(`numeric_processing_message_mapper.cpp:49/:140`),
registered document field и validation. Если меняется удалённо, включить CONTROL
mapping, CAS/replay, capabilities. Версии менять только у затронутых форматов;
DATA4 не требует новой edition исключительно из-за частоты при прежних timestamps.
Unknown JSON fields сохраняются; malformed/future edition отвергается атомарно.

## Что считать современной и достаточно простой реализацией

Сохранить C++20 spans/views, RAII ownership, immutable prepared plan и один
executor. Добавить явные temporal/resource/delivery policies и operation-specific
state. Callback registration должна иметь lifetime/cancellation identity:
простого удаления std::function из реестра недостаточно, когда drain уже скопировал
callback. Этот риск UAF из первого отчёта остаётся при непрерывном ядре.

Не требуются reactive framework, coroutine pipeline, универсальный event bus или
новая DDD иерархия. Их стоимость и смысл пока не обоснованы. C++ Core Guidelines
полезны здесь прежде всего для resource ownership и явных интерфейсов.
[Первичный источник](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines).

## Приоритет следующего блока реализации

1. Устранить потенциальный UAF подписок и разделить loss-sensitive PCM delivery
   с GUI preview. Добавить адресный oracle: три PCM inputs между drains дают
   network все три по порядку, GUI может получать newest; overload маркируется.
2. Согласованно удалить public run intent через application/executor/CONTROL/
   presentation/bootstrap, сохранив shutdown и stable wire numbering. Firmware
   продолжает source/process при transport outage; working graph живёт во время
   Prepare и сохраняется при неуспешной замене.
3. Ввести одну execution policy default 60 с validation, sample-hop scheduling,
   persistence/control и embedded link/event admission. Проверять смену target
   без искажения sample clock и времени сохранённой History.
4. Исправить отдельно Merge, Window unavailable continuity, batch History masks,
   History rebase и helper из первого отчёта. Новый lifecycle их не закрывает.
5. Выделить operation-specific state и ownership границы; актуализировать docs.
   Оптимизации rolling storage/Mel делать по профилю. Затем независимый review и
   интеграционные/hardware measurements в заявленном scope.

## Область доказательств

Этот pass — source/architecture review, без новых builds и runtime/hardware
измерений. Ранее воспроизведённые probes описаны в первом отчёте и относятся к
его снимку. Внешний чат менял InputSelector во время ревью; такие изменения не
откатывались и не объявлялись независимо принятыми этим анализом.

Проверенный снимок runtime.cpp SHA256:
`DCB5B72DB0A237E576F624EF3F56282760B04AFCFF9AD208820E56929719177A`.
runtime.h: `B17D8156A5C9CBC02FCC72CF4CD3BE0A94C46BF765D8664AC85D12C34B22109B`.
dsp.cpp: `3AF8363F019B855C8A1F3E002F37E8BA5217F9F73B188F8BD7196DDD8AC02B62`.
processing_contracts.cpp: `F585DCB7CA7533616B42AB628207D1D16559362ED6D9EE9ECD293B5DFD27649A`.
graph_definition.cpp: `7DC5FF919E18F457A22BB475C07AAE781F207BD4605101B55A177424BE9BEE43`.
data_frame.cpp: `571D0F12EBF15F1CB12F1194A60662A68AB40C5EBE11BDADDCEF326876677EA8`.
numeric_engine.cpp: `1C68EE225EB3F8FD96DE7B0D8420784BE5E820E52C9708C17E6AA5AAA9B3F480`.
firmware main.cpp: `EFD0251557983A6E2EE72D8750228C7566BB39757B506A743CB38C8190278EAE`.
