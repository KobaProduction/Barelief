# Утверждённый план реализации

Актуальный scope: **2026-10-07, Europe/Moscow**. Новый canonical numeric DSP/UI/device
SDK продукт. Порядок и ownership — [vertical-delivery-plan](vertical-delivery-plan.md);
статусы — [progress-checklist](progress-checklist.md); ограничения native UI —
[codex-plan-tracking](codex-plan-tracking.md). Меняется существующий план, новая
документация для микрозадач не создаётся.

## Authority и рабочий процесс

Прямое указание пользователя заменяет прежние legacy/TDD/gates требования:
реализация крупных автономных блоков R/P/G/T/E/K → I composition и actual build →
независимый code audit готовых блоков → owner fixes → FINAL после общей интеграции.
Промежуточные тесты/TDD/characterization отменены. Новые тесты, адаптация retained
oracles новой системы и согласованные полные проверки выполняются на FINAL.
Исторические test reports остаются фактами своего scope, не обязательными repeat gates.

Каждый владелец сам выбирает реализацию и последовательность в своих файлах.
Один итоговый отчёт или неразрешимый в его границах блокер. Root организует работу,
не пишет код и не утверждает каждую функцию/API. Общий API/ownership конфликт
решается напрямую с владельцем одним конкретным сообщением. Независимые направления
не ждут legacy или промежуточного author GREEN. I единственный CMake/shared Ninja
владелец, согласует только необходимое короткое окно чтения changing compiled inputs.
Контекст существующих чатов сохраняется. Все mandatory software deliverables приняты root после I integration и трёх FINAL reviews/latest compact amendments; software V1–V4/FINAL completed, manual/hardware отдельно.

Перед implementation/review каждый новый чат читает AGENTS.md и полностью
[закреплённый Clean Architecture / DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).
Справочник определяет границы ответственности, а не обязательный набор паттернов.
[architecture-map](../architecture-map.md) описывает ответственность компонентов.
Назначенные файлы сохраняют ownership; чужие edits не откатываются.
Без установок, веток, коммитов, push, ZIP, новых micro-docs и Markdown UI fallback.
Пользовательские config.json/output не заменяются.

## Текущий scope и крупные результаты

Единственный путь — canonical numeric. Legacy старых режимов/configs/schemas/wire,
старый Qt protocol, compatibility bridges, up/down migrations, inverse, future
backport и schema1 goldens исключены. P/R shared legacy helper grant отменён.
Устаревшие реализации/tests снимаются узко их владельцами, registration — I;
массовое удаление тестов/архивов не разрешено. Новые format versions/reject future,
один JSON, exact types, lossless values, atomic save/apply, revision/session ownership,
checked admission/bounded memory и semantic type compatibility обязательны.

| Этап | Результат | Статус/поставка |
| --- | --- | --- |
| N0 | Общие dtype/shape/semantics/layout/schema, Converter policies/caps и exact ownership | Готовые контракты используются; новые общие API согласуются владельцами напрямую |
| N1 / V2–V3 | Fixed dtypes/rank0..2, Converter/DSP/routing/count/history, checked budgets/noalloc | Completed: current software implementation/integration/independent evidence accepted root; hardware/manual отдельно |
| N2 / V1–V4 | JSON/storage/projection, DATA, BOTH codecs/control/session | Completed: current software implementation/integration/independent evidence accepted root; hardware/manual отдельно |
| N3 / V1–V4 | Editor/renderer/device/NVS/SDK/GUI/CLI | Completed: current software implementation/integration/independent evidence accepted root; hardware/manual отдельно |
| N4 / FINAL | Current affected integration, независимая проверка compact ESP32 и global acceptance | Completed: current software implementation/integration/independent evidence accepted root; hardware/manual отдельно |

Ownership и точки подключения подробно в vertical plan. R владеет назначенным
numeric runtime/application API packet, P canonical storage, G renderer/ports,
T также разрешёнными NEW numeric_remote_endpoint/numeric_control_server h/cpp,
E embedded, K codecs. I владеет local engine/GUI/CLI roots, CMake/manifests/SDK/build
и baseline-and-ownership; этот workflow-чат baseline не редактирует.
Bounded Foundation/CONTROL acceptance не распространяется на целые V-пакеты.
Whole V1–V4/FINAL completed по root software acceptance; аппаратная приёмка не заявлена.
## Утверждённые контракты узлов и обработки

Portable C++20: signal_contracts, dsp_kernels, dataflow_runtime, bdsp_protocol, domain/application,
embedded_support не требуют Qt, OS sockets, miniaudio, ESP-IDF, RTTI/exceptions. Платформенные реализации
собираются в adapters/composition roots. Единственный исполнитель — PreparedGraph/GraphExecutor;
GraphLifecycle использует preparation/runtime ports. JSON и I/O не входят в prepared processing.

Configured node/port descriptor общий: stable identity, index, direction, semantic type, quantity/unit.
Sources не имеют входов, processors принимают/выдают, sinks не имеют выходов. Все views — строгие sinks,
их display channel/reduction не меняют DSP routes. Каталог и контекстное меню используют одну роль/descriptor.
Форма не заменяет тип: PCM, window/windowed, stereo-spectrum, spectrum, Mel, History, Scalar,
WLED packet не соединяются без явно совместимого контракта.

Поправка numeric authority: canonical Source/Processor/Sink, RU Источник/Обработчик/Получатель;
каталог Источники/Обработчики/Выходы. Consumer — историческая терминология; stable IDs сохраняются,
Старые aliases/migrations не входят в текущий scope. Общий descriptor имеет независимые semantics,
numeric element, rank/extents, representation/layout, coordinates/time и structured schema identity.
Numeric types: Int8/UInt8/Int16/UInt16/Int32/UInt32/Int64/UInt64/Float32/Float64; byte=UInt8 alias,
platform int не public contract. Scalar rank0 — одно число любого dtype; Vector rank1; Matrix rank2.
Rank3 — future extension; 3D display показывает Matrix. Channels/axis/planar/interleaved/endian/
alignment/stride явные, не hidden stereo float. FFT/Mel bins/bands и History [time,bins] сохраняют
семантику; structured WLED/custom packet не совместим с UInt8 Vector без schema-aware processor.
PCM valid bits16/24/32 отделены от storage: packed24 против valid24-in32 с explicit sign/bit placement,
channel order/layout/rate/timebase/normalization. Source capture/device adapter объявляет actual format.

Domain/editor/import/control запрещают несовместимые связи и implicit cast/Converter insertion.
Explicit Processor Converter задаёт input/output dtype/layout и rounding, overflow/clipping, scaling,
nonfinite, precision-loss, endian/pack/unpack policies по numeric-data-contracts §2. PCM→FFT→Mel —
отдельные semantic DSP processors. Достаточен compact descriptor/typed access/prepared specialization,
без generic Tensor framework, per-element virtual boxing или дублированных type registries.
Prepare готовит Converter output/scratch и проверяет advertised combination/kernel support и checked
byte-width/stride/alignment/channel/retention/staging memory до working mutation. Universal dtype
contract не обещает все kernels для всех dtype/device; borrowed view не сохраняется async.

Splitter fanout 2–8; Switch только 1→N, 2–8 выходов, selected с нуля; Gate один вход/выход.
Known inactive с известными контрактом/часами выдаёт typed silence; unknown распространяет unavailable.
History inactive добавляет новую zero row, сохраняет старые строки. Explicit Stop/device change отделены
от rewiring; только несовместимое затронутое состояние сбрасывается.
History хранится [кадр][полоса], GUI update_frame_history преобразует для renderer; возраст newest равен нулю,
индексы не называются Hz/seconds без координат/меток. Hot path не растит контейнеры и не копирует всю history.
FrameView borrowed; async queue использует bounded owning buffer.

ScalarReduce: maxabs, signed mean, RMS; оба stereo channels; History только latest row.
Spectrogram port 0 — History, port 1 — Scalar filled count. Identity выхода node+port сохраняется в callbacks,
snapshots, previews, subscriptions и unavailable; scalar не перезаписывает history того же node.
Numeric scope: ScalarReduce объявляет result dtype/accumulation precision/population; count Scalar
UInt32/UInt64, exact64 не проходит через float/double. Accumulator явно задаёт input/output dtype/
shape/axes; Window/Spectrogram сохраняют semantics/channels. Splitter/Switch/Gate exact contract
всех outputs; silence учитывает unsigned PCM bias и packet encoding; reset только incompatible state.
Plots Sinks без processing output/implicit conversion. Renderer-only conversion вне DSP имеет bounded
declared cost/precision loss; integer preview сохраняет exact value/text и сообщает display loss.
WLEDEncode — pure encoder отдельно от sink/destination; старый WLED bridge не поддерживается.
Новый WLED output задаёт wire, cadence, send counter и stale silence; утверждённый черновой stale threshold 300 ms.
WLED wire не должен тянуть device-specific формат в пакет общей BDSP framing responsibility.

## Управление, codecs и хранение

Typed application commands: capabilities, state, prepare, apply, set parameters, start, stop, explicit persist.
Запрос имеет request_id/expected_revision; conflict/validation/unsupported/limits — atomic reject,
working graph/revision/resources остаются согласованы. Portable команды не содержат Qt/JSON widgets.
Typed Start/Stop reject атомарно сохраняет lifecycle state, run intent, working graph, revision
и ресурсы; intent/state/revision фиксируются после успешного перехода, повтор состояния — accepted no-op.
Prepared storage — **один глобальный slot, max 1**, token принадлежит session + base revision,
graph identity/executor. Foreign Prepare при занятом slot — Busy, старый token не меняется.
Invalid candidate сохраняет прежнюю valid preparation; успешный Prepare владельца заменяет её атомарно.
TTL token — 30 s по host monotonic clock. Apply чужого, expired, stale revision или foreign bundle
отвергается до commit; disconnect освобождает session slot, reconnect не переносит владение.
Смена revision делает прежнюю preparation stale. R владеет lifecycle/runtime invariants,
T/E — session/revision/token orchestration в назначенных adapters; никаких per-session дополнительных prepared slots.
ControlCodec интерфейс: caller-owned buffers и explicit limits; decode сначала проверяет boundary,
application/domain проверяют смысл. CBOR default negotiated, Protobuf рабочая альтернатива.
TinyCBOR v7.0 и nanopb 0.4.9.2 фиксируются commit/hash; sources generated checked in,
обычный SDK build без codegen; воспроизводимая генерация документирована.
Конкретные dependency hashes фиксирует I для actual SDK/composition;
названия version сами по себе не достаточный pin.

Новый canonical BDSP DATA/control использует общие application commands.
TCP/UART parameter edit не reconnect unaffected bindings; resource reconciliation по stable identity и
resource-affecting config. Регистрация capabilities должна поддерживать новые semantic types.

BOTH CBOR/Protobuf выражают один новый numeric DTO с exact Int64/UInt64.
DATA явно кодирует endian/stride/shape/schema; DATA4 Native bytes refusal сохраняется.
Новые processing/JSON/wire versions различаются. Schema1 и legacy float goldens
не обязательны. JSON64 использует lossless encoding без QJson double narrowing.
Unsupported format отвергается до mutation без silent cast.

Один JSON рядом executable; CLI/user explicit path имеют приоритет. Логические части:
processing, presentation, app, presets, draft/applied; schema version отдельно processing/wire.
Malformed/future version запрещает apply и overwrite. Старые форматы не мигрируются.
Один atomic save сохраняет unknown fields и stable IDs, defaults не перезаписывают explicit overrides.
Runtime/history state не является проектом.
Processing projection исключает UI fields в обоих направлениях; FFT display params относятся presentation.
Pending layout привязан к accepted request/revision/identity; reject/timeout/new request не оставляет stale layout.

## Qt и embedded

ScalarView — bounded display history 128; hidden skip render; gap marked; Stop clears; duplicate,
discontinuity и first-zero identity корректны. Packet preview декодируется read-only, содержит status fields,
metrics и units; preview не меняет node params и не синхронизирует native spinbox internals.
RU/EN через ui::text/ui::bind, только ui::Theme и semantic palette roles, Light/Dark/System;
active/inactive/disabled, popup/menu/dock separators/editor/OpenGL labels. Existing dock gestures сохраняются.

ESP32 объявляет actual capabilities до prepare; configurable limits, default FFT 2048 per channel,
history off. TCP/UART command service использует общие application/runtime/codecs.
Explicit save → NVS versioned processing config, restore last known good; slider не пишет flash.
Host предоставляет network provisioning. IDF adapters владеют platform resources, portable library — нет.
SDK/export/components используют те же portable sources. Не устанавливать новые platform toolchains,
не дублировать SDK, не hardcode compiler paths; брать существующие environment/cache.
Numeric device caps включают actual dtype/rank/layout/kernel/backend/converter policies и byte pool/
stack budgets. Optimized Float32 backend, default2048 и explicit4096 configs сохраняются. Общий
Float64 контракт не обещает ESP32 Float64 FPU/FFT64; unsupported dtype/conversion/memory reject
предшествует working mutation. Host archive build не является hardware/taskstack acceptance.


## Требования, проверенные при software приёмке

Unavailable должен доходить до всех node+port outputs; History/count не перезаписывают
друг друга. Prepare не использует скрытые desktop caps. Registry принимает новые
numeric contracts. Resource reconciliation сохраняет unaffected network bindings.
FFT display params исключены из processing. Malformed/future version отвергается
до mutation; pending layout не переживает reject/timeout/new request. Каталог/ports,
ScalarView lifecycle и read-only packet preview согласованы с новыми descriptors.
WLED wire находится в adapter/output boundary. Full-copy/queue/alloc/task-stack
обязанности остаются реальными требованиями. Требования сверены root с итоговой evidence; mandatory software fixes приняты, manual/native ограничения отдельно.

Проверенная default конфигурация заменяет прежний optional96KiB пример: CONTROL wire4KiB/primary arena8KiB, packet1024/read512; полный stereoFFT2048 supplied320KiB принят в host/fake scope. External memory не подразумевается; physical heap/stack остаются manual UNVERIFIED.

## Итоговая software приёмка — completed

**2026-10-07, Europe/Moscow. Root FINAL ACCEPTANCE:** все обязательные software
deliverables актуального canonical scope приняты; actionable findings отсутствуют.
V1, V2, V3, V4 и FINAL completed. Аппаратная/native приёмка этим решением не заявлена.

Root полностью прочёл текущий
[I combined report](../../build-refactor-integration/numeric-combined-integration-report.md),
оба independent compact amendments к
[architecture/SDK report](../../build-final-architecture-review/review-report.md) и
[runtime/protocol/storage/device report](../../build-final-runtime-protocol-review/review-report.md),
FFT/ownership/remote Persist fixtures/logs и deployment hashes. Прежняя
[desktop independent acceptance](../../build-final-desktop-review/review-report.md),
включая P1 incarnation closure, сохранена. Контракты/runtime/control/storage/Qt/embedded/
FINAL сверены с этой evidence и latest compact.

| Проверенный результат | Итоговый evidence scope |
| --- | --- |
| Тесты | Full batch44/45 сохранил RED; test-only fix focused1/1 author0.25s и independent0.14s. Итого45 cases verified across batches; полного45/45 log нет. Registered session independent1/1 и current component6/6 PASS |
| Desktop delivery | Actual current deployment build-gui/bin: 3 exe hashes совпали, 16 DLL +16 plugins, 0 mismatches |
| SDK/native | SDK install consumer accepted; current Xtensa C++20 archives/UART object build0 и независимая перепроверка accepted в software/crosscompile scope |
| Compact default | Full eight-node stereo FFT2048 TCP/UART, BOTH CONTROL, ExplicitPersist/restore accepted на host/fake resources с supplied320KiB; исчерпанный old+new replacement честно rejected атомарно |
| Extended profile | Explicit1024KiB FFT4096 и atomic replacement accepted; бюджет не подразумевает наличие такой памяти на обычной плате |
| Runtime/codecs | R compact ownership/runtime и K exact measure affected integration + independent analytic FFT/ownership/remote persistence accepted |
| Решение | Все mandatory software deliverables приняты root; whole V1–V4/FINAL completed, физические/manual проверки отдельно |

Проверенный default: CONTROL wire4KiB, primary arena8KiB, packet1024, read chunk512.
Shared cold workspace разделяет decode/NVS phases; immutable capabilities и exact
BOTH reply preflight предшествуют preparation. Actual supplied memory ограничена
configured ceiling; PSRAM не является обязательной portable зависимостью.
Host ledger326522 <=327680 bytes включает full32KiB stack, cold owner/DTO snapshots;
1158 bytes headroom — ledger, а не измерение heap платы.

## Morning/manual проверки — UNVERIFIED

- [ ] Physical heap/allocator/contiguous memory/stack на реальном ESP32.
- [ ] Firmware ELF/link, flash, power-cut NVS, реальные UART/TCP и WLED timing.
- [ ] Native GUI/OpenGL/Light-Dark-System/themes/popups и physical capture.

Host/fake-device/crosscompile/offscreen PASS не закрывают эти пункты.
Коммиты/push/новые установки и замена пользовательских config/output не выполнялись.
## FINAL и независимая приёмка

I обязан передать собранные actual desktop и SDK, используя доступные environment/cache.
На FINAL после всех integrations новые tests и retained new-system oracles покрывают:
ten dtypes/rank0..2; PCM24/exact64/endian/stride/channels; Converter overflow/NaN/
rounding/normalization; semantics/axes; typed links; ownership/lifetime/noalloc/checked
memory; atomic prepare/apply/save/revision/session; BOTH codecs/new DATA; JSON/presets/
projection; editor/renderer/device caps/NVS. Legacy-only tests не восстанавливаются.

Prepared contract проверяет global max1 slot, foreign Busy, valid preparation при
invalid candidate, token ownership/TTL30s/stale revision, atomic Start/Stop и сохранение
working/resources при отказе. Это требования реализации, промежуточные тесты не нужны.
Новые независимые architecture/runtime/protocol/storage/desktop/UI/device аудиторы
читают actual готовый код и выполняют финальные проверки. Findings исправляет owner;
author GREEN не заменяет явную acceptance. Hardware/native/task-stack ограничения
фиксируются; host/offscreen их не доказывает. Linux без отдельного запроса не проверять.
Тестовый сигнал: 5–10 меняющихся тонов в pipeline без звука/музыки.
Отчёт владельца один: сделано, точки подключения, реальные ограничения.

## Исторический снимок до изменения scope

### Исторический optional device-bound пример (заменён verified default)

Уточнение numeric device bounds (root → I): CONTROL16KiB был implementation bound,
а не пользовательским hard constraint. Допустим честный configurable full RAW +
spectral 8-node профиль: CONTROL96KiB/arena64KiB/text32KiB/packet4096 при объявленных
actual limits и доказанном memory budget, без gate bypass или сокращения scope.
Общий 2MiB external budget нельзя считать доступным обычному ESP32: capabilities
объявляют фактическую storage, без неё admission reject предшествует working mutation.
Малый RAW16KiB профиль — RAW-only; full2048/channel resource/default example проверяется
отдельно. Portable SDK не требует PSRAM. No-external-runtime/hardware proof остаётся
pending; малый RAW не заменяет требование полного pipeline. I/E/T и независимые
reviewers реализуют и проверяют это в своих границах самостоятельно.


### Исторический compact snapshot до окончательной приёмки

Все whole owner implementation блоки R/P/U/G/T/E/K готовы. Desktop P1 закрыт
независимой приёмкой; все три прежних FINAL review приняты root только в назначенных
bounded границах. Оставшийся реальный блок — compact ESP32, затронутая интеграция
и независимая перепроверка текущего состава. Whole V1–V4/global не completed.

| Направление | Актуальный результат | Что осталось |
| --- | --- | --- |
| Прежняя architecture/SDK приёмка | [FINAL report](../../build-final-architecture-review/review-report.md) принят root: A1–A5 closed, installed SDK/consumer/source/crosscompile в перечисленном scope | Новый compact R/K/T состав требует affected build/SDK/native refresh и независимой перепроверки |
| Прежняя runtime/control/storage/device приёмка | Official FINAL report прочитан root; bounded functional/fake-device scope и latest incarnation приняты | Runtime reviewer сейчас выполняет независимые текущие binaries и analytic FFT oracle для compact ESP32 |
| Desktop приёмка | [Amended FINAL report](../../build-final-desktop-review/review-report.md) принят root; P1 same-device incarnation закрыт, bounded current reconnect/UI/editor/presentation/storage scope принят | Native UI/hardware ограничения сохраняются; прежнее закрытие не подменяет независимую проверку нового compact состава |
| Compact ESP32 I author integration | I active turn: full stereo FFT2048/320KiB по TCP и UART + BOTH CONTROL + ExplicitPersist/restore author PASS; replacement under320 честно rejected без mutation; extended4096/replacement проходят | Implementation/integration evidence, НЕ independent acceptance; runtime current binary/analytic oracle verdict и обновлённые I SDK/native evidence ожидаются |
| R/K compact реализации | R compact runtime ready; K exact Codec::measure BOTH canonical wire/scratch/arena/aggregate text, noalloc/no writes/aligned decode footprint и arena_bytes enforcement ready | Affected final build/rereview не закрыты; прежняя codec acceptance относится к прежнему snapshot |
| Итоговое решение | Автономные владельцы, I единственный shared build owner; независимые проверки текущего compact состава идут | Координаторская whole/global приёмка после current integration и независимых verdict; whole checkboxes открыты |

2MiB external storage нельзя предполагать на обычном ESP32. Actual capabilities,
memory/storage, atomic rejection при нехватке и full2048 default profile остаются
требованиями. Малый RAW-only профиль не заменяет full pipeline; scope не сокращён.
Native hardware/heap-stack/firmware-link proof остаётся UNVERIFIED, host/fake-device/
objects/crosscompile evidence не расширяется до аппаратной приёмки.


### История предыдущих FINAL статусных снимков (ненормативна)

ESP completion implementation active: I coordinates R DTO ownership/shared immutable zero + T phased CONTROL reuse/lazy NVS; gap confirmed open. K Codec::measure(Message, Limits) ready in five owned files: BOTH canonical wire/scratch/arena/aggregate-text sizing, no allocations/writes, same aligned decode footprint, arena_bytes enforced; git diff check only, I build/current independent verification pending. Earlier bounded codec acceptance covers its earlier snapshot only.


Текущее desktop закрытие: root прочёл amended FINAL report current requirements/
findings/evidence; formal reviewer completed. Принят bounded P1 reconnect scope:
current presenter/adapter oracle compile0/run0 и independent runtime copied/rebuild0;
same graph/lower epoch, observed/coalesced retirement, first Frame/History before
snapshot, compatible revision no clear. Current affected4/4 author и independent
app normal/rejected/editor/workspace + engine8tones current libraries успешно перечитаны.
Три FINAL reviews теперь bounded ready/accepted; I retained new test1/1 ready report
прочитан. FULL global completion остаётся открытой только по actual ordinary ESP32
profile/completion requirements; I active memory investigation. Whole packages не закрыты.


Актуальная функциональная приёмка: root полностью прочёл official FINAL
runtime/protocol/storage/device review report и reconnect log failures0,
persistence54, affected4/4 current PASS. Принят перечисленный bounded
runtime/control/storage/fake-device scope; latest incarnation independent PASS.
Desktop formal closure pending. Global acceptance остаётся открытой: ordinary ESP32
no-external profile gap направлен I/E — fixed CONTROL1120KiB + adapters64KiB +
stack32KiB превышает ordinary internal RAM ещё до processing. Fake2MiB/low-heap reject/
source SDK PASS не доказывают default full2048 без PSRAM. Требуется честный actual
profile/result; no-external runtime proof pending. Scope полного pipeline не сокращён.


Актуальная архитектурная приёмка: root полностью прочёл UPDATED
[final architecture report](../../build-final-architecture-review/review-report.md)
после official FINAL ready. Принят назначенный bounded architecture/source/installed
SDK/crosscompile scope: A1–A5 CLOSED, включая effective native C++26 override → C++20.
Installed72canonical/36public/11composition exact; RAW/2048/4096 consumers zeroalloc512ticks;
History140 exactly1row/noalloc; exported find_package consumer exit0.
Actual57-step Xtensa SDK/NVS/TCP/UART objects build0 independently rules/elf32 verified.
Runtime/control functional review и reopened desktop P1 pending.
Whole V1–V4/global не приняты; hardware/heap-stack/firmware-link proof не расширен.


Текущая коррекция статуса: desktop bounded review REOPENED P1 selected same-device
incarnation. GuiPresenter проверяет только graph equality; empty-graph retirement
может coalesce, old epoch блокирует new lower epoch. I реализует explicit incarnation
reset до frames, сохраняя docks/camera/settings; G empty-graph node views fix ready.
Independent desktop reviewer 01a11750-a4d7-7ab2-bd61-fe57d8a2796a ACTIVE targeted
rereview. Прежние44/44 и bounded desktop PASS сохраняются только за прежним snapshot,
не закрывают новые changes. I сообщает current SDK/native Xtensa TCP/UART build0;
root final report/log inspection pending. Whole V1–V4 не закрыты.


Текущий финальный evidence-блок: root прочёл actual numeric-final-all-ctest.log —
100% tests passed, 0 failures/44 tests, 7.78s. I подтверждает current desktop build
и stock host RAW/spectral2048/4096 в прежних plan/stack bounds.
Root полностью прочёл [architecture report](../../build-final-architecture-review/review-report.md)
(20:43:44): bounded source/host PASS, A1 pin/A2 installed incremental140appends
zeroalloc/A3 C++20/A4 stock profile admission CLOSED, exact72source/36header hashes,
installed consumers/no Qt/RTTI/exceptions PASS. Reviewer active, I latest reconnect
installed SDK/native Xtensa refresh и final consolidated acknowledgement pending;
runtime reviewer active final reconnect recheck. Native hardware/heap-stack/firmware
link UNVERIFIED. Whole V1–V4 не закрыты.


Текущая integration evidence: root полностью прочитал
[combined report](../../build-refactor-integration/numeric-combined-integration-report.md).
Production core_app/pipeline_cli/registry build0; portable C++20 SDK без exceptions/RTTI
build/install и installed canonical consumer run0; silent CLI 8 tones/750ms exit0;
registry metadata и actual CONTROL discovery/disconnect 2/2 PASS.
Actual Xtensa SDK/NVS archive0 предшествует latest R/E и требует refresh;
current UART/TCP TUs host fake-IDF compile0 не доказывает hardware/task-stack.
Все R/P/U/G/T/E/K whole owner blocks READY. R latest cold optional kernel plans/resource dimensions и incremental History Converter авторски GREEN; Current desktop build и final CTest44/44 PASS (7.78s); bounded architecture A1–A4 independently closed. I latest reconnect installed SDK/native Xtensa refresh и consolidated reviewer acknowledgement pending. U preview интегрирован; bounded desktop/storage independent acceptance сохранён. WLED wiring и stock ESP NVS restore/retire остаются integration pending.
FINAL active: R/E/P/U current-API canonical test adaptation; independent reviewers
выполняют собственные fixtures. GUI original stale graph revision oracle bounded PASS,
expanded run1 RU WLED caption открыт. Whole integration/acceptance не подтверждены.




V1 composition GREEN относился к прежнему snapshot:
[report](../../build-refactor-integration/v1-composition/integration-report.md).
Поздний actual save_preset FAIL98 expected2/read1 и rejected/config PASS2:
[report](../../build-refactor-integration/v1-presets/integration-report.md),
[ctest.log](../../build-refactor-integration/v1-presets/ctest.log).
P visual3 repair author-ready, actual RED → focused GREEN, I closure ACK;
whole V1 acceptance не получена. Pending intermediate oracle repeats отменены.
Bounded CONTROL mask accepted: [review](../../build-gate-numeric-control-review/mask-rereview/review-report.md),
P2 closed, 66 checks/0 failures, 11-file snapshot unchanged; не whole V4 acceptance.
Исторические compatibility PASS не доказывают canonical-only продукт.
## Исторический журнал evidence и прежних процессных ограничений

Весь журнал ниже ненормативен для текущего scope. Legacy/migrations/TDD/старые gates здесь описывают только прошлую работу; результаты не переписаны и не расширены.

Записи ниже читаются по дате и точному freeze/scope. Их лимиты worker-слотов,
ожидания builds и NOT STARTED описывают тот момент, не текущие назначения.
Актуальные V1–V4, параллельные направления и ресурсные ограничения — в начале
этого плана, vertical-delivery-plan и progress-checklist. Исторические PASS/FAIL
сохраняются; новые назначения не расширяют их acceptance.

## Slice 2 — bounded D2 integration status, 2026-10-06

Coordinator authorized I build wiring and only the frozen D2 helper tests while
R2/T2 implement their sources. wled_protocol target/export/install/ESP manifests,
scalar_reduce dsp_kernels source, transitional old WLED include dependency,
runtime codec dependency and final graph_domain desktop DTO dependencies are wired.
T2 port_delivery_codec.cpp is registered but awaited; desktop/runtime compilation
is held until the assigned owners freeze. Own no-Qt C++20/no RTTI/exceptions build
and assertions-enabled scalar_reduce_tests + wled_protocol_tests passed 2/2.
Exact logs, hashes, commands and remaining integration are in the baseline ledger.
This is I author integration evidence, not independent Slice 2 acceptance.
I D2 WIRING READY / IDLE; SDK consumers and whole desktop build/deploy/tests pending.

### Slice 2 portable integration complete, desktop pending

After coordinator-confirmed T2 portable codec partial freeze, I built the exact
nine portable targets/tests: 9/9 PASS; SDK library build/install exit 0;
five external installed-package consumers PASS 5/5 with C++20/assertions/no
RTTI/exceptions. Checked installed source manifest/helper/runtime/BDSP hashes
match current files. Interim node_routing link failure is resolved by its PRIVATE
BDSP test dependency, without reintroducing runtime→BDSP. Ledger has exact evidence.
I PORTABLE READY / IDLE. T2 desktop freeze, whole desktop/DLL build/short integrations
and independent Slice 2 acceptance are still required; full goal remains incomplete.

### Slice 2 partial independent acceptance — D2 only

Coordinator received independent FINAL PASS for the seven frozen D2 helper/API/test
files from reviewer 01a112e6-fce8-7fa0-94d2-d2439609b6ca. Actual evidence read:
build-gate2-device-review/review-report.txt, review-final.log, source-hashes.txt
and run-review.ps1 (five direct compile/run cases, all exit 0). No actionable
product finding. Exact independent scope/evidence is recorded in the baseline ledger.
This partial PASS covers numerical reduction, pure WLED codec and forwarding
compatibility only; it does not accept R2 runtime or T2 destination behavior.
GATE 2 NOT ACCEPTED: independent R2 review is active in
01a112ea-e77b-7043-bdfa-e523b72a76bd and T2 implementation remains active.
I portable 9/9 and SDK consumers 5/5 are separate author integration evidence.
No additional build/source/commit actions; I DOC UPDATE COMPLETE / IDLE.

### Slice 2 independent R2 review — FAIL, repair/re-review gate

Actual build-gate2-runtime-review/review-report.md records F1 P1: consecutive
closed History Gate/Switch ticks resurrect prior silenced rows; F2 P2: removal
of an unused Splitter output resets surviving downstream histories. R2 owns both
repairs and focused regressions; its source freeze is released. Independent
run-repro.ps1 compile exit 0 / repro exit 1 reproduces four failed expectations,
although the existing reviewer targets passed 7/7. Exact scope/evidence and
required regression dimensions are in the baseline ledger.
D2 partial independent PASS remains valid; T2 implementation remains active.
I portable 9/9 and SDK consumers 5/5 now describe the pre-fix integration snapshot,
not accepted R2 behavior. GATE 2 NOT ACCEPTED. R2 READY/FROZEN handoff, assigned I
checks and independent repeated review are required before acceptance.
Doc-only update; no builds/product source fixes/commits or future implementation.
I DOC UPDATE COMPLETE / IDLE.

### Slice 2 R2 authorfix integration current

R2 section 13 authorfix source freeze consumed. Own I portable exact nine cases
PASS 9/9 (0.22 s), current SDK build/install exit 0, five installed external
consumers PASS 5/5 (0.17 s). Six frozen source/test hashes match; installed
runtime/signal sources and public headers match published authorfix hashes.
No source/fixture edits; actual logs/hashes in baseline ledger. This is current
author integration evidence, not independent repeated acceptance of F1/F2.
T2 desktop freeze/review and whole desktop integration remain pending;
GATE 2 NOT ACCEPTED. ProposedSlice3 documentary dispatch is ready/NOT STARTED.
I CURRENT PORTABLE READY / IDLE.

### Slice 2 current partial R2 acceptance and T2 integration hold

Coordinator accepted limited independent R2 FINAL PASS from actual
build-gate2-runtime-review/rereview-report.md: F1/F2 closed, unchanged repro0,
separate24-tick oracle0/noalloc, affected7/7 PASS, six frozen hashes match.
T2 must still fence local accepted delivery identity before apply_history_route
and retain owners during borrowed reading; that caller behavior is not R2 acceptance.
D2 partial PASS unchanged. GATE 2 NOT ACCEPTED.
I desktop apps/test binaries build+DLL deployment GREEN after own CMake codec
wiring repair. Portable repaired codec test1/1 PASS. Desktop exact13 stop-on-failure
run attempted4: codec/legacy transport/capabilities PASS, engine_typed_views FAIL
"Default graph preparation failed"; remaining9 not run. T2 owns narrowly released
source fix; all further builds/tests held until re-freeze. Exact commands/logs/XML,
EXE/DLL paths and capture hashes in baseline ledger. Offscreen not native acceptance.
ProposedSlice3 documentary map ready/NOT STARTED. I INVENTORY/DOC COMPLETE / IDLE.

### Slice 2 Qt macro integration repair pending, 2026-10-07

Coordinator identified BufferPool public constructor parameter slots being expanded
by Qt's slots macro, yielding an empty slot span and exhausted5 during default
preparation. R owns narrow header repair; T owns startup --default-preparation-only
fixture/diagnostics. I read-only verified both current locations; focused CTest
registration/checks await next frozen handoff. No builds or R/T source edits.
Prior independent R2 algorithm/F1/F2 partial PASS remains scope-limited; new header
fix needs narrow independent Qt-macro repeated review and fresh desktop rebuild
of all header dependents. Both zones unfrozen, builds/tests held. Gate 2 NOT ACCEPTED.
I DOC UPDATE COMPLETE / IDLE.

### Slice 2 Qt header accepted; T2 Stop clearing integration FAIL

Actual slots-narrow-report.md read: coordinator accepts independent narrow R PASS
for current BufferPool public header; prior R2 algorithm/F1F2 acceptance remains.
Fresh all-dependent desktop rebuild/deploy GREEN after one transient archive-lock
retry (no competing live Ninja, old failure log preserved). New test-only ID13
fixture refreeze rebuilt; default startup focused PASS1/1. Normal engine_typed_views
then FAIL "remote stopped view did not clear". Further desktop14 suite held/routed T2;
no full-suite PASS. Affected portable3/3 and installed consumers3/3 PASS, SDK pool
header hashes match. Exact current hashes/commands/XML/inventory and failure logs
in baseline ledger. No foreign source patches. GATE2 NOT ACCEPTED;
ProposedSlice3 documentary ready/NOT STARTED. I REPORT READY / IDLE.

### Slice 2 T Stop repair current integration

Fresh desktop build/deploy GREEN on T Stop repair freeze; normal engine test
PASS standalone and in exact15 run. Source hashes match published T freeze.
The15 run stopped after six PASS at graph_preparation_transaction assertion179
(remote History latest_views differs from fed history); remaining8 NOT RUN,
including newly registered Stop-delivery case. Source finding routed T2; no source
patches/test weakening, further checks held. Exact current logs/XML/commands/hashes/
EXE-DLL inventory in baseline ledger. Portable/SDK unchanged, no reruns.
GATE2 NOT ACCEPTED; T fix/re-freeze/integration GREEN required before independent
T review. R/D partial acceptance unchanged. ProposedSlice3 ready/NOT STARTED.
I REPORT READY / IDLE.

### Slice 2 desktop current integration GREEN, independent T gate pending

T History repair sources frozen/current hashes verified; fresh five-app/nine-test
build+DLL deployment exit0. Original graphprep + focused externalHistory2/2 PASS;
full assigned16cases16/16 PASS24.72s, including all prior NOTRUN canonical/Stop/
registry/history/WLED cases. Actual hashes/commands/XML/logs/inventory in baseline.
This is author I integration evidence; fresh independent T source review required,
GATE2 NOT ACCEPTED by I. Prior R/D partial acceptance remains scope-limited.
Memory scope explicit: idle canonical cases operator-new0, active legacy owning
projections3304bytes at8/400 prove only capacity independence. Existing desktop
external matrix flatten/full owner copies and legacy owning allocations remain
FINAL SLICE5 removal/optimization/independent boundary-verification obligations;
whole processing zero-allocation/no-full-copy goal is not complete. Explicit full
query outside processing does not excuse canonical callback defects; reviewer checks.
No portable/SDK reruns/foreign source edits/tools/userdata/commits.
ProposedSlice3 ready/NOT STARTED; I CURRENT DESKTOP GREEN / READY / IDLE.

### Slice 2 independent desktop FINAL FAIL

Actual build-gate2-desktop-review/review-report.md read: F1 P1 agreed registry
stream limits replaced with Engine message/endpoints schema; F2 P1 empty complete20
snapshot for selected endpoint clears live canonical cache. Independent oracles
both exit1 despite frozen41 hashmatch and own16 regression PASS. Narrow T2 repair
ownership/re-review gates and remaining requirements recorded in baseline ledger.
O1 unknown feature criticality is undefined observation, not a third blocker;
clarify existing advertisement semantics without new wire.
Previous I16GREEN is historical pre-fix integration evidence, not accepted T.
Builds/tests held until T2 re-freeze; exact independent oracle repeats required,
then remaining acceptance obligations. GATE2 NOT ACCEPTED; ProposedSlice3 NOT STARTED.
Doc-only update, no builds/source fixes/commits; I DOC UPDATE COMPLETE / IDLE.

### Slice 2 F1/F2 repaired snapshot integration GREEN

T2 nine-file freeze verified. Fresh five-app/nine-test dependent build + DLL deploy
exit0; two new complete-coverage/stream-limits cases2/2 PASS. Full agreed16+two
cases18/18 PASS25.56s. Current hashes/commands/XML/logs/EXE-DLL inventory in ledger;
no product source edits. O1 optional feature advertisements clarified without
new wire. This I author integration does not close independent desktop FAIL:
original F1/F2 oracles and remaining requirements need fresh independent review.
GATE2 NOT ACCEPTED; R/D partial scope unchanged, Slice5 memory boundary obligations
remain, ProposedSlice3 ready/NOT STARTED. No portable/SDK reruns.
I CURRENT F1/F2 DESKTOP GREEN / READY / IDLE.

### Slice 2 independent repeat — new F3 P1 reconnect incarnation

Coordinator reports original independent F1/F2 closed; new oracle
build-gate2-desktop-review/incarnation_repro.cpp/run-incarnation.ps1 compile0/run1
accepts DATAepoch77 after registryincarnation88 exact resubscription/ACK. Expected
registry incarnation is not passed NetworkRuntime→Subscriber; first new-session
DATA is insufficiently fenced. T2 owns narrow F3 repair and independent repeated
oracle gate; remaining desktop obligations not accepted. I18/18 integration GREEN
remains pre-fix evidence, not accepted T behavior. GATE2 FAIL/NOT ACCEPTED;
slices3–5 NOT STARTED. Doc-only update, builds held/no source fixes/commits.
I DOC UPDATE COMPLETE / IDLE until new T freeze.

### Slice 2 F3 snapshot current integration GREEN

Six frozen T F3 hashes match. Fresh five-app/nine-test build+DLL deploy exit0;
engine/stream incarnation focus2/2 PASS, full prior18+two20/20 PASS27.63s.
Current commands/hashes/XML/logs/EXE-DLL inventory in ledger. This author I
integration does not close independent F3 or accept remaining desktop obligations;
GATE2 NOT ACCEPTED pending repeated independent T review. No product patches or
portable/SDK reruns. R/D scope unchanged; slices3–5 NOT STARTED,
ProposedSlice3 documentary ready. I CURRENT F3 DESKTOP GREEN / READY / IDLE.

### Slice 2 independent F3 closed; F4/F5 block desktop acceptance

Independent repeat closes F3, but contract_repro.cpp reveals F4 P1 rejected
Stopped available epoch999 poisons accepted fences and drops valid11clear/12resume;
F5 P2 active fragment20 permits interleaved status7, violating section9.
Actual stopped-poison-result.log/interleave-result.log and fixture source read;
both oracle modes exit1. T2 owns narrow repairs and repeated independent oracle
checks. I20PASS is pre-fix integration evidence, not desktop acceptance.
GATE2 FAIL/NOT ACCEPTED, builds held until T freeze, slices3–5 NOT STARTED.
Doc-only update, no builds/product fixes/commits. I DOC COMPLETE / IDLE.

### Slice 2 F4/F5 current integration GREEN

Three frozen T repair hashes match; fresh five-app/nine-binary build+DLL deploy
exit0. New Stop-poison/fragment-interleave focus2/2 PASS; full prior20+two22/22
PASS28.49s. Current logs/XML/hash snapshot/commands/inventory in ledger, prior
failure evidence preserved. No product source edits. Author I GREEN is not
independent F4/F5 closure/remaining desktop acceptance; GATE2 NOT ACCEPTED pending
repeat independent review. R/D scope unchanged; slices3–5 NOT STARTED,
ProposedSlice3 documentary ready. No portable/SDK reruns.
I CURRENT F4/F5 DESKTOP GREEN / READY / IDLE.

### Slice 2 exact independent F1–F5 closed, whole T incomplete

Actual f4f5-rereview-report.md read: unchanged original oracles all0, I41+T3
hashmatch, F1–F5 bounded closure. Whole T INCOMPLETE/NOT ACCEPTED; I22PASS remains
author integration evidence. Publisher queue experiment stopped at reported
systemError/cybersecurity flag, no completed result or exposed detailed error;
coordinator forbids repeat/bypass. Limitation and remaining independent matrix
recorded in ledger. New reviewer01a11343-19cf-7b13-8fea-6fbe963352ae assigned only
local unit/static ownership/lifetime and fakeclock/faketransport WLED/no real sockets.
Source frozen, no I builds/fixes; primary reviewer idle. Gate2NOTaccepted,
slices3–5NOTstarted. I DOC UPDATE COMPLETE / IDLE.

Complementary independent contracts reviewer01a11344-2e6e-79b0-9d0a-04be7a83b9ce owns socket-free unit/static codec/registry ordering/COMPLETE/delta/fragments/limits/types/metadata/retry review in build-gate2-delivery-contract-review; complements ownership/WLED reviewer. Static/executed evidence separate, unexecuted UNVERIFIED. Real sockets/blocked experiment/source-copy networking workaround forbidden. Implementation frozen, Gate2NOTaccepted; I docs complete/IDLE.

### Slice 2 complementary independent acceptance/findings

Actual formal reports read. Accepted bounded ownership/lifetime/canonical retention
and fakeclock/faketransport WLED PASS,185product hashes unchanged; no real sockets,
no native timing/allocation proof. Contract review FAIL: C1P2 normative versions
executed4checks/3failures; C2P1 allports delta configured-membership admission gap
STATIC only/transport UNVERIFIED. T owns narrowC1/C2 repairs, shared pure public
admissionhelper approved; I builds held until freeze. Report refs/evidence/limits
in ledger. Queue arithmetic STATIC unresolved, blocked experiment remains untested/
repeat-bypass prohibited. WholeGate2FAIL/pending,22PASS pre-fixintegration only.
Slice5 legacy allocation/fullcopy obligations unchanged; slices3–5NOTstarted.
Doc-only update/no builds/product fixes/commits; I DOC UPDATE COMPLETE / IDLE.

### Slice 3 independent codec dependency groundwork

Coordinator explicitly authorized independent codec/storage work while Gate2
remains NOT ACCEPTED. Earlier blanket Slice3 NOT STARTED entries are historical;
this entry starts dependency groundwork only, not network-control integration.
TinyCBOR v7.0 commit6442e749ca811e24afad1551338a45c75e09808f and nanopb0.4.9.2
commit160d4f09e5fabb2b66aa2dea32d4f38ace2c4b3f are minimally vendored with exact
upstream blob/SHA256 manifests and licenses. Static TinyCBOR configuration headers
are explicitly marked derived. C99 caller-buffer runtimes build and pass one
CBOR integer/Protobuf varint roundtrip; undefined-symbol audit finds no allocator.
This is dependency evidence, not control-codec validation or hot-path proof.

Project-local explicit offline runner `tools/control_codegen/run.py` verifies
Python3.14.7 executable hash, official protoc36.2 and pure protobuf7.36.2 inputs,
and nanopb generator hashes. `--check-tools` PASS; no control output generated.
Normal CMake/build/install/firmware never invoke generator or network. Runtime
export and ESP source-package mappings prepared; generated C/header and codec
bodies remain K-owned and absent from build/test registration until freeze.
Evidence: `build-refactor-integration/control-deps/runtime-{configure,build}.log`,
`codegen-check.log`, `vendor-smoke.c`, `runtime-undefined-symbols.txt`.
Full desktop build remains held for T consolidated C3–C10 freeze; no firmware,
hardware, GUI, sockets, full suite, or blocked publisher experiment run.

Process lessons applied: target checks must establish a specific invariant;
collect owner freezes before one shared integration build; update acceptance docs
once per completed evidence batch, keeping author GREEN separate from acceptance.
I DEPENDENCY READY / IDLE; K implementation slot can start after coordinator handoff.

### Consolidated T C3–C10 / P S1–S2 integration complete

Current T17/P7 freezes SHA256 match before/after. I adapter/test CMake wiring
complete, fresh five-app + affected-library/test build and Qt DLL deployment0.
18 agreed socket-free tests PASS0.80s plus capabilities metadata-only0. Commands,
logs/XML, exact target closure, frozen hashes and executable/DLL/plugin receipts:
build-refactor-integration/consolidated-tp/integration-report.md. Live K control
zones excluded from source/hash/build; existing vendor/runner handoff unchanged.
Network Engine/stream/registry/TCP/UDP cases build-only/execution pending. Blocked
publisher queue experiment untouched; no native-pressure or C6/C7/C9 execution claim.
Single batch no product-source fixes, user config/output, hardware/audio/native UI,
tool install, firmware, full suite, branch or commit.

P independent rereview bounded PASS read and accepted by coordinator: S1/S2 closed,
original38/10/13 + adjacent26 + existing6 + manager path PASS, seven hashes unchanged.
WholeGate3 and strict production pending state remain open. Gate2 awaits consolidated
independent C1–C10 closure including source proof. Independent parallelSlice3 codec/
storage user override persists; dependent T3 network gated, Slice5 allocations and
fullcopy unfinished. Author integration GREEN is not independent acceptance.
I COMPLETE / READY / IDLE; coordinator can use reviewer slot.

### Frozen K3 codec integration and current transition decision

I CMake/ESP manifests now integrate real three-codec cpp/generated C/private include
and pinned runtimes; three assert/allocator-guard tests registered. K14/T17/P7hash
match, vendor/tools unchanged. Portable3/3PASS0.12s, desktop3/3PASS0.13s+fiveappDLL;
SDK install/export external BOTHcodec consumer noQt/noexceptionsRTTI PASS. Ordinary
host configure/build/install never invokes Python/protoc/generator/download.
ESP installed source manifest complete; actual existing optimized4096+transports
Xtensa codec/generated objects compile. Full ESP archive FAILED pre-existing domain
default_text aggregate initialization warning-as-error; owner error routed via
root without source edits/suppression. Full archive/firmware/nativehardware pending.
Exact evidence and gapmatrix: codec-integration/integration-report.md under common build.

Coordinator accepts Gate2 DEVELOPMENT TRANSITION AFTER independent codec acceptance:
C1/C2+C3–C10 bounded CLOSED in repair-recheck-report.md + prior common18checks; R/D/
ownershipWLED PASS unchanged. Whole-goal/nativeTCP/timer/queue acceptance not implied.
Keep socket/timer/replay/serial/slowpeer/partialwrite BUILD-ONLY/PENDING as final
automatic acceptance obligations; blocked native queue unverified/repeat forbidden,
C6/C7/C9 static effects not executed. Codec independent review pending; dependentT3
functional implementation gated, contractprep continues. Slice5 fullcopy/alloc
unfinished; boundedP3 accepted/wholeGate3pending; independentparallelSlice3 override
persists. I consolidated report complete/READY/IDLE; no previous18 repeat.

### Domain initializer bounded retry / K3 independent acceptance

R frozen graph_definition.cpp22338569D3F14EFC5C6CC7E390A7C78FBBAFE5DBEDBF4BD669D56433B2246D61
verified before/after, K14/T17/P7 preserved. Exact incremental ESP32 optimized4096+
transports build0/16actions now produces full SDK and three component archives;
no I source edits/warning suppression/new tools. Host domain build0/3actions,
domain1/1PASS0.04s, SDKinstall and installed graph/application consumer1/1PASS0.02s,
desktop five apps relink/DLL0/7actions. T3 live source excluded. Prior ESPRED retained
as historical; binary manifest/SHA receipts/exact commands in domain-retry/integration-report.md
under common build. Domain independent review later; full-archive build is not
firmware hardware/native taskstack acceptance.

Coordinator ACCEPTS K3 bounded independent report:20,934 public API probes/4tests/
installed consumer/two exact generations/vendor-tool pinsPASS,14hashstable/nofindings.
Stable DISJOINT caller storage required; runtime overlap rejection not approved
original requirement/not implemented. Encode3920B/callback2496B/decode1824B host
frames + nested costs carry to Slice4 taskstack composition, not ESP peak proof.
T3 phaseA newfile mapper/controller active; existing Engine/audio/network still
readonly until Rlegacycontract. WholeGate3/goal/native socket/timer/queue execution
and final gapmatrix obligations preserved; Slice5fullhistorycopy/alloc unfinished.
I bounded retry complete/READY/IDLE.

### Пользовательская numeric поправка / T3 impact, 2026-10-07

Предыдущие записи сохраняют evidence и решения в прежнем scope. Поправка N0–N4 выше отменяет
основание продолжать зависимую numeric implementation по float-only модели. T3 author сообщает
STOP numeric mapping: частичные `adapters/control/include/control/processing_message_mapper.h`,
`adapters/control/src/processing_message_mapper.cpp`,
`adapters/desktop/include/control/control_session.h`,
`adapters/desktop/runtime/src/control_session.cpp`, `tests/desktop_control_session_tests.cpp`
пока НЕ ПРИНЯТЫ. Owning requests/rebased views/session framing могут использоваться после проверки;
mapping nine FrameTypes→domain aliases,26kinds/uint32 parameters/старый DTO — только legacy
compatibility. Element dtype/rank/custom schema/packing/Converter отсутствуют. Ограничение
max_capability_types=9 и codec arena profile K1 требуют нового bound audit. Старый accepted K14
codec/schema/goldens PASS не является новым numeric capability/schema proof.
R legacy application/session policy независима от FrameType/rank/layout/params и может продолжаться;
её sizeof session evidence не доказывает typed payload memory. Частичные T3 files здесь не меняются.
Повторное использование T3 требует N1 contract, N2 schema/caps/arena/mapper tests, нового freeze/
integration и fresh independent review; handoff на зависимый numeric code выдаёт только root.
Typed foundation, explicit Converter, runtime checked byte buffers, GUI descriptors/display,
capture PCM16/24/32 и config/control/DATA schema migrations входят в текущую задачу.
Status: DOC CORRECTION READY; N0 agreement/API/IDs/ownership и N1–N4 implementation/acceptance
PENDING. Это не gate PASS и не пересмотр исторических bounded доказательств задним числом.
