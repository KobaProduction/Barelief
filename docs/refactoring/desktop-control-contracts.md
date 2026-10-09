# T3 desktop control — contract/preparation handoff

Дата: 2026-10-07, Europe/Moscow. **DOCUMENT READY / CONTRACT PROPOSAL ONLY**.
Единственная writable зона этой фазы — этот документ. Implementation, source/test
files, CMake, сборки, сокеты и native queue experiments не разрешены. Следующий
handoff должен явно подтвердить Gate2, codec API/freeze и конкретное владение.
Никакого автоматического старта реализации после появления соседних файлов.

## 1. Authority, прочитанная редакция и evidence scope

Полностью прочитаны AGENTS.md, implementation-plan.md, architecture-map.md,
storage-control-contracts.md, desktop-delivery-contracts.md,
project-storage-contracts.md, control-codec-contracts.md и application
processing_control/graph_lifecycle headers и implementation. В
baseline-and-ownership.md прочитаны ProposedSlice3 и актуальные Gate2 residual/
consolidated T/P status; typed-node-routing.md прочитан как исторический draft.
Перед дальнейшей реализацией/архитектурным ревью каждый рабочий чат должен полностью
прочитать [pinned Clean Architecture / DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).
T3 прочитал всю эту редакцию, включая разделы composition root/practical rules.
Справочник задаёт ответственность, не требует новых паттернов или переименования IDs.

`git rev-parse HEAD` подтвердил b18cfbf3c2ac62b4baf022de5348f5a29d46e2cc.
Dirty coordinator draft не является accepted implementation. Более поздние записи
implementation-plan/baseline и явный dispatch имеют приоритет над историческими
статусами в начале соседних документов. P3 strict projection/storage получил
bounded independent PASS; public API стабилен. Gate3 целиком не принят. T2
DATA18/19/20 sources frozen, но independent consolidated C1–C10/Gate2 остаётся
pending. K headers/schema существуют; codec bodies/generated files LIVE, их
наличие не означает codec acceptance или разрешение T3 integration.

В этой фазе executed evidence — только read-only Get-Content/rg/git inventory и
чтение pinned reference. Все tests ниже **PROPOSED / NOT RUN BY T3**. Авторские
GREEN соседей, независимый bounded PASS, static source proof и native execution
различаются. Publisher queue experiment operationally blocked, результата нет;
повтор/обход, real sockets и копирование networking implementation для обхода
запрещены. Этот документ не снимает ограничение и не заменяет native evidence
арифметикой или fake transport.

## 2. Реальные seams и текущие callsites

Пути repository-relative; номера строк не фиксируются при живой работе соседей.

| Seam / файл | Наблюдаемый callsite и будущая ответственность |
| --- | --- |
| `application/include/application/processing_control.h`, `application/src/processing_control.cpp` | `ProcessingControlService::open_session/submit/close_session/expire_prepared`, caller-owned `ControlSessionSlot` span, immutable caps/clock/runtime/persistence. GLOBAL token, replay, revision принадлежат service. `GraphLifecycleControlPort` делегирует transactional lifecycle, не создаёт executor. |
| `application/include/application/graph_lifecycle.h`, `application/src/graph_lifecycle.cpp` | `prepare_transactional/apply_transactional/start_transactional/stop_transactional/discard_prepared`; historical `prepare/apply/start/stop` имеют отдельную failure policy. R-owned read-only API. |
| `adapters/desktop/runtime/src/audio_pipeline.cpp`, `include/pipeline/audio_pipeline.h` | `command()` сериализует control на Qt thread. `prepare_runtime()` сейчас сразу создаёт delivery pools, задаёт ingress specs/runtime_limits, вызывает historical Prepare+Apply. `apply_graph()` компилирует GraphProgram и обновляет `_graph`, `_graph_revision`, `_epoch`, caches/source workers после этого. `_executor` и `_lifecycle` уже единственные runtime/working graph. `negotiate_ingress()` вызывает тот же prepare_runtime; его нельзя оставить обходом typed pending slot. |
| `adapters/desktop/runtime/src/engine_client.cpp` | `execute()` вызывает AudioPipeline напрямую и ведёт отдельный legacy `rev`; `collect()` включает JSON graph/history. `EngineClient::submit()` отправляет Command5, `Impl::reply()` публикует snapshot. `presentation_graph` записывается при ApplyGraph до ответа, `publish()` делает restore_graph_layout для любого remote snapshot. Это конкретная точка pending-layout integration. |
| тот же `engine_client.cpp`, `include/pipeline/engine_client.h` | `EngineServer::Impl::server.accept`, Command5 decode/read-only check/execute/Reply6/broadcast; `Wire::send/receive/reset_assembly` используется client и server. DATA18/19/20 canonical negotiation, nonce/session/catalog/fences принадлежат T2 и сохраняются при T3 integration. Control session не является DATA subscription. |
| `presentation/ports/include/pipeline/engine_port.h`, `engine_models.h` | Stable EngineCommand Kind0..10, wildcard UINT64_MAX, subscription bits, EngineReply/EngineSnapshot и callbacks. Новый typed control должен быть additive; old derived consumers должны иметь явное unsupported, не успешную пустую реализацию. Qt DTO отдельно от portable application. |
| `adapters/desktop/platform/pipeline/src/network_runtime.cpp`, `include/pipeline/network_runtime.h` | `NetworkState` poll5ms вызывает refresh при revision/epoch/running change; refresh начинается с stop и заново создаёт publisher/subscribers/registry. `resolve(Input&)` уже сравнивает endpoint fingerprint host/port/incarnation/limits/canonical identity. Input callback захватывает receiver_epoch; output callback захватывает binding_epoch/revision. Сохранить socket недостаточно: callbacks/fences необходимо обновлять при accepted local Apply. |
| `adapters/desktop/include/pipeline/serial_config.h` | `read_serial_binding`, `apply_serial_options`: explicit UART endpoint связан с existing NetworkInput, overrides меняют runtime copy. Это DATA binding, не implicit control endpoint. |
| `adapters/desktop/include/pipeline/stream_transport.h`, `platform/pipeline/src/stream_transport.cpp` | NetworkPublisher::try_advertise/limits/publish/incarnation; NetworkSubscriber::start overload с limits/expected incarnation, start_serial/start_channel/stop. DATA11–14, ACK/version/lineage/retirement/retry сохранить. |
| `adapters/desktop/include/desktop/qt_byte_transport.h`, `platform/core/src/network/tcp_packet_socket.cpp` | QtPacketChannel и QtTcp/SerialByteTransport используют общий PacketStream; unsent suffix/bytesToWrite/partial write различаются. Эти core paths read-only T2/I baseline; изменение требует отдельного coordinator assignment. |
| `libs/bdsp_protocol/include/bdsp_protocol/{byte_stream,stream_fragments}.h` | PacketStream caller buffers, borrowed PacketView до следующего packet; enqueue snapshots bytes, flush переносит suffix. DATA FrameAssembler grammar ID13 total/offset не годится как скрытая control assembly. Existing framing/fragment files read-only. |
| `libs/bdsp_protocol/include/bdsp_protocol/{control_message,control_codec,control_limits}.h`, `schema/control.proto` | K3 wire schema1, CBOR/protobuf codec mask1/2, WireControlMessage, caller ControlDecodeArena. detail archive implementation LIVE; T3 не зависит от detail API, не меняет schema/generated/codec. |
| `adapters/desktop/include/desktop/graph_document.h`, `src/graph_document.cpp` | P3 `project_processing_dto` и `processing_dto_document` — strict boundary. Historical project_processing_graph/processing_graph_document/restore_graph_layout не подменяют этот API. |
| `adapters/desktop/include/desktop/project_display_settings.h`, `src/project_display_settings.cpp` | P3 capture/merge проверяют graph/node/kind и exact edge provenance; не знают request/session/revision. T3 owns association/expiry. |
| `apps/desktop_core/main.cpp` | Сейчас constructs AudioPipeline, apply_graph initial/config, EngineServer::listen, NetworkRuntime::refresh. T3 proposed root для control endpoints; GUI core_application/ConfigManager composition P3-owned, не T3. |
| `adapters/desktop/include/aggregator/{capabilities,packet_codec,utils}.h`, `platform/aggregator/src/{utils,aggregator_server,aggregator_client}.cpp` | Registry16/control_available/engine_port и validated capabilities. T3 добавляет только explicit negotiated control metadata; stream_delivery_features и protocol_versions не считать control proof. |

Не создавать GraphExecutor в mapper/session/server. Единственный desktop
ProcessingControlService собирается рядом с существующим AudioPipeline runtime;
local client, TCP, UART и legacy bridge используют этот же service/serialization.
Application/domain не включают Qt, codec, socket, miniaudio, filesystem или ESP-IDF.
`adapters/control` связывает application и wire DTO; зависимости направлены внутрь.

## 3. ProposedSlice3 exact ownership после нового handoff

Это предложение, **не текущая permission на запись**. T3 не один в workspace,
не откатывает чужие изменения. Cross-owner API delta сначала coordinator.

| Owner | Exact source/test responsibility |
| --- | --- |
| T3 existing headers | `adapters/desktop/include/pipeline/{engine_client,audio_pipeline,stream_transport,network_runtime,serial_config}.h`; `presentation/ports/include/pipeline/{engine_port,engine_models}.h`. |
| T3 existing sources/root | `adapters/desktop/runtime/src/{engine_client,audio_pipeline}.cpp`; `adapters/desktop/platform/pipeline/src/{stream_transport,network_runtime}.cpp`; `apps/desktop_core/main.cpp`. |
| T3 registry narrow zone | `adapters/desktop/include/aggregator/{capabilities,packet_codec,utils}.h`; `adapters/desktop/platform/aggregator/src/{utils,aggregator_server,aggregator_client}.cpp`, только negotiated control features/binding metadata. |
| T3 new zones | `adapters/control/include/control/processing_message_mapper.h`, `adapters/control/src/processing_message_mapper.cpp` (std-only); `adapters/desktop/include/control/{control_session,legacy_control_bridge}.h`, `adapters/desktop/runtime/src/{control_session,legacy_control_bridge}.cpp`. |
| T3 tests | Existing `tests/{network_runtime,stream_transport,capabilities,registry_discovery,engine_typed_views}_tests.cpp`; new `tests/{desktop_control_session,legacy_control_bridge,resource_reconciliation}_tests.cpp`. Mapper tests находятся в desktop_control_session_tests, отдельный testfile без выдачи не создавать. |
| K3 | `libs/bdsp_protocol/include/bdsp_protocol/{control_message,control_codec,control_limits}.h`, `src/{control_message,control_cbor_codec,control_protobuf_codec}.cpp`, `schema/control.proto`, `control.options`, `generated/control.pb.h/.c`; `tests/{control_cbor_codec,control_protobuf_codec,control_codec_golden}_tests.cpp`. |
| P3 | config_document/config_migration, graph_document, ConfigManager, project_display_settings, GUI core_application/desktop_engine_provider storage/projection composition и их ProposedSlice3 tests. T3 только вызывает их strict APIs. |
| I | Все CMake/tests cmake/manifests/export/install/vendor pins/licenses/tools/control_codegen/runner/ledger. Подключает new mapping/session/tests, codec dependencies/generated sources после freezes; один Ninja/build owner. |
| R/D, portable application/runtime | Domain descriptors/validation, processing_control/graph_lifecycle, signal/runtime/DSP/WLED — read-only; required semantics/API передать coordinator и отдельному владельцу. |

`port_delivery_codec`, `owned_frame`, Qt byte core adapters, portable byte_stream/
stream_fragments не попадают автоматически в T3 new ownership. Существующие T2
source bodies в пересекающихся файлах меняются только после Gate2/freeze handoff,
с сохранением agreed identity/queue/geometry/reconnect guards.

## 4. Typed wire ↔ application mapper

Предлагаемые public signatures в T3 std-only зоне, названия подлежат acceptance:

```cpp
MappingResult map_command(const bdsp_protocol::WireCommand&,
                          const MappingLimits&, MappedProcessingCommand&);
MappingResult map_reply(const application::ProcessingReply&,
                        const MappingLimits&, ReplyMappingStorage&,
                        bdsp_protocol::WireReply&);
```

`MappedProcessingCommand` owns optional ProcessingDTO и graph_identity, держит
CommandHeader/ProcessingCommand с rebased pointers в свою storage; запрещено
слепо копировать/move self-referential wrapper без rebase. Lifetime до завершения
serialized submit; asynchronous dispatch получает owned wrapper/arena, не spans
из decode. ReplyMappingStorage caller-owned typed arrays/text; failure оставляет
published destination прежним. Concrete storage shapes согласуются с frozen K
public API, не с detail::ControlArchive. Limits ограничивают arrays/text/conversions
до resize/copy; mapper не вводит собственный runtime admission/validator.

SessionIdentity не получается из непроверенного wire header: controller сравнивает
wire identity с issued connection context, затем передаёт server context в submit.
Mapper не открывает sessions, не назначает epochs/tokens, не применяет Prepare.

| Wire field / explicit ID | Application mapping и точность |
| --- | --- |
| commands1/2 | GetCapabilities/GetState, header request_id/expected_revision uint64 сохраняется; query не mutation. |
| prepare3 | Own ProcessingDTO: graph_identity→graph.graph_id, sole processing_version→graph.processing_version; signed int32 node id, node_kind(canonical text), enabled, registered typed parameters и exact edges/from/to/uint16 ports. Unknown kind сообщает unsupported, не default kind. |
| apply4 / release5 | ApplyProcessing(token, owning graph_identity) / ReleasePrepared(token). Не находить план по JSON/hash, не подменять token. |
| set_parameters6 | SetProcessingParameters(node, explicit typed parameters, enabled). Нет FFTViewParameters wire alternative; NoParameters не разрешает обновлять view — service/domain решают. |
| start7 / stop8 / persist9 | StartProcessing/StopProcessing/ExplicitPersist(graph_revision). Без capture/WLED destination setters, implicit save или optimistic intent. |
| parameter families | WireNoParameters→NoParameters; WireAudioSource→AudioSourceParameters; WireMerge→MergeParameters; WireGain→GainParameters; WireMel→MelParameters; WireHistory→HistoryParameters; WireWindow→WindowParameters; WireWindowFunction→WindowFunctionParameters; WireTestSignal→TestSignalParameters (все поля); WireNetwork→NetworkParameters; WireRouting→RoutingParameters; WireReduction→ReductionParameters. Explicit enum switches, не variant index/static_cast ordinal. |
| frame enum0..8 | PCM, Window, Windowed, StereoSpectrum, Spectrum, Mel, History, Scalar, WledPacket через explicit switches; форма массива не даёт права reinterpret semantics. |
| reply | request_id, accepted, named ControlError0..12/Status0..10/GraphError0..13/Lifecycle0..3 mapping; validation node/legacy edge_index отдельно от validation_location/error_location. Preserve optional direction/port/edge absent versus zero, static diagnostic key copied для async output. |
| ControlState | epoch/revision/graph_revision/persisted_graph_revision, lifecycle/run_requested, applied graph, own_prepared_token; typed applied без JSON/layout/history. No optimistic graph before accepted Apply. |
| memory/capabilities | state/buffer/scratch и ВСЕ DeviceCapabilities/RuntimeLimits/ControlLimits fields плюс explicit_persist и kind/type lists; checked uint64→size_t и uint32/uint16 conversion до writes. Не усекать budgets на 32-bit host. Actual profile, не desktop defaults для ESP. |

Unknown required representation/invalid enum/unsupported mapping возвращает явный
boundary result до submit, без partial service call. Numerical ranges, cycles,
compatibility, actual caps и memory admission остаются existing domain/service.
Все finite doubles, signed zero, uint64 maxima и optional semantics сохраняются
для обоих codecs; numeric normalization нарушила бы exact-request replay.
Outbound graph с presentation params считается internal boundary failure, не
молча исполняется/передаётся. Нет hidden second parameter registry.

## 5. Negotiation и BDSP ID proposal

Registry audit: engine_client constants5..10/17; stream_transport11..14;
registry packet16; historical serial1..4, packet15 reserved/untouched. Separate
registry-service local enum1..4 не освобождает общие message IDs. DATA18/19/20
приняты ранее и read-only. K WireCommandId1..9 и codec mask1/2 НЕ BDSP IDs.

**Предложение coordinator approval:** 21 ControlHello, 22 ControlHelloAck,
23 ControlCommand, 24 ControlReply, 25 ControlFragment. Это reservation proposal,
не allocation; внешний private peer registry не проверен, positive handshake
обязателен. Не менять existing data/framing/goldens и не переиспользовать Error14
для нового control negotiation. K schema1 остаётся payload authority.

Transport-level hello/ack bounded fixed little-endian envelope отдельно от K
codec, иначе невозможно согласовать codec при no-common. Proposed v1 поля:

* Hello21: envelope_version:u16=1, nonce:u64 nonzero, schema_version:u32,
  codec_mask:u32, max_control_bytes:u32, device_identity length:u16 + UTF-8 bytes
  (1..256). Это ожидаемая peer identity, не permission/client credential.
* Ack22: envelope_version:u16=1, nonce:u64, status:u8,
  selected_schema:u32, selected_codec:u8, effective_max_control_bytes:u32,
  allow_mutations:u8(0/1),
  client:u64/session:u64/epoch:u64, device_identity length:u16+bytes,
  diagnostic length:u16+UTF-8 bytes(0..256).
* Status proposal:0 accepted,1 unsupported_envelope,2 unsupported_schema,
  3 no_common_codec,4 limit_exceeded,5 access_denied,6 busy,7 malformed,
  8 wrong_device. Rejected Ack selected fields/context zero; exact grammar,
  unknown/trailing/duplicate nonce refused. Ack size <=1024; Hello <=512.

Fixed-envelope schema/IDs/status require coordinator/K consultation before source
creation; they do not extend control.proto. Registry proposed optional
`control_features:["typed-control-v1"]`, `control_schema_versions:[1]`,
`control_codecs:["cbor","protobuf"]` actual subset, `control_max_message_bytes`
и existing engine_port/control_available validate only with explicit feature.
Exact JSON names/validation gate требуют coordinator approval; отсутствие feature
означает Unsupported typed control, не implicit schema1. Engine DATA support и
stream_protocol_versions не выбирают control codec. Metadata — discovery hint,
handshake — positive support proof. Stable existing keys/UUID не переименовываются.

После matching positive Ack выбрать frozen public
select_control_codec(local_mask, remote_mask, local_schema, remote_schema): общий
CBOR default, иначе общий Protobuf. No common known bit или schema mismatch —
explicit rejection, никаких application mutations. Peer выбранный codec обязан
поддерживать реально; protobuf не placeholder. Capabilities/State получают через
commands23/replies24 выбранным codec до Prepare/Start/etc. Actual effective codec
limits intersect local/peer, не увеличиваются из untrusted remote advertisement.

Old peers не получают21 без feature/explicit configured typed endpoint. Timeout
hello5s (proposed monotonic control policy) → Unsupported/timeout; fallback в
legacy допускается только явно выбранным режимом, без silent retry mutation.
Endpoint/session allocation/read-only authorization предшествуют commands.
Read-only server может принять handshake с allow_mutations=0 и обслуживать
Capabilities/State; mutation refusal не должна лишать клиента доступа к queries.
UART control requires explicit separately supplied open channel/endpoint: current
serial DATA binding не становится control service и ESP streaming Session не
начинает слушать21–25. No implicit UART discovery/extra config migration.

## 6. TCP/UART framing, bounded assembly и arena lifetime

Оба транспорта используют existing BDSP PacketStream/QtPacketChannel framing.
Один ControlSession controller/parser для TCP и UART; OS endpoint ownership
снаружи. New control bodies не QDataStream и не JSON. Не вставлять control IDs
в DATA FrameAssembler или legacy Wire::receive без явного negotiated routing.
DATA18–20 и legacy fragment10 остаются byte-identical; same byte chunking через
TCP/UART должно дать identical complete control payload/result.

Proposed25 fragment grammar (requires approval): envelope_version:u16=1,
issued_session:u64, message_sequence:u64 nonzero monotonically increasing,
total:u32, offset:u32, inner_id:u8(23/24 only), chunk_length:u16, chunk bytes.
Fixed header29 bytes, chunk<=60000; total>65535 и <= negotiated control bytes.
Unfragmented23/24 payload<=65535; никаких implicit fragments для hello/ack.
Одна assembly per direction/session, no interleaving even unrelated normal packet
на этом control channel во время assembly; strict sequence/offset/kind/total,
session match до copy. Start offset0 не сбрасывает уже active assembly молча.
Reserve полный total до первого copy, exact final length до codec decode.
Expired5s/new session/close/cancel/malformed/failed send освобождают partial
bytes/timer/arena ownership; partial command никогда не dispatch. Deadline не
продлевается каждым tiny chunk; fake monotonic clock, не QDateTime/DSP timestamp.
Срок DATA assembly не меняется и не выводится из этого control proposal.

Max_control_bytes proposal<=K effective wire512KiB default, actual lower advertised;
per-peer total budget<=2MiB default configurable lower. Этот общий budget считает
device pending bytes + PacketStream unsent suffix + full reserved inbound assembly
+ complete framed outbound reservation + retained queued request/reply storage,
а codec scratch/arena/session replay/prepare resources имеют отдельный checked
host total memory reserve. Count limits не заменяют byte limits. Outbound message
preflight целиком, включая29-byte headers/framing, до первого fragment enqueue.
Использовать accepted T2 framed bound `q+6+floor((q+4)/223)` при safe additions;
это source arithmetic contract, не native bytesToWrite measurement. Counter/budget
overflow → explicit refusal before effects; negative device pending invalid.

Partial write сохраняет exact unsent suffix/offset, zero/would_block не busy-loop;
bounded poll/ready notification на host thread. Read0 не EOF, terminal result
с transferred>0 обрабатывается по ByteTransport contract с final close fence.
Не повторно encode/replay prefix после partial send. Если send failure происходит
после accepted mutation, application не откатывается из-за доставки: close session,
explicit indeterminate outcome и fresh state after reconnect. Не маркировать это
atomic mutation rejection; это отдельная transport failure.

Lifetime цепочка:

1. PacketView и complete assembly borrowed: synchronous codec.decode до reuse,
   либо bounded owned bytes retained в контроллере. Parser callback не сохраняет
   чужой span в QObject event/std::function.
2. Decode output смотрит только в caller ControlDecodeArena; source bytes можно
   переиспользовать после success. Destination/arena unchanged на decode failure.
3. До asynchronous handoff mapper делает bounded owning application request;
   либо queue владеет всей arena и views rebased/stable до synchronous submit.
   Один arena slot нельзя переиспользовать, пока предыдущий handler ждёт dispatch.
4. ProcessingReply graph/caps pointers borrowed (replay graph до следующего own
   cache replacement/close); map+encode или owned reply projection завершить под
   serialized boundary до next submit/apply/close. Encoded queue owns bytes.
5. Encode source/destination/scratch/arena disjoint, caller buffers lifetime и
   required_* размеры frozen K API; enqueue только после successful encode.

Bounded pending commands proposal max128 per connection и aggregate <= advertised
service/caller host budget, никогда128 на каждую session без total reserve. Отказ
queue full не расходует replay/service mutation. Expiry poll100ms proposal,
service проверяет TTL перед каждой operation; close cancels queued uncommitted
requests в том же serialized порядке. ISR/DSP callback не выполняет control/I/O.

## 7. Session, request, revision, token и два клиента

Service owns server epoch, server-issued session и opaque token; controller
establishes nonzero client identity tied к live connection, не принимает чужую
identity из payload. Device UUID stable отдельно от epoch. DATA canonical session,
publisher incarnation, producer epoch/generation и local DeliveryStamp отдельны
от application service epoch; нельзя приравнивать все числа.

Каждая mutation требует exact expected_revision из current state. Request_id
ненулевой uint64, reply echo exact; proposed client monotonic per session avoids
старый ID после replay-cache eviction. Current service хранит **только последний
completed exact request/reply** per session: это не durable inbox/unbounded replay
map и не exactly-once после reconnect. Client dispatch serializes uncertain
mutation/retry до нового запроса; exact same header+payload retry возвращает cache,
same request_id changed payload/header conflict. Signed zero и complete parameters
часть equality. Earlier ID после другой completed operation нельзя безопасно
повторять как mutation; controller high-water admission policy требует acceptance.

A Prepare → B Prepare(valid/invalid/oversized) Busy before runtime/domain preparation;
GLOBAL max1, TTL30000ms default actual host monotonic. Own invalid replacement
сохраняет old token/bundle/expiry; success заменяет атомарно. Apply проверяет
owner+token+graph identity+base revision+executor. Foreign/stale/expired/consumed/
superseded refusal сохраняет working/revisions/resources; только accepted Apply
consumes token и increments revision/graph_revision. SetParameters требует empty
slot даже для owner. Failed typed Start/Stop сохраняет intent/state/revision/token;
accepted no-op не increments revision. Persist отдельный explicit operation с
accepted graph_revision; slider/display/Apply не пишет project/flash.

Disconnect: cancel queued requests, close_session releases own global slot, fences
old replies/assembly, unrelated session не закрывается. Reconnect issues new
context, fetch caps/state/epoch, Prepare заново. Old token/request/session не
переносятся; late old Ack/reply/session_closed нельзя приклеить к new context.
Timeout клиентской Apply не доказывает server rejection: discard pending layout,
fresh state reconciliation, не автоматическая повторная mutation.

Typed revision не равна legacy EngineServer rev (нынешний increment каждого accepted
nonquery) и не равна local graph delivery epoch. На integration требуется один
authoritative application revision для всех processing mutations, включая legacy;
legacy presentation/adapter accounting допускает отдельное compatibility поле,
но не независимый working graph/control controller. Exact translation recorded
characterization и approved legacy policy; direct e.apply_graph в execute после
integration не остаётся обходом service.

## 8. Legacy Qt characterization → SAME application service

Сначала characterize HEAD и отдельно frozen accepted legacy behavior, сохранять
разницу dirty draft. Byte anchors: Command5/Reply6/Snapshot7/View8/Audio9/Fragment10/
TypedViews17; Qt_6_0, LittleEndian, SinglePrecision, protocol u16=1. Kind0..10 и
subscription bits не перенумеровываются; wildcard UINT64_MAX только legacy.
Тесты literal goldens фиксируют command fields/order, null/empty strings/arrays,
graph compact JSON boundaries, errors, trailing rejection, >65535 fragment grammar.
Existing original expectations не ослаблять для новой реализации.

| Legacy operation | Bridge responsibility |
| --- | --- |
| Snapshot | GetState того же service плюс legacy adapter status/history projection вне processing. |
| ApplyGraph | P3 strict project_processing_dto, serialized Prepare/Apply через тот же service с отдельными internal request IDs, shared global-slot exclusion. Legacy external request correlation отдельно; no extra prepared slot. |
| StartAnalyzer/StopAnalyzer | Processing Start/Stop routing к тому же service; historical failure/run-intent/adapter effects сначала characterize и явно approve policy. Не переименовывать capture/display/WLED activity в portable run intent. |
| SetMerge | Перевод к зарегистрированным processing params/working candidate при наличии actual target node; legacy selector behavior/ambiguity characterize. Не invent node ID или bypass domain. |
| SetDisplayChannel | Local presentation only, не wire processing param, prepare/revision/persist/reconnect. Compatibility reply accounting отдельный policy вопрос. |
| Subscribe | Delivery subscription controller, не processing mutation; unchanged masks/DATA negotiation separation. |
| StartCapture/StopCapture/StartWled/StopWled | Desktop platform/adapter commands остаются наружной responsibility, fields не leak в portable graph; если меняют processing state, orchestration должна проходить same service и agreed failure policy. |

**API gap до implementation:** processing_control.h сейчас имеет только typed
submit, не explicit legacy failure-policy entry. GraphLifecycle historical methods
разрешают раннюю очистку pending и изменение intent при failure; typed service
этого не разрешает. Нельзя назвать direct lifecycle bypass «bridge к тому же
service» или молча заменить historical guarantees typed behavior. Coordinator
должен назначить application owner для минимальной explicit legacy-policy handoff
либо утвердить доказанную характеристиками адаптацию без API delta. Proposed
semantic entry `submit_legacy(...)` (имя/signature НЕ утверждены) использует ту же
service consistency boundary/global-slot/revision и existing executor; foreign
typed reservation Busy, не уничтожается legacy command. Typed defaults unchanged.
Transport не хранит duplicate run intent/prepared bundles. До решения этот seam
блокирует implementation legacy integration, а не documentary preparation.

## 9. P3 strict projection и pending display settings

Outbound local graph: capture_project_display_settings(local draft) для локального
provenance; project_processing_dto(candidate) → mapper → codec. Только registered
Processing/ApplicationAdapter fields, views NoParameters. Opaque nested JSON,
FFTView channel/merge, position/color/docks/camera/native spinbox/history не
попадают в command даже как extension. Unknown fields сохраняются в local draft.
Inbound: typed applied DTO → processing_dto_document или matching merge_project_
display_settings, никогда generic restore_graph_layout/latest submitted graph.
Strict serializers симметричны; negotiated wire не whole project для save.

T3 owns bounded two-phase map:

* Pending key `{device_identity, application_epoch, issued_session, request_id}`;
  value graph_id, node+stable kind provenance, captured ProjectDisplaySettings,
  expected/base revision, operation phase, monotonic deadline, candidate association.
* Prepare accepted не приклеивает layout: carry matching capture/token/graph identity
  в следующий Apply request с новым request_id. Rejected/newer preparation cancel
  старый association; orphan local capture не становится accepted settings.
* Только matching accepted Apply reply → accepted record keyed
  `{device_identity, epoch, session, request_id, accepted_graph_revision, graph_id}`.
  Accepted revision берётся из reply, не predicted expected+1. Для GetState snapshot
  lookup по device/epoch/graph_id/accepted_graph_revision; сохраняется provenance
  origin request/session. Duplicate cached reply idempotent, не новая attachment.
* Validate live pending, issued identity, request correlation, accepted status,
  graph_id и revision relation ДО merge/publication. P3 проверяет graph/node/kind/
  exact edges; reused node ID другого kind/foreign graph/rewired edge получают
  processing-only state. Не искать layout по одному node или graph_id.
* Reject/timeout/disconnect/cancel/device change/new candidate removes pending;
  stale/out-of-order reply не resurrect entry и не overwrites current accepted
  settings. Late old reply того же epoch но superseded request также ignored.
  Out-of-order remote state update не приклеивает last draft, bounded accepted
  records eviction оставляет processing-only snapshot, не чужой layout.

Для legacy reply5/6 correlation использует legacy epoch/request/accepted snapshot
graph_revision и отдельный internal typed association. Session absent в legacy
bytes остаётся controller-local connection fence, не добавленным wire field.
Один outstanding Apply per device упрощает ordering, но не заменяет explicit
keys/timeout handling. Scope/display-only local edits не Prepare и не bump
processing revision; обновление already accepted local display record проверяет
тот же provenance. Local editable draft не затирается remote accepted snapshot.
ConfigManager/P3 alone выполняет save active whole JSON; никаких config/output
fixtures на реальных пользовательских путях.

## 10. Selective resources: acquire → stage → commit / rollback

NetworkRuntime refresh перестаёт трактовать любой graph_revision/epoch change
как общий Stop. Resource plan строится на control plane из strict accepted
candidate+runtime config copy+validated registry metadata, не DSP callback.

| Stable key | Resource-affecting fingerprint | Не причина reopen |
| --- | --- | --- |
| NetworkInput graph_id/node/kind/output port + explicit binding ID | TCP host/port или validated provider UUID/stream_id; type/selected payload+subscription versions, canonical remote endpoint/incarnation/actual limits; UART explicit port_name/baud/data/parity/stop/flow and node binding | gain, selected/enabled/routing при сохранённом contract, layout, local revision/delivery epoch, optional registry fields/order |
| NetworkOutput graph_id/node/kind/Input0 + stream_id | sink semantic contract/type, listener address/port, catalog membership/limits, destination/binding settings | downstream view demand, preview, display, compatible processing values |
| Publisher/listener | explicit endpoint + catalog of stable immutable stream bindings | endpoint/type remap requiring actual incarnation transition; listen address/port | compatible gain/routing; registry optional metadata |
| Registry client | device UUID/server address+port/session token | registry endpoint/credentials/service availability metadata | ordinary accepted graph parameter revision |

Compare exact structured values, не hash-only equality/JSON object byte order.
Current resolve fingerprint сохраняется по semantics, producer lineage отдельно.
Unchanged TCP/UART objects/channel/subscriber ACK state/retry/retirement/producer
lineage сохраняются; local callback fences меняются после commit на accepted stamp.
Захваченные old receiver_epoch/binding_revision нельзя оставить в lambda иначе
socket живой, а все новые frames discard. Нельзя relabel old queued frame новым
stamp; incompatible/removed endpoint clear/invalidation selective, independent
history/branch unaffected. Known disabled выход runtime даёт typed silence,
unknown unavailable; controller не заменяет это общим Stop.

Transaction sequence предложен в desktop implementation existing runtime port:

1. Classify unchanged/add/change/remove и preflight all budgets/catalog/serial
   validity; save old state. Stage GraphProgram/control-plane descriptor/delivery
   pool/ingress config и affected resource acquisition без publishing/callbacks.
   Это не второй PreparedGraph slot; graph pending единственный у lifecycle.
2. Acquire new affected TCP/UART/listener ресурсы заранее. Failure closes staged
   resources only, keeps working graph/token/revision/bindings. Для exclusive
   UART/listener endpoint невозможно обещать второй open того же endpoint:
   unchanged handle reuse; replacement requiring close-before-open должен иметь
   proven platform restore или явный refusal до commit. Не destructive probe.
3. Apply повторно проверяет service session/token/revision/expiry; stage admission
   valid; frame-boundary lifecycle commit того же executor. На его failure restore
   precommit adapter staging; own prepared token/bundle remains usable по typed
   contract. Resource plan не outstanding second bundle/token registry.
4. После успешного commit только no-fail swaps/publication prepared handles,
   bindings/callback fences/metadata and local projections; никакие allocations,
   open/listen/subscribe которые могут failed after acceptance не скрывать в этом
   commit. Внешний peer ACK readiness может быть asynchronous resource state,
   а не ложный rollback already accepted graph.
5. Fence/remove old affected queues, unsubscribe wait in-flight без deadlock,
   retire changed handles на их owning Qt thread. Unaffected object/channel and
   pending writes/lineage preserved. Async delivery failure reports exact resource,
   не общий reset и не undo accepted application revision.

Реальный gap: prepare_runtime совмещает Prepare+Apply; runtime_limits очищает
pending, negotiate_ingress может менять его вне service. T3 должен разделить
desktop staged preparation/application в своих audio_pipeline files через
existing ProcessingControlRuntimePort/lifecycle; internal discovery serialized,
не стирает retained typed token. Если negotiated ingress реально требует новой
runtime revision/plan, policy service invalidation/Busy/retry согласовать с R/S
через coordinator до изменения portable header. GraphLifecycleControlPort direct
делегация сама по себе не stages physical capture/delivery/network; нужен desktop
adapter к тому же lifecycle, не second runtime. Раздельные service counters и
`_graph_revision/_epoch` обязаны согласованно публиковаться после success.

## 11. TDD / characterization / independent test matrix

Все строки ниже новые требования к evidence после explicit implementation
handoff. Сначала characterization для переносимого поведения, RED для нового;
фиксировать реальные assertion/link failure отдельно. Compile-only fixtures не
считать behavioral RED/PASS. Assertions enabled, target granularity, fakeclock/
fake ByteTransport для deterministic host policy. CMake/test registrations у I.

| ID / sole owner file | Сценарий / required oracle | Evidence scope и gate |
| --- | --- | --- |
| CH1 T3 legacy_control_bridge_tests | Literal HEAD command/reply/snapshot and existing legacy fragments; wildcard/exact conflict, read-only, null/empty/trailing/malformed protocol. Compare actual encode/decode bytes before replacement. | Executed characterization; без socket. Existing legacy tests preserved, не dirty draft acceptance. |
| CH2 T3 legacy_control_bridge_tests | Old apply/prepare/start/stop failure intent/revision, display/capture/WLED side effects; same live executor through service, no bypass/global slot stealing; two clients legacy/typed Busy. | Fault-injected runtime + bridge integration. Requires approved service legacy policy; direct lifecycle-only test insufficient. |
| M1 T3 desktop_control_session_tests | All9 commands,12 parameter families,26 kinds,9 frame types, every named reply/error/status/lifecycle, caps/memory/diagnostic optional0; graph/port identities. | Executed mapper table/parity for both codec outputs, no hidden domain validator. |
| M2 T3 desktop_control_session_tests | Unknown kind/enum, parameter mismatch, uint64→size_t overflow, signed zero, nonfinite, oversized text/nodes/edges; mapping failure preserves published output and calls runtime0. | Boundary unit; actual K malformed preflight separate K tests. |
| K1 K3 three codec tests | CBOR/protobuf literal goldens, semantic all-branch parity, required/optional/default, max integers/UTF8/signed zero. | Executed actual TinyCBOR/nanopb after freeze; T3 consumes independent accepted result, compile presence not enough. |
| K2 K3 codec tests | Every truncation prefix, duplicate/unknown/missing/trailing/indefinite/depth, narrow integer/finite refusal, short dest/scratch/arena; required counts; zero produced/consumed + unchanged arena/output; no hidden alloc. | Codec unit + allocator/source audit exact limits; not native queue/hot-path proof. |
| N1 T3 desktop_control_session_tests | both common→CBOR; protobuf-only→protobuf; unknown bit only/no common/schema mismatch/old peer/wrong device; read-only handshake permits queries/refuses mutation; matching nonce/single positive Ack; no command before Ack. | Fake-clock negotiated controller, calls submit0 on negotiation/mutation refusal. IDs/envelope approval prerequisite. |
| N2 T3 capabilities/registry_discovery tests | Feature absence, lower actual caps, malformed/future metadata atomic refusal, optional unknown preservation, no DATA feature misread as control; stable UUID. | Metadata-only unit/static wiring. No unapproved live discovery/socket action. |
| F1 T3 desktop_control_session_tests | Same literal BDSP command/reply across fake TCP/UART ByteTransport, one-byte/random split reads, coalesced packets, partial writes0/1/N, terminal+transferred, would_block. | Actual shared PacketStream plus ControlSession/controller fake transport; not OS TCP/QSerial scheduling. |
| F2 T3 desktop_control_session_tests | >65535 payload fragments25, exact offsets/total/session/seq/type; duplicate/restart/interleave/trailing/max/max+1; nothing submitted until complete. | Executed pure controller assembly; existing DATA/legacy grammars remain unchanged. |
| F3 T3 desktop_control_session_tests | Timeout before/at5s monotonic, tiny chunks not extend; cancel/close/reconnect failed send clears partial bytes/timer/arena and stale callbacks; next valid message accepted cleanly. | Fake-clock real controller policy; static production wiring proof separately. |
| B1 T3 desktop_control_session/resource_reconciliation tests | checked aggregate inbound/outbound/arena/queue reserve, headers/framing/peer pending suffix, exact fit/+1/overflow/negative device bytes; no partial enqueue on refusal. | Public pure arithmetic execution AND complete send/receive source tracing. Native queue experiments NOT RUN/prohibited. |
| S1 T3 desktop_control_session_tests | A Prepare→B valid/invalid Prepare Busy→A Apply; own invalid retains token/TTL; valid replacement; foreign release/apply; stale/expired/consumed; fake TTL29999/30000. | Actual ProcessingControlService with fault runtime, end-to-end mapper+codec/controller bothformats; accepted existing service tests remain R/S-owned. |
| S2 T3 desktop_control_session_tests | SameID exact cached reply/no second mutation, changedID payload conflict including signedzero; multi-request queue bounded; session/client forged/wrong epoch rejection, fresh reconnect state/caps. | Executed controller+service ownership/replay; not durable/exactly-once claim. |
| S3 T3 desktop_control_session_tests | typed failed Start/Stop state/intent/revision/token/resources unchanged; same-state accepted no-op; SetParameters while global slot Busy; persist explicit current graph only/failure preserves working. | Fault injected actual lifecycle/service adapter integration, separate legacy policy cases. |
| S4 T3 desktop_control_session_tests | Close serialized before queued mutation blocks it; accepted Apply then send failure gives indeterminate client outcome/fresh state; oldreply/oldtoken cannot replay after reconnect. | Deterministic queue fake transport, not OS disconnect timing. |
| P1 T3 engine_typed_views/desktop_control_session tests | Actual P3 strict APIs outgoing/incoming all known/opaque/nested UI keys; NoParameters views, network typed settings; invalid processing refused, draft unchanged; State/Apply no history/layout. | P3 library integration + codec decode inspect; P3 owns unit projection source/tests. |
| L1 T3 desktop_control_session_tests | Pending captures A/B distinct; only exact accepted Apply merges graph/node/kind and correct revision; accepted Prepare never attaches; duplicate reply idempotent. | Actual EngineClient reconciliation/pending policy seam, P3 merge; pure helper without actual caller wiring insufficient. |
| L2 T3 desktop_control_session_tests | Reject/timeout/cancel/disconnect/device switch/new request/out-of-order/reused node kind/foreign graph/rewired edge; late older accepted reply cannot revive layout; draft preserved. | Deterministic pending/controller integration; static publish/reply/timeout paths reviewed, native GUI unnecessary. |
| R1 T3 resource_reconciliation_tests | Gain, Switch selected, Gate enabled, display edits: unchanged TCP/UART object/channel/ACK/retry/retirement IDs, no open/close; fresh local stamps admitted, old queued data fenced. | Injectable resource factory + actual reconcile callsites execution; socket identities/native traffic only if separately authorized. |
| R2 T3 resource_reconciliation_tests | Remove/type/endpoint/incarnation/limits/serialbaud change affects exact handles only; unaffected history/producer metadata continue; unavailable marker exact endpoint. | Deterministic resource doubles plus existing graph runtime integration with synthetic5–10 tones, no speakers/music. |
| R3 T3 resource_reconciliation_tests | Acquire/listen/UART exclusive refusal, fail preparation/apply/subscription staging rollback; old resources/working/token/revisions preserved; postcommit delivery failure resource-specific. | Fault injection actual desktop runtime adapter; source ordering audit, native exclusive reopen behavior UNVERIFIED absent approved evidence. |
| R4 T3 network_runtime/stream_transport tests | Internal retry unchanged incarnation/producer lineage; explicit binding remap invalidates only affected; callbacks no stale captured revision; no accidental DATA stream control activation. | Socket-free policy seam/source proof now; existing live tests build-only until explicit coordinator execution permission. |
| I1 I build/target wiring | Frozen T3/K/P hashes; std-only mapper noQt/OS + graph application/BDSP dependency; whole desktop five-app build/DLL deploy + short affected tests; SDK installed consumer. | I one build owner after source freezes; author integration distinct from independent Gate3 acceptance. |
| RV independent reviewer | Read actual diff/source, repeat immutable oracles, inspect every admission/commit/lifetime/close path, confirm no secondruntime/hiddenvalidator/presentation leakage; original Gate2 guards unchanged. | Independent bounded review plus explicit coordinator Gate3 decision. Unexecuted native tests remain UNVERIFIED. |
| FINAL Slice5 assigned owner/I/reviewers | Remove remaining legacy owning hot-path allocations, external full-history conversion/copy; verify actual processing→adapter→GUI/network boundaries, full agreed final run/three fresh reviews. | Mandatory whole-goal obligation, not excused by explicit query or3304bytes equal at8/400. Not implemented/accepted by this handoff. |

No blanket GUI/editor/native launch required for this contract document; future
UI scenario только конкретный bug/отдельный запрос. Never full/soak/FPS/Linux/
hardware per small change. Tests используют temporary paths/fake endpoints, не
user config.json/output. Independent reviewer не суммирует static observations,
author unit GREEN и partial P3/R/D PASS в whole Gate2/3/goal PASS.

## 12. API questions и dependent gates перед реализацией

1. **Coordinator Gate2:** принять consolidated independent C1–C10/source proof и
   exact frozen DATA18/19/20/T2 header ownership. I latest socket-free integration
   GREEN не заменяет acceptance; C6/C7/C9 native execution не заявлено.
2. **K3 freeze:** exact public schema/API/limits, actual CBOR/protobuf bodies+
   generated sources, goldens/parity/refusal evidence; detail archive не dependency.
   Новые BDSP21–25/envelope/metadata proposals координатор должен утвердить или
   скорректировать; без этого никакой wire allocation/source change.
3. **Application legacy policy:** назначить owner/signature/revision reconciliation
   для SAME service и historical failure compatibility. T3 не меняет portable
   processing_control/graph_lifecycle самостоятельно. Нужен explicit ответ до bridge.
4. **Desktop runtime staging:** принять desktop wrapper existing runtime port,
   decomposition prepare_runtime, service-authoritative processing revision и
   ingress discovery pending-slot policy; handle physical capture/delivery failures
   до commit. Existing runtime_limits invalidation не использовать обходом token.
5. **Resource ownership/channel budget:** принять stage/commit APIs в owned T3
   NetworkRuntime/header и bounded resource factory seam для tests; exclusive UART/
   listener replacement policy. Core QtPacketChannel delta, если понадобится,
   отдельно owner/coordinator. Фабрика тестов не second networking implementation.
6. **UART control composition:** выбрать explicit caller-opened dedicated channel
   handoff без auto DATA promotion; config keys/CLI changes не изобретать до acceptance.
   ESP control/NVS остаётся Slice4, streaming example unchanged.
7. **P3 integration:** stable strict APIs consume now; pending request/revision
   association T3-only, local+remote storage GUI callsites P3-only. Cross-owner
   composition handoff через coordinator, не simultaneous edit.
8. **Evidence permission:** real TCP/UART loopback/native OS queues остаются
   UNVERIFIED в этой фазе. Предложенные fake/controller/source proofs отделены;
   runtime integration execution scope координатор выдаёт явно. Blocked publisher
   experiment не повторять/обходить при любом будущем тестировании.

Главные риски: direct legacy/runtime path обойдёт service/global token; staging
физических ресурсов выполнится после graph commit; preserved network callback
захватит old stamp; lost reply ошибочно станет accepted/rejected layout; single
last-request cache будет выдан за reconnect-safe replay; codec heap-free результат
будет ошибочно назван zero-allocation processing всего desktop. Матрица выше
проверяет конкретные границы и сохраняет Slice5 mandatory final obligation.

Финальный статус T3: **CONTRACT/PREPARATION DOCUMENT READY; SOURCE/TEST/BUILD
IMPLEMENTATION NOT STARTED; IDLE AFTER COORDINATOR REPORT**. Только этот файл
создан T3. Ни tool install, branch/commit/push/PR/ZIP, subagents/new chats, user
config/output, sockets/native queue experiments, source/CMake/vendor/schema/
generated/test files, сборки или tests не выполнялись.

## Phase A partial implementation status — 2026-10-07

This section supersedes the preparation-only status above. Coordinator authorized
only the mapper header/source, session header/source, desktop session test and
this document. The five new source/test files are live, not a whole-phase freeze.
Engine, composition, legacy bridge, physical resources, CMake and shared build
registration remain outside T3 writes. Phase B is still required; these classes
are not yet integrated application behavior.

User numeric-foundation steering takes precedence over the historical mappings
in this preparation: independent element type, shape, semantics, PCM packing and
explicit Converter must be planned before further numeric mapping. Existing
nine FrameType mappings and twelve parameter alternatives are historical payload
compatibility only. They do not certify the new numeric contract. No numeric
mapping/shape/port work continues until the new coordinator handoff.

Implemented author scope: owned mapper values with borrowed synchronous reply
projection; explicit endpoint admission; fixed LE hello/ack, IDs 21–25, CBOR
preference/protobuf fallback; one correlated request; supplied shared typed
service; checked resource arithmetic; bounded partial PacketStream I/O and
fragment assembly; immutable original 5-second deadlines; identity/session
checks and cleanup. Server capability preflight is conservative for the existing
historical schema only. Its full branch/bound proof remains pending.

Evidence in `build-desktop-control-t3`:

* Mapper stub compiled, then GetState mapping assertion failed (run exit 3):
  `mapper-red-build.log`, `mapper-red.log`. Implementation compiled/ran exit 0:
  `mapper-green-build.log`, `mapper-green.log`.
* Session stub compiled, then client open assertion failed (run exit 3):
  `session-red-build.log`, `session-red.log`. Minimum session implementation
  compiled/ran exit 0: `session-green-build.log`, `session-green.log`.
* Expanded session batch initially linked the historical I application archive
  with the live changed runtime vtable/session slot header. It compiled but ran
  ACCESS_VIOLATION (-1073741819): `session-policies-build.log`,
  `session-policies.log`. This is a mixed ABI artifact configuration failure,
  not evidence of an application crash. Those archive-linked executions stopped.
* Coordinator froze the matching R application header/source (493B8FB9… / 
  3D0CCC10…). Direct compilation now uses actual processing_control.cpp,
  graph_lifecycle.cpp, graph_definition.cpp and graph_validation.cpp, with no
  application/domain archive mixed in. `session-matching-build.log` exit 0;
  `session-matching-run.log` exit 3 at test line 136. The positive transaction
  fixture incorrectly expected empty graph acceptance; current domain explicitly
  refuses empty graphs. No product defect is inferred. SHA receipts
  `session-matching-before.txt` / `session-matching-after.txt` are identical.
* That positive fixture remains reproducible with `--pending-transactions` and
  is not claimed green. It awaits a valid graph fixture authorized by the numeric
  handoff. Successful Prepare/Apply, global-token and TTL cases in that section
  remain unverified through this session adapter.
* Default unaffected batch: `session-unaffected-build.log` strict C++20,
  assertions on, no exceptions/RTTI, -Wall -Wextra -Werror, compile exit 0;
  `session-unaffected-run.log` run exit 0. Before/after receipts identical.
  Both codecs and deterministic fake TCP/UART byte segmentation use the actual
  PacketStream and supplied actual ProcessingControlService. Coverage includes
  blocked/partial writes, no-common-codec, endpoint/read-only admission, query
  replay/conflict, empty graph refusal with zero runtime prepare calls, bad
  fragment sequence/offset/session/interleaving, 70,000-byte assembly completed
  with an intentionally invalid codec payload, unchanged original assembly
  deadline after a tiny fragment, peer budget failure and cleanup, handshake and
  request deadlines, negative device pending bytes and overflow arithmetic.
  This proves framing failure behavior, not successful large graph projection.

Author checks are not independent acceptance. Remaining: new numeric plan and
mapping handoff, all mapper branches/ownership tests, valid large payload/reply
bounds, full session admission/replay/malformed matrix, Phase B resource/legacy
characterizations, application integration/build by I, independent review and
final Slice5 acceptance. No native GUI, socket, publisher queue, hardware, audio,
user config/output, branch, commit, archive or new chat was used here.

Status: **PARTIAL LIVE PHASE A; NUMERIC WORK ON HOLD; PHASE B NOT STARTED.**
