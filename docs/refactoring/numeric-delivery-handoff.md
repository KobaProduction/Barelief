# N0 / N2 numeric delivery handoff

2026-10-07, Europe/Moscow. **DOC READY / PROPOSAL; numeric delivery implementation NOT STARTED.**
This assignment owns only this document. T17, codecs, desktop/network/registry,
schemas/generated, CMake and user data are read-only. No build, test, native UI,
socket, queue experiment or install was performed. The blocked publisher experiment
must not be repeated or bypassed; whole current native queue behavior remains
**UNVERIFIED**. Historical bounded acceptance is not numeric acceptance.

Authority read: AGENTS.md, [numeric-data-contracts](numeric-data-contracts.md),
[updated plan](implementation-plan.md), [R foundation](numeric-foundation-handoff.md),
[D DSP](numeric-dsp-handoff.md), [K control/storage](numeric-control-storage-handoff.md).
The [pinned Clean Architecture / DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md)
was fully read earlier in this chat; every implementation/review recipient must read
that exact edition fully. Contracts remain portable; transport translates them,
domain owns compatibility, and PreparedGraph/GraphExecutor remains the executor.

## 1. Concrete negotiated boundary and proposed IDs

Root approved the packet/version/feature allocation and explicit numeric table
direction below for forthcoming N2; this is NOT code authorization. Independent axes:
numeric DATA payload **v4**, delivery grammar **v1**, registry advertisement **v3**;
processing/control/document versions belong to R/S/K/P. BDSP framing stays unchanged.
Legacy FrameType IDs0–8, payload v2/v3 and literal goldens stay byte-exact.
Engine Subscribe18/Ack19/Frames20 retain their old grammar; control21–25 retain
the root-approved T3 grammar. No new payload is silently emitted on those IDs.

| Proposed BDSP packet ID | Purpose |
| --- | --- |
| 26 | NumericSubscribe: nonce, exact endpoint selection, supported v4 features, limits |
| 27 | NumericAck: selected grammar/features/limits, session and immutable contract table |
| 28 | NumericEvents: complete binding catalog or delta frame/HistoryRowEvent/marker entries |
| 29 | HistorySnapshotQuery: request ID, endpoint, exact contract and lineage fence |
| 30 | HistorySnapshotReply: status, pinned immutable snapshot and exact snapshot row ordinal |
| 31 | NumericFragment: fragmentation exclusively for26–30 |

Features proposed as explicit strings: `numeric-data-v1`, `structured-data-v1`,
`history-row-events-v1`, `history-snapshot-query-v1`. Row/query features are a pair:
a peer without repair support cannot subscribe to row delivery. Selection is the
actual intersection of complete capability rows, not a dtype Cartesian product.
ACK must precede events, echo nonce, and positively select v4 and exact endpoint
contracts. Unnegotiated packets, unsupported mandatory features and unknown IDs
fail explicitly before accepted state mutation; optional advertisement notes remain
optional. Old peers see only their legacy advertisement/grammar.

| Namespace | Explicit proposed translation table, never enum ordinal casts |
| --- | --- |
| Category | 1 Numeric, 2 Structured |
| DType | 1 Int8, 2 UInt8, 3 Int16, 4 UInt16, 5 Int32, 6 UInt32, 7 Int64, 8 UInt64, 9 Float32, 10 Float64; byte aliases2 |
| Rank | 0 Scalar, 1 Vector, 2 Matrix |
| Semantic | 1 Scalar, 2 PCM, 3 Window, 4 Windowed, 5 SpectrumMagnitude, 6 MelEnergy, 7 History, 8 Numeric |
| Axis | 0 None, 1 Sample, 2 Bin, 3 Band, 4 Time, 5 Declared |
| Storage | 1 fixed LE, 2 fixed BE, 3 packed signed24 LE, 4 packed signed24 BE; Native has no wire ID |
| Layout / placement | Layout1 Planar/2 Interleaved; placement1 LSB/2 MSB |
| Normalization | 0 None, 1 SignedUnit, 2 UnsignedUnit, 3 FloatUnit |
| Coordinates / values | Coordinates0 None/1 Regular/2 explicit Float64 LE; value domain1 FiniteOnly/2 AnyIeee |
| Numeric unit | 0 Unspecified, 1 NormalizedAmplitude, 2 RawAmplitude, 3 Frames, 4 Hertz, 5 Seconds, 6 Values |

Quantity and legacy Unit keep their existing named meanings via a reviewed table
shared with K; no ID assignment is inferred here. Structured schema is bounded
UTF-8 identity + uint32 schema version + encoding identity, not a numeric dtype.
Root-approved WLED identity `wled-audio-sync`, schema1, encoding `audio-sync-44-le`,
min=max44 refers to the existing pure WLED packet bytes; it does not change that
device protocol. Custom identities use strings with independently versioned schemas.

Proposed new portable `numeric_data_codec.h/.cpp` provides count/validate/encode/
decode into disjoint caller output, scratch and descriptor arenas. No allocating
overload. Failure reports required capacities, writes zero bytes and leaves output,
arena and destination unchanged. Old stream_codec APIs remain an explicit legacy
adapter, never a second numeric validator or executor.

## 2. Descriptor, endpoint and ownership

Use actual live R `data_contract.h`, `data_frame.h`, `numeric_policy.h`; their current
implementation is not an acceptance receipt. Numeric descriptor carries dtype,
rank/axes/bounded extents, semantic/history element, quantity/unit, ordered unique
uint32 channel IDs, encoding/storage width/valid bits/placement/normalization,
alignment, layout profile, coordinate modes/rational origin+step and clock domain/
positive rational tick duration/sample rate. Actual frame carries uint64 extents,
offsets/strides per plane, coordinate bytes, payload span and status. Channels are
homogeneous planes, never rank3. Scalar is one value and one plane.

Control/ACK registers each immutable descriptor under nonzero uint32 contract_key
scoped to session + graph revision + endpoint binding. A key is an index, not a hash
proof; compare complete descriptor contents and domain authority before accepting
binding changes. Frame reference never resolves outside that scope. Inline descriptor
form is allowed for a standalone codec record but also counts toward message limits.
This avoids repeating long contract strings on a tiny device's every PCM block.

Identity is graph ID, int32 node ID, direction and uint16 port; NetworkOutput remains
its actual Sink Input endpoint. Envelope owns nonce/session, uint64 delivery sequence,
local delivery epoch/generation/graph revision. Frame owns producer epoch, source
sequence, source generation, timestamp int64 ticks and stream ID. Preserve producer
lineage separately from local delivery stamps. Reconnect creates a new nonzero session;
old session/revision/key/nonce cannot poison fences or mirrors. Counter exhaustion
refuses before commit. COMPLETE must cover exactly admitted endpoints; delta is a
subset. Validate all entries atomically before catalog/fences/queues/callback mutation.

Wire header/length fields are fixed LE; numeric storage retains explicitly selected
LE/BE. Native endian is resolved before serialization, not advertised as portable.
Packed24 is logical Int32/storage3, valid24-in32 storage4 with checked canonical
sign-extension/zero padding, PCM32 distinct. Metadata never enables unpack, normalize,
cast, merge or resample implicitly. Source representation support is exact, finite
coordinates mandatory. AnyIeee may preserve IEEE payload bits only on a negotiated
support row; finite-only ingress checks actual values, not producer assertions.

Validate every uint64→size_t/u32 narrowing, width, extent product, alignment, plane
end and stride/overlap before pointer arithmetic or allocation. No bare int/double
container for exact64. Preserve logical payload bits including negative zero;
padding is either explicit owned storage or deterministic zero in a declared dense
serialization plan, never leaked unrelated ring/driver bytes. Sparse/repacked storage
must match a separately admitted representation; byte transport is not a converter.

Owned delivery composition contains an R OwnedDataFrame plus bounded owned endpoint,
binding/contract-table owner and stamps. Payload AND channel descriptors, strings,
coordinates, masks and schema must outlive queue/callback. Published owners are
immutable; copy/move shares only immutable buffers and rebases views. Pools outlive
every owner. Retired old-contract owners count until their last reference releases.
No mutable ring pointer becomes immutable merely by acquiring a handle.

## 3. History row events and snapshot repair

Proposed canonical `OwnedHistoryRowEvent` owns compact latest Vector row with original
element semantic/dtype/channel order/column coordinates, owned History contract,
uint64 filled_count/capacity/accepted_row_ordinal, row timestamp and complete lineage/
delivery identity. Event variants: Row, Empty, Reset, Unavailable; transport gap is
separate. Filled count is min(accepted rows, capacity), never lifetime ordinal;
ordinal is monotonic within lineage and rejects overflow. Count output is distinct
Scalar UInt64 (explicit UInt32 only when supported), never metadata magnitude.

**Required R seam before N1b:** current history_row returns a borrowed Vector whose
storage still spans the full ring; current retention_requirements/retain_frame copy
storage.size(). Thus calling retain_frame on that row is NOT compact row retention.
R needs prepared compact row gather/retention or a bounded immutable row slot whose
view already spans row-sized storage. Gather only selected row's logical elements,
all planes, owned column coordinates and semantic silence; rebase offsets/strides.
Account metadata separately. It must not copy unselected ring bytes or scan old rows.
T composes the event around that owner; it does not duplicate R ring arithmetic.

N1b callback remains borrowed only during tick; publication uses preallocated row
slots and bounded queue, no heap/full-history copy. R exposes accepted row ordinal,
clock/lineage and Empty/Reset alongside latest row; current FrameStamp has no such
ordinal. Multiple rows produced in a tick need one bounded event per accepted row;
overflow is explicit gap, not pretending that latest-only contains every row.

| Mirror input | Required behavior before publication |
| --- | --- |
| Next ordinal, matching binding/contract/clock | Append once, evict oldest only at capacity; logical old→new, newest age0 |
| Duplicate/older event | Ignore without advancing accepted fences or count |
| Missing ordinal / dropped queue event | Mark incomplete/gap and request repair outside tick; no fabricated historical rows |
| Empty | Available History rows0, count0, capacity>0; no fake zero row; reduction empty is unavailable |
| Known inactive/new silence row | Append semantic silence, preserving compatible older rows; unsigned PCM uses bias, not memset |
| Reset/new clock/discontinuity/incompatible descriptor | Reset only affected mirror; establish new lineage, reject old events, then repair/bootstrap |
| Unavailable / unresolved | Publish unavailable, stop stale rendering; do not claim an available unknown contract |

Mirror is a bounded presentation/delivery cache, not a DSP executor. It may expose
latest row while marked incomplete, but full History consumers get unavailable until
repair completes. On gap, retain at most queue_slots pending events; on overflow,
discard pending events, remain incomplete and schedule one new repair, no recursive
full-copy fallback. Explicit snapshot query pins a revision/session/contract/lineage
and returns rows through ordinal S; adopt atomically, then replay buffered >S events
only if contiguous. Reject stale replies. Rate-limit to one in-flight query per binding.

Full snapshot is an explicit measured operation outside processing, subject to slot/
peak admission. Mutable runtime ring requires an R safe snapshot boundary; asynchronous
T code must not race-read it. External already immutable full-owner input can share
the full owner with immutable route metadata; budget the unique allocation once and
references separately. A locally mutable mirror never supplies that sharing shortcut.
Stop clears affected mirrors/queued events, device/clock changes fence prior lineage.
Wire IDs are N2 concerns and must not block R primitive N1a; compact-row seam blocks
N1b delivery/incremental History support until concretely implemented and tested.

## 4. Concrete envelopes for root/R/K/S approval

These are proposed adapter/codec ceilings, not universal foundation defaults or
advertised kernel support. Effective limits are min(local configured, peer selected,
actual support row). Zero remains restrictive. No existing config is rewritten.

| Bound | Desktop proposal | Initial ESP stream proposal |
| --- | --- | --- |
| Planes per contract / aggregate admitted bindings | 16 / 2048 | 2 / 32 |
| Streams / registered contracts / schemas | 128 / 256 / 32 | 16 / 32 / 4 |
| Rank / History rows | 2 / 2000 | 2 structurally; History OFF, rows0 support |
| Logical elements per frame, all planes | 800,000 | Initial raw block512; configurable derived shape, hard envelope131,072 |
| Payload bytes / complete logical message bytes | 800,000 / 900,000 | Initial raw block2048 /2240; configurable computed frame, hard logical envelope131,072 |
| Descriptor bytes per binding / aggregate table arena | 65,536 / 262,144 | 1024 / 8192 |
| Explicit coordinate bytes per frame | 32,768 | 256, within the same message cap |
| Schema/clock text bytes per string | 256 | 64 |
| Graph/stream identity bytes | 256 | 32 for new numeric profile only |
| Aggregate owned text per table | 65,536 | 2048 |
| Event slots per binding / in-flight full snapshot | 2 / 1 per peer | 2 / 0 while History OFF |
| Peer framed transport budget | existing 2MiB | Initial combined4608; larger assembly needs explicit configured combined budget, independent of4608 channel spans |
| Aggregate T delivery resources per peer | 8MiB, separately configured/admitted | inside configured adapter reserve, no extra implicit heap |

Each compound message also bounds total planes, schemas, coordinates and text by
the aggregate/table and message cap; multiplying per-record ceilings is forbidden.
Ranks/axes fixed2. Numeric descriptor/profile table is shared with K via approved
field tables, not independently duplicated caps. K's arena/generated limits require
their own exact sizeof/alignment accounting; this proposal does not assign K buffers.
Device 2240 leaves room for legacy stereo Float32 block2048 plus at most192 envelope/
actual-layout/ID bytes using a registered descriptor; larger metadata reduces payload
or refuses. A giant inline descriptor cannot bypass that cap. Defaults remain untouched.

Actual read-only profiles: embedded_support/profile.h defaults memory256KiB,
adapter32KiB, max_frame2304, queue2, block256 at8kHz, FFT2048, Mel8, history OFF.
examples/esp32_streamer/main/main.cpp uses fixed2304-byte slots,4608-byte in/out
spans, adapter64KiB, task stacks8192 bytes each; Kconfig allows memory64–512KiB,
FFT4096 default memory384KiB, UART2M baud/TCP200k B/s and utilization80%.
These are configured admission assumptions, not measured heap/CPU/stack/network
capacity. Keep existing FFT2048/explicit4096; no numeric support inferred from them.

Proposed numeric fragment header31 bytes: grammar u16, session u64, message sequence
u64, kind u8, total/offset/chunk u32; chunk1..60000. Full message preflight/reservation,
single non-interleaved assembly, contiguous offsets, exact kind/session/sequence,
checked total and monotonic timeout; any failure releases assembly and pending state.
No fragment may advance accepted application fences. Framed bound B(q)=q+6+
floor((q+4)/223), q≤65535, from pinned BDSP; max fragment B(60031)=60306.
Admit complete sum of fragment bounds plus socket bytes, channel pending suffix,
active full assembly and existing control/legacy traffic under the SAME peer budget.
Desktop max900,000 uses15 fragments: full outbound bound15×60306=904590.
For one maximal inbound assembly AND one maximal outbound message plus131072 bytes
of existing suffix/socket/control/legacy traffic, total900000+904590+131072=1935662
≤2097152 (remaining161490). The ceiling alone does not admit a message: larger
existing traffic, multiple pending messages or a legacy/control assembly may exhaust
the shared budget and force refusal/backpressure. ESP2240 single fragment gives
B(2271)=2287; simultaneous2240 assembly+2287 send=4527≤4608, leaving ONLY81 bytes
for all other traffic. Control pending above81 forces refusal/backpressure; this is
not a claim of guaranteed concurrent usability. Zero other traffic is a conditional
worked case, not a measurement. A larger configured device logical frame requires
its own corresponding combined budget;4608 is not silently treated as sufficient.
Partial send failure aborts session and releases reservations, never silently clears
queued suffix. These are proposed source invariants, not native pressure evidence.

Keep two explicit ledgers. **Traffic admission ledger:** A=sum full declared active
assemblies (numeric/control/legacy), Q=actual socket+channel pending framed bytes,
R=sum still-reserved unsent framed suffixes of accepted messages. Admission tests
A+Q+R+B(next)≤peer_budget with checked subtraction; reservation moves from R to Q
as bytes enqueue, never counted twice and never omitted.131072 above is a worked
bound on Q+R from other messages, not extra capacity or permission to ignore it.
Receive admission also counts outbound reservations and every assembly before copying.
No protocol-specific independent2MiB allowance on the same peer. A1.9MiB assembly
plus a1.9MiB outbound message would fail even with no other traffic; neither old
message ceilings nor negotiation bypass aggregate admission.

**Physical memory ledger:** outgoing owned logical payload/metadata, incoming storage,
actual channel/socket buffer reservations, arenas/staging, rings/mirrors, retained
owners and snapshot copies count their actual unique allocations at peak. A wire
reservation is admission credit, not an allocated payload; owning a logical payload
does not remove its wire reservation. Logical payload owner and encoded queued copy
are distinct allocations and both count. Share-counting is allowed only for the
same proven immutable allocation; reference/control blocks still count separately.
Both ledgers must pass; physical8MiB proposal cannot enlarge the traffic2MiB budget.

Preparation peak is checked old graph + candidate + prepare scratch + old retained
owners + row slots + mirrors + snapshot slots + table/codec arenas + adapter reserves.
Use actual storage_width (packed24=3), last-address strides, align padding, mask and
coordinates, not sizeof(float). Count shared immutable allocation once with a proven
allocation ledger; count distinct staging separately. Fragment reservation is charged
when declared, before copying. Admission checks configured throughput/cadence as well
as memory; device double-width PCM blocks may require smaller advertised blocks or
refusal. Stack/nested codec costs and hardware throughput need separate actual evidence.

Device wire chunks, assembled logical frames and total peer resources are distinct.
Proposed device numeric chunk≤2240 bytes plus31-byte fragment header has framed
bound2287≤2304; larger frames use more bounded fragments, never a larger unchecked
packet buffer. The numeric logical frame size is computed from the actual selected
source/stream port's planes×extents×storage width, checked stride span, metadata and
coordinates. Hard131,072 is a proposal ceiling, not an allocation or advertisement.
Default source block256 at stereo Float32 has payload2048; FFT window2048 accumulates
those blocks and does not imply a16384-byte source packet. Existing spectral stream
exports Mel8, not necessarily a full FFT/PCM window. Thus the current2304 assembled
profile is coherent for its documented raw/Mel export, but insufficient for genuinely
full stereo PCM2048 (16384-byte payload) or4096 (32768-byte Float32 payload).
Those configurable export cases require explicit larger assembled-frame slots,
separately sized bounded fragment/queue pools and a larger combined peer reservation.
Use configured port shape to derive the envelope; refuse inadequate profiles before
Apply/subscription without changing existing defaults. Admission includes both FFT
scratch/output rings and old+candidate/retained resources: sufficient wire size alone
does not establish RAM sufficiency for4096. R/I/N3 must prove the actual profile peak.

## 5. Registry and strict legacy bridge

Proposed registry v3 advertises `payload_versions:[4]`, selected numeric features,
exact limits/support-profile rows and descriptor-key contents for each stream.
Legacy registry v2 retains `audio/window/windowed/stereo-spectrum/spectrum/mel/history/
scalar/wled-packet` and current payload2/3 negotiation. New v3 IDs: `numeric` for all
NumericContract streams and `structured` for StructuredContract; dtype/rank/semantic/
schema are explicit fields, not inferred from that category name. Existing user stream
IDs/provider UUIDs/graph/node/port identities are retained, never renamed for this map.

Scalar(UInt64 Frames) and other Scalar dtypes traverse source→same runtime port→
NetworkOutput Sink Input→publisher26–31→registry v3→subscriber→prepared ingress with
the SAME descriptor/key/identity. Structured WLED44 traverses the same path with its
schema identity and device validation; UInt8 Vector with44 bytes is incompatible.
Registry selection compares full semantic binding keyed by stream ID; optional notes/
ordering do not restart streams, changed dtype/layout/clock/schema does. Expected
provider incarnation is passed to subscriber before first DATA, not inferred from it.

Legacy conversion accepts only exact old Float32 semantic/channel/layout/coordinates
and known WLED schema. Generic integer dtype—including exactly Float32-representable
UInt64 count—is refused as a changed contract, not downgraded sample-by-sample.
Legacy historical Float32 count remains only in a separately selected legacy route.
Int64/UInt64 beyond2^53 are never sent through double; PCM24, custom Structured,
new axes/layout/value policies and unrepresentable metadata refuse with no mutation.
Explicit user Converter plus a legacy-compatible output contract is the route to old
peers. Negotiation failure is not authorization to retry with silent narrowing.

## 6. Exact proposed source ownership and acceptance

No table entry grants code ownership in this DOC-only slice. Root releases the next
concrete implementation slice after R/K/S seams and ID approval.

| Owner | Proposed files/modules |
| --- | --- |
| R | signal_contracts/data_contract.h,data_frame.h,numeric_policy.h and corresponding sources; compact-row retention + runtime row ordinal/safe snapshot boundary in dataflow_runtime/runtime.h/.cpp; strict domain contracts and prepared ingress |
| D | dsp_kernels/numeric_convert,numeric_reduce,kernel_support; pure representation math/support; no transport/native capture |
| T portable | new libs/bdsp_protocol/include/bdsp_protocol/numeric_data_codec.h,numeric_data_message.h,numeric_data_limits.h and src/numeric_data_codec.cpp,numeric_data_message.cpp; existing stream_codec legacy bridge after narrow release |
| T delivery | new presentation/ports/include/pipeline/numeric_delivery.h; new adapters/desktop/include/pipeline/numeric_port_delivery_codec.h and runtime/src/numeric_port_delivery_codec.cpp; new include/pipeline/numeric_history_mirror.h and runtime/src/numeric_history_mirror.cpp; existing owned_frame/engine_port/engine_models/Engine client and audio_pipeline only under explicit migration handoff |
| T platform | adapters/desktop/platform/pipeline/src/stream_transport.cpp,network_runtime.cpp; include/aggregator/capabilities.h and platform/aggregator/src/aggregator_server.cpp,aggregator_client.cpp; include/pipeline/audio_source.h,capture_buffer.h and platform/pipeline/src/audio_source.cpp for native capture; control mapper/controller after K/R DTO freeze |
| K | numeric control DTO/CBOR/Protobuf/shared field tables/schema/generated/arenas; no DATA sample boxing or JSON ownership |
| P | one-file JSON64/versioned DTO projection/migration/atomic save and unknown preservation; no runtime history persisted |
| S/root | immutable application capabilities, session/revision/negotiation composition, version/ID allocation and device envelope decisions |
| I | all CMake/export/install/SDK/ESP manifests, one shared build and baseline ledger; embedded adapter owner separately assigned for source hooks/task resources |

Prepared ingress carries resolved DataContract and FrameLimits, bytes/planes/layout/
clock/value support, not max_samples as a universal payload bound. Miniaudio/I2S/source
hook reports actual PCM16/24/32/Float32/64 endian/layout/order/rate and supported options;
frame representation and conversion cost are explicit in capabilities. Native resources
remain adapters. D live KernelContract64planes/256clock ceiling is intersected with
T16/2 envelope; Float64 arithmetic flag never advertises Float64 FFT or ESP acceleration.

Planned targeted tests, **NOT RUN**: public caller-buffer codec round trips ten dtypes
rank0/1/2, literal exact64 and IEEE negative-zero/subnormal/NaN policy bits; hand-written
LE/BE goldens, packed24 min/max, valid24 padding, padded/interleaved3planes; WLED44
schema vs UInt8 mismatch; legacy byte goldens and strict downgrade refusal. Malformed
IDs/flags/keys/version/UTF-8/schema/length, SIZE_MAX and32-bit narrowing, overlapping
strides/short arenas/one-byte-short output, partial fragments/interleaving/timeout
must leave destination/fences unchanged. Test encode/decode separately, not only
round trips using the same implementation as oracle.

Ownership tests release input/descriptor tables, next tick/Stop/Prepare/queue drop,
copy/move owners and pool exhaustion; sparse row retention bytes stay independent of
capacity, with silence/coords rebased. Fake clock/transport exercises gap→query→S→
pending replay, stale reply/session/key rejection, empty/reset/new-clock/wrap, bounded
query retry, immutable full sharing vs mutable snapshot. Prepared callback/tick+row
enqueue must allocate0 and copy only bounded inserted rows; measure separately from
explicit full snapshot/query. Source/golden tests do not prove native queue behavior.
Use5–10 changing test tones inside pipeline, no speakers/music. After code freeze I
builds application + short affected integrations, then fresh independent review;
no full suite/GUI/hardware/socket experiment is authorized by this document.
Proposed T targets/files: tests/numeric_data_codec_tests.cpp,
numeric_delivery_tests.cpp, numeric_history_mirror_tests.cpp and
numeric_registry_tests.cpp; legacy tests retained. I alone registers build targets.

## 7. Decisions still needed and owner receipts

Root approved ID/version/feature allocation and numeric table direction; envelope
limits remain proposals. K/R/S must finalize
quantity/unit/support profiles, descriptor-key equality/presence and aggregate memory
ledger. R N1b must supply compact row retention, ordinal/reset and safe snapshot APIs;
that seam is required before incremental History, not before primitive N1a. Snapshot
query is a data read operation, not implicit Prepare/Apply or persisted state.
Device actual numeric source/transport/task-stack support requires a later assigned
implementation and measured acceptance; current fixed buffers are not a new firmware
numeric PASS. T17/native queue status stays unchanged and UNVERIFIED.

Direct R receipt agrees owned row/contract/coords/count/capacity/ordinal/lineage,
empty/reset/gap and outside-tick immutable query, homogeneous History planes/ring
mask ownership;16host/2device are adapter envelopes only, never foundation defaults.
R also confirmed that existing retain_frame(history_row) is full physical snapshot;
proposed R `gather_compact_row(view,row,prepared MutableNumericView)` followed by
retention of that compact view is the missing N1b seam; final signature awaits root.
D receipt states live max64planes/256clock, explicit KernelLimits+FrameLimits, History
Converter UnsupportedCombination until N1b, existing FFT/window/Bark/Mel Float32
finite-only. These are author coordination receipts, not independent acceptance.
**DOC READY; T idle pending root's concrete source handoff.**

Root approval receipt:26–31 have no existing project packet bindings (unrelated
vendor CBOR constants and aggregator's separate0–4 namespace excluded). Numeric
v4/delivery v1/registry v3 and named positive-intersection features are approved
for future N2; legacy0–8/payload2–3/data18–20/control21–25 remain unchanged. WLED
schema identity above is approved foundation vocabulary; the existing device packet
version stays independent. Arbitrary R generic schema test strings are not a known
WLED device binding. Consolidate K/R shared translation tables before implementation.
Budget correction receipt: desktop ceiling reduced to900000/800000 logical/payload;
the worked bidirectional total1935662 includes full assembly, all outbound fragment
credit and131072 existing traffic. Actual checked shared admission and separate
unique-allocation peak accounting remain mandatory. ESP4527 leaves81 only under the
stated conditions; pending control may refuse/backpressure. No native queue evidence
or source implementation is claimed by either approval or arithmetic.

## 8. N2 portable slice implementation seam digest

Root now authorizes only three new numeric_data headers, two new cpp files, one
numeric_data_codec_tests.cpp and this appendix. Public DTO owns descriptor values
but borrows bounded strings/channel/layout/payload spans; frame_view reconstructs
contract pointers after copy/move. Decode copies all variable data into disjoint
caller byte/text/channel/layout arenas only after complete preflight; output remains
borrowed from those arenas until explicit R retain_frame by an asynchronous caller.
No queue or runtime gather is implemented here. One inline frame record encodes
Frame, compact HistoryRow, explicit Snapshot or Query; batch catalogs/contract-key
tables/Subscribe/Ack/registry and transport session state are later adapter slices.
Positive feature intersection is exposed as a pure selection helper, never auto-v4.
Compact row carries its original History descriptor/count/capacity/ordinal; full
History refuses ordinary Frame/Row paths. Snapshot is an explicit message kind.
The fixed field table uses approved symbolic mappings, version/presence/length
checks and bounded optional extension TLVs; unknown mandatory extensions refuse.
Encode counts then stages in caller scratch; decode validates/counts then commits
to caller arenas. Actual R primitives validate geometry/value domain and retention;
no second dtype/semantic registry or numerical conversion is introduced.

Root adds compact binding-reference records within the same portable slice. Proposed
ReferenceContext supplies immutable nonce/session/revision plus bindings with uint32
binding+contract IDs, exact endpoint/stream/value-domain/DataContract and optional
row-origin History descriptor. Flag1 selects this form; all other unknown flags refuse.
Full envelope/stamp/dynamic actual extents/ring/mask/coordinates/availability/discontinuity
remain on wire. Omit texts, descriptor and plane layout ONLY after exact binding match
and canonical dense layout proof; reconstruct dense planar/interleaved offsets from
declared widths/extents/planes. No arbitrary host-stride inference or implicit cast.
Decode deep-copies contextual metadata into caller arenas before publishing, so later
table release cannot dangle output. No actual Subscribe/Ack registry implementation.
Fixed reference PCM stereo256:2048 payload +187 record bytes=2235≤2240; framing and
shared traffic/pool admission remain separate. Unknown/wrong revision/key/binding or
unmatched identity/contract/layout refuses atomically. This reference form does not
authorize default v4 output without the later positive negotiation adapter.

## 9. N2 portable author freeze (2026-10-07)

**PORTABLE CODEC READY / SOURCE FROZEN; I integration and independent acceptance pending.**
Actual scope: the three granted new numeric_data headers, two cpp files, one test,
and this appendix. Old stream codec/wire/T17/desktop/registry/audio/owned_frame/
CMake/control schema/generated/user config/output are unchanged.

Public measure/validate/encode/inspect/decode perform count/complete validation
before committing to caller buffers. Encode requires output and scratch≥required.wire.
Stable DISJOINT input/output/scratch/arena/source objects and immutable context are
preconditions, not a runtime alias detector. Decode byte arena base requires alignment8;
typed channel/plane arenas contain existing live objects. All strings/channel/layout/
mask/coordinate/schema/payload/extension bytes are copied, including contextual metadata.
Message fixed object/stack cost is additional to variable arena requirements. Copy/move
shares caller arena spans; frame_view rebases its contract pointer. Async callers need
separate R retention AND owned endpoint/envelope. Empty/Reset/Query are control markers,
not complete physical NumericViews; has_payload distinguishes them. Snapshot/Query
require nonzero request IDs. No queue or new owning domain DTO was implemented.

Actual inline field table: NDAT magic, u16 payload4/grammar1, explicit record kind,
flags0/value domain/reserved0; envelope seven u64; int32 node/direction/uint16 port/
graph text; R stamp six u64/int64 ticks/stream text/availability/discontinuity;
tagged complete contract, history count/capacity/ordinal and row-origin descriptor;
actual extents/ring/silence/layout/byte/coordinate/extension spans. Fixed numbers are
LE, text lengths u16, byte lengths u32, geometry u64 checked before host narrowing.
Native has no wire ID; explicit LE/BE/packed24 samples are bit-preserved. One public
symbol_id/symbol_value table supplies future K serializers. Root-approved Quantity
0–5 stays separate from NumericUnit0–6. Unknown tags/enums/flags/versions/booleans
refuse; optional unique TLV tags1024–32767 replay exact, mandatory/shadow/duplicate
tags refuse. No generic UInt8 array becomes WLED by shape; known WLED identity is
schema1/encoding audio-sync-44-le/44 bytes, device fields remain adapter authority.

Reference flag1 replaces texts/descriptor/layout with uint32 binding+contract IDs,
preserves full envelope/stamp/status/dynamic geometry and deep-copies resolved metadata.
Dense planar/interleaved layout is proved against exact selected contract. Ordinary
numeric reference actual extents must equal declared maxima and storage its canonical
span; padded/variable layout stays inline, never inferred. Snapshot History keeps
capacity/oldest/mask/filled rows. Compact row requires row-sized bytes and exact
row-origin contract/count/ordinal; R compact gather remains REQUIRED N1b.

Desktop logical/payload ceilings900000/800000 and aggregate field limits are explicit.
Device block profile2240/2048,2 planes/512 elements, identity32/schema64/metadata1024,
History OFF. Inline full-descriptor stereo256 cannot fit2240 and refuses. Positive
compact-reference Float32 stereo256 is **2235 bytes**, including2048 payload, proven
by complete literal golden and encode/decode fixture. This is logical codec evidence;
actual framing/fragment reservation/shared traffic/physical pools remain later
integration. Larger configured device frames do not change current defaults.

TDD evidence in build-refactor-numeric-data-codec: initial public UInt64 RED
compile0/run1 (red-build.log/red-run.log); compact stereo256 reference RED compile0/
run1 (reference-red-build.log/reference-red-run.log). Intermediate compiler failures
were corrected; script never executes binary after compiler failure. Final strict
C++20/no exceptions/no RTTI/assertions/-Wall/-Wextra/-Werror/static compile0/run0:
**4683 checks,0 failures,0 observed operator-new allocations**. Six new source/test
and six readonly R dependency hashes match before/after final run. This observer
does not measure malloc/OS/native pipeline or ESP stack.

Executed cases: ten dtypes×rank0/1/2×LE/BE signed/unsigned endpoints, UInt64max/>2^53
lineage; Float64 negative zero/subnormal/max/AnyIeee NaN bits; PCM packed24 literal
LE/BE min/max, valid24 placements/bad padding/truncation;3-plane interleaved/padded
storage/finite coordinates; complete408-byte scalar and2235-byte reference literal
goldens plus independent golden decode; every prefix truncation, all descriptor
symbols/flags/version, one-short output/scratch/each arena, overflow/profile/text/
metadata, unaligned input and misaligned destination; TLV exact replay/refusals;
WLED category/schema; row-versus-full backing, wrapped snapshot/mask ownership,
empty/reset/query/unavailable; stale/missing/duplicate reference context, endpoint/
layout mismatch, donor release, explicit R owner valid after wire/arena/source release.

Repeat: `& ./build-refactor-numeric-data-codec/run.ps1` from repository root. Exact
compiler/flags/sources are in that script; build.log/run.log/final-status.log and
final-before/after-hashes.txt contain final evidence. Seven-file source-freeze-hashes
and author report sent to root. No shared Ninja/CMake/broad suite/Linux/native UI/
audio/socket/publisher experiment/install/branch/commit. I alone wires/builds affected
application/SDK targets and short integration after freeze; fresh independent DATA
review precedes dependent desktop use. Subscribe/Ack/catalog/registry/fragment/session/
capture/desktop adapters and N1b runtime gather remain mandatory future handoffs.
**Not whole N2 complete; current native queue behavior remains UNVERIFIED.**

## 10. Independent F1 repair freeze (2026-10-07)

Independent numeric DATA review FAIL exposed P2 F1: Snapshot checked History
invariants only if its contract already had History semantics, without requiring
that contract. The complete independent401-byte UInt8 Scalar Snapshot/request55
was accepted by measure/encode/inspect/decode. Section9 author GREEN is historical,
not independent acceptance. Root released the same new codec/test/doc ownership.

Minimal common validate guard now requires every Kind::Snapshot to contain Numeric
Semantic::History, before payload processing/commit. Scalar, ordinary Vector/Matrix
and Structured snapshots refuse; request correlation and valid History geometry/
lineage rules are unchanged. No wire/table/router/adapters or R code changes.
Only numeric_data_message.cpp, numeric_data_codec_tests.cpp and this appendix changed;
other four source-freeze entries remain unchanged.

TDD in own build-refactor-numeric-data-codec: byte-identical original reviewer
oracles.cpp copied to f1-oracles.cpp, never edited. Before repair strict compile0/
run1: original5976 checks/1failure and all four Scalar Snapshot statuses0;
own added Scalar/Vector/Matrix inline/reference regressions compile0/run1,
4696checks/12failures. RED logs and source hashes retained. After minimal guard,
final strict actual-source builds and runs exit0: own4700checks/0failures and
unchanged original5976checks/0failures; operator-new0 both. Original probe statuses
are all3 (incompatible_contract), produced/consumed0. Plain/wrapped History and
independent reference History snapshot positives remain GREEN.

Scripts run.ps1 and run-f1-oracles.ps1 compile only actual current sources into our
own artifacts and run only after compiler0. f1-author-{red,green}-*.log,
f1-oracle-{red-build,red-run,build,run}.log, f1-final-status.log and f1-before/after-
hashes.txt record evidence; new source/test plus readonly dependencies stay identical
before/after final executions. Reviewer source and own copy hashes match. New single
six-code/test+doc freeze and report go to root for same independent reviewer recheck,
then I integration. Author repair GREEN does not close F1 independently or complete
N2. No CMake/shared Ninja/native experiments/user data/source ownership expansion.
**REPAIR FROZEN / READY / IDLE; native queue remains UNVERIFIED.**
