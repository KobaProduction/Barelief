# Контракты узлов и переносимого runtime — срез 0

Статус на 2026-10-06: GATE 0/1 accepted координатором после независимых R/S/C
PASS и actual integration. R2 среза 2 реализован после принятия API handoff;
source frozen после authorfix независимых F1/F2 для повторной приёмки.
Разделы 11/12 сохраняют первую стадию; актуальные результаты — раздел 13.
В срезе 0 изменялся только этот документ; paths реализации среза 1 перечислены
в разделе 9. Следующий срез выдаёт координатор после независимого ревью.

Основания: AGENTS.md, [карта архитектуры](../architecture-map.md),
[описание черновика маршрутизации](../typed-node-routing.md), исходный запрос
`C:/Users/Koba/.codex/attachments/4887cc54-112e-4836-84d3-82950e988245/Вставленный текст.txt`
и утверждённый координатором план срезов 0–5. Полностью прочитан
[Clean Architecture / DDD, закреплённая редакция](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).
Будущему исполнителю и независимому архитектурному ревью передавать эту ссылку
с требованием полного чтения; справочник не задаёт backlog и не разрешает
переименование стабильных идентификаторов.

## 1. Baseline и границы доказательств

HEAD: `b18cfbf3c2ac62b4baf022de5348f5a29d46e2cc`. На старте присутствует общий
незакоммиченный черновик; его не откатывать и не считать принятым. `config.json`
и `output/` не читать для тестовых fixtures и не изменять. В этом срезе не
выполняются сборка, запуск приложения или авторские тесты функциональности:
изменяется только документ, а исполнение общего черновика не было бы проверкой
HEAD. После будущей интеграции сборка приложения и целевые проверки обязательны,
с одним владельцем общего build-каталога.

| Область | Проверенный исходный факт | Состояние общего черновика / следующий контракт |
| --- | --- | --- |
| `domain/include/domain/graph_definition.h` | HEAD содержит 19 NodeKind; Edge уже хранит input/output index; NodeId — int32 | Добавлены роли, семь видов узлов и processing_version; общего configured descriptor пока нет |
| `libs/signal_contracts/include/signal_contracts/frame.h` | HEAD имеет типы 0–6, borrowed FrameView, кольцевую HistoryView | Добавлены Scalar/WledPacket; generic shape не заменяет semantics |
| `libs/dataflow_runtime/include/dataflow_runtime/runtime.h` | Один PreparedGraph/GraphExecutor, подготовка и mutable context отделены | OutputView получил output_port, но ещё не все пути используют identity |
| `libs/dataflow_runtime/src/runtime.cpp` | Подготовка/выполнение и перенос состояния находятся в одном исполнителе | prepare дважды использует desktop_capabilities; unavailable emit агрегатом оставляет port=0; sink emit пользуется max(1, output_ports) |
| `libs/signal_contracts/src/frame.cpp` | Проверка geometry/finite и отдельная unavailable форма | Определение stereo по enum range и разбор WLED bytes внутри общего frame validator требуют устранения зависимости от порядка/формата |
| `libs/bdsp_protocol/include/device_protocols/wled_packet.h` | В HEAD этого файла нет | Header-only WLED wire encoder помещён в BDSP; runtime теперь линкует bdsp_protocol ради него |
| `libs/embedded_support` | Profile/admit, bounded Session, synthetic pipeline; history в текущем Profile выключена | Session расширен для новых типов; это не control service, NVS или аппаратная приёмка |
| `libs/dsp_kernels` | Подготовленные FFT/Mel workspace, ANSI/ESP-DSP backend, C++20 | Численная библиотека не должна получать NodeKind, JSON, wire или GUI |

Baseline проверен чтением `git show HEAD:<header>`, текущих headers/implementations,
`git diff --stat -- domain libs/signal_contracts libs/dataflow_runtime libs/embedded_support`
и `rg` по identity, unavailable, prepare/caps и enum casts. Список общего
черновика получен `git status --short`. Это статическое исследование, не результат
приёмочных тестов. Ошибочный поиск `wled_audio_sync.h` исправлен: фактический
черновой путь — `device_protocols/wled_packet.h`.

## 2. Ответственности и направления зависимостей

* `signal_contracts`: семантические типы, геометрия/контракт, quantity/unit,
  метаданные, borrowed views, bounded retention. Не парсит WLED AudioSync fields,
  BDSP, JSON или control commands.
* `domain`: configured node/port descriptors, роли, допустимые parameters,
  connections, capabilities и topology validation. Зависит от signal_contracts;
  не от runtime, serializer, UI или платформы.
* `application`: typed commands/results, ревизии и GraphLifecycle через
  preparation/runtime/persistence ports. Зависит от domain/contracts;
  исполнитель реализует порты, а не создаётся внутри use case.
* `dataflow_runtime`: immutable schedule/contracts + RuntimeContext со state,
  workspace, clocks, history; единственный PreparedGraph/GraphExecutor. Зависит
  от application/domain/contracts, kernels и отдельного pure WLED codec.
* `dsp_kernels`: только численные операции и подготовленная память. Простую
  редукцию допустимо оставить внутри runtime; новая самостоятельная library
  ради трёх формул не обязательна.
* Будущий `libs/wled_protocol` / target `wled_protocol`: pure bounded
  encoder/decoder WLED AudioSync v2. Зависимость максимум от стандартного C++ и
  общих status/contracts; не от BDSP, application, UDP или Qt. Название нового
  target — предложение, существующие публичные совместимые имена сохраняются.
* `bdsp_protocol`: framing и stream payload bytes, scalar/packet transport;
  opaque WLED bytes не превращают BDSP в владельца стороннего wire format.
* `embedded_support`: portable admission, bounded stream/control orchestration
  через порты; ESP-IDF TCP/UART/NVS и provisioning находятся в адаптерах/root.
* Desktop adapters: capture, queues, transport, destination/cadence, JSON mapping,
  legacy bridge и selective resource reconciliation. Presentation: каталоги,
  подписки, display settings/history/rendering. Ни один из них не выполняет DSP
  вместо PreparedGraph.

Все portable targets компилируются C++20 без Qt, miniaudio, OS sockets, ESP-IDF
и обязательных RTTI/exceptions. JSON/CBOR/Protobuf/generated transport DTOs
не входят в prepared processing path. Assembly и экспорт пакетов — работа SDK
composition root; firmware компилирует те же исходники, не копию runtime.

## 3. Configured descriptor и identity

Сохраняются NodeId, graph_id, UUID, kind_id, type_id, существующие indices и wire
IDs 0–6. Не выводить поддерживаемые типы из числового диапазона enum. Явная
таблица kind/type descriptors с lookup проверяет membership; отсутствующий или
некорректный enum возвращает ошибку, а не роль Processor по умолчанию.

Предлагаемый API среза 1 (имена новых сущностей можно согласовать, смысл обязателен):

```cpp
struct PortIdentity { domain::NodeId node; std::uint16_t port; };
enum class PortDirection { Input, Output };
struct PortDescriptor {
    PortIdentity identity; // index сохранённого Edge, не номер GUI-строки
    PortDirection direction;
    signal_contracts::FrameType type;
    Quantity quantity;
    Unit unit;
};
struct ConfiguredNodeDescriptor {
    domain::NodeId node;
    domain::NodeKind kind;
    domain::NodeRole role;
    std::span<const PortDescriptor> inputs, outputs;
};
Status describe_node(const domain::NodeDefinition&,
                     std::span<PortDescriptor> input_storage,
                     std::span<PortDescriptor> output_storage,
                     ConfiguredNodeDescriptor& result) noexcept;
```

Port identity во внешнем envelope включает graph/revision и направление;
`node+port` однозначно определяет выход внутри графа. Quantity/Unit — семантика,
а не переводимый label: например, `filled_frame_count`/`frames`,
`amplitude`/`normalized_amplitude`, `packet`/`bytes`. Для неизвестного смысла
используется явно unspecified, не выдуманные Hz/seconds. Display labels RU/EN
строятся снаружи. Type определяет совместимость; quantity/unit сохраняют смысл
числа и не требуют нового неутверждённого автоматического конвертера.

describe_node не выделяет память, валидирует parameter variant и port count;
Полная проверка значений параметров/endpoint выполняется parameters_valid и
validate_graph отдельно: неполный NetworkInput draft сохраняет configured ports.
result не содержит dangling references на temporaries. Caller storage живёт
дольше descriptor. На buffer_too_small result не публикуется; требуемые counts
доступны отдельно либо в result с явно невалидными spans. input_ports,
output_ports, input_type, output_type и node_role становятся совместимыми
обёртками над этим источником правил. Runtime, editor/catalog, copy/paste,
validation и capabilities используют одну таблицу, без собственных списков.
В срезе 1 R вместе с S согласует в этом же registry descriptor параметров:
stable key, тип/range/default и classification `processing` / `presentation` /
`application-adapter` / `opaque-extension`. Classification не берётся из
widget fields; known presentation key нельзя отправлять как opaque processing
extension. Это handoff для projection S, без зависимости domain от JSON/Qt.

## 4. Узлы, данные и маршруты

| Узел/группа | Роль | Входы → выходы | Инвариант |
| --- | --- | --- | --- |
| AudioSource/TestSignal/NetworkInput | Source | 0 → configured output | Adapter поставляет ingress, часы/контракт явные |
| Существующие DSP stages | Processor | Existing semantic input → existing output | PCM/Window/Windowed/StereoSpectrum/Spectrum/Mel различны |
| Все View, включая ScalarView | Sink | 1 → 0 | Display settings не изменяют данные graph route |
| Spectrogram | Processor | Mel → history:0, scalar:1 | Port 1 = текущий filled count, 0..capacity; не magnitude metadata вместо порта |
| Splitter | Processor | T:0 → T:0..N-1 | N=2..8, fanout; borrowed frame без полной копии history |
| Switch | Processor | T:0 → T:0..N-1 | Только 1→N; selected=0..N-1; inactive outputs получают typed silence |
| Gate | Processor | T:0 → T:0 | enabled пропускает или выдаёт typed silence; output count всегда 1 |
| ScalarReduce | Processor | Numeric T:0 → Scalar:0 | maxabs/signed mean/RMS; Packet отклоняется |
| WLEDEncode | Processor | Mel:0 → WledPacket:0 | Pure encoding отдельно от доставки |
| WLEDOutput/NetworkOutput/legacy WLED | Sink | Configured input → 0 | Destination ресурсы у adapter; legacy WLED остаётся bridge |

Defaults только из утверждённого плана/совместимых текущих defaults:
Routing type Mel, outputs=2, Switch selected=0, enabled=true; ScalarReduce
Mel/maxabs; WLED gain=1; ScalarView display capacity=128. Gate не обязан хранить
selected/outputs; существующие неизвестные поля сохраняет document adapter.
Существующие history_size=400/Mel bands=50/window=4096 не переписывают явные
пользовательские значения. ESP defaults и ограничения отдельно от desktop.

Новые routes допускают все поддержанные T, включая Scalar/Packet/History;
редукция поддерживает существующие численные frame types и Scalar. Два stereo
канала участвуют в одном reduction population: max(abs(x)), sum(x)/N,
sqrt(sum(x*x)/N); double accumulator и checked finite output. History reduction
смотрит только newest logical row, включая silence override, O(bands).
Отсутствующая history row — unavailable до готового кадра, не валидное пустое
число. Packet reduction запрещён независимо от числа байтов.

History хранится `[frame][band]`, logical oldest→newest; renderer преобразует
через явный GUI boundary. Inactive ветка не обнуляет старые rows: каждый новый
clock tick даёт новую zero row, filled count растёт до capacity. При маршруте
уже готовой History borrowed override может представлять newest row без
изменения исходного ring; это не заменяет накопление независимой downstream
history. Проверять два rings в разных ветках и повторное включение. Fanout не
позволяет consumer изменять общий буфер.

Known contract + known clock при disconnect/disabled/inactive = доступный
типизированный zero frame со следующими sequence/timestamp и сохранённой
совместимой history. Packet silence — валидный WLED silence packet, не 44 нуля.
Unknown contract/clock = unavailable без stale payload. Первый валидный scalar
может быть 0; он не отбрасывается как признак отсутствия кадра.

## 5. Runtime API, состояние и память

OutputView должен всегда нести output identity, kind, contract/semantic frame и
metadata. Для каждого configured output emit/unavailable соответствуют его
type/index. Для sink нужна отдельная доставка принятого входа (например,
`SinkInputView{node,input_port,frame}` и `SinkInputCallback`), а не фиктивный
output 0. Совместимый desktop callback adapter может объединять эти события,
но не выдаёт sink-input за graph output. Этот интерфейс согласовать до среза 2.

```cpp
// Новый optional argument не подменяется desktop-capabilities внутри runtime.
PreparedGraph::prepare(definition, ingress_specs, capabilities, limits,
                       plan_out, context_out, previous_plan, previous_context);
GraphExecutor(ingress_specs, capabilities); // owned capability snapshot
// Все control/process вызовы сериализует owner, включая изменение capabilities.
```

GraphLifecycle валидирует реальные device caps, PreparedGraph повторно защищает
публичную direct-prepare границу теми же caps и resource limits. GraphExecutor
не игнорирует validated order. Старый API допустим только как явная desktop
compatibility wrapper вне portable runtime; embedded не получает desktop set
по умолчанию. Empty sets поддерживают ничего. Ошибка capabilities/version или
limits возникает до публикации plan/resources.

Подготовка строит per-output contracts, adjacency keyed by source port,
schedule/workspace и state transfer. Смена параметров готовит следующий plan;
apply атомарно меняет его на frame boundary. Plan immutable, mutable history,
sequence/epochs, WLED encoder scratch — в RuntimeContext. Foreign/stale prepared
bundle нельзя применить: сохраняется существующая проверка executor ownership
перед static casts, без RTTI. Не вводить второго scheduler/executor.

Состояние переносится по стабильной node/output identity и совместимому input
contract/geometry/clock lineage. Route enable/selected/gain/reduction без смены
контракта не очищают все histories; reset только несовместимых зависимых узлов.
Перенос/копия rings разрешены в prepare/apply, не в hot path. Explicit Stop
отдельно прекращает обработку и fence очередей/retries; смена устройства и
discontinuity отдельно инвалидируют соответствующую lineage. Graph revision
не равен producer epoch; sequence/generation не переиспользуются как telemetry.

Закреплённая политика Start/Stop для GraphLifecycle (gate 0): сначала runtime
принимает переход, затем lifecycle/control публикуют новый run intent, state/
running и revision. Failed Start или Stop не меняет working graph, run intent,
running/lifecycle state и revision; runtime port также сохраняет предыдущее
рабочее состояние при отказе. Нельзя предварительно менять intent и оставлять
его после rejected runtime call. Explicit Stop остаётся отдельной операцией от
parameter Prepare/Apply; применение параметров после failed Stop использует
прежний intent, после accepted Stop сохраняет stopped intent до accepted Start.
Повтор уже достигнутого состояния — accepted no-op без новой revision, как в
storage/control контракте. R меняет lifecycle только в выданном срезе; S использует
результат runtime/lifecycle для публикации control state.

Уточнение после gate 0 FAIL: R предоставляет transactional typed Start/Stop
(например, отдельные методы lifecycle `start_transactional` /
`stop_transactional`). Единственный shared run intent хранится в lifecycle и
commits только при runtime success; S не хранит второй intent. Runtime-port
contract гарантирует atomic failure без частичных effects, включая running,
ресурсы и fencing. Typed failed Start из stopped → последующий Apply остаётся
stopped; typed failed Stop из running → последующий Apply остаётся running;
оба отказа не меняют revision. Legacy adapters сохраняют characterization
прежнего requested-intent/Stop-fencing через явно выбранную legacy lifecycle
policy/API, а не через неявное изменение typed policy или duplicate intent S.
И typed, и legacy policy обращаются к одному GraphLifecycle/GraphExecutor.

Process не расширяет containers, не сериализует JSON/control и не делает
socket/flash I/O. Frames и stream_id borrowed лишь на callback/tick; async
consumer обязан retain в caller-provided BufferPool или bounded owned snapshot.
Pool exhaustion — явный Status, без fallback в unbounded allocation и без
partial publish; out retain остаётся прежним при ошибке. Packet bytes и ID
копируются вместе с samples/history. Size multiplication/addition проверяются
до allocation/subspan. Full retained history copy разрешена только boundary
с ограниченной частотой/ёмкостью, не как обязательный шаг каждого tick.

Новые public bounded APIs возвращают Status/error+used/required size, принимают
spans и явные limits; не бросают исключения и не используют RTTI. Подготовка
может использовать существующий bounded-by-limits heap; `-fno-exceptions` сам
по себе не обещает восстановление после allocator OOM — это ограничение надо
отделять от заранее обнаруживаемого превышения бюджета. MemoryRequirements
учитывает multi-output state/zero buffers/packet scratch/history/retention,
а platform admission отдельно учитывает adapter/control/transport резерв.

## 6. Capabilities и внешние границы

DeviceCapabilities должен явно объявлять supported processing versions,
node/type descriptors, transport/control codec support и effective limits:
nodes/edges/ports, FFT per channel, bands/history, runtime bytes, ingress/packet/
control message bytes, queue counts, destination limits. Effective limits =
intersection compiled kernel limits, configured profile и реально реализованных
adapter features. Доступность network/WLED transport не следует из NodeKind.
Не менять существующее ограничение одного desktop WLED endpoint без отдельного
согласования; encoder capability независимо от delivery capability.

ESP32 default FFT=2048/channel и history OFF, configurable limits; firmware
caps доступны до Prepare и не копируют desktop_capabilities. `Profile::admit`
даёт оценки резерва/throughput, не измерение heap или обещание hardware latency.
Actual capabilities и подтверждённый budget передаются host composition root.
TCP/UART byte transport, clock и explicit NVS persist реализуются снаружи;
slider/setparams не пишет flash. NVS last-known-good/version/storage ошибки
не являются runtime DSP state.

Typed application control поверх GraphLifecycle: capabilities/state queries,
Prepare/Apply/SetParameters/Start/Stop/ExplicitPersist. Commands содержат
request_id, expected_revision, processing DTO; результаты содержат status,
accepted revision/identity и structured error. Atomic reject сохраняет working
graph, revision, run intent и ресурсы. Prepare имеет идентификатор pending bundle,
связанный с base revision; последующий Apply не принимает чужую подготовку.
Точный DTO и revision policy принадлежат S (новый application/control), но не
могут обойти доменную validation или direct-prepare capabilities.

### Один global prepared slot: точный R/S handoff

Финальная политика — один global pending slot на device, advertised
`max_pending=1`; не per-session bundles и не multi-plan runtime. S владеет
reservation/token `{session, graph identity, base revision, expires_at}` и
проверяет reservation до вызова R Prepare. Foreign-session Prepare возвращает
Busy до validation/preparation и любого изменения R slot, независимо от
валидности чужого candidate. A Prepare → B invalid/valid Prepare = Busy в обоих
случаях; A Apply остаётся валидным при неизменной base revision.

R предоставляет transactional typed Prepare: candidate plan/context готовится
в staging, старый valid pending bundle не очищается до успешного завершения
validation/preparation. Same-session invalid Prepare сохраняет предыдущие
token/bundle; same-session successful Prepare атомарно заменяет свои token/bundle.
При первой неуспешной подготовке, когда прежнего valid bundle нет, S освобождает
новую reservation без публикации token. Existing legacy prepare reset behavior
сохраняется только явной legacy policy, не определяет typed Prepare.

Предлагаемый handoff API R: `prepare_transactional(candidate)` со staged commit,
`apply_transactional()` с consume только при success и `discard_prepared()` для
release. S сериализует `reserve/check → R prepare → publish token` и
`check session/token/revision/expiry → R apply → consume token` в одной owner
boundary; application command service не создаёт plan/context или executor.
R apply failure сохраняет pending bundle; S conflict/stale failure сохраняет
token/reservation до release, successful reprepare или expiry. Stale token не
даёт обходить revision check, но не удаляется автоматически при failed Apply.

Expiry default 30 s, configurable через ControlLimits; S использует injected
Clock с монотонным смыслом deadline. Explicit release/disconnect/expiry
инвалидируют только принадлежащую session reservation и вызывают R discard;
foreign-session release не касается чужого slot. Успешный Apply потребляет
token/bundle и освобождает reservation. Legacy adapter обязан согласовать доступ
к тому же slot с S, без обхода активной typed reservation; детали compatibility
вызова закрепляются при реализации с characterization. Нет второго executor,
второго run intent или списка runtime pending bundles.

SetParameters выполняется S только при пустом global slot. Если slot занят
любой session, включая вызывающую, S возвращает Busy до R. При пустом slot S
stage/apply внутренний candidate; отказ освобождает лишь этот внутренний
candidate, сохраняя working/intent/revision. R не восстанавливает заменённый
пользовательский pending и не создаёт второй slot/executor.

ControlCodec — adapter с caller buffers/limits; CBOR negotiated default и
рабочая Protobuf alternative, TinyCBOR v7.0/nanopb 0.4.9.2 с pinned hashes,
checked-in generated sources и reproducible generation у codec/SDK владельца.
BDSP framing и прежние data bytes не меняются; новые control message IDs и
новые stream types требуют negotiation. Нельзя отправлять Scalar/Packet под
wire type 0–6. JSON schema version, processing version и wire/codec version
независимы. Legacy Qt control adapter явно отображает старые команды на typed
application contracts, без Qt зависимости внутренних слоёв.

JSON document adapter: один пользовательский файл, logical processing /
presentation / app / presets / draft-applied, явная legacy migration. UI fields
исключаются в обеих control directions, неизвестные stable fields сохраняются
в storage. Malformed/future version нельзя нормализовать в supported default,
apply/save-overwrite отклоняются; unsupported lossy downgrade — ошибка.
RuntimeContext/history не проект. FFT channel/merge display-only settings
выносятся из processing с совместимым чтением старых полей. Presentation pending
layout связан с accepted request/revision/identity, не применяется после reject.

Callback, snapshot, preview, subscription, registry announcement и unavailable
доставка используют node+port вместе с graph/producer identity. Desktop должен
разделять Spectrogram output0 history и output1 count; нельзя записывать второй
кадр в node-only cache поверх первого. Поля packet preview декодируются pure
codec read-only, единицы явно отображаются. WLED cadence/send counter/stale
silence 300 ms принадлежат delivery adapter; encoder sequence не должен
подменять исторический send counter. Golden characterization старого WLED
фиксирует эти правила до переноса; socket retry не повторно выполняет DSP.

## 7. Проверки последующих срезов

Названия новых cases ниже — требования к поведению, не утверждение существующего
покрытия. Сначала failing tests нового поведения; перенос требует characterization.
Конкретный общий test-file ownership подтверждает координатор перед правками.

| Срез | Быстрые целевые тесты | Приёмочный результат |
| --- | --- | --- |
| 1 | `domain_contract_tests`, `signal_contracts_tests`: configured counts/indices, wrong variant/enum, descriptor caller buffers, roles, semantic type/quantity/unit, caps empty/unsupported/version, finite/overflow | Единые descriptors; стабильные IDs/0–6 goldens; shape не разрешает несовместимость |
| 2 | `node_routing_tests`, `dataflow_runtime_tests`: every output available/unavailable, heterogeneous Spectrogram ports, sink-input delivery, fanout/Switch1→N/Gate, two histories switching/disconnect, scalar zero/negative/stereo/RMS/latestrow, no allocation | Все пути маршрутизируются по порту; совместимое state не теряется; память ограничена |
| 2 | `signal_contracts_tests`, `codec_buffer_tests`, будущий `wled_protocol_tests`: pool exhaustion/packet lifetime, invalid geometry, encoder/decoder old golden, valid silence, read-only decode | Opaque packet ownership и WLED wire отделены от BDSP; cadence/counter у adapter |
| 3 | Runtime direct-prepare restricted caps; application lifecycle revision/reject/foreign bundle; control codec bounded malformed/truncated/oversize | Тот же runtime для local/remote; atomic reject, caller buffers и negotiation |
| 1–3 | `application_lifecycle_tests`: injected failed Start/Stop сохраняют graph/intent/state; последующий parameter Apply сохраняет прежний intent; accepted Stop/Start публикуют state лишь после runtime acceptance; control owner проверяет unchanged revision при отказе/no-op | Rejected transition не оставляет скрытого intent и не меняет работу схемы |
| 1 | R lifecycle: staged invalid Prepare сохраняет valid bundle; failed Apply сохраняет pending; typed failed Start→Apply stopped, failed Stop→Apply running; legacy requested-intent/Stop-fencing characterization отдельно. S service: A Prepare/B invalid и valid Prepare Busy/A Apply valid; same-session fail keeps token/success replaces; expiry 30 s/configurable Clock, release/disconnect ownership, failed conflict/stale keeps slot | Один global slot, `max_pending=1`; R/S atomic handoff без duplicate intent/executor |
| 3–4 | Adapter tests владельцев: two clients/conflict, node+port subscriptions/previews/registry, unchanged TCP/UART bindings при param edit, projection both ways | Не затронутые network ресурсы живут; UI не протекает в processing |
| 4 | `embedded_sdk_tests`, profile/admission tests, owner NVS failure injection; portable SDK install consumer без Qt/RTTI/exceptions/codegen | Реальные firmware caps/limits до Prepare, explicit persist/LKG, общие SDK sources |
| 5 | Совместимость history/old WLED/old documents/wire, desktop build+DLL, short integrated cases; согласованный полный auto run отдельно | Независимое новое ревью; author pass не заменяет приёмку |

На hot-path проверках история достаточно большая, чтобы поймать O(history)
копирование; reduction сравнивает newest row при отличающихся старых rows.
Malformed FrameView проверяется до использования subspan, nonfinite payload
не транзитит downstream. Synthetic сигнал 5–10 меняющихся тонов без playback.
Не запускать полный UI/FPS/30-minute suite или Linux специально. Native themes,
OpenGL/ESP-IDF crossbuild/hardware не считаются доказанными portable/offscreen
проверкой; доступный existing toolchain ищет соответствующий владелец.

## 8. Будущее владение и согласования

Сверены [implementation-plan.md](implementation-plan.md),
[storage-control-contracts.md](storage-control-contracts.md) и
[baseline-and-ownership.md](baseline-and-ownership.md). Основные semantic/control
решения совпадают. Typed control DTO и wire остаются у S, GraphLifecycle у R;
parameter classification согласуется в общем registry. Portable baseline 8/8,
описанный I, относится dirty workspace и не становится независимой приёмкой.
Приоритет имеет последнее прямое уточнение координатора: все CMake/manifests/
export/install — I; R — domain/signal/runtime/dsp и GraphLifecycle;
новый application/control — S. Предложения desktop/embedded в baseline не
являются разрешением брать эти файлы: назначение отдельно для каждого среза.

Сейчас разрешена запись только этого документа. Следующая таблица задаёт
предлагаемое точное владение для выдачи среза; до подтверждения координатора
никаких правок этих путей. Общий workspace: не откатывать чужие изменения.

| Ответственность | Будущий владелец / пути |
| --- | --- |
| Node/domain descriptors и validation | R: `domain/**`, кроме всех build manifests; новые descriptor headers/sources только в выданном срезе |
| Signal contracts/retention | R: `libs/signal_contracts/**`, кроме всех build manifests |
| Multi-output plan/state/runtime | R: `libs/dataflow_runtime/**`, кроме всех build manifests |
| GraphLifecycle / preparation-runtime ports | R: `application/include/application/graph_lifecycle.h`, `application/src/graph_lifecycle.cpp`; cross-owner requirements согласовать с S до записи |
| Целевые portable tests | R по baseline: `tests/domain_contract_tests.cpp`, `tests/signal_contracts_tests.cpp`, `tests/application_lifecycle_tests.cpp`, `tests/dataflow_runtime_tests.cpp`, `tests/node_routing_tests.cpp`; новые cases/files отдельно назначаются; registration у I |
| Численные kernels | R: `libs/dsp_kernels/**`, кроме build manifests; ABI и численные goldens сохраняются; правки лишь в выданном срезе |
| Pure WLED codec | Предложение среза 2: `libs/wled_protocol`; новый root/header/test назначает координатор после согласования R/S/I; targets/export/install у I |
| Application commands/revisions/persistence ports | S: новые `application/include/application/processing_control.h`, `application/src/processing_control.cpp` и control/persistence contracts; исключение GraphLifecycle — R |
| BDSP/control codecs/legacy protocol | S: `libs/bdsp_protocol/**`, кроме build manifests; перенос чернового WLED header согласовать с R/I до записи |
| Desktop delivery/documents/resource reconciliation | Назначение отдельно по срезам координатором; существующие S/P reservations из baseline сохраняются, этот чат не забирает `adapters/desktop/**` глобально |
| Qt presentation/ports | P: `presentation/**`, кроме build manifests; получает descriptors и semantic unit mapping; display transform только здесь |
| Embedded/control/NVS/platform | Назначение отдельно по срезам; этот чат сейчас не берёт `libs/embedded_support/**`, `adapters/esp_idf/**`, examples или SDK tests; proposal baseline не равен выдаче |
| Все build/SDK manifests и composition | I: все `**/CMakeLists.txt`, `cmake/**`, `tests/*.cmake`, build manifests, dependency pins/codegen scripts, export/install/SDK packaging и `apps/**` roots; R передаёт требования targets/sources/tests, не пишет эти файлы |

Обязательные handoffs до реализации: подтвердить sink-input callback и
per-output identity envelope; согласовать caps construction/ownership с
application/embedded владельцами; закрепить WLED codec ownership/export и
characterization send-counter; закрепить portable test files. Вопросы направлять
координатору, не менять чужие paths. Срез 0 готов к независимому ревью; известные
дефекты черновика и аппаратные ограничения остаются открыты до будущих срезов.

## 9. Отчёт среза 1 R

Gate 0 принят, scope R выдан координатором. Авторская реализация не означает
независимой приёмки. Изменены только следующие исходники и четыре закреплённых
test files; этот документ обновлён. Уже существовавшие изменения
`buffer_pool.cpp`, runtime CMake, BDSP/desktop/Qt/embedded не принадлежат этой
работе и не откатывались. HEAD diff включает непринятый предыдущий черновик:
не приписывать весь diff этого файла только срезу 1.

* `domain/include/domain/graph_definition.h`, `domain/src/graph_definition.cpp`:
  explicit node registry, roles/Unknown, configured descriptors с caller storage,
  identity/direction/quantity/unit, parameter metadata/classification и explicit
  processing node mapper. Variant/type/count errors не публикуют spans; endpoint
  readiness не стирает порт неполного draft.
* `domain/include/domain/graph_validation.h`, `domain/src/graph_validation.cpp`:
  strict validate_processing_graph, FFTView NoParameters наряду с legacy display
  variant в general validation; numeric bounds берутся из parameter registry;
  frame/node membership не выводится из enum ranges.
* `libs/signal_contracts/include/signal_contracts/frame.h`,
  `libs/signal_contracts/src/frame.cpp`: explicit stable frame wire values 0–8,
  static semantic frame descriptors, shape/stereo lookup и checked unknown type.
  Прежняя packet-specific validation сохранена до согласованного переноса среза 2.
* `application/include/application/graph_lifecycle.h`,
  `application/src/graph_lifecycle.cpp`: transactional prepare/apply/start/stop,
  discard/query для handoff S; единственные pending_ и run_requested_. Старые
  методы явно помечены legacy policy, requested-intent/Stop-fencing сохранены.
* `libs/dataflow_runtime/include/dataflow_runtime/runtime.h`,
  `libs/dataflow_runtime/src/runtime.cpp`: direct prepare с explicit actual caps,
  GraphExecutor owned caps snapshot, отказ до plan publication, лимиты Window/
  History проверяются даже без известного ingress. Numeric enum shortcuts
  заменены named membership без расширения маршрутизации.
* Tests: `domain_contract_tests.cpp`, `signal_contracts_tests.cpp`,
  `application_lifecycle_tests.cpp`, `dataflow_runtime_tests.cpp`.

### Доступные APIs для S/C/P

GraphLifecycle: `prepare_transactional(graph)` и
`prepare_transactional(graph,const RuntimeLimits&)`, `apply_transactional()`,
`start_transactional()`, `stop_transactional()`, `discard_prepared() noexcept`,
`run_requested() const noexcept`, `pending_memory_requirements() const noexcept`.
Limits overload применяет лимиты к candidate и не вызывает legacy setter,
очищающий slot. S reserve/check сериализует вызовы; S хранит лишь metadata.
MemoryRequirements — by value; working_graph pointer borrowed до следующего
successful Apply/destruction. Runtime-port failure обязан сохранять переданный
bundle, working/running/resources/fences; foreign bundle identity проверяется
перед casts. Revision/Clock/session policy реализует S.

`PreparedGraph::prepare(graph,ingress,const DeviceCapabilities&,limits,plan,context,
previous=nullptr,previous_context=nullptr)` и
`GraphExecutor(ingress,DeviceCapabilities)` используют explicit actual caps.
Старые overload/constructor оставлены как документированная legacy desktop
compatibility для existing callers; typed/device composition использует новые
с тем же caps snapshot, что GraphLifecycle/S. Адаптация чужих callers здесь
не выполнялась.

Domain lookup: `node_descriptors()/node_descriptor(kind)`,
`parameter_descriptors(kind)/parameter_descriptor(kind,key)`,
`parameter_classification(kind,key)`, `describe_node(node,input_storage,
output_storage,result)`. Registry pointers static lifetime; descriptor spans
принадлежат caller buffers, counts доступны в required_inputs/required_outputs.
Общий `enabled` находится через parameter_descriptor(kind,"enabled"); остальные
registered keys перечисляет parameter_descriptors(kind). Unknown key —
OpaqueExtension; known FFTView channel/merge — Presentation. `project_processing_node`
создаёт явную копию с view NoParameters без изменения исходных display values;
`validate_processing_graph` отвергает legacy display variant в typed path.

### Red/green и команды

Изолированная сборка `build-refactor-domain`, GNU 13.1, Release, portable SDK,
GUI OFF, exceptions/RTTI OFF. CMake/compiler/Ninja/BDSP source найдены из
существующего `build-refactor-baseline/CMakeCache.txt`; ни tools installation,
ни hardcoded project paths не добавлены. Manifests не изменены, использованы
existing targets. Configure exit 0.

1. Новые tests до реализации: build exit 1, missing descriptor/transactional/
   actualcaps APIs (red compile).
2. После API implementation: build exit 0, четыре целевых tests 4/4 passed.
3. Direct prepare неизвестного Window при max_frame_samples=512: test red
   (принимал default 4096); после admission до построения plan — green.
4. Draft NetworkInput с пустым endpoint: test red (исчезали ports); после
   разделения структурного descriptor и полного domain validation — green.
5. Финальная сборка/проверки после дополнительных legacy/no-op/snapshot cases:
   build exit 0, CTest exit 0, 4/4 passed, 0.06 s.

```text
cmake --build build-refactor-domain --target domain_contract_tests signal_contracts_tests application_lifecycle_tests dataflow_runtime_tests -j 4
ctest --test-dir build-refactor-domain --output-on-failure -R "^(domain_contract_tests|signal_contracts_tests|application_lifecycle_tests|dataflow_runtime_tests)$"
```

Артефакты: build-refactor-domain/bin/*_tests.exe, CMakeCache.txt,
Testing/Temporary/LastTest.log. Проверены actual caps deny/accept и owned snapshot,
processing/version/receive rejection, prepare invalid/budget/preparer failure
preserves pending, apply failure retry, typed failed Start→Apply stopped и failed
Stop→Apply running, accepted no-op, legacy intent/fencing, descriptor ports всех
видов, count semantics, strict scalar/unknown types и FFT projection preservation.

Ограничения: общая desktop сборка и targeted integration выполняются I после
интеграции владельцев, не заменяются этими portable tests. Global slot/session/
expiry/revision тесты принадлежат S; этот срез предоставляет R handoff. Native/
hardware не проверены. WLED library extraction, per-port unavailable callbacks,
sink-input callback и прочая маршрутизация остаются срезом 2, не исправлены здесь.
Статус: R ready for integration and independent review; срез 2 требует нового gate.

## 10. Исправления после первичной независимой приёмки FAIL

Замечания domain/runtime reviewer: actual-caps direct Prepare и GraphExecutor
принимали legacy FFTViewParameters несмотря на strict validation; configured
Mel bands обходили max_frame_samples при неизвестном ingress. Дополнительно
storage/control reviewer потребовал bounded DiagnosticLocation и authoritative
parameter-update preflight. Scope остаётся срезом 1, не маршрутизацией среза 2.

### Исправленный runtime и geometry admission

Actual-capabilities PreparedGraph overload и двухаргументный GraphExecutor
constructor теперь используют strict validate_processing_graph. Старые overload
и одноаргументный/default constructor сохраняют отдельную явную private legacy
policy с validate_graph; это тот же prepared implementation/executor.
FFTViewParameters{Right,Maximum} в typed/device path возвращает invalid_argument,
в legacy path сохраняется совместимость. После отказа executor прежний pending
bundle всё ещё можно применить.

Новый общий helper `application::validate_configured_limits(graph,limits)`
используется lifecycle и direct runtime до discovery/allocation. Проверяет
Window size, Mel bands, History capacity и известные Scalar=1/WledPacket=44
input/output размеры, включая Spectrogram count output. Остальные PCM/vector
размеры зависят от ingress и проверяются при discovery; WindowFunction/FFT и
другие сохраняющие/уменьшающие geometry stages не добавляют самостоятельной
configured размерности. History bands приходят из Mel/ingress; capacity известна
из параметров. Packet размер сравнивается с существующим column limit так же,
как в прежнем known-ingress path, без нового wire/packet format.

NetworkInput(Spectrum) → Mel(50) при max_frame_samples=16 отклоняется exhausted
как с unknown ingress, так и с known Spectrum columns=8/FFT=16. Lifecycle
отказывает до preparation-port call, оставляя prior pending/working/run intent;
меньший Mel(8) остаётся доступен. Direct admission не публикует partial plan.

### Domain diagnostic API / handoff S

```cpp
struct DiagnosticLocation {
    NodeId node = -1;
    std::string_view parameter_key;
    std::optional<PortDirection> direction;
    std::optional<std::uint16_t> port;
    std::optional<std::size_t> edge_index;
};
// Append location after previous ValidationResult fields; keep old aggregates.
ParameterValidationResult validate_parameters(NodeKind, const NodeParameters&) noexcept;
ValidationResult validate_processing_parameter_update(
    const GraphDefinition&, NodeId, const NodeParameters&) noexcept;
```

ValidationResult.location appended; legacy node/edge_index сохранены. Edge zero
отличается от отсутствующего edge только через location.edge_index. Keys — static
registered/root literals с lifetime процесса, не request-owned strings; отсутствуют
при structural variant mismatch/нерегистрированном legacy поле. Domain сообщает
amplitude, minimum_hz, period_seconds и другие конкретные нарушения; connection
ошибки несут source-output либо destination-input identity/index и edge.
Network unsupported type несёт data_type + direction/port без edge.

Preflight update не копирует graph/NodeParameters и не мутирует candidate.
Missing node возвращает MissingNodeOrPort с requested node identity; view update
возвращает InvalidParameters, включая NoParameters/enabled-only (граница среза 1
подтверждена S). FFTViewParameters update указывает channel (Presentation).
S делает literal copy ValidationResult.location в reply и строит candidate только
после success; controller не повторяет правила/выбор keys. API и lifetime
согласованы через координатора и напрямую с S.

### Проверки, paths, заморозка

Изменены в этой серии исправлений: runtime header/source, lifecycle header/source,
domain graph_definition.h/graph_validation.h/graph_validation.cpp, tests
dataflow_runtime_tests.cpp/application_lifecycle_tests.cpp/domain_contract_tests.cpp
и этот документ. CMake/manifests/S/C/I/desktop/BDSP не редактировались. Own build
`build-refactor-domain` автоматически перечитал обновлённые manifests I, но не
использовал shared build.

Red до правок: dataflow_runtime_tests assert strict direct rejection exit 1;
application_lifecycle_tests configured Mel unknown-ingress admission exit 1;
diagnostic tests compile exit 1 из-за отсутствующего location. После исправлений:
build exit 0, финальный целевой CTest exit 0, 4/4 passed, 0.07 s. Команды те же
две из раздела 9, без новых tools/install/codegen/targets. Проверены strict versus
legacy, prior bundle apply after rejected direct prepare, known/unknown Mel,
границы Window/Mel/Scalar/WLEDEncode/NetworkInput Scalar/History, pending/working
preservation, precise parameter keys и missing/view/connection identity.
`git diff --check` scoped exit 0; Windows LF→CRLF warnings не являются ошибками.
Результат независимого исходного repro не присваивается авторской проверке:
reviewer самостоятельно повторяет build-gate1-domain-review/repro.cpp.

Статус: R implementation frozen / ready for independent re-review. S проверяет
reply translation отдельно, I делает shared rebuild только после готовности
владельцев. Общая desktop сборка/hardware не подтверждаются этим прогоном;
никакие ветки/коммиты/user config/output не изменены. Срез 2 запрещён до gate.

## 11. R2: proposed API handoff среза 2 (источники не изменены)

Прочитаны актуальные AGENTS.md, implementation-plan.md и ProposedSlice2 из
baseline-and-ownership.md. Закреплённый архитектурный справочник полностью
прочитан ранее в этом чате; повторное чтение той же редакции не требуется.
Первичный gate 1 FAIL и авторские ремонты выше — история; координатор принял
gate 1 после независимой повторной приёмки и общей интеграции. Незакоммиченный
черновик среза 2 ещё не принят. В этой стадии изменяется только этот документ,
без configure/build/test или runtime implementation.

R2 владеет runtime.h/.cpp, signal frame/buffer_pool headers/sources и
node_routing_tests/dataflow_runtime_tests/signal_contracts_tests. Domain descriptor
уточнения только по согласованию. D2 владеет numerical/WLED helpers и extraction;
T2 — desktop delivery/cache/stream DTO/legacy adapters; I — все manifests,
deps/export/install/build. Application/control, JSON, UI, IDF не меняются.
Gate 1 strictcaps, transactional lifecycle и единственный executor сохраняются.

### 11.1 Canonical envelope и callbacks — точное предложение R2/T2

```cpp
struct DeliveryStamp {
    std::uint64_t graph_revision = 0; // local accepted applied revision
    std::uint64_t epoch = 0;          // local engine/session identity scope
};
struct PortFrameView {
    std::string_view graph_id;        // borrowed from owning prepared plan
    domain::PortIdentity identity;    // {node, port}; stable configured index
    domain::PortDirection direction;
    domain::NodeKind kind;
    signal_contracts::PortContract contract;
    bool contract_known = false;
    signal_contracts::FrameView frame;
    bool window_emitted = false;
    DeliveryStamp delivery;
};
using OutputFrameCallback = void (*)(void*, const PortFrameView&);
using SinkInputCallback = void (*)(void*, const PortFrameView&);
struct RuntimeCallbacks {
    OutputFrameCallback output = nullptr;
    SinkInputCallback sink_input = nullptr;
    void* user = nullptr;
};
// PreparedGraph:
Status process_ports(domain::NodeId ingress, const FrameView&, RuntimeContext&,
                     DeliveryStamp, RuntimeCallbacks = {}) const noexcept;
// GraphExecutor:
Status process_ports(domain::NodeId ingress, const FrameView&,
                     DeliveryStamp, RuntimeCallbacks = {}) noexcept;
```

Stamp передаётся явно: production caller T2 использует accepted local revision
и engine epoch; нули допустимы для direct portable characterization, не означают
автоматический inference из incoming frame. Envelope.delivery не переписывает
producer FrameMetadata: epoch/producer_epoch/generation/sequence/source_sequence/
timestamp и incoming graph_revision сохраняют собственную lineage. Local
graph_revision и producer graph_revision не смешиваются. Canonical cache/subscription
key `{delivery.epoch, graph_id, node, direction, port}`; accepted graph_revision
версионирует запись/reconciliation, не создаёт новый NodeId. Transport endpoint
identity добавляется T2 за runtime boundary.

Output callback получает только реальные configured output ports с Output
direction. Sink callback получает принятый кадр на реальном input port Sink,
с Input direction и actual configured kind; sinks не создают output 0. События
для processor input не добавляются: это не новый трассировщик всех edges.
Sources/processor outputs и sink-inputs используют тот же frame metadata/contract
с корректным типом, включая отдельный Spectrogram History:0 и Scalar count:1.
Оба callbacks используют один caller user pointer, вызовы сериализованы, без
reentrancy prepare/apply/stop внутри callback. Null callback не отключает DSP
или последующий sink delivery, только наблюдение.

`contract_known=true` означает resolved geometry в prepared plan, даже когда
текущий marker unavailable. false означает неизвестную geometry/clock, но
declared semantic type остаётся в contract.type/frame.type. Не строить geometry
из пустого marker payload. Unavailable выдаётся per-output exact type/index;
sink unavailable выдаётся только Input. Frame payload пуст, available=false,
sequence=0, discontinuity=true. Остальные идентификаторы producer lineage и
local stamp сохраняются; invalid/stale source не маскируется под zero.

Borrowed envelope, graph_id, stream_id, samples, packet и History rows валидны
только во время callback. Даже если часть storage физически живёт до next tick,
consumer не сохраняет spans. Snapshot/queue owner копирует до возврата. Callback
не сериализует control/JSON и не меняет параметры; wire encode/retention — внешний
bounded boundary. Нельзя удерживать ссылку на RuntimeContext/PreparedGraph как
замену owning frame queue без гарантии lifetime и mutable ring isolation.

### 11.2 Bounded envelope retention / per-port queries

Предлагается в runtime.h, без domain зависимости signal_contracts:

```cpp
struct RetainedPortFrame {
    // Owns frame payload+stream_id and graph_id separately via caller pools.
    signal_contracts::RetainedFrame payload;
    signal_contracts::BufferHandle identity_storage;
    // Copied POD tags and graph_id span into identity_storage.
    // view() reconstructs the envelope by value, never allocates.
    PortFrameView view() const noexcept;
};
Status retain_port_frame(const PortFrameView&,
    signal_contracts::BufferPool& payload_pool,
    signal_contracts::BufferPool& identity_pool,
    RetainedPortFrame& out) noexcept;
// PreparedGraph (control plane, borrowed history valid only until next mutation):
std::optional<PortContract> port_contract(domain::NodeId,
    domain::PortDirection, std::uint16_t port) const noexcept;
HistoryView history(domain::NodeId, std::uint16_t output_port,
                    const RuntimeContext&) const noexcept;
```

No heap fallback. Payload pool копирует packet bytes, samples/logical retained
History rows и stream_id; identity pool копирует graph_id. POD tags/delivery stamp/
contract копируются by value. Отказ любой acquire/validate/overflow оставляет
out прежним и освобождает временные handles. Caller pools/storage переживают
все owners; copy/move retained owner сохраняет обе buffer handles и неизменные
bytes. Запись в shared published buffer запрещена. Если pools совпадают, caller
резервирует два slots на один frame; рекомендуются раздельные identity/payload
pools. Размер graph_id ограничивает caller slot admission, не новый глобальный
лимит с изменением старых IDs. queue capacity, sizes, dropped/overflow policy
фиксирует T2 до запуска; старые live pointers не сохраняются при exhaustion.

Полная History не копируется на каждом DSP tick. T2 материализует её только
по demand через сериализованный snapshot вне process; newest_override учитывает
отдельно для конкретного endpoint до snapshot. Retention API допускает bounded
owning snapshot, но не предписывает eager копирование всей истории в callback.

port_contract возвращает nullopt для invalid/unresolved port; descriptor
различает declared valid identity от unknown geometry. Node-only history(id,ctx)
остаётся explicit legacy port-0 wrapper. history(id,nonhistory_port,ctx) пуст;
count port никогда не получает History snapshot. Sink history snapshot относится
к Input и реконструируется из принятого входа через T2 retained delivery, а не
через fake graph output. Если T2 нужен дополнительный input-history query,
согласовать directional overload до implementation, не вводить node-only cache.

### 11.3 Explicit legacy compatibility (согласован R2/T2 seam)

Координатор разрешил сохранить old process/OutputView consumers, включая
embedded consumers вне текущего scope. Canonical callbacks никогда не получают
sink как output. Старый process становится явно документированным compatibility
wrapper того же executor. Точная форма compatibility API:

```cpp
struct LegacyNodeDeliveryView {
    domain::NodeId node;
    domain::NodeKind kind;
    FrameView frame;
    bool window_emitted = false;
    std::span<const float> display_values;
    std::uint16_t output_port = 0;
    domain::PortDirection direction = domain::PortDirection::Output;
    std::string_view graph_id;
    DeliveryStamp delivery;
};
using OutputView = LegacyNodeDeliveryView;
using OutputCallback = void (*)(void*, const OutputView&);
// PreparedGraph: existing signature preserved
Status process(domain::NodeId, const FrameView&, RuntimeContext&,
               OutputCallback = nullptr, void* user = nullptr) const noexcept;
// GraphExecutor: existing signature preserved
Status process(domain::NodeId, const FrameView&,
               OutputCallback = nullptr, void* user = nullptr) noexcept;
```

Старые fields/order сохранены; appended direction/local stamp/graph identity.
Legacy process передаёт DeliveryStamp{} (revision=0, epoch=0), не выводит local
stamp из producer metadata. display_values остаётся пустым, как сейчас.
Для исторического sink event direction=Input, old output_port
поле трактуется только как deprecated legacy slot index; оно не объявляет
существование output. Нельзя передать такой event canonical output callback.
Нет второго scheduler/plan/state; legacy wrapper адаптирует canonical events.

T2 преобразует delivery в старые Qt DTO/signals на границе: исторический port 0
может оставаться legacy view, но Spectrogram count:1 не перезаписывает History:0.
Все новые snapshots/previews/subscriptions/unavailable keyed by port. Owner
temporary Qt bridge/removal — T2, gate после consumer handoff среза 4 и parity;
embedded legacy wrapper removal — назначенный embedded owner отдельно, не R2
за чужого владельца сейчас. Публичные необходимые compatibility includes/IDs
не удаляются ради структуры. ABI требует общей recompilation I; не смешивать
старые/new archives, explicit alias не означает обещание прежнего sizeof.

### 11.4 D2/R2 helper seams

Reduction согласован по смыслу, точная сигнатура предложения D2:

```cpp
enum class ScalarReduction { MaximumAbsolute, SignedMean, Rms };
struct ScalarReduceLimits { std::size_t max_values; };
ScalarReduceStatus scalar_reduce(ScalarReduction,
    std::span<const float> first, std::span<const float> second,
    ScalarReduceLimits, float& output) noexcept;
```

D2 enum/status std-only, без NodeKind/FrameView/JSON/Qt. Runtime explicit named
mapping domain Reduction, не cast по enum ordinal. R2 выбирает оба stereo
канала, одиночный vector/scalar или latest logical History row incl override.
Empty History — unavailable, не stale/available scalar. Total count проверяется
до scan против prepared checked limit; double accumulator, finite checked result,
output unchanged on failure. Status mapping R2: EmptyInput→unavailable,
LimitExceeded→exhausted, NonFiniteResult→overflow, InvalidOperation/NonFiniteInput
→invalid_argument; outputs invalidated typed unavailable без старого scalar.
Обычный numeric input validation происходит до numerical helper.

WLED package/API пишет D2. Согласован strict bounded encode_mel(mel,peak_hz,
magnitude,gain,counter,destination,limits); pure decode/validate; valid silence
packet; fixed44 result used/required и caller destination. Scratch50+16 fixed,
без heap, no stream/UDP/BDSP dependencies. encode_bins legacy sanitation
сохраняется отдельно для старого make_packet bridge; encode_mel rejects nonfinite/
gain вне0..100 и вычисленный overflow. Mel source может иметь >50 bands из
NetworkInput; passed max_mel_values = prepared input columns, без нового
hardcoded50 source cap. Старое interpolation/wire поведение фиксируют D2 goldens.

Proposed graph packet counter=neutral0; destination T2 меняет counter через
validating D2 write_counter в своём owning44-byte send copy. Pure encoder не
инкрементирует counter/clock, не хранит socket state; legacy cadence/sendcounter/
stale300ms остаются T2. Не кодировать metadata.sequence как sendcounter.
Точные signatures/statuses находятся в [D2 handoff](dsp-device-codecs.md):
destination перед Limits, explicit limits без defaults. Counter policy подтверждён
D2/T2; decoder bounds и primary-source evidence принадлежат D2.

Signal generic validator R2 проверяет opaque Packet geometry и common finite
metadata, не AudioSync header/float offsets. Device-byte validation/decoding —
D2 pure helper на type-specific consumer/ingress boundary; invalid device packet
не отправляется UDP. R2 runtime output из encode/silence всегда validated helper.
D2 owns extraction старого device_protocols header/forwarder, I deps/export;
R2 меняет только свои include/call sites после принятия API.

### 11.5 Routing/state invariants и целевые tests

Splitter 1→N (2..8), Switch только1→N, Gate1→1. Prepared adjacency/contracts
переносят port identity, не node-only output. Known inactive с clock получает
typed zero; Unknown contract/clock — unavailable. При Gate/Switch на History
новая logical row zero, прежние rows сохраняются; own downstream accumulators
независимы. Новые zero rows не создаются на duplicate/stale event. Packet zero —
valid D2 silence, не44 нуля. State transfer только совместимые затронутые
contracts; selected/enabled не очищают все histories. Explicit Stop и accepted
device change остаются отдельными lifecycle событиями gate1. Hidden GUI
останавливает только render, не callbacks/DSP.

| R2 test file | Предлагаемые точные acceptance cases |
| --- | --- |
| node_routing_tests.cpp | Every Splitter/Switch output identity; Spectrogram History0/Scalar1 typed available и unavailable; sink callback Input only/no canonical fakeoutput; different stamps/source epochs; typedzero/scalar firstzero; Gate/Switch selected only1→N; both histories retain oldrows + appendzero on inactive; packet valid silence; no hotpath allocations |
| dataflow_runtime_tests.cpp | Per-port contracts/history query; detached known-clockzero vs unknown-unavailable; port shrink/reset only affected; revision/producer lineage/dedup/discontinuity; single prepared executor/strict caps/lifecycle regressions; legacy callback sink direction and oldconsumer characterization |
| signal_contracts_tests.cpp | Opaque packet generic validation; retain payload/packet/streamID lifetime; RetainedPortFrame graphID ownership lives after source plan destruction; pool exhaustion/overflow unchangedout/refcounts; both ports retained concurrently without overwrite |

Новые tests сначала failing, перемещение DSP/codec — D2 characterization.
T2 отдельно проверяет History/count cache, accepted localrevision, subscriptions/
preview/unavailable, UDP golden counter/cadence/stale и legacy oldconsumers.
Wire identities не меняются скрыто: IDs0..6/payload/framing goldens, новые
negotiated Scalar/Packet не masquerade as legacy. Subscription identity extension
назначает T2 после API handshake/координатора; CBOR/control среза3 здесь не вводятся.

### 11.6 Статус handoff

R2↔D2 reduction/status/ownership и WLED signatures согласованы; D2 документ прочитан.
R2↔T2 exact callbacks, local stamp epoch/revision, per-port queries и retention
подтверждены. Sink snapshots используют T2 retained Input delivery; дополнительный
directional history query не нужен для текущего handoff. Legacy сохраняет exact
старые signatures и fields с additive direction/graph_id/delivery, как выше.
Canonical runtime API не зависит от Qt или wire. Coordinator implementation
authorization ещё не выдан; этот отчёт не является acceptance.

Изменён только docs/refactoring/node-runtime-contracts.md. Build/tests этой
контрактной стадии не запускались; own future build-refactor-runtime-slice2
только после API gate и assignment I manifests. R2/D2/T2 direct signature exchange
авторизован координатором. Status: R2 CONTRACT READY, implementation NOT STARTED.

## 12. R2 source freeze / author evidence среза 2

Координатор принял section11 и явно разрешил R2 implementation. Файлы R2 этой
стадии: runtime.h/runtime.cpp, signal_contracts/src/frame.cpp,
tests/node_routing_tests.cpp, tests/signal_contracts_tests.cpp и этот документ.
Другие domain/application/signal файлы не изменялись в срезе2. Dirty изменения
среза1 и других владельцев остаются отдельными; git diff от HEAD не равен R2 diff.

Реализованы canonical process_ports, Output/Sink Input callbacks, per-port
contract/history queries, local delivery stamp без перезаписи producer revision/
epoch, bounded retained envelope с owning graphID/streamID/payload и atomic failure.
Публичный header передан T2 сразу. No full History materialization на process path.
Unknown contracts распространяют declared typed unavailable по всем outputs;
Spectrogram unavailable отдельно History:0 и Scalar:1. Legacy unavailable observer
сохраняет исторический node/port0 projection; canonical получает все реальные ports.
Old process/OutputView alias и additive direction/graphID/stamp сохраняются.

Один private process_impl обслуживает оба entrypoints, с одним планом, state и
расписанием. Координатор отдельно принял explicit legacy observer: canonical sink
получает accepted Input до исторического WLED gain; gain/result доступен только
legacy callback. Canonical WLED sink не выполняет этот presentation/device transform.
Characterization fixture проверяет Mel .25→legacy .5 при gain2, canonical Input .25,
producer storage unchanged. T2 владеет дальнейшим разделением desktop bridge.
Node-only history сохраняет старые sink snapshots; новый directional port contract
и output-port history не создают fake sink outputs.

D2 frozen headers получены и pure helper sources используются без дублирования.
ScalarReduce explicit named mapping, обе stereo channels или latest logical History
row incl override; checked prepared population budget. Ошибки helper переводятся
по section11, outputs unavailable без stale scalar. WLEDEncode strict encode_mel,
prepared input.columns limit (без source cap50), neutral counter0. Fixed valid
silence готовится до processing; Switch/Gate/detached packet используют его.
Runtime ingress packet проверяет pure device validator. Generic signal validate
проверяет только opaque44 geometry/common metadata; AudioSync header/float offsets
удалены из signal_contracts. Codec/forwarder и numerical helper принадлежат D2.

### 12.1 Actual checks

Red1: direct g++ -std=c++20 -fno-exceptions -fno-rtti compile
tests/node_routing_tests.cpp exit1: отсутствовали PortFrameView/process_ports/
port_contract/history overload. Fixture typo g.id исправлен на g.graph_id.
Red2: signal_contracts_tests executable exit1 на новом opaque44 packet fixture;
после удаления device wire parsing green. Во время миграции обнаружены и исправлены
legacy unavailable count regression и unknown PCM geometry admission regression.

Own folder build-refactor-runtime-slice2 настроен existing CMake/Ninja/MinGW,
Release portable SDK, GUI OFF, TESTING ON, PORTABLE_DISABLE_EXCEPTIONS_RTTI ON,
FETCHCONTENT_FULLY_DISCONNECTED ON и BDSP source из build-refactor-domain cache.
Ничего не устанавливалось; все CMake/manifests принадлежат I.
До подключения новых helper deps CTest runtime/node/signal/lifecycle: 4/4 passed.
После helper integration runtime tests собирались напрямую, поскольку I linkage
ещё не внедрён. Последний author evidence:

* r2_node_routing_direct.exe: compile/run exit0; весь compile с -fno-exceptions
  -fno-rtti, assertions enabled, allocation guard after preparation. Canonical
  ports/types/stamps/producer metadata; unavailable sinks; unknown contracts;
  graphID/streamID/payload retention/copy/exhaustion/identity failure cleanup;
  обе PCM channels signed mean/RMS; first available zero; latest wrapped History
  row/override; Gate/Switch newestzero; both histories old rows + appended zeros;
  packet valid silence/private storage/neutralcounter/malformed ingress/overflow.
* r2_dataflow_direct.exe: runtime object compile с -fno-exceptions -fno-rtti,
  executable compile/run exit0. Existing target preserves capability/prepare/
  lifecycle/continuity/retired producer epoch/noallocation regressions. Test harness
  operator new throws, поэтому сам harness собирался с exceptions, runtime — без.
* CMake build signal_contracts_tests/application_lifecycle_tests/domain_contract_tests
  exit0; CTest exact regex этих трёх targets: 3/3 passed, 0.07s.
* Scoped git diff --check exit0 (Windows LF→CRLF warnings не ошибки).

Direct compile reproducible command shape (cwd repository, tool paths из cache):

```powershell
$env:PATH='C:/Qt/Tools/mingw1310_64/bin;'+$env:PATH
$r2Includes=@('-Ilibs/dataflow_runtime/include','-Ilibs/signal_contracts/include',
 '-Iapplication/include','-Idomain/include','-Ilibs/bdsp_protocol/include',
 '-Ilibs/dsp_kernels/include','-Ilibs/wled_protocol/include')
$r2Sources=@('libs/dataflow_runtime/src/runtime.cpp','libs/signal_contracts/src/frame.cpp',
 'libs/signal_contracts/src/buffer_pool.cpp','libs/dsp_kernels/src/scalar_reduce.cpp',
 'libs/wled_protocol/src/wled_packet.cpp')
$r2Archives=@('build-refactor-runtime-slice2/lib/libgraph_application.a',
 'build-refactor-runtime-slice2/lib/libgraph_domain.a',
 'build-refactor-runtime-slice2/lib/libdsp_kernels.a',
 'build-refactor-runtime-slice2/lib/libbdsp_protocol.a',
 'build-refactor-runtime-slice2/lib/libdsp.a')
& 'C:/Qt/Tools/mingw1310_64/bin/g++.exe' -std=c++20 -O2 -fno-exceptions -fno-rtti `
 @r2Includes tests/node_routing_tests.cpp @r2Sources @r2Archives `
 -o build-refactor-runtime-slice2/bin/r2_node_routing_direct.exe
& build-refactor-runtime-slice2/bin/r2_node_routing_direct.exe
```

Dataflow direct command использует отдельно скомпилированный теми же flags
runtime.cpp → r2_runtime.o, затем g++ -std=c++20 -O2 с теми же includes, harness
tests/dataflow_runtime_tests.cpp, этим object, остальными четырьмя r2Sources и
archives без bdsp_protocol → r2_dataflow_direct.exe.

### 12.2 I integration / independent acceptance

I: link dataflow_runtime к dsp_kernels (scalar_reduce.cpp registration D2) и новому
wled_protocol target/public include; убрать runtime→bdsp_protocol при отсутствии
других usages. node_routing_tests теперь напрямую использует wled_protocol API и
старый forwarding include для compatibility; target deps/include соответственно.
Новых R2 targets/source files не требуется; existing test registrations сохранить.
Совместимость старых includes/IDs/wire остаётся D2/T2/I ответственностью.

Required final whole desktop/DLL build и короткие affected integrations выполняет I
после освобождения слота. Этот R2 прогон не подтверждает whole desktop, Qt/UDP wire,
hardware/native graphics или independent acceptance. T2 full-history demand/queues/
counter/cadence/stale/remote schemas проверяет отдельно. Полный suite/FPS/soak/Linux,
UI действия, config.json/output, branches/commits/ZIP не выполнялись.
Source hashes находятся в build-refactor-runtime-slice2/source-hashes.txt.
Status: R2 SOURCE FROZEN, AUTHOR CHECKS PASS, awaiting I integration + independent gate.

## 13. Independent FAIL F1/F2 — authorfix freeze

Первый independent review FAIL: build-gate2-runtime-review/review-report.md и
independent_repro.cpp доказали Gate/Switch [1,2,3,0] вместо [1,2,0,0], а также
reset surviving branches при Splitter3→2. Координатор освободил freeze для R2 fixes.
Author checks первого freeze не подтверждали acceptance; актуальный freeze ниже.

F1: HistoryView additive zero_rows:uint8span, zero_row:floatspan, zero_oldest:size_t.
row(i) разрешает отдельное логическое кольцо флагов перед newest_override/storage.
RuntimeContext содержит независимые prepared flag rings для real History outputs
и accepted sink inputs. Accepted tick меняет один новый флаг; старые закрытые строки
остаются нулями при reopen/wrap. Инициализация копирует только флаги, не числовую
историю; process не копирует full History и не выделяет память. Compatible prepare
переносит masks/cursors; независимый logical cursor выдерживает уплотнение numeric
ring и external normalized capacity=frames. New producer/discontinuity очищает flags;
duplicate/stale не продвигает их. Masks включены в preflight/actual buffer_bytes,
containers/cursors — sizeof(State). Eight-versus-two output test: ровно6*capacity
дополнительных bytes. Validation проверяет bounds/0..1/zero-only row. retain()
материализует row(i) при explicit demand и не сохраняет borrowed mask spans.

F2: outputs count не делает routing state несовместимым. Semantic type/resolved
geometry/clock остаются проверяемыми; surviving endpoints сохраняют histories.
Splitter и Switch unused3→2/expand2→3 дают empty reset_nodes и counts2→3.
Removed connectedport2 оставляет unaffected sibling и detached compatible history;
следующая строка detached known-clock route typed-zero. No reset совместимых
accumulators ради числа outputs; исчезнувший endpoint отсутствует в descriptors.

R2/T2 agreed additive external query (header frozen, T2 подтверждён adapter):

```cpp
signal_contracts::HistoryView PreparedGraph::apply_history_route(
    domain::NodeId, domain::PortDirection, std::uint16_t port,
    const FrameView& owning_source, const RuntimeContext&) const noexcept;
```

Caller владеет source payload до полного чтения результата; returned view также
заимствует prepared mask/zero row из context. Runtime не сохраняет source spans.
Serialized query под evaluation mutex, без concurrent process/prepare/Apply/Stop.
T2 BEFORE query проверяет LOCAL delivery.epoch/applied graph_revision и current
graph/endpoint/cache. Method не принимает DeliveryStamp и не выводит local revision
из producer metadata; координатор явно согласовал caller fence.
Runtime проверяет plan/context, actual direction/port, resolved History contract,
validate(source), matching geometry и latest producer sequence/epoch/producer_epoch/
generation/incoming graph_revision/source_sequence/timestamp_ns. Mismatch,
invalid/unavailable/stale → empty HistoryView, без raw unmasked fallback.
Sink принимает Input:0, fake Output отвергается. External history(node,port,ctx)
остаётся empty без внешнего owner; T2 подаёт owning normalized source в method и
материализует исключительно row(). Canonical queries после edit до нового tick
не меняют уже доставленные rows; node-only legacy latest projection сохранена.

Actual RED: independent cases добавлены в node_routing_tests без смены ожиданий.
Build target exit0, executable exit1: Gate actual[1,2,3,0], assert good.
Дополнительный red canonical query до нового tick: row(1)==2 exit1. Extended
wrap/reopen выявил physical-index mask transfer — исправлено logical flag ring.
Failed budget fixture соблюдает прежний prepare API: новые result holders пусты,
working plan/context сохранены отдельными owners (не требование сохранения failed
output holder, которое противоречило existing regression tests).

Final GREEN commands (PATH cached MinGW bin, собственный build folder):

```powershell
& 'C:/Qt/Tools/CMake_64/bin/cmake.exe' --build build-refactor-runtime-slice2 `
 --target node_routing_tests dataflow_runtime_tests signal_contracts_tests `
 application_lifecycle_tests domain_contract_tests -j4
& 'C:/Qt/Tools/CMake_64/bin/ctest.exe' --test-dir build-refactor-runtime-slice2 `
 -R '^(node_routing_tests|dataflow_runtime_tests|signal_contracts_tests|application_lifecycle_tests|domain_contract_tests)$' `
 --output-on-failure
```

Build exit0, CTest5/5,0.09s. Extended fixtures: local/external closed/reopen/wrap/
prepare/newproducer/duplicate, independent Switch outputs, unchanged source,
retention after next tick, stale-source refusal, exact mask budget and failed
admission preserving working history, no processing allocations.

Unchanged reviewer repro copied ONLY to own
build-refactor-runtime-slice2/f1-f2-author-repro.cpp. g++13.1 -std=c++20 -O2
-UNDEBUG -fno-exceptions -fno-rtti, public include roots runtime/signal/application/
domain/dsp/wled; links own freshly built archives dataflow_runtime, graph_application,
graph_domain, signal_contracts, dsp_kernels, wled_protocol, dsp inside quoted
'-Wl,--start-group'/'-Wl,--end-group'. Executable bin/f1-f2-author-repro.exe,
compile0/run0: Gate/Switch[1,2,0,0], histories2/2→3/3, noalloc, failures0.
Evidence paths in own build folder: authorfix-repro-build.log, authorfix-repro.log,
Testing/Temporary/LastTest.log, source-hashes-authorfix.txt. Scoped diffcheck0.

Authorfix files: runtime.h/.cpp, signal frame.h/.cpp, node_routing_tests.cpp,
signal_contracts_tests.cpp и этот документ. D2/T2/I sources/manifests untouched.
Whole desktop build/affected integration у I; independent repeat review отдельно.
No next slice/fullsuite/FPS/UI/Linux/hardware/userdata/branches/commits.
Status: R2 F1/F2 AUTHORFIX SOURCE FROZEN; author GREEN не означает gate2 acceptance.

## 14. Narrow Qt slots macro fix — 2026-10-07

T2 воспроизвёл exhausted при default desktop prepare: Qt slots macro удалял
одноимённый inline constructor parameter, slots_(slots) становился slots_().
Координатор разрешил только macro compatibility fix. BufferPool parameter
переименован в slot_storage; signature/types/layout/limits/API/ABI не изменены.
Нет Qt зависимости или production #undef. Runtime algorithms остаются frozen.

Existing signal_contracts_tests включает public buffer_pool.h при пустом slots
macro, затем восстанавливает test scope. Existing allocate/exhaustion/retain/
reference/lifetime assertions проверяют сохранение caller pool capacity.
RED: cmake --build build-refactor-runtime-slice2 --target signal_contracts_tests -j4
exit0, executable exit3 на pool.acquire(16,a)==ok. GREEN тот же build exit0;
ctest --test-dir build-refactor-runtime-slice2 -R '^signal_contracts_tests$'
--output-on-failure exit0,1/1 passed,0.04s. Scoped diffcheck0.

Evidence own folder: slots-macro-red-build.log, slots-macro-red.log,
slots-macro-green-build.log, slots-macro-green.log, source-hashes-slots-macro.txt.
Source edits только buffer_pool.h и signal_contracts_tests.cpp; manifests untouched.
R2 narrow source refrozen, T2 уведомлён для startup repro; narrow independent
review и whole desktop rebuild/smoke у I ещё требуются. No userdata/commits/UI actions R2.
