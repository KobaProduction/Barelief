# Storage / processing control: контракт среза 0

Статус: gate 0 PASS по сообщению координатора; срез 1 S разрешён и реализован
в portable control области, авторские проверки описаны в §12; integration и
независимая приёмка среза 1 ещё обязательны. Baseline: `b18cfbf3c2ac62b4baf022de5348f5a29d46e2cc`. Незакоммиченный
код и `docs/typed-node-routing.md` — непринятый черновик, а не доказательство
выполнения. В срезе 0 изменялся только этот файл. Assignment среза 1 S:
новые `application/include/application/processing_control*.h`,
`application/src/processing_control*.cpp`, `tests/processing_control_tests.cpp`
и этот документ. Existing lifecycle/domain/runtime, config и CMake чужие.

Обязательные источники прочитаны полностью: `AGENTS.md`,
`docs/architecture-map.md`, `docs/typed-node-routing.md`, исходный запрос
`C:\Users\Koba\.codex\attachments\4887cc54-112e-4836-84d3-82950e988245\Вставленный текст.txt`
и [Clean Architecture / DDD, pinned revision](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).
Этот справочник задаёт границы ответственности, не перечень обязательных паттернов.

## 1. Baseline и карта черновика

| Граница | Наблюдаемое состояние | Требуемое решение |
| --- | --- | --- |
| `application/graph_lifecycle` | Portable prepare/apply через порты; pending сбрасывается перед новым Prepare; apply сохраняет working при ошибке; вызовы сериализует host | R добавляет typed stage Prepare без ранней очистки и transactional typed Start/Stop; historical behavior сохраняет explicit legacy policy R. S использует один executor, без duplicate run intent |
| `config/config_document.h` | Qt JSON read/resolve/overlay; standalone graph, pipeline/scene fallback, preset ID раньше имени, неоднозначность запрещена | Сделать явный version gate и мигратор до resolve/apply/save |
| `ConfigManager` | Один `QSaveFile`, overlay raw document, отдельные scene/pipeline graph; path сейчас задаётся CLI `config.json` | Сохранить атомарность/extensions, default рядом exe, явный путь имеет приоритет |
| `desktop/graph_document` | Baseline reader v2 после legacy migration; черновик добавляет processing_version, projection/restore | Не принимать черновую projection как контракт; отделить typed projection от JSON сохранения |
| `pipeline/graph.cpp` | Boundary v1→v2 добавляет Window/WindowFunction/BarkWeighting; legacy SurfaceView alias | Characterization прежней семантики, явная цепочка up/down; стабильные ID существующих узлов |
| `engine_client.cpp` | Qt `QDataStream`, commands 5 / replies 6 / snapshots 7 / views 8 / audio 9 / fragments 10; typed views 17; bounded pending, revision checks | Старые bytes/API оставить в desktop compatibility adapter; новый negotiated control codec |
| Черновик engine client | Один `presentation_graph` записывается до ответа и применяется к любому snapshot | Pending layout должен принадлежать запросу, device epoch, accepted revision и graph identity |
| `embedded_support` | Bounded stream Session, admission Profile, shared runtime; control/NVS ещё отсутствуют | Отдельная control session и persistence port; ESP-IDF ресурсы снаружи portable SDK |

Точные наблюдения: `processing_graph_document()` переносит весь `internal-data`
(включая FFTView channel/merge), использует `toInt(1)` для версии; текущий reader
по-прежнему регистрирует FFTView параметры как processing. Это дефекты черновика.
HEAD capabilities не содержит processing version; черновик добавляет только
одну версию. Ни один из этих фактов не подтверждает новый контракт.

Срез 0 не запускает приложение и не занимает общий build. Команды исследования:
`git rev-parse HEAD`, `git status --short`, `git show HEAD:<path>`,
`git diff --stat -- <scoped paths>`, `rg --files`, `rg -n`, `Get-Content` и
чтение pinned raw GitHub. Unit/integration и app build здесь не выполнялись:
изменён только документ. Их обязательные gates после реализации указаны ниже.

## 2. Модели и направление зависимостей

- Domain владеет configured node/port descriptor, parameter contracts,
  совместимостью и DeviceCapabilities. Этот чат не дублирует descriptor registry.
- Application владеет `ProcessingDTO`, typed commands/replies, revision и
  orchestration prepare/apply/persist через порты. DTO содержит portable значения
  и `domain::GraphDefinition`, не `QJsonObject`, encoded blob или widget state.
- `GraphLifecycle` использует существующие GraphPreparationPort/GraphRuntimePort.
  PreparedGraph/GraphExecutor — единственный plan/executor. Command service не
  содержит DSP, scheduler, истории или альтернативного runtime.
- Desktop JSON adapter владеет raw project document, миграцией и обратимым
  overlay; mapper переводит разрешённые processing поля в ProcessingDTO.
- Portable BDSP control codec владеет wire DTO/schema и byte codec. Wire DTO
  отличается от ProcessingDTO; mapper проверяет wire representation до domain.
  Codec не знает Qt, filesystem, sockets, NVS, runtime или GUI.
- Desktop и embedded controllers переводят wire DTO в application commands;
  composition roots собирают codec, transport, capabilities, storage и executor.
  Application/domain не зависят от codecs или vendor headers.

Portable C++20 собирается без Qt/OS/miniaudio/ESP-IDF и без обязательных RTTI и
exceptions. Ошибки — явные enum/result, не исключения как control flow.
Control plane может готовить память в пределах admission; hot path не вызывает
JSON, codec, persist, OS I/O и не расширяет контейнеры. Borrowed spans не переживают
callback; асинхронные ответы и pending requests используют bounded owned storage.

## 3. Typed processing commands

Имена ниже задают API-смысл; legacy `EngineCommand` IDs не перенумеровываются.
Рекомендуемый новый header: `application/processing_control.h`.

`ProcessingDTO = {GraphDefinition graph}`; processing_version имеет единственный
authoritative источник `graph.processing_version`, не дублируется. NodeId/graph_id и port identities
передаются без переименования и регенерации. Имена параметров и enum IDs берутся
из общего descriptor, никогда из native spinbox internals.

`CommandHeader = {request_id:uint64, expected_revision:uint64}`. Request ID
назначает клиент, ответ возвращает его неизменным. Новые mutations требуют
точного expected_revision; legacy wildcard остаётся только в compatibility
adapter. Session/device epoch в controller отделяет старые ответы от новых.

| Операция | Вход | Ответ / эффект |
| --- | --- | --- |
| GetCapabilities | request_id | supported processing versions, node/port/parameter contracts, receive/publish types, resources, persist availability |
| GetState | request_id | epoch, revision, graph_revision, lifecycle/run intent, applied ProcessingDTO, prepared token при доступе владельца, bounded diagnostics; без layout/history |
| Prepare | header, ProcessingDTO | validation/error location, memory estimate, server-issued opaque PreparedBundleToken, graph identity и expected runtime revision; working/run intent/running/revision не меняются; failed Prepare сохраняет прежний token |
| Apply | header, PreparedBundleToken, graph identity | Atomic check session/client ownership + graph identity + expected runtime revision + executor identity; success consumes token и возвращает accepted graph_revision/revision/applied DTO |
| ReleasePrepared | context, own token | Освободить own reservation/bundle без изменения working/revision; foreign token отвергается |
| SetParameters | header, node identity, typed registered processing fields | Только EMPTY global slot, иначе Busy даже для owner; clone working на control plane, strict validate/transactional prepare/apply; failed internal candidate discarded, working/intent/revision unchanged |
| Start / Stop | header | Явное run intent только после успешного перехода; failed Start/Stop сохраняет working graph/run intent/running/revision; повтор уже достигнутого состояния — accepted no-op |
| ExplicitPersist | header, accepted graph_revision | Сохранить именно applied processing snapshot через порт; отдельный persist result; нет implicit flash при Apply/SetParameters |

Reply: `{request_id, accepted, error_code, error_location, revision,
graph_revision, epoch}` плюс typed payload операции. `ProcessingReply.error_location`
копирует `ValidationResult.location` из domain: requested node identity, статический
зарегистрированный parameter key и optional direction/port/edge_index. Отсутствие
edge отличается от edge 0. Domain владеет выбором key и missing/view update policy;
control не выбирает ключи и не создаёт свою parameter validation registry.
Минимум ошибок: malformed, unsupported_version/codec/kind/type/parameter,
limit_exceeded, invalid_graph, conflict, no_prepared, busy,
foreign_preparation, preparation_expired, session_closed,
resource_failure, persistence_failure. Протокольный отказ не masquerade as silence.

Mutations всех clients сериализуются в одной application consistency boundary.
Prepared execution bundle владеет сервер, а клиент хранит только opaque token.
Одна глобальная запись сервера: `{token, session_identity, client_identity,
graph_identity, expected_runtime_revision, executor_identity, expires_at}`.
PreparedExecution принадлежит единственному pending slot R lifecycle/runtime;
S хранит reservation/token metadata, а не дополнительный executor/bundle map. Client
identity устанавливает session controller; это не непроверенная строка из wire.
Codec CBOR/Protobuf и transport TCP/UART не меняют ownership и lifetime.

Portable typed API-смысл: `prepare(context, header, dto) -> PrepareReply`,
`apply(context, header, token, graph_identity) -> ApplyReply`,
`release_prepared(context, token) -> Status`, `close_session(context) -> Status`,
`expire_prepared() -> Status`. S получает application `Clock` port с monotonic time;
TTL не измеряется DSP timestamp, wall clock или временем клиента.
Context задаёт server-issued session/client
identity и device epoch; никакие pointers/prepared memory не передаются по wire.
GetState показывает own pending token только владельцу и не раскрывает foreign
prepared bundles. Token уникален в пределах server epoch и не переиспользуется
так, чтобы старый reply мог сослаться на новую подготовку.

ControlLimits объявляет `max_pending_prepared=1` GLOBAL (не per-client),
`prepared_ttl_ms=30000` по умолчанию (configurable положительный bounded TTL),
`max_sessions`, `max_prepared_bytes`, `max_control_scratch_bytes` и pending
request/codec limits. Effective limits/TTL advertised в capabilities до Prepare.
Нельзя увеличить число pending bundles настройкой host: текущий контракт — один.
Session table bounded отдельно и не означает множество prepared slots.
Реальный API использует caller-owned `ControlSessionSlot` span и `ControlClock`.
ControlLimits также содержит bounded graph identity (default 256 bytes), string
fields (default 256 bytes), `max_prepared_bytes`, `max_replay_bytes_per_session`
(default 512 KiB), `max_replay_bytes_total` (default 4 MiB) и
`max_control_scratch_bytes` (default 512 KiB). Constructor проверяет overflow-safe
консервативный bound request+reply+owned snapshots и total sessions×cache до
admission; недостаточный budget запрещает session/request до runtime effects.
Embedded host обязан задать меньшие реальные effective limits и учитывать их
в admission; desktop defaults здесь не обещают fit ESP32 profile.

S резервирует session в global slot ДО вызова R typed stage Prepare. Если slot
принадлежит другому active client/session, любой его valid/invalid Prepare
возвращает busy до domain validation/prepare и без изменения R pending bundle,
token, working, intent или revision. Нет очереди prepared candidates и eviction
чужого owner. Если empty-slot Prepare fails, новая reservation освобождается.
При own replacement R может готовить временный candidate в explicit scratch/
memory budget рядом с old pending bundle; это staging в одном executor, не второй
outstanding prepared slot. Old bundle нельзя освободить до success candidate.
Exhaustion возвращает limit_exceeded/resource_failure и сохраняет old token.

Успешный Prepare атомарно заменяет только own old token новым и освобождает
старый bundle. Неудачный Prepare (validation, conflict, limits, allocation/runtime
prepare error) оставляет прежний token/bundle, working graph, run intent, running
и revision неизменными; TTL старого token не продлевается failed Prepare.
Успешный Prepare назначает новый token и expires_at = Clock.now + TTL с checked
arithmetic. Prepare не делает visible Apply и не увеличивает revision.
После успешной mutation runtime revision прежний token становится stale: он
не может быть применён, даже если payload графа совпадает. Его bounded record
может оставаться до own successful replacement/release/disconnect/expiry; это не разрешение
выполнить старый план.

Apply в одной serialized transaction повторно проверяет active session/client,
token ownership, graph identity, exact expected runtime revision, capabilities и
executor identity, затем commits на frame boundary. Foreign token —
foreign_preparation; неверная graph identity — invalid_graph; mismatch header
revision или token base revision устарела — conflict без изменений; unknown/
consumed/superseded token — no_prepared. Только accepted Apply consumes/releases
token. Rejected Apply сохраняет own pending bundle/token и всё working состояние;
runtime port не должен разрушать retained bundle при failed commit.
ReleasePrepared, disconnect/close_session и expiry инвалидируют/освобождают own
global reservation и pending resources через R typed discard, без Apply и без
изменения working/revision. Foreign release — foreign_preparation. TTL истекает
при Clock.now >= expires_at; S проверяет его перед каждой control операцией и
host вызывает expire_prepared на bounded polling cadence. Query/dedup reply/
failed mutation не продлевают TTL. Expired Apply возвращает preparation_expired
при обнаружении истечения известного token, иначе no_prepared после освобождения;
нет unbounded tombstone map. Atomic ownership/revision/expiry check precedes commit.
Disconnect также отменяет own queued commands; чужие sessions не затрагиваются.
Reconnect начинает
без token, получает caps/state/revision и обязан Prepare заново. Closed context
получает session_closed; old token в новой session не применим. Controller
отменяет queued command до commit при закрытии session в том же serialized order.

Failed typed Start/Stop не меняет working graph, run intent, running state,
revision, prepared token или ресурсы; R обеспечивает transactional typed API:
intent и runtime effects публикуются только после success. S не дублирует intent
и не пытается компенсировать failed effects снаружи R. Explicit legacy policy R
сохраняет старое legacy behavior; typed service не вызывает legacy path.
Ошибка mutation также не изменяет counters, network bindings или persisted data.
Rejected requests не увеличивают revision. Persist не увеличивает graph_revision;
его success обновляет отдельно persisted_graph_revision. Статус ресурса сообщается
отдельно от факта принятия processing graph.

Request deduplication bounded и session-scoped: одинаковые ID+payload в retention
window возвращают прежний reply без повторного Apply/flash, одинаковый ID с другим
payload отвергается. Evicted ID/потерянная session не дают обещания exactly-once:
после reconnect клиент читает state и согласует revision перед mutation.
В срезе 1 retention window = ровно последний completed bounded request каждой
active session. Owned request/graph identity и applied reply snapshot не ссылаются
на caller temporary; equality сравнивает exact typed fields, floating bits и
expected_revision, не hash. Exact retry возвращает исторический completed reply
без повторного runtime/persist вызова; TTL не продлевается. Cached Prepare reply
после expiry сообщает прежний completed result, но token больше не применим.
Reused request ID после вытеснения из этого window не обещает deduplication;
wire negotiation/schema и более широкий replay policy относятся срезу 3.
Legacy ApplyGraph использует explicit R legacy policy через compatibility adapter
как сериализованную операцию над тем же executor. Policy различает historical
failure semantics и новый typed transaction; не разрешено silently clear own typed
reservation. Cross-mode exclusion согласуется с R, foreign reservation даёт busy.

## 4. Capabilities и selective reconciliation

Публикуются реальные поддерживаемые processing versions, NodeKind stable IDs,
semantic frame/port contracts, parameter ranges/defaults и доступные transports,
codec versions, max nodes/edges/ports/FFT samples per channel/Mel bands/history
frames, runtime/control memory, message bytes, nesting/string/extension limits,
pending requests/queues и persistence support. Пустой supported set ничего не
разрешает. Advertised caps до Prepare совпадают с применёнными admission limits.

Desktop defaults остаются текущими, пока явный профиль их не меняет: graph 128/256,
runtime budget 64 MiB, frame samples 4096, history max 2000, WLED destinations 1.
Embedded defaults: FFT 2048 на канал с ограничением compiled backend; history OFF;
остальные значения profile/configurable, не копия desktop_capabilities. Текущий
Profile 16 nodes/16 edges, 256 KiB total, 32 KiB adapter reserve, 2×2304-byte
frame slots — исходный reference для admission, не измеренная память устройства.
Control buffers/codec arena/NVS serialization входят в budget явно. Unsupported
node/shape/version и превышение лимита отклоняются до запуска/замены working.

Применение переносит совместимое состояние по node identity и совместимому
configured contract; выходы идентифицируются node+port. Switch 1→N, Splitter
2..8, Gate, Scalar/WLED packet и History count port проверяются общим registry.
Known inactive с контрактом и clock → typed zero; unknown → unavailable. Это не
Stop. History сохраняет старые строки и добавляет zero row на inactive tick.

Controller готовит resource change set по stable binding identity и существенным
transport/destination параметрам. FFT gain, routing selected/enabled и display
параметры не пересоздают незатронутые TCP/UART подключения. Явные Stop/device change
отделены от edits. Resource acquisition failure до commit сохраняет рабочие bindings;
после commit async delivery failure отражается статусом конкретного ресурса.
Реализация NetworkRuntime reconciliation назначается отдельно в future cut;
этот контракт задаёт typed результат, не разрешает правки desktop transport.

## 5. Единый JSON project document

Default: `<executable directory>/config.json`, не cwd. CLI explicit `--config`
или пользовательский Open/SaveAs выбирает один абсолютный active path; успешный
SaveAs меняет его, failed SaveAs — нет. Не заводить отдельные ui/core JSON.

Рекомендуемый физический envelope сохраняет существующие stable keys. Новое
`schema_version:1` обозначает первую версионированную project envelope; это
НЕ graph `version:1`. Логические разделы не требуют переименования old keys:

```text
schema_version: 1                 # version project document
pipeline_graph: {...}             # applied processing graph; graph version stays 2
scene_graph: {...}                # editable draft + preserved node layout
ui: {...}                        # presentation, rendering, workspace, editor_view,
                                 # existing pipeline_presets IDs/name/graph
core / wled_ar_server / ...       # existing application/device settings keys
devices: {...}                    # existing per-device draft/applied/presentation
<unknown keys>: <original value>  # opaque preserved extensions
```

Разделы processing/presentation/app/presets/draft/applied имеют typed accessors;
root duplication новых `processing`/`presentation` структур не создаётся по
умолчанию. Graph `version` (формат графа v1/v2), `processing_version` (семантика
SDK, default 1 в explicit legacy migration) и control wire version независимы.
Согласовать envelope с координатором до реализации, если соседний контракт
предлагает иное размещение; не вводить две конкурирующие canonical schemas.

Draft может быть невалидным/неподдерживаемым и хранится без запуска. Applied
обновляется только после accepted Apply, не по optimistic GUI editor state.
На старте применяется applied; scene draft восстанавливается редактором отдельно.
Если в legacy есть только standalone graph — migration создаёт applied+draft
представления одного исходника без изменения ID. Если есть explicit pipeline_graph,
он authoritative applied: malformed не вызывает fallback на scene. При его
отсутствии legacy scene мигрируется как candidate applied и проверяется до запуска.
Per-device документы не подменяют local applied при remote edits.

Ни история DSP, ни ScalarView 128 samples, sequence, pending tokens, epochs,
preview counters или runtime buffers не входят в проект. Explicit user values
никогда не заменяются новыми defaults. Default применяется лишь при отсутствующем
поле соответствующей поддерживаемой legacy/schema версии, не при malformed/null.
Совмещённый legacy WLED сохраняет identity и читается через compatibility bridge;
автоматическое разделение с заменой пользовательских UUID не выполняется.

## 6. Миграция, extensions и atomic save

Порядок: parse → строгий version classification → explicit migration chain →
boundary schema checks → typed projection → domain validation → Prepare → Apply.
Load не переписывает файл. Save разрешён только для успешно прочитанной известной
project schema или явно созданного нового документа. Malformed/future versions
блокируют apply и overwrite/autosave исходного файла, включая prefs-only save.
Отсутствие legacy версии отличается от present null/string/fraction/negative/0;
malformed никогда не нормализуется `toInt(default)`.

API adapter: `classify_document(raw)`, `migrate(raw, target_schema,
target_graph_version) -> MigrationResult`, `project_processing(document) ->
ProcessingProjectionResult`, `overlay_project(raw, typed_changes)`,
`save_atomic(path, document) -> SaveResult`. MigrationResult содержит source/target
versions, candidate, error path и возможность обратимого перехода. Application
не вызывает JSON functions.

Цепочка up/down состоит из отдельных проверяемых переходов: legacy unversioned→
project schema 1; legacy graph v1→v2 сохраняет прежнюю численную семантику
Window/WindowFunction/BarkWeighting. Новые ID выделяются только добавленным
узлам с collision/overflow check, старые ID/UUID/edge ports сохраняются.
Down migration разрешена лишь когда обратима: явно распознанные миграционные
узлы/defaults можно восстановить; user edits, multi-output/new kinds/непредставимые
parameters вызывают unsupported/lossy downgrade error. Нельзя молча удалять узлы,
extensions, выходы или преобразовывать смысл. Нет обязанности изобретать будущую
версию: каждый новый переход вводится с парой up/down и negative tests.

Unknown root/node/parameter/preset/device fields сохраняются JSON-семантически
(не обещается whitespace/order). Extension preservation не означает выполнение
unknown processing: descriptor-known fields исполняются, opaque поля живут в
document adapter; unsupported kind/required feature отвергается. Новый control
wire может переносить ограниченную typed extension tree для processing keys,
но не GUI JSON blob. Неподдерживаемые optional wire fields bounded skip/preserve
по schema; unknown mandatory feature/command — reject, без default interpretation.

Save собирает один полный candidate поверх raw_document, обновляет только
authoritative известные поля и выполняет одну atomic replacement через QSaveFile
(без direct-write fallback). Failure parse/migrate/write/commit оставляет старый
файл и active path. Все ветки prefs, presets, local/remote draft и applied используют
тот же adapter, чтобы не затереть чужие unknown поля.

## 7. Processing projection и accepted layout

Projection использует общий configured descriptor с classification
`processing` / `presentation` / `application-adapter` / `opaque-extension`.
В processing входят graph identity, processing_version, node stable kind/id,
enabled, зарегистрированные DSP/routing параметры и edges node+port. View sinks
остаются в graph topology, но FFTView channel/merge, colors, axes, camera, dock,
position, node preview/internal widget fields уходят в presentation. Boundary
validation сохраняется перед projection: фильтрация не скрывает неверную версию.
Application/device transport configuration не пересылается как произвольный UI
объект: только поддерживаемые typed adapter settings операции/узла.

UI исключается в ОБОИХ направлениях: command и state/reply не несут presentation
параметров. Локальные display selectors не меняют processing revision, не запускают
prepare, network reconnect и persist. Unknown extensions сохраняются локально;
known presentation key нельзя отправить как opaque processing extension.

Pending layout map keyed by `{device_identity, epoch, request_id}` содержит
graph_id, proposed node identities и layout snapshot. Отправка не меняет accepted
layout. Только accepted reply данного запроса назначает layout его graph_revision;
reject/timeout/disconnect уничтожает pending entry. Snapshot merge допустим лишь
для того же device+epoch+graph_id+accepted graph_revision и совпадающей node identity
(при reuse ID проверяется kind/descriptor identity). Новый remote graph/reconnect,
out-of-order replies и rejected Apply не получают layout последнего submitted
graph. Сохранённый draft остаётся отдельно и не теряется от processing snapshot.

## 8. ControlCodec и transport negotiation

Рекомендуемый portable interface в `bdsp_protocol/control_codec.h`:

```cpp
struct ControlLimits; // bytes, depth, strings, nodes, edges, extension entries
struct ControlResult { Status status; size_t consumed; size_t produced; };
class ControlCodec {
public:
    virtual ~ControlCodec() = default;
    virtual ControlResult encode(const WireControlMessage&,
        std::span<std::uint8_t> destination, const ControlLimits&) const noexcept = 0;
    virtual ControlResult decode(std::span<const std::uint8_t> source,
        ControlDecodeArena&, WireControlMessage&, const ControlLimits&) const noexcept = 0;
};
```

DecodeArena и wire DTO bounded/caller-owned; decode использует scratch и публикует
output только после полного успеха. Overflow/truncated/duplicate mandatory keys,
невалидные enum/UTF-8/числа и trailing garbage отвергаются, частичная command не
вызывается. Allocation failure explicit; сам codec не выделяет скрытый heap.
Encode заранее проверяет ёмкость; failure не enqueue частичный packet. Layout,
history и GUI данные отсутствуют в WireControlMessage. Обе реализации имеют
одинаковую семантику uint64 IDs/revisions, finite numbers, optional/default fields
и ошибок; codec choice не меняет application поведение.

CBOR default negotiated; Protobuf — рабочая alternative, не placeholder.
TinyCBOR v7.0 и nanopb 0.4.9.2 фиксируются immutable commit и source hash в vendor
manifest при реализации; сейчас commit/hash не проверялись и не утверждаются.
Generated nanopb sources/schema/options checked in. Normal SDK build/install не
запускает generator, Python или protoc; отдельная документированная generation
команда с pinned tools воспроизводит bytes checked-in output.

BDSP framing/data IDs и старые payload goldens неизменны. Новые control message
IDs выделяются после сверки полного registry (не использовать 5..17/исторические
IDs без согласования). Session hello сообщает protocol version, supported codecs,
limits и capabilities; selected codec/version подтверждается до первой mutation.
Оба peer предпочитают общий CBOR, иначе согласуют Protobuf; отсутствие общего
codec/version даёт explicit отказ. Новые сообщения не посылаются old peer без
negotiation. Legacy QDataStream desktop service изолирован за adapter и сохраняет
старые bytes; firmware не обязана его поддерживать. Streaming Session не начинает
принимать control автоматически.

TCP/UART используют одинаковый codec/framing и bounded incremental packet assembly,
partial writes, one-message-in-progress, timeout/cancel на disconnect; limits
проверяются до reserve/copy. Control polling сериализуется host-ом, не ISR и не
DSP callback. Network/WiFi provisioning даёт host. Безопасный reconnect сначала
получает state/caps/epoch; не replay старые mutations без revision reconciliation.

## 9. Embedded explicit persistence и recovery

Application persistence port рекомендует
`persist_applied(ProcessingDTO, graph_revision) -> PersistResult` и
`load_last_good() -> bounded ProcessingDTO/result`. Concrete NVS adapter живёт
в `adapters/esp_idf`, host вызывает restore до Start через обычный validate/prepare/
apply. Versioned processing record не равен whole JSON project и не содержит UI.

NVS record: storage schema version, processing version, codec/version, generation,
bounded length, integrity checksum и processing payload. Два слота/generation
позволяют записать candidate и commit валидного marker, сохранив last-known-good
при write/commit/power loss. Recovery проверяет bounds/integrity/version, декодирует,
проверяет реальные caps и prepares до выбора; failed newer slot допускает валидный
старый. Если оба invalid/unsupported — stopped/default safe state с явной ошибкой,
без автоматического overwrite NVS. Downgrade прошивки не переписывает future record.

ExplicitPersist фиксирует accepted applied revision, не draft и не stale candidate.
Slider/SetParameters/Apply/Start не пишут flash. Failure persist сохраняет working
graph и прежний persisted revision, возвращает отдельную ошибку. Firmware restore
не создаёт второй executor. Default history OFF и FFT 2048 остаются admission
ограничениями; NVS restore не обходит их.

## 10. Проверки и gates следующих срезов

Новое поведение: failing targeted tests перед implementation. Перенос legacy:
сначала characterization из HEAD fixtures/goldens. Авторские проверки не заменяют
новый независимый review chat каждого среза.

| Срез | Обязательные targeted сценарии |
| --- | --- |
| 1 | Typed command defaults/errors; advertised global max_pending=1 и TTL=30s/config override; fake Clock before/at expiry, release/disconnect/reconnect; A Prepare→B valid/invalid Prepare Busy→A Apply valid; own invalid Prepare сохраняет token/bundle/TTL, own valid replacement only success; foreign Apply/release, stale conflict, consumed token; bounded scratch/exhaustion; failed typed Start/Stop сохраняет graph/intent/running/revision/token; separate legacy characterization; raw malformed version против absence; migration standalone/unversioned/v1/v2/presets; unknown/overrides/draft-applied/up-down/lossy rejection |
| 2 | Multi-output identity в DTO/diagnostics, History count отдельно от History; old WLED identity preservation; runtime owner проверяет typed silence/state compatibility |
| 3 | CBOR и Protobuf semantic roundtrip; caller buffer/depth/length limits, truncated/duplicate/overflow/unknown mandatory; legacy wire goldens; fragmented TCP/UART; negotiation отказ; two clients conflict/stale token/dedup/reconnect; projection excludes UI both directions; accepted/rejected/timeout/out-of-order layout; one atomic save failure injection; unchanged TCP/UART binding при параметрах |
| 4 | Embedded actual caps до prepare; control TCP/UART host tests; NVS failure до/после commit, corruption/future version/both invalid, last-good restore, zero flash writes при slider; reproducible generation; no-Qt/no-RTTI/no-exceptions SDK consumer и existing ESP-IDF toolchain crossbuild |
| 5 | App build + короткие unit/integration затронутых сценариев; compatibility resolver/presets/CLI path; финальный согласованный full automated run; fresh architecture, runtime/protocol/storage, desktop/UI reviews |

Исходные test anchors: `application_lifecycle_tests`, `graph_document_adapter_tests`,
`config_document_resolver`, `console_config_selection`, `engine_transport`,
`engine_typed_views`, `network_runtime_independence`, `registry_capabilities`,
`bdsp_protocol_tests`, `embedded_sdk_tests`. Новые migration/control/NVS cases
добавляются целево; exact test registrations согласуются с build owner.
Один общий Ninja/build только у координатора/назначенного build owner. App build
обязателен после интеграции функциональных правок. Hardware ESP32/WLED и native
graphics не подтверждены host/offscreen тестами; Linux без запроса не запускается.

## 11. Будущее владение и согласование

Сверены [node-runtime-contracts](node-runtime-contracts.md),
[implementation-plan](implementation-plan.md) и
[baseline-and-ownership](baseline-and-ownership.md). Основные semantic/control
решения совпадают. Окончательное владение задаёт актуальный baseline-and-ownership
и явное назначение координатора для каждого среза (S/R/P/I). Уточнение координатора:
config в срезе 1 назначается отдельному worker; desktop delivery и embedded
назначаются отдельно в future cuts. Эти назначения и исключения CMake имеют
приоритет над предложениями ниже и в
соседнем node/runtime документе. Node/runtime descriptor API пока описывает порты;
classification параметров требуется согласовать как extension общего registry.

Ниже reservation для следующих заданий, а не разрешение менять код в срезе 0.

| Владелец | Точные границы |
| --- | --- |
| Storage/control S | Новые `application/include/application/processing_control.h`, `application/src/processing_control.cpp`, portable persistence port рядом processing control и новые processing_control/control_codec tests. `application/graph_lifecycle` принадлежит R |
| Отдельный config worker в срезе 1 | `adapters/desktop/include/config/config_document.h`, migration/project adapter файлы под `adapters/desktop/configuration/`; `adapters/desktop/include/desktop/graph_document.h`, `adapters/desktop/src/graph_document.cpp`; tests config_document / graph_document_adapter / project_migration. Точное assignment выдаёт координатор, эти файлы не назначены S автоматически |
| Storage/control чат | Новые `libs/bdsp_protocol/include/bdsp_protocol/control_*.h`, `libs/bdsp_protocol/src/control_*.cpp`, `libs/bdsp_protocol/schema/`, `libs/bdsp_protocol/generated/`; existing stream codec/fragments принадлежат S по baseline; semantic payload согласовать с R. Vendor manifests/pins/codegen scripts у I, location согласуется до добавления |
| Отдельное назначение desktop delivery / presentation в future cuts | `engine_client.cpp`, `ConfigManager`, pending request transport map, `presentation/ports/*`, GUI layout/display preferences. `apps/**` меняет I. Handoff: control выдаёт accepted request/revision/epoch; presentation применяет локальный layout по этому ключу. Эти paths не разрешены S текущим заданием |
| Domain/runtime R | `domain/**`, `libs/signal_contracts/**`, `libs/dataflow_runtime/**`, `libs/dsp_kernels/**`, `application/include/application/graph_lifecycle.h`, `application/src/graph_lifecycle.cpp`, связанные assigned tests; configured descriptor/parameter classification/caps и GraphExecutor state. Все CMake исключены, stream protocol/codec сюда не входят |
| Отдельное назначение desktop transport в future cuts | `NetworkRuntime`, desktop `stream_transport`/`stream_codec` и соответствующие headers/tests, registry protocol adapters; selective reconciliation/legacy engine wire. Portable `libs/bdsp_protocol` stream codec/fragments/protocol принадлежат S, semantic payload согласовать с R |
| Отдельное назначение embedded в future cuts | `libs/embedded_support/**`, `adapters/esp_idf/**`, `examples/esp32_streamer/**` без CMake/build manifests; portable admission/controller и concrete NVS/control transport. Использует общий typed service/codec/persistence port; exact assignment и host assembly согласуются через координатора и I |
| Integration I | Все CMake/build manifests, vendor pins/hashes/codegen scripts, dependency wiring, install/export, generation target, `apps/**` roots и общий build. S владеет codec schema/generated files; I подключает generation/dependencies. Этот чат передаёт required files/links/tests, не запускает конкурирующий Ninja |

Координатор должен согласовать: descriptor classification API (processing/view/
adapter/extension), canonical project envelope/accessors, new wire IDs, shared
numeric bounds/vendor location, transaction с selective resource acquisition и
точное разделение EngineClient/GraphLifecycle/CMake изменений. GraphLifecycle
принадлежит R; решение gate 0 зафиксировано: failed Start/Stop сохраняет working
graph/run intent/running/revision; failed typed Prepare сохраняет prior prepared
bundle, replacement только при successful Prepare. R владеет typed stage prepare,
transactional typed Start/Stop/discard и explicit historical legacy policy;
S владеет GLOBAL single-slot reservation, token session/graph/revision/TTL и
Clock/ControlLimits orchestration по §3, без duplicate intent/executor/map.
S не исправляет чужие
lifecycle файлы самостоятельно.
Существующие файлы
со смежным владельцем меняются только после явной передачи; чужой черновик не
откатывается. Пользовательские `config.json` и `output/` не изменяются.

Готовность среза 0: gate PASS; координатор разрешил срез 1 S. Future codec/wire,
desktop/UI/storage/NVS реализация, app build, vendor verification и hardware
acceptance не входят в текущее assignment и не объявляются выполненными.

## 12. Авторский отчёт среза 1 S

Выполнено: `processing_control.h/.cpp` содержит portable ProcessingDTO,
capabilities/effective RuntimeLimits+ControlLimits, typed commands/replies/errors,
Clock/runtime/persistence ports, caller-owned bounded sessions, GLOBAL reservation,
token session (client/session/server epoch)/graph/revision/TTL, release/disconnect/expiry и one-last-completed
exact owned replay. `GraphLifecycleControlPort` делегирует согласованным R
transactional APIs, не владеет executor, PreparedExecution или run intent.
Strict `domain::validate_processing_graph` общий с R; views NoParameters, UI
parameters не нормализуются в typed API. SetParameters clone работает лишь на
control plane при empty slot; concrete persistence/codec/wire пока не добавлены.

Тесты `tests/processing_control_tests.cpp`: fake Clock/caps/runtime/persistence
и live GraphLifecycle с fault-injected preparation/runtime ports. Проверяются
A prepare → B valid/invalid/oversized/null Busy → A apply; own invalid Prepare
сохраняет token, failed Apply сохраняет bundle; successful replacement; foreign/stale/consumed tokens;
TTL default/configurable exact edge, release/disconnect/reconnect; transactional
Start/Stop rejection и accepted no-op; old server epoch rejection;
request correlation, exact retry, same-ID conflict, signed
zero payload difference, owned cached reply после mutation другим client;
empty-slot SetParameters success/failure/Busy; strict view parameters; unsupported
version/kind; limits/cache/session admission; explicit persistence fail/retry.

TDD evidence:

- Initial stub service compile against baseline domain archive: behavioral RED,
  exit 3, assertion `admitted bounded control sessions`.
- Live sources + added null boundary case: RED exit 3, expected malformed vs limit.
- После null fix, exact signed-zero replay case: RED exit 3, same-ID conflict.
- После исправлений live source compilation и executable: GREEN exit 0,
  `Processing control reservation/revision/TTL/atomic failure tests passed`.

Самостоятельный build только `build-refactor-control`, compiler найден из
existing `build-sdk/CMakeCache.txt`, без установки/hardcoded project toolchain:

```powershell
$taskCpp = ((Get-Content build-sdk/CMakeCache.txt |
  Where-Object { $_ -match '^CMAKE_CXX_COMPILER:' }) -split '=',2)[1]
$env:PATH = (Split-Path $taskCpp) + ';' + $env:PATH
& $taskCpp -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror `
  -Wno-missing-field-initializers -I application/include -I domain/include `
  -I libs/signal_contracts/include application/src/processing_control.cpp `
  application/src/graph_lifecycle.cpp tests/processing_control_tests.cpp `
  domain/src/graph_definition.cpp domain/src/graph_validation.cpp `
  libs/signal_contracts/src/frame.cpp `
  -o build-refactor-control/processing_control_tests.exe
& ./build-refactor-control/processing_control_tests.exe
```

`-Wno-missing-field-initializers` относится R ParameterDescriptor aggregate
initializers; S source отдельно проходит `-Wall -Wextra -Werror` без этого
исключения. Shared CMake/build не запускался S; все manifests меняет I. Integration
requirement: добавить `application/src/processing_control.cpp` в graph_application,
новый processing_control_tests link graph_application, assertions ON, C++20;
используются public application/domain/signal includes, Qt/BDSP/OS не нужны.

Memory limitation: control plane использует bounded-by-count/strings owned
std containers и temporary graph clone/validation; DSP hot path от этого не
получает allocations. Объявленные budget — консервативный retained payload bound,
не byte-exact измерение heap; allocator overhead и runtime prepare transient reserve
host admission учитывает дополнительно. `-fno-exceptions` не гарантирует recovery
после физического allocator OOM; заранее обнаруживаемое превышение limits
отвергается до mutation. Ports/caps/Clock/slots должны жить дольше service;
caps immutable, control вызовы сериализованы, runtime не изменяется в обход service.
Reply graph pointers borrowed: обычный до successful runtime Apply, replay до
replacement own completed cache/close; transport обязан сделать bounded owned
projection перед async delivery. TTL использует Clock sample при admission
Prepare; queries/retries/failures не продлевают её.

Статус исходной авторской реализации: author-tested. Первичное независимое
S review FAIL по двум P2; исправления и повторная author verification ниже.
Повторная независимая приёмка обязательна; срез 2/3 без gate не начинается.

## 13. Исправления независимого S review среза 1

Review `01a112b5-dafe-7da0-936c-3071d311620e` обнаружил два P2; авторские
предыдущие PASS не считаются их приёмкой. Координатор разрешил S fixes только в
owned processing_control header/source/tests и этом документе. Domain API
реализован владельцем R; S его файлы не менял.

F1: expiry обходилась ранними `malformed`, `session_closed` и same-request-ID
payload-conflict returns. TTL=17, Prepare при now=100, now=117, request_id=0
сохранял pending и discards=0. Regression RED exit 3 на
`expiry runs before every submit return`. Исправление: maintenance выполняется
в самом начале submit/close_session, до session/header/replay/bounds checks.
Captured expiry event передаётся private dispatch, чтобы новый Apply возвращал
preparation_expired. Exact completed retry по-прежнему возвращает исторический
cached reply, при этом expired resource освобождён и cache TTL не продлевает.
Тесты отдельно проверяют malformed, same-ID conflict, retired epoch, exact replay,
expired Apply и foreign/closed close_session; discard ровно один, working/revisions
не изменяются, следующая state query показывает token=0.

F2: прежний ValidationResult не различал неверные параметры, ранний SetParameters
missing/view branch терял requested node. Новый RED exit 3 подтвердил
`missing-node diagnostic preserves requested identity`. R добавил bounded
`DiagnosticLocation` к ValidationResult и domain
`validate_processing_parameter_update(graph,node,params) noexcept`; static
registry keys имеют process lifetime, optional direction/port/edge не выделяют
память. S вызывает helper до candidate clone и копирует location в reply
без самостоятельного выбора ключей. Empty working использует тот же helper с
пустым graph. Missing-node получает requested identity/MissingNodeOrPort;
view update (включая NoParameters/enabled-only) отвергается, FFTViewParameters
даёт зарегистрированный `channel`, variant без display field — empty key.

Author GREEN cases: amplitude=2 → `amplitude`, period_seconds=0 →
`period_seconds`, no field confusion, exact rejected replay сохраняет location;
missing node=123/view node=2 identities; output port=9/edge=0 → direction Output,
port 9, optional edge 0; parameter failures без port/edge. Runtime prepare/apply
не вызывается при preflight rejection, working/revision сохранены. Ошибочный
author assertion одинакового адреса registry string исправлен: контракт обещает
зарегистрированное имя и static lifetime, не pointer identity разных литералов.

Команды: тот же standalone live recipe §12 (owned build-refactor-control),
source separately `-c ...processing_control.cpp` с `-Wall -Wextra -Werror`,
затем C++20 live application/domain/signal compilation с
`-fno-exceptions -fno-rtti` и targeted executable. Финальный compile/run exit 0:
`Processing control reservation/revision/TTL/atomic failure tests passed`.
Scoped diff/whitespace checks clean. Shared CMake/Ninja, installs, config.json,
output и чужие source не менялись. Кодеки/wire/срез 3 не начаты.

Статус fixes: S READY/FROZEN для I integration и повторного независимого
control review; решение gate среза 1 остаётся у координатора.
