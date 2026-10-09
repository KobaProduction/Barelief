# Desktop delivery — согласование API среза 2

Дата: 2026-10-06 (Europe/Moscow). T2 — отдельный рабочий чат
`01a112d7-eb98-7ca0-9168-b6f3bf956ef8`. Статус: **IMPLEMENTATION AUTHORIZED;
T2 IN PROGRESS**. GATE 1 принят координатором только для
назначенного среза 1. HEAD `b18cfbf3c2ac62b4baf022de5348f5a29d46e2cc`;
общий dirty draft вне принятого scope не наследует acceptance.

Прочитаны AGENTS.md, implementation-plan.md, node-runtime-contracts.md,
storage-control-contracts.md, ProposedSlice2 в baseline-and-ownership.md,
architecture-map.md, typed-node-routing.md и полностью
[Clean Architecture / DDD, pinned revision](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).
Будущему implementation/review чату передать эту ссылку и требование полного
чтения. Справочник определяет границы, а не дополнительный backlog.

## 1. Ownership и обнаруженные пути

Точная выдача T2 — строка Desktop delivery ProposedSlice2. Headers:
`adapters/desktop/include/pipeline/{audio_pipeline,graph,engine_client,stream_codec,stream_transport,network_runtime}.h`;
runtime sources `{audio_pipeline,graph,engine_client,stream_codec}.cpp` в
`adapters/desktop/runtime/src`; platform sources `{stream_transport,network_runtime}.cpp`
в `adapters/desktop/platform/pipeline/src`;
`presentation/ports/include/pipeline/{owned_frame,engine_models,engine_port}.h`;
WLED headers в `adapters/desktop/include/wled` и
`adapters/desktop/platform/wled/src/wled_audio_reactive_server.cpp`.
Registry: `adapters/desktop/include/aggregator/{capabilities,packet_codec,utils}.h`,
`adapters/desktop/platform/aggregator/src/{utils,aggregator_server,aggregator_client}.cpp`.
Portable DATA codec: `libs/bdsp_protocol/include/bdsp_protocol/stream_codec.h`
и `libs/bdsp_protocol/src/stream_codec.cpp`.

Все CMake/manifests/export/install — I. Runtime/signal — R2; pure numerical/WLED
codec — D2. T2 не пишет JSON/config migration, control service/control codec,
Qt widgets/editor/renderer, embedded/ESP-IDF/NVS. Network resource reconciliation
остаётся срезу 3: исправление DATA identity не разрешает переделать refresh.
Source/API shared owner changes возвращаются владельцу, чужие правки не откатываются.

Текущие дефекты подтверждены чтением исходников, а не запуском: GraphOutput
ports/views/previews, AudioPipeline _views и typed subscription фильтры keyed
только node; output_port не участвует в сохранении; category outputs формирует
fake output_events sinks. capabilities.h разрешает лишь семь старых имён,
AggregatorClient::set_capabilities принудительно задаёт version=2. Stream
черновик выбирает v3 через enum range; binary header не содержит node/port.
Engine packet 17 содержит node-only map. WLED готовый packet отправляется с
producer counter, хотя destination counter увеличивается отдельно.

## 2. Согласованный R2/T2 runtime seam

Последнее согласование напрямую с R2 имеет приоритет над промежуточным предложением
переписывать FrameMetadata.graph_revision. Exact proposed portable API:

```cpp
struct DeliveryStamp {
    std::uint64_t graph_revision = 0;
    std::uint64_t epoch = 0;
};
struct PortFrameView {
    std::string_view graph_id;
    domain::PortIdentity identity; // node + uint16 port
    domain::PortDirection direction;
    domain::NodeKind kind;
    signal_contracts::PortContract contract;
    bool contract_known;
    signal_contracts::FrameView frame;
    bool window_emitted;
    DeliveryStamp delivery;
};
using OutputFrameCallback = void (*)(void *, const PortFrameView &);
using SinkInputCallback = void (*)(void *, const PortFrameView &);
struct RuntimeCallbacks {
    OutputFrameCallback output = nullptr;
    SinkInputCallback sink_input = nullptr;
    void *user = nullptr;
};
// PreparedGraph:
Status process_ports(NodeId, const FrameView &, RuntimeContext &,
                     DeliveryStamp, RuntimeCallbacks = {}) const noexcept;
// GraphExecutor:
Status process_ports(NodeId, const FrameView &,
                     DeliveryStamp, RuntimeCallbacks = {}) noexcept;
```

Runtime header/type owner R2; здесь signatures зафиксированы для handoff, не
реализованы. delivery — LOCAL applied epoch/revision от serialized desktop owner.
FrameMetadata включая incoming graph_revision, epoch, producer_epoch, generation,
sequence, source_sequence, timestamp_ns остаётся метаданными производителя.
Local graph edit не меняет producer identity. graph_id принадлежит owning plan;
все spans/string_views действуют только во время callback. Не сохранять их
в QObject event/queue, std::function capture или desktop cache.

contract_known означает разрешённую полную geometry prepared plan, включая
unavailable события. false не означает неизвестный semantic type: contract.type
всегда declared type configured port. Canonical output callback вызывается только
для реальных Output ports, sink callback — для Input ports. У sink нет Output 0.
Unavailable: exact endpoint/type, available=false, empty payload, sequence=0,
discontinuity=true; отсутствие producer clock не выдумывает valid zero.
Known inactive с clock получает typed silence от runtime; T2 не рассчитывает DSP.

R2 предоставляет per-port `port_contract(node,direction,port)` и
`history(node,output_port,context)`; node-only history wrapper означает port 0.
Старые process/OutputView сохраняются явным compatibility wrapper с appended
direction; исторический sink event имеет Input. Embedded consumers переводятся
в срезе 4. Wrapper использует тот же PreparedGraph, без второго исполнителя.

## 3. Exact desktop DTO / callback / subscription proposal

Qt-free definitions в owned_frame.h; Qt maps и handlers в engine_models/engine_port:

```cpp
struct EndpointIdentity {
    std::string graph_id;
    std::int32_t node = -1;
    domain::PortDirection direction = domain::PortDirection::Output;
    std::uint16_t port = 0;
};
struct DeliveredPortFrame {
    EndpointIdentity endpoint;
    dataflow_runtime::DeliveryStamp delivery;
    domain::NodeKind kind;
    signal_contracts::PortContract contract;
    bool contract_known = false;
    TypedFrame frame; // owning payload; metadata remains producer metadata
};
struct PortPreview {
    EndpointIdentity endpoint;
    dataflow_runtime::DeliveryStamp delivery;
    bool available = false;
    std::size_t values = 0;
    float peak = 0;
};
using PortHandler = std::function<void(DeliveredPortFrame)>;
// AudioPipeline; synchronous producer-thread callback; same unsubscribe fence.
uint64_t subscribe_ports(PortHandler, std::vector<EndpointIdentity> endpoints);
void unsubscribe_ports(uint64_t token);
// Empty endpoints = all admitted endpoints, not an unbounded wildcard graph list.
// Snapshot/control-plane access, not called from DSP callbacks:
std::vector<DeliveredPortFrame> latest_port_frames() const;
```

Final include dependency decision: owned_frame.h must stay Qt-free and must NOT
depend on concrete dataflow_runtime. Therefore actual desktop DeliveryStamp is
the same two-field value DTO declared in owned_frame.h, mapped explicitly from
runtime DeliveryStamp; snippets above show matching shape, not permitted linkage.
Endpoint direction uses domain contract or explicit compatible boundary enum;
no independent role inference from GUI category. Comparator is exact
graph_id/node/direction/port lexicographic order, not hash-only equality.

Canonical cache key `{delivery.epoch, endpoint}`; graph_revision is version of
entry, not a changing port identity. Epoch fences reconnect/Stop queued delivery.
Snapshots contain owned per-port frames and previews, shared snapshot stamp and
endpoint-specific history handles. GraphOutput adds canonical port_frames,
sink_events and port_previews; ViewFrame and EngineSnapshot expose canonical
port collections beside existing fields. Existing node-only maps/signals remain
explicit projections: Output port0 only; views come from true Input events of
the matching sink. Port1 never updates node-only History/previews.

EnginePort proposed additive handler `set_port_frame_handler(PortHandler)` and
query of owned per-port snapshot; existing pure virtual consumers must remain
source-compatible via a documented default unsupported capability until moved.
No success/no-op dummy callback: caller can query supported delivery modes.
Local EngineClient adapts new AudioPipeline callbacks. Remote canonical exposure
requires negotiated identity support (§5); legacy mode returns legacy data only,
never manufactures ports from ambiguous node events.

Subscription is exact configured endpoint membership + accepted local delivery
stamp. Registration validates endpoint existence and count before publication;
old node filters are explicit wrappers selecting Output0 or declared sink Input0.
Graph removal/type/port changes invalidate only affected records; unavailable
is sent for every affected endpoint before stale payload can be observed.
No callback may execute pipeline commands or subscription mutation; unsubscribe
waits in-flight callback and endpoint destruction cancels pending delivery.

Spectrogram `{Output,0}` History and `{Output,1}` Scalar filled count are separate
entries and subscriptions, available and unavailable alike. Demand for count
does not materialize History. History snapshot is `[frame][band]`, logical
oldest-to-newest; sink display conversion stays at existing GUI boundary.
Demand snapshot runs under serialized executor access outside process tick;
borrowed HistoryView never escapes. newest_override/zero-latest state and remote
owned History source are tracked by endpoint, not a global boolean/node alias.
History count is a scalar payload, never magnitude/peak substitute.

## 4. Bounded ownership / limits proposal

R2 bounded RetainedPortFrame/retain_port_frame(view,payload_pool,identity_pool,out)
copies samples/packet/stream_id into payload pool and graph_id into identity pool;
all tags/stamp copied as values, view returned by value. out unchanged on any
failure, no heap. Identity pool slot bytes are admitted caller limits; no new
portable global graph_id limit is imposed. Buffer pools outlive retained handles.

Desktop processing reserves endpoint cache/pool entries at preparation/subscription
time. Callback does bounded copy into prepared slots, then queues owning handle;
Qt/vector materialization happens on adapter delivery thread. Full History copy
is never mandatory per tick. Network History serialization/materialization only
at explicit bounded demand/cadence under serialized access; async ingress history
has independent bounded owning source storage. No queue stores borrowed frame.

| Resource | Proposal / existing cap retained |
| --- | --- |
| Desktop graph | existing actual admission: 128 nodes / 256 edges; configured ports via descriptor (Splitter/Switch max8) |
| Stream payload | MaxFrameBytes 2 MiB including 112-byte header + stream_id + payload |
| stream_id | 1..256 UTF-8 bytes; exact bytes, unique per advertised publisher |
| Registry document/streams | 32768 bytes / 128 streams; reject over budget before encode/store |
| Network delivery mailbox | max64 frames AND 4 MiB payload, with checked sums and admitted identity/allocator overhead reserve |
| Publisher | max16 peers; max2 MiB queued bytes per peer, include framing/fragment overhead in preflight |
| WLED | one configured destination; one latest pending 44-byte owned packet + stamp; no unbounded event-per-frame backlog |
| Engine snapshots | max2 MiB serialized message; canonical endpoint count separately admitted, never silently reuse old max128 node limit |
| Engine local delivery | latest-only owning mailbox; subscription-generation/epoch/revision fence; dropped frame marks gap |

Per-port cache theoretical max cannot be derived as 128 nodes alone. Before
implementation, preparation computes actual sum of input/output descriptors and
payload budget; declared transport snapshot limit may reject oversized telemetry
without rejecting a valid graph or truncating identity. Explicit API budget for
callbacks/subscriptions uses actual configured count; no incremental hot-path
map insertion. Physical heap OOM recovery is not implied by no-exceptions build.

Exhaustion: return explicit status/drop counter; next available event discontinuity.
Unavailable supersedes stale queued payload for SAME endpoint. If marker cannot
be retained/enqueued, fail affected stream/disconnect it; do not leave stale values
advertised as current. Stop/epoch fences pending handles; rewiring does not issue
global Stop. Queue memory admission includes identity bytes/slots/control reserve,
not solely serialized payload_size. Exact allocator accounting remains host reserve.

## 5. DATA wire identity / capabilities proposal — pending coordinator

Do not change existing BDSP framing, fragmentation, IDs11 Subscribe/12 Ack/13
Data/14 Error, registry packet16 or engine17 bytes. Old semantic wire IDs0..6
retain payload v2/header112/goldens. Explicit descriptor lookup maps wire ID/type;
no enum-range membership or unknown-string-to-Mel fallback.

Recommended stream identity: opaque existing stream_id identifies one immutable
advertised endpoint binding, not a new binary node/port field. Binding is
`publisher UUID/incarnation + stream_id -> {graph_id,node,direction,port,type}`.
NetworkOutput is a sink Input endpoint; identity does not pretend it produces
Output0. Network ingress has separate local configured Input/source binding;
remote producer identity is preserved separately. Relays assign their own
advertised stream binding and preserve producer clock lineage.

Capability proposal retains existing JSON version2 envelope and packet16, adding
explicit `stream_delivery_features:["port-identity-v1"]` and per-stream
`graph_id`, `node_id`, `direction` ("input"/"output"), `port_index`,
`payload_version`, `subscription_version`. New feature validates all fields,
exact integer ranges and unique binding/id; legacy feature absence means legacy
stream identity only. Registry membership uses shared semantic descriptor names
and explicit transport-supported set, including Scalar7 and WledPacket8 only
when adapter codec exists. No copying desktop caps into an unknown device.

Legacy streams use existing request bytes `00 02 type stream_id`; new Scalar7 /
Packet8 request `00 03 type stream_id`, exactly 4..259 bytes. v3 Ack must echo
the exact request (including type/version/id) before new DATA accepted. No
matching capability/Ack => explicit unsupported/timeout; no fallback alias to
0..6. v3 payload retains current proposed112-byte header, Scalar one finite float,
Packet44 opaque bytes; packet validation belongs D2 codec, not BDSP. Binary
stream_id plus agreed immutable binding preserves endpoint identity even on
unavailable frames. Reject wrong id/type/version, stale incarnation and illegal
binding changes; new configuration fences queue/negotiation before replacing
binding. graph_revision in stream payload remains producer revision; local
receiving delivery revision belongs desktop envelope.

Registry old-peer behavior is material: old registry validation rejects an entire
mixed Scalar/Packet advertisement; old clients reject it too. Existing old-only
advertisements remain byte-compatible/readable. This proposal requires explicit
coordinator decision on feature advertisement and unsupported registry behavior;
it does NOT promise mixed discovery through an old registry. No automatic JSON
config migration or invented control negotiation is part of this proposal.

Engine17 node-only typed-view bytes cannot represent canonical per-port telemetry.
Coordinator explicitly requires canonical remote delivery in slice2; the earlier
deferral option is withdrawn. Separate negotiated DATA extension proposed below,
pending exact ID/schema approval. Merely setting an unknown subscription bit is
not negotiation (old server may accept it silently). Never append port fields to
engine17 or send v3 to an old typed-view consumer without feature proof.

Portable codec proposed overload adds explicit byte storage for Packet:
`decode(bytes,samples,stream_id,packet_storage,out) noexcept`, with required_packet_bytes
appended to CodecResult; existing overload remains characterized compatibility.
Packet output must not alias caller float storage through reinterpret_cast in
canonical API. All outputs unchanged on malformed/short buffers. Exact bytes,
dimensions, checked arithmetic and complete finite validation precede copying;
BDSP treats44bytes as opaque geometry. D2 decode validates destination preview/send.

## 6. D2/T2 WLED handoff — agreed semantics

Pure codec package D2 `libs/wled_protocol/include/wled_protocol/wled_packet.h`
and src/wled_packet.cpp. Encoder emits neutral counter0; T2 never uses producer
sequence as send counter. D2 `write_counter(span<uint8_t> owned_packet,uint8_t,Limits)
noexcept` validates full packet then patches counter offset17. Adapter owns
private44byte copy before patch; producer packet/fanout consumers unchanged.

Each destination send attempt consumes destination counter, first0, rollover
255→0, including failed UDP write. Timer17ms Qt::PreciseTimer; stale iff monotonic
elapsed >300ms (exact300 not stale). Last valid update refreshes freshness;
invalid typed packet is rejected without extending freshness or replacing valid
payload. Known unavailable clears pending and chooses valid silence at next
cadence; no last-nonzero packet after unavailable. Send failure stops timer and
publishes adapter error per legacy behavior. start/stop retain historical counter
behavior, no reset added without characterization.

Pure silence/decoder/encode_bins/encode_mel and exact validation checks — D2.
Final coordinator/D2 decoder correction supersedes early proposed numeric bounds:
foreign packet requires exact44/header/four finite floats; preserve finite values
including negative/>255 and raw uint8 sample_peak (any >0 indicates peak).
Do not impose encoder numeric clamps as foreign wire invariants. Typed encoder
remains strict; write_counter uses final codec validation and explicit Limits.
Legacy update/make_packet wrappers preserve characterized sanitation, clamp,
truncation and historical50-band mapping. Legacy combined WLED keeps node/kind/UUID,
destination settings and resource behavior; runtime R2 emits accepted Input,
T2 compatibility delivery selects legacy path. New WLEDOutput accepts packet;
legacy input remains Mel, not a fake packet output. Diagnostic decode read-only,
never synchronizes preview fields to node params. Device hardware acceptance not
claimed by host UDP fixtures.

## 7. Characterization / TDD / integration handoff

T2 test ownership exactly ProposedSlice2: typed_graph_tests.cpp,
graph_continuity_tests.cpp, graph_preparation_tests.cpp,
desktop_history_demand_tests.cpp, engine_typed_views_tests.cpp,
stream_transport_tests.cpp, capabilities_tests.cpp, registry_discovery_tests.cpp,
codec_buffer_tests.cpp, bdsp_protocol_tests.cpp. Tests not listed require new
assignment; CMake registrations I. No tests/build run at API stage.

| Tests | Required narrow evidence after authorization |
| --- | --- |
| typed_graph / engine_typed_views | same Spectrogram node History0/count1 distinct callbacks/cache/snapshot/preview/subscription; unavailable both exact types; sink Input only; legacy Output0 projection bytes |
| continuity / preparation | compatible rebind preserves two histories/producer lineage; removed endpoint invalidation only; rejected preparation retains working delivery; revision cannot relabel old queue |
| desktop_history_demand | count-only causes zero full History materialization; demanded newest override handled; independent histories; borrowed lifetime/pool exhaustion |
| codec_buffer / bdsp_protocol | HEAD0..6 goldens before migration; unchanged short-buffer outputs; unknown IDs/version/flags/geometry/finite/overflow; Scalar/Packet separate byte storage, roundtrip ownership |
| stream_transport | subscribe2 goldens; explicitv3 Ack and oldpeer reject; fragment boundaries, wrongstream/type; queuefull unavailable cannot leave stale; private WLED counters first/rollover/failure,17ms/stale300 vs301 |
| capabilities / registry_discovery | old-only v2 pass; Scalar/Packet membership advertised through server/client; bounded malformedfields/duplicateid; unsupported oldregistry behavior; immutable endpoint bindings |

Characterization anchors must distinguish HEAD from dirty draft: old WLED
byte/mapping/counter fixtures from D2; stream0..6/framing and engine17 bytes
captured from baseline before migration. New behavior starts with failing targeted
test, then implementation. Synthetic signal5–10 changing tones, no speakers/music.
Only assigned own build-refactor-delivery-slice2 after coordinator authorization;
never concurrent shared Ninja. Required whole desktop build/deploy and short
affected unit/integration after freeze are coordinated by I, then independent
review. No UI/profile/full/FPS/soak/Linux tests at this stage.

## 8. Temporary bridge removal and approval items

| Bridge | Remove owner / condition |
| --- | --- |
| Node-only maps/signals/engine17 | T2 keeps explicit Output0/sink Input projections; removal only after Qt/remote consumers migrated and approved new wire in slice3/4 with compatibility checks |
| Runtime legacy process/OutputView | R2; T2 converts canonical desktop calls, embedded migration slice4; public compatible wrapper may remain |
| device_protocols WLED include | D2 owns forwarding bridge; T2 migrates delivery consumers, I links/exports; no premature shared header deletion |
| Combined legacy WLED adapter internals | T2 cleanup after byte/cadence/counter/stale parity; stable legacy kind/config/IDs remain supported |
| Float-backed packet decode | T2 retains old overload only as compatibility, canonical uses byte span; removal needs SDK/embedded caller handoff and goldens |

Coordinator approval required for: exact desktop DTO/EnginePort additive method
signatures and budgets; exact registry feature/old-registry refusal fields;
stream_id binding identity strategy; remote DATA18/19/20 exact schema/limits;
packet byte-storage overload. R2/T2 and D2/T2 seams
agreed directly; those agreements do not themselves authorize implementation.

Author evidence: read-only status/diff/source/rg inventory and pinned reference;
only this document written. No source/config/output/CMake edits, configure/build,
tests, installs, branch/commit/push/PR, GUI or hardware actions. Status:
**T2 CONTRACT DRAFT READY; wait coordinator handoff before implementation**.

## 9. Exact remote PortFrames DATA proposal (coordinator requested)

Inventory audit: source search across adapters/libs/apps/examples/presentation
finds legacy serial1..4, desktop engine5..10, streams11..14, registry16,
engine typed views17. Registry enum uses1 error/2 registration/3 full info/4 state
on its separate service. Packet15 has no assigned canonical use in checked-in
source; reserved untouched. Explicit search for IDs18..29 assignments/sends in
repository source found none (rg exit1 = no matches). Existing embedded Session
uses11/12/13/14 only. Proposed new **18 PortSubscribe,19 PortAck,20 PortFrames**
on EngineServer connection only; not allocated until coordinator accepts. No
new application control commands, control codecs, CBOR/Protobuf or registry IDs.
External peer private IDs not audited; absent positive negotiation means unsupported.

Byte grammar: unsigned integers little-endian; signed node int32 represented
two's-complement; float binary32 little-endian. Lengths explicit uint16/uint32;
UTF-8 roundtrip exact, no embedded NUL. Enum membership explicit switch/registry.
No QDataStream/packed struct ABI for new messages. No implicit version defaults.

PortSubscribe18:
`version:u16=1, flags:u16=0, nonce:u64(nonzero), max_message_bytes:u32,
max_endpoints:u16, frame_type_mask:u16, filter_count:u16`, followed by endpoint
filters `{graph_id_len:u16,graph_id:bytes,node:i32,direction:u8,port:u16}`.
direction0=Input,1=Output. filter_count0 requests all current admitted endpoints.
frame_type_mask uses bits0..8 only, must include every selected endpoint type;
no per-type fallback. All reserved/unknown flags/bits reject. max_message_bytes
between256 and2MiB; max_endpoints1..2048. Endpoint filters unique and must exist
in accepted graph, otherwise reject all. A new18 replaces subscription only
after successful validation/ACK, nonce never reused within connection; max one
active request/subscription per connection. No mutable graph operation in18.

PortAck19:
`version:u16=1,status:u16,nonce:u64,session_epoch:u64,graph_revision:u64,
max_message_bytes:u32,max_endpoints:u16,frame_type_mask:u16,graph_id_len:u16,
graph_id:bytes,error_len:u16,error:bytes`.
status0=Accepted,1=UnsupportedVersion,2=UnsupportedType,3=LimitExceeded,
4=InvalidEndpoint,5=Busy,6=Malformed. error<=256 bytes diagnostic text; not a
localized UI label. Accepted limits are intersection requested/actual compiled
limits; mask describes explicit implemented transport types. session_epoch is
nonzero EngineServer connection incarnation, distinct from frame producer epoch.
No20 until matching nonce/accepted19. Old peer ignoring18 times out after5s,
old peer rejecting/closing18 reports UnsupportedDelivery; no canonical callback
and no automatic downgrade. User-selected legacy mode may explicitly reconnect
using unchanged17, marked legacy capability. New server never sends19/20 to a
peer which did not request18. New negotiation is independent of old Subscribe
command/mask, which remains byte-identical.

PortFrames20:
`version:u16=1,flags:u16,nonce:u64,session_epoch:u64,local_epoch:u64,
local_graph_revision:u64,delivery_sequence:u64,graph_id_len:u16,graph_id:bytes,
entry_count:u16`, then entries:
`node:i32,direction:u8,port:u16,kind_id_len:u16,kind_id:bytes,entry_flags:u8,
type:u8, sample_rate:u32,fft_size:u32,bands:u32,history_capacity:u32,
columns:u32,frequency_step:f32,contract_flags:u8,payload_bytes:u32,payload:bytes`.
flags bit0 = complete snapshot, bit1 = gap before this delivery; others reject.
entry_flags bit0 contract_known,bit1 window_emitted; contract_flags bit0
bark_weighted only. kind_id is stable domain descriptor id, not NodeKind enum
ordinal or translated label. payload is existing BDSP stream payload (v2 for
0..6, v3 for7/8), containing complete producer metadata INCLUDING producer
graph_revision. Header local epoch/revision belongs DeliveryStamp. geometry
checked against declared type before commit, no dummy unresolved dimensions.
Unknown geometry permits zero fields with contract_known=false and unavailable
payload; declared type always present and matches payload. Empty payload not
used as unavailable shorthand: valid BDSP unavailable marker is carried.

Every entry includes exact direction/index; sink Input0 and processor Output0
cannot collide. Batch entries unique. All entries share graph_id/local epoch/
revision; flush at boundary before new revision. delivery_sequence starts1,
strictly increases within session; a gap sets flags and does not change producer
sequence. Rejected/stale batches never update snapshot/cache partly. Preview is
computed read-only from delivered payload/contract at receiving boundary, keyed
by same endpoint; no widget/internal/display fields transmitted.

After Accepted19, send complete snapshot20 before deltas. Explicit resubscribe18
with new nonce requests fresh complete snapshot. Zero-entry snapshot is valid
only for an empty selected set/graph. Deltas carry affected endpoints, including
unavailable. Removed endpoint emits unavailable with previous declared type,
then new snapshot atomically reconciles membership. Type change likewise clears
old typed record before replacement; no stale scalar/history cross-use. Delivery
epoch changes fence retained frames; graph_revision updates reconcile version
without resetting compatible producer lineage/history. New server graph_id or
ports exceeding subscription limits suspend canonical delivery with bounded
19 refusal for active nonce; no silent truncation, no implicit legacy17 fallback.

Limits: messages incl headers/entries<=negotiated max2MiB; graph_id1..256 UTF-8
bytes (WIRE admission only, not runtime global limit); kind_id1..64 bytes;
entry_count/filter_count<=2048 AND actual configured endpoint count; total18
message<=2MiB; ACK<=1024 bytes. Oversized complete snapshot causes LimitExceeded
before subscription becomes active, leaving previous valid subscription. No
partial snapshot. Later oversize/materialization failure reports refusal and
fences active delivery. Existing fragment10 envelope may carry20 ONLY after19;
same60000-byte fragments, one message-in-progress,2MiB assembly cap,5s timeout,
strict sequence/offset/total/type and no interleaving. Legacy fragment decoding
acceptance/bytes unchanged for5..9/17. Unknown19/20 before negotiation rejected.

Server per-peer2MiB serialized queue preflights fragment/framing overhead. Latest
owning delivery mailbox with endpoint slots admitted before process; batching/
encoding on event-loop adapter, no borrowed async spans/full History tick copy.
Unavailable replaces queued stale entry for same endpoint; inability to deliver
clearing marker fails connection. Drop available event increments diagnostics,
next event gap. Handshake/subscription limits and counters are delivery state,
not mutations/revisions of application processing graph.

Exact EnginePort boundary amendment proposal:
`enum class PortDeliveryMode { Unsupported, LegacyNodeOnly, CanonicalPortsV1 };`
`PortDeliveryMode port_delivery_mode() const noexcept;`
`bool subscribe_port_frames(std::vector<EndpointIdentity>,QString* error=nullptr);`
`void set_port_frame_handler(PortHandler);`
Local adapter CanonicalPortsV1 immediately; remote Unsupported until19; mode never
inferred from receipt17/capabilities packet16. Existing derived EnginePort has
default Unsupported / explicit false+error subscription; all repository owned
local/remote EngineClient overrides are part of T2 implementation. No dummy
successful unsupported APIs. Canonical port snapshots are additive DTO fields;
legacy callbacks and packet17 explicit bridge remain available.

## 10. Exact registry defaults/refusals/binding epochs

Envelope version2 is historical capabilities schema, NOT new type proof.
Canonical feature `stream_delivery_features:["port-identity-v1"]` requires
`stream_protocol_versions:[2,3]` (actual subset allowed, nonempty explicit ints),
`stream_limits:{max_frame_bytes:2097152,max_stream_id_bytes:256,max_streams:128}`,
and `publisher_incarnation` nonzero uint64 represented canonical decimal string
to avoid JSON double precision. Actual configured lower caps advertised instead
of maxima. Existing protocol_versions field/control_available unchanged and
never interpreted as support for PortFrames18/19/20.

Each canonical stream requires exact id/data_type plus graph_id1..256 bytes,
node_id nonnegative int32, direction input/output, port_index0..65535,
payload_version/subscription_version2 for0..6 or3 for7/8. Defaults: no feature
= LegacyStreamOnly; missing fields with feature = InvalidCapabilities, not0.
Unknown mandatory feature/version/type = UnsupportedCapabilities. Optional
unknown fields bounded preserve/ignore according to historical JSON behavior.
packet16<=32768 bytes, streams<=128 and IDs unique; binding identities need not
be globally unique (two explicitly named streams may publish same endpoint),
but one stream_id can never resolve two endpoints/types in one incarnation.

No feature on old16 is not canonical identity support, regardless extra fields.
Old registry rejects mixed newtype doc with existing bounded error/disconnect;
client reports UnsupportedRegistryCapabilities and does not re-advertise new
types as old/no streams. Old-only docs without feature retain normal discovery.
New registry/client fully roundtrip Scalar/Packet plus binding fields; reject
malformed feature atomically without replacing old accepted metadata.

Binding lifetime: stable device UUID stays; publisher incarnation changes after
publisher restart/reconnect or endpoint/type remap. Compatible graph revision,
gain/selected change and producer generation do NOT change binding endpoint.
Producer generation/clock from payload remains independently fenced. Reconnect
clears negotiated state, queues/assembly, obtains caps and exactAck again; old
incarnation packets discarded, never relabeled into new binding. Accepted graph
revision update does not require DATA payload/header change or reconnect of an
unchanged stream; resource selective reconciliation implementation remains3.
Endpoint removed/remapped requires old binding unavailable/fence before replace;
existing config stream_id retained, no auto-generated config key/UUID rewriting.
For direct-address legacy stream without registry canonical metadata, subscriber
can use only an explicitly supplied trusted binding; absent mapping is legacy
stream mode and cannot claim remote node+port identity. Canonical remote Engine
telemetry uses18/19/20 and does not require registry discovery.

Coordinator accepted sections9–10 IDs18/19/20 and registry design; these are upper
desktop wire caps, actual lower caps are mandatory and do not imply ESP budgets.
Nonce strictly increases within connection (one scalar high-water fence, no
unbounded used-nonce set). Before any allocation/copy, complete lengths/counts/
geometry/fragments are validated and total queue+assembly budgets admitted;
rejection atomic. Positive ACK intersection covers every selected endpoint/type.
Final shared DTO direction/kind uses domain::PortDirection/NodeKind; I supplies
PUBLIC graph_domain to qt_engine_ports and portable_stream_codec. No independent
boundary enum/catalog; wire kind_id comes from stable descriptor, not enum ABI.
New owned DATA adapter codec paths approved: adapters/desktop/include/pipeline/
port_delivery_codec.h and adapters/desktop/runtime/src/port_delivery_codec.cpp.
R2 metadata and D2 codec seams are agreed; no implementation/build authorization
is inferred from general direction alone: explicit T2 implementation handoff was
received after exact IDs/schema approval. All CMake remain I.

## 11. Реализованная граница T2 и проверка (2026-10-06)

Canonical cache подготовлен до process_ports: endpoint entries и retention pools
ограничены 2048 endpoints и суммарно 64 MiB. Prepared callback удерживает только
последнюю логическую History row; владение полный History материализуется по
точному demand под evaluation mutex. AudioPipeline доставка polling 33 ms,
Engine mailbox polling 17 ms; callbacks owning DTO выполняются вне DSP callback.
У portable runtime нет зависимости от BDSP/Qt. `runtime_port_history` использует
`history(node,port,context)` для локального источника и `apply_history_route`
для собственного внешнего источника, затем исключительно `HistoryView::row()`.
Canonical и legacy materialization учитывают подготовленную zero_rows маску;
producer graph_revision/epoch остаются отдельными от local delivery stamp.

18–20 реализованы в desktop `port_delivery_codec` и EngineClient/EngineServer.
19 подтверждает все requested endpoints/types и полный первый snapshot20;
отказ не заменяет принятую server subscription. Nonce replay fence отдельно от
активного nonce. Client принимает20 только после положительного19, проверяет
session, graph binding, sequence, selected membership/type и монотонные local
revision/epoch. Fragment10 grammar/bytes сохранены; geometry, type, sequence,
offset и бюджет проверяются до chunk-copy/reserve; общий queue+assembly2MiB,
expiry5s. Legacy5–17 не получают вымышленных endpoint IDs.

Registry16 сохраняет schema2. Canonical feature требует decimal-u64 incarnation,
versions2/3, limits и exact endpoint binding в каждом stream. Scalar/Packet
не принимаются старым registry без feature; request11 требует3 и точный ACK12.
Binding immutable в incarnation. NetworkOutput подписывается на SinkInput0,
queue владеет DTO, проверяет local stamp и сохраняет producer payload metadata.

WLED destination использует pure validation и private44byte copy. Входящий
counter не меняется; byte17 меняется только в private send copy, начиная0 с
modulo256 на каждую попытку. Cadence17ms, stale строго elapsed>300ms; invalid
не обновляет payload/time, unavailable выбирает silence на следующей cadence;
failed send сохраняет прежнее stop/error поведение.

Фактические author checks до integration build: direct codec_buffer executable
GREEN (`build-refactor-delivery-slice2/codec-direct-{build,tests}.log`) с прежними
literal golden0–6 и byte-owned Packet; registry probe RED→GREEN
(`red-registry.log`, `green-registry.log`); UDP WLED probe GREEN
(`wled-probe-{build,tests}.log`) counter0/1, untouched producer, invalid-not-refresh,
stale silence и unavailable cadence. Direct source/test syntax записана в
`final-source-syntax.log`. Эти проверки не означают acceptance desktop среза.

Назначенные integration cases для I после source freeze:
`typed_graph_tests --port-identity-only`,
`engine_typed_views_tests --canonical-ports-only` и обычный engine typed scenario,
`registry_discovery_tests --canonical-streams-only`,
`desktop_history_demand_tests --canonical-history-only` и demand allocation cases,
`stream_transport_tests --typed-routing-only`, `capabilities_tests`,
`codec_buffer_tests`, targeted continuity/preparation. Полный desktop app build,
deployment и свежая независимая приёмка выполняются координатором/I.

## 12. Integration finding: Qt macro pool constructor (2026-10-07)

I desktop build/deploy5apps+9testbinaries passed; first CTest run stopped at
engine_typed_views default startup. Actual diagnostic: unavailable delivery
marker node0 retention status5(exhausted). Portable default lifecycle prepare
and apply independently returned0 under unchanged desktop caps/limits.

Bounded standalone reproducer `build-refactor-delivery-slice2/qt_macro_pool_probe.cpp`
defines Qt-compatible empty `slots` macro before buffer_pool.h; caller supplies
64bytes,2zeroedBufferSlots,32byte block and acquires1byte. RED status5 is recorded
in `qt-macro-pool-red.log`. Public inline constructor parameter `slots` is erased
by Qt macro; initializer `slots_(slots)` becomes `slots_()` and creates empty
span. Marker is valid and memory is sufficient. No validation or limits changed.
Shared-header parameter-only fix belongs R2, explicitly authorized by coordinator;
T2 did not edit R sources or undefine the Qt macro.

Narrow Tchanges: audio_pipeline.cpp retains cache failure diagnostic and default
status code; construction failure releases prior retained handles before pools
are destroyed. engine_typed_views_tests --default-preparation-only characterizes
actual default History/count output identities and WLEDEncode Packet output /
WLEDOutput Input identity, with unavailable owned markers and no fake sink output.
Temporary diagnostic member was removed; AudioPipeline layout unchanged.

T narrow source re-frozen pending Rheaderfix and I fresh desktop rebuild/tests.
No GREEN startup claimed from binaries compiled with stale inline constructor.

Повтор после R2 parameter-only header fix: тот же bounded macro probe GREEN
status0 (`qt-macro-pool-green.log`). Focused startupcase был заново скомпилирован
с текущим AudioPipeline.cpp и исправленным header; `--default-preparation-only`
GREEN exit0 (`default-startup-green.log`). Уточнён только ошибочный assertion
нового testcase: default Window10/WindowFunction11/BarkWeighting12 занимают эти
stable IDs, WLEDOutput имеет13; application schema не менялась. T source re-frozen,
полный I rebuild/13cases и независимая acceptance остаются отдельными проверками.

## 13. Integration finding: Stop clear ordering (2026-10-07)

Normal engine_typed_views RED: remote Stop clear missing. Status delivery is
notified immediately while view delivery polls17ms, so Stopped Snapshot7 can
arrive before the matching all-unavailable TypedViews17. Client's old blanket
Stopped rejection discarded this valid clear. Narrow fix permits only nonempty
all-unavailable17 while Stopped, preserving subscription/session/revision fences;
available or mixed snapshots stay blocked. Canonical20 separately drops available
or mixed delivery while Stopped, accepts current unavailable markers and ignores
stale nonce/session/sequence/localepoch/revision without erasing current cache.
No payload metadata is replaced by local stamps; legacy wire bytes are unchanged.

Real legacy17 limitation: it carries server session+graphrevision, without the
independent pipeline Stopepoch, and unavailable payload sequence0. Arbitrarily
injected stale clear with identical session/revision cannot be distinguished from
a fresh Running ingress-clear by wire alone. Authorized EngineServer rejects old
pipelineepoch before serialization; TCP orders already serialized Stop clear
before a subsequent Running snapshot. A real Stop→Start→newdata before polling
case proves this queue boundary for both17 and20. No invented IDs/fields added.

New --stopped-delivery-only fixture covers Stopped available/mixed rejection,
all-unavailable current clear, wrong session/revision clear, subscription0,
new Running/Apply data protected against old clear, actual reconnect42→43,
canonical unavailable and preserved producer epoch800/revision900, stale canonical
localstamp/session, plus the real server Stop/Start polling boundary. Fixture
NetworkInput supplies required stream_id explicitly; actual domain validation
remains strict. The existing new canonical protocol fixture likewise supplies
required stream_id. Normal Stop test additionally checks destination clear and
no legacy/canonical resurrection under continued publication.

Author evidence: engine-stop-red.log normal RED exit1;
engine-stop-fences-red.log RED 'Stopped rejected current all-unavailable legacy clear'.
Fresh engine_client.cpp + tests compiled against current headers/I portable libs:
engine-stop-fences-green.log --stopped-delivery-only GREEN0;
engine-stop-final-normal.log normal GREEN0;
engine-stop-canonical-protocol.log --canonical-ports-only GREEN0.
Command uses g++ -std=c++23 -UNDEBUG, I Ninja CXX defines/includes/link libraries,
-Ilibs/wled_protocol/include -Itests, sources tests/engine_typed_views_tests.cpp
and adapters/desktop/runtime/src/engine_client.cpp, output
build-refactor-delivery-slice2/engine_stop_fences_green.exe. Same executable ran
all three listed cases; no timeout extension, R/D changes or shared Ninja run.
Narrow T sources re-frozen pending I application rebuild/full integration suite
and fresh independent T acceptance.

## 14. Integration finding: external History query projection (2026-10-07)

I original graph_preparation_transaction RED: unchanged assertion comparing
latest_views()[1].history with supplied external matrix failed at old line179.
Standalone I binary reproducer exit3 is history-preparation-red.log. Two desktop
adapter defects caused strict apply_history_route refusal: query constructed a
HistoryView over empty storage (frame_view only borrows its supplied span), and
owned external cache retained missing legacy sequence/producer_epoch/capacity
instead of the values admitted by processing normalization.

Narrow audio_pipeline.cpp fix: flatten the owning matrix ONLY at explicit full
History demand, under evaluation mutex; inspect nonempty rectangular geometry,
<=2000 rows and <=50 columns before reserve/copy, preserving desktop ingress
admission. Temporary scratch stays alive through strict query and row() copying.
The existing accepted source copy stores sequence/producer_epoch/history_capacity
from the actual admitted processing view. Explicit metadata stays identical;
producer epoch/revision/sequence are not replaced with local delivery stamps.
Caller input is untouched. No runtime API/algorithm change, stale-check bypass,
raw History fallback, new full copy per tick or higher limits.

Added graph_preparation_tests --external-history-only: legacy omitted fields
normalized once, unchanged caller, independent explicit second source with
seq77/epoch1700/producer1701/revision1800/capacity4/source100/time123 preserved,
caller storage released, both matrices survive compatible unrelated prepare,
consecutive closed Gate rows stay zero including after caller-clear+prepare,
secondary History remains independent. Existing assertion/expectation unchanged.

Fresh g++ -std=c++23 -UNDEBUG compile current audio_pipeline.cpp into
build-refactor-delivery-slice2/audio_history_repair.obj with I Ninja defines/includes;
link tests/graph_preparation_tests.cpp against that object and I libraries into
history_preparation_green.exe. Both --external-history-only and unchanged full
preparation target GREEN0 (history-defaults-focused.log, history-preparation-green.log).
Reuse the same fresh object for desktop_history_demand_tests: --canonical-history-only
GREEN0 (history-demand-canonical-repair.log); normal demand tests GREEN0
(history-demand-repair.log), idle Mel/History/History ingress allocations0,
processing allocated bytes3304 for both capacity8 and400.

T narrow source re-frozen; hashes source-hashes-history-repair.txt. I must rebuild
applications and repeat failed target/full15 integration cases. Prior NOTRUN
cases and independent T acceptance are not claimed GREEN by these author checks.

Memory scope clarification for independent T review: the measured3304bytes
come from desktop `measure(capacity)` with active legacy `subscribe_typed(node0)`.
That subscription forces feed_typed→evaluate_view rather than the reusable idle
path. GraphProgram display_snapshot/owned_snapshot and GraphOutput build owning
legacy vectors/maps/output-event/cache projections. Measurement covers desktop
wrapper's C++ allocations during feed_typed; equal3304 at8/400 proves only that
this compatibility work is independent of retained History depth. It does NOT
prove allocation-free whole feed_typed/capture/network/GUI processing; Qt/system
allocator activity is not covered by the fixture's operator-new counter.

Portable process_ports/PreparedGraph callback contract remains separate: prepared
workspaces and retention pools; T retain_port_delivery retains exactly newest
logical row/typed payload with preallocated endpoint slots, never full numeric
History. Warm idle Mel/localHistory/externalHistory cases execute this canonical
retention path and report operator-new allocations0. Explicit fullHistory query
flatten/materialization is outside the processing callback under evaluation mutex.
Existing desktop owning ingress conversion flattens incoming external matrix and
maintains its bounded owner at the adapter boundary; this pre-existing fullinput
copy and legacy owning projections are not newly introduced portable DSP work.
Eliminating remaining desktop compatibility allocations/conversions belongs final
slice5 boundary completion; this repair neither broadens that work nor claims a
whole-application zero-allocation hot path. R portable noalloc gate is its own
independent evidence, not inferred from equal desktop3304bytes.

## 15. F1/F2 author repair (2026-10-07)

The agreed registry schema remains section10: stream_limits contains mandatory
integer max_frame_bytes (113..2097152), max_stream_id_bytes (1..256), and
max_streams (1..128). Engine20 limits max_message_bytes/max_endpoints cannot
substitute for these fields. Each advertised stream must fit its catalog count,
UTF-8 id limit and minimum112-byte DATA header plus id. Unknown additional
stream_delivery_features strings are optional advertisements; neither a name
containing "mandatory" nor an undefined prefix creates critical semantics.
Selected payload/subscription version, known type and port-identity-v1 requirements
remain mandatory. No new critical feature extension was introduced.

Desktop StreamLimits is an additive adapter value. NetworkPublisher::limits()
reports actual2MiB/256-byte/128-stream ceilings. try_advertise returns false before
mutating catalog/producer bindings on invalid id/type,129th stream, or a different
type for an existing id. The source-compatible void advertise wrapper delegates
to that admission; NetworkRuntime uses the boolean result and fails refresh before
publishing metadata. publish checks the complete serialized size after replacing
producer stream_id with the advertised id, before encoding or updating producers.
NetworkRuntime emits the publisher's actual limits; canonical registry consumers
pass declared lower limits to the additive subscriber start overload and include
limits in their reconnect binding. Legacy feature-missing consumers use existing
defaults. Subscriber validates limits/id/type before connection, configures the
assembler's logical storage span to max_frame_bytes, and rejects an oversized
fragment total before copying it. Catalog count is enforced by registry validation;
id is checked before subscription and returned DATA must match exactly. Caller
owns Qt thread/lifecycle; no portable processing or BDSP wire change is involved.

For accepted Engine canonical subscriptions, COMPLETE batches must cover the exact
explicit selection once each; missing entries require explicit unavailable markers.
The all-ports selection checks configured graph membership when a graph node catalog
is present; an empty complete batch is valid only for an empty selected graph.
Per-entry membership/type mask and configured node kind/port type are checked;
accepted bindings cannot change kind/type at the same local graph revision.
A peer with no node catalog supplies its first type binding in the first accepted
complete batch (ACK19 has no per-endpoint type field). Invalid grammar, coverage,
membership or binding batches are rejected before accepted sequence/local delivery
fences/cache/callbacks change, preserving the previous live cache. Deltas retain
subset semantics. Stale nonce/session/sequence/local fences continue to be dropped.
Server retains only bounded thin unavailable templates for explicitly selected
endpoints, without numeric payload copies, and includes markers in replacement
complete snapshots when a selected configured endpoint is removed. Subscription
replacement clears these templates only after a positive accepted admission.

Evidence: exact independent_repro.cpp copied without changes to the author build
folder, original reviewer artifacts untouched. F1 literal schema oracle and F2
empty COMPLETE oracle changed RED to GREEN. Targeted cases cover lower frame/id
admission,128/129 publisher registration, immutable type binding and rewritten id
send budget; client initial/replacement incomplete batches, multi-output omission,
wrong membership/direction/type, duplicate wire keys, future fence poisoning and
valid unavailable complete; real server removed-endpoint marker. Final source hashes
and logs accompany the coordinator handoff. This author repair is not full desktop
acceptance; independent remaining ownership/budget/protocol checks still apply.

## 16. F3 registry publisher-incarnation admission (2026-10-07)

NetworkSubscriber adds start(host,port,id,type,StreamLimits,uint64_t expected)
without changing the existing overloads. Confirmed canonical registry bindings
pass the nonzero canonical publisher_incarnation; direct-address/legacy/serial
callers use expected0 and retain their existing first-DATA identity semantics.
Expected publisher identity is retained through internal retry/reconnect and is
reset/replaced at explicit stop/start. It is independent of DATA producer_epoch.
After complete DATA decode and exact id/type validation, mismatched DATA epoch is
discarded before accepted producer identity, retirement, generation, sequence,
gap or callback changes. This includes available frames and unavailable markers;
wrong old/future transport epochs cannot poison subsequent current delivery.
Producer metadata is never relabeled to match the registry.

NetworkRuntime passes the confirmed incarnation to the subscriber. Canonical
binding fingerprint includes only graph/node/direction/port/id/type and selected
payload/subscription versions, together with host/port/incarnation/limits; a
confirmed endpoint remap therefore clears and re-subscribes. Unknown optional
metadata does not redefine binding. Registry malformed capability replacement
is refused by existing validation before emitting a replacement/resolve; tests
confirm a live valid binding/cache remains and current delivery continues.
No BDSP wire, portable runtime, control or application changes are involved.

Author evidence: unchanged copy of independent incarnation_repro.cpp changed
old_incarnation_available1/exit1 to0/exit0. Checked-in focused subscriber and real
AggregatorServer/Client + NetworkRuntime scenarios cover initial77 then registry88,
late old frames/markers, future99, UINT64_MAX producer/generation and high sequence,
valid current88 with exact producer metadata, same-binding TCP retry, confirmed
port remap, malformed metadata atomic refusal, and explicit-host legacy0 behavior.
F1/F2 oracles and affected delivery/stream regressions are repeated with fresh
objects/binaries. Independent acceptance and integration app build remain separate.
F3 fixture clarification: actual pipeline keeps producer700/generation9 sequence
continuity through clear_ingress. Current transport88 therefore uses sequence2,
retry3; valid remap changes registry incarnation99 with node3->6 and retains
producer700/generation9 sequence4/5. Same-incarnation binding mutation is rejected
by the existing registry and was not treated as a product defect. Standalone
subscriber proves first-current88 sequence1 after mismatch999. No R code change.
The immutable-binding refusal probe uses a separate registry observer: same
incarnation99 node6->7 replacement is rejected and confirmed metadata remains
99/node6. Existing server rejection explicitly disconnects the advertiser, which
may change registry availability and clear ingress; this is not a promise of
live-cache preservation across disconnect. Malformed setter validation (099)
refuses without disconnect and separately preserves working endpoint/live cache.

## 17. F4/F5 accepted-state and fragment admission (2026-10-07)

EngineClient canonical20 validates Stopped availability admission and current
cache.graph_revision before updating accepted sequence/local epoch/revision or
publishing cache/callbacks. Stopped available or mixed batches are discarded;
valid all-unavailable delivery remains accepted. Rejected high epoch/sequence
cannot poison a later valid clear or Running delivery. Stale cache-revision
batches likewise leave accepted state untouched.

EngineClient and EngineServer share Wire::receive in engine_client.cpp; there is
no separate fragment assembler in audio_pipeline.cpp. While fragment10 assembly
is active, every nonfragment message is an interleave violation. Policy is guard,
release partial QByteArray/reset receiving,total,kind/assembly timer, abort socket,
and return without dispatching that message. Malformed fragment rejection uses
the same cleanup/abort path before application processing. Successful complete
assembly resets receiving geometry/timer before dispatch; a following normal
message remains valid. Existing2MiB total+outbound budget, fragment geometry/order,
canonical type negotiation and5s expiry preflight remain enforced.

Author evidence: unchanged contract_repro.cpp copy changed both original RED
cases toGREEN: Stopped epoch999 rejected then unavailable11seq3/current12seq4
accepted; active partial20 interleaved status7 not accepted. Checked-in focused
Stopped poison test adds sequence999 rejection then valid sequence3/4. Existing
Stopped mixed test adds epoch1000 rejection then clear11. Fragment test covers
client abort/no interleaved status application, valid consecutive fragments20
above65535 bytes followed by normal status, and shared server receive rejection
of interleaved Stop5 while Command5 partial assembly is active, retaining running
engine status/graph. All unchanged F1–F5 oracles and affected author regressions
are repeated with fresh engine objects/binaries. Full independent desktop
ownership/budget/lifetime/WLED acceptance and integration app build remain pending.

## 18. C1/C2 socket-free contract repair (2026-10-07)

Canonical stream_protocol_versions is a nonempty unique actual subset of2/3,
with integer numeric entries. Unsupported99, strings, fractions, empty/duplicate
advertisements are refused. Each stream's integer selected payload/subscription
version must match its type's required version and occur in the advertised subset.
Thus Scalar-only3 and Mel-only2 adapters are discoverable; existing publisher
advertises both because it implements both. Optional unknown features remain
optional; no feature criticality or wire changes were introduced.

Public Qt-free desktop PortBinding and validate_admitted_membership(Frames,
span<PortBinding>) implement one control-plane membership rule: delta is a subset,
COMPLETE covers every admitted endpoint exactly once; exact graph/node/direction/
port/kind/type and duplicate checks precede success. Removed explicit endpoints
are admitted only as unavailable markers using their last accepted binding.
Validation is const, allocation-free and does not own or mutate receiver state.

Production EngineClient maps configured graphs to actual Output endpoints plus
Sink Input endpoints, restricts explicit selection, and calls this SAME helper
for both COMPLETE and delta before accepted catalog/sequence/local stamps/cache/
callbacks. An absent node or processor Input cannot enter all-ports delta state.
The graph-only peer fallback (ACK19 has no per-endpoint types) admits the first
complete catalog with real kind/port role bounds; stores it only on accepted
publication; later deltas/COMPLETE cannot grow it arbitrarily. New positive ACK
clears that fallback catalog. Stopped/revision/schema/negotiation rejection leaves
prior accepted catalog and fences unchanged. Catalog construction is a desktop
control-plane adapter action, outside DSP; no portable domain/runtime change.

Evidence bounded to socket-free execution: original reviewer contracts.cpp was
copied byte-for-byte, never edited. Normative4checks3failures/exit1 changed to
4checks0failures/exit0; its bounded534 checks passed unchanged. Public helper RED
characterization allowed unknownnode and failed; implementation GREEN rejects
unknownnode/futurestamp, processorInput/wrong direction, port/graph/kind/type,
duplicates, partial COMPLETE, available removed marker, and growth of an empty
catalog, while accepting count/output/SinkInput and valid unavailable markers.
Metadata tests cover version subsets and atomic decode refusal. Fresh changed
codec/engine objects and existing Engine test binary compiled; no real sockets or
networking implementation copies executed. C2 receiver accepted-state ordering is
static evidence plus shared public-helper proof, NOT a new transport experiment.
Earlier F1–F5 fixtures/guards remain preserved, but their transport executions
were not repeated under this release's socket-free restriction. Integration app
build and independent reviewer recheck remain separate obligations.

The independently observed queue-overhead arithmetic shortfall remains unresolved
and outside this repair authorization. No queue/bytesToWrite experiment, queue
policy change or substitute networking implementation was introduced. Full desktop
acceptance remains pending independent remaining contracts/ownership/budgets.

## 19. Consolidated C3–C10 repair (2026-10-07)

This section supersedes section18's unresolved queue arithmetic statement.
The complete batch is frozen once; application integration and independent
acceptance belong to the coordinator/I/reviewer, not to author unit evidence.

| Finding | Enforced invariant and evidence |
| --- | --- |
| C3 | Checked framing and aggregate peer budget; public numeric tests plus source tracing of all send/receive paths. No native queue experiment. |
| C4 | Available Frames entries require a resolved contract, on encode and decode before payload allocation; public codec tests also accept unresolved unavailable markers. |
| C5 | Graph-only bootstrap uses the shared public semantic binding validator: fixed ports retain their types, Sink Inputs retain their role, network and parameter-dependent routing ports accept legitimate supplied types. Public helper tests. |
| C6 | Shared Engine Wire releases partial assembly, geometry and timer on disconnect, new session, malformed/interleaved input and failed send. Source proof only. |
| C7 | Each accepted Engine connection receives a unique nonzero canonical session counter; ACK and Frames use that value. Counter exhaustion refuses the connection. Legacy server epoch remains unchanged. Source proof only. |
| C8 | Registry same-incarnation comparisons project semantic binding fields by stream ID. Reordering and optional metadata changes remain accepted and preserved; semantic changes remain refused. Metadata-only tests. |
| C9 | Retired producer identities are never evicted within an accepted binding. A seventeenth retirement disables automatic retry and requires explicit resubscription before any accepted fence/callback mutation. Source proof only. |
| C10 | Shared portable geometry validation rejects contradictory legacy metadata before caller copy or desktop allocation. Codec tests retain legacy missing/zero metadata compatibility and all type0–6 round trips. |

C3 derives the conservative framed bound from pinned BDSP
2c68056dfaeffba2f34fc64eee9bc9e331e83c7c: payload q has at most q+4
raw bytes, COBS ZPE emits at most one additional code per223 raw bytes,
then a final code and delimiter. The public bound is q+6+floor((q+4)/223).
It is60283 for a60000-byte DATA fragment plus8-byte envelope and60298 for
the Engine23-byte envelope. Checked subtraction admits a message only if socket
bytesToWrite + channel pending suffix + full reserved inbound assembly + complete
framed outbound message fits2MiB. Finalized stream IDs participate in size checks.
Initial Engine snapshot and its ACK are preflighted together. DATA, Engine,
Subscribe, ACK and error sends use bool send_packet outcomes; partial failure
aborts the recipient and frees assembly. TCP channel receives an explicit2MiB
budget. Caller-owned QtPacketChannel admission also counts device pending bytes;
this does not change or make claims about its default budget. Numeric tests and
source inspection establish arithmetic and ordering, not observed native pressure.

C5 resolves fixed semantics from existing domain definitions rather than from
FrameType alone. ScalarReduce Output and scalar Spectrogram/View ports cannot
bootstrap History. NetworkInput/Output retain valid supplied contracts. Splitter
and Switch allow actual configured output range0–7; index8 is refused. No domain
identifier, control protocol or application execution behavior changes.

C9 retains16 tombstones and refuses the next identity transition instead of
overwriting the oldest. The current producer can continue at capacity. Explicit
start/resubscription establishes a new binding and resets retirement tracking;
automatic reconnect cannot silently erase the exhausted binding's protection.

C10 applies signal_contracts validation to every DATA type and shares
compatible_geometry between portable encode/decode and the desktop adapter.
History rows are at most2000, supplied bands match columns, and supplied capacity
covers rows. Supplied window FFT size, spectrum FFT/2 and Mel bands match their
actual dimensions. Missing zero-valued legacy coordinates remain accepted;
unavailable geometry remains empty. Wire header112, version/type identifiers,
literal golden bytes and ScalarPacket v3 remain compatible.

Fresh author builds compile all eight changed translation units and relink codec,
metadata, public contract, unopened publisher, Engine and unchanged F1–F5 oracle
binaries. Final socket-free executions all exit0: codec characterization,
capabilities --metadata-only, original contracts534 and normative4 checks,
residual144 checks, unopened publisher139 checks, and registry-only schema probe.
Original C4/C10 reproductions now refuse the invalid encode (their early return);
additional checked-in tests prove decode refusal without caller mutation/allocation.
F1–F5 network fixtures are compiled only, not executed. Original reviewer sources
are unchanged. No real socket, queue, GUI, audio or native graphics test was run.

Process correction: establish one field/invariant table across codec, metadata,
bootstrap and receiver admission before changing code; identify each accepted-state
commit point and every adjacent send path; distinguish fixed semantics from
parameter-dependent ports and absent legacy metadata. Reuse immutable public
oracles, add decode-side assertions where an early return hides coverage, compile
exact owned objects, then issue one batch freeze. This improves coverage and avoids
repeated integration/reviewer cycles without overstating source proof as execution.
