# Numeric control/storage handoff — N0 proposal for N2

Status: **DOC READY; implementation NOT STARTED**. Ownership K: this document only.
The existing K14 schema1/public codecs/generated sources/vendor pins and their
bounded acceptance remain immutable. This proposal does not extend that acceptance
to numeric processing, DATA, JSON or embedded task stack usage.

Authority: [numeric-data-contracts.md](numeric-data-contracts.md), updated
[implementation-plan.md](implementation-plan.md),
[numeric-foundation-handoff.md](numeric-foundation-handoff.md) (R proposal),
[numeric-dsp-handoff.md](numeric-dsp-handoff.md) (D proposal),
[control-codec-contracts.md](control-codec-contracts.md), and
[project-storage-contracts.md](project-storage-contracts.md).
R/D documents are working proposals, not frozen interfaces. The pinned
[Clean Architecture / DDD reference](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md)
was fully read in this chat; portable contracts and application DTOs must not
depend on Qt, JSON, TinyCBOR, nanopb or platform resources.

## 1. Boundary and ownership

R owns the domain contract, parameter/policy types, compatibility and portable
processing DTO. D owns numeric conversion semantics and implemented kernel
combinations. K maps those types to bounded control DTOs and two serializers.
T owns negotiated DATA descriptors/payload, framing and asynchronous ownership.
P owns Qt JSON parsing, document projection, migrations and atomic persistence.
S owns capability negotiation/session orchestration; root assigns versions and IDs.
The adapters translate explicitly; a transport enum or JSON shape does not become
the domain's source of numeric meaning.

Control carries contracts, Converter parameters, graph/state and capability rows.
DATA carries actual frame extents, plane offsets/strides, coordinates, sequence,
ring traversal or row events, and exact payload bytes. A control contract describes
allowed geometry/representation; it never serializes pointers, ring cursors or a
full runtime history as processing configuration. Actual layout must be validated
against its negotiated profile. A history snapshot is an explicit bounded query;
the normal asynchronous path carries owned row events, not borrowed frame views.

## 2. Concrete DTO/API proposal

Names below are proposals for separate N2 headers, not additions to frozen v1.
Use the actual SDK standard C++20, plain structs, tagged unions/variants and
pointer/count views; no RTTI,
exceptions, owning STL containers or codec-specific generated types in public DTOs.
Numeric registries are explicit translation tables with unknown-ID rejection.
There is no ordinal cast from old `FrameType` or from a domain dtype enum.

```cpp
struct WireNumericContract {
    WireDType dtype;                 // ten fixed types, explicitly mapped IDs
    WireSemantic semantic;          // stable identity plus quantity/unit metadata
    WireShape shape;                // rank 0..2, ordered axes and bounded extents
    WireChannels channels;          // homogeneous ordered channel planes
    WireRepresentation representation;
    WireCoordinates coordinates;   // rational grids / explicit bounded coordinates
    WireClock clock;                // identity, timebase, lineage requirements
    WireValueDomain value_domain;  // R/D authority; structural vs finite-only
};
struct WireStructuredContract {
    StringView schema_id;
    uint32_t schema_version;
    WireStructuredEncoding encoding;
    uint64_t min_bytes, max_bytes;
};
using WireDataContract = TaggedUnion<WireNumericContract, WireStructuredContract>;

struct WireNumericLiteral {
    LiteralTag tag; // Signed64 / Unsigned64 / Binary32Bits / Binary64Bits
    LiteralPayload value; // int64_t / uint64_t / uint32_t / uint64_t
};
struct WireConverterParameters {
    WireDataContract input, output;
    WireConversionPolicy policy; // rounding, overflow, precision, nonfinite,
                               // scaling mode, exact literals/dyadic exponents
};
struct WireCapabilityRow {
    StringView operation_id, kernel_id, backend_id;
    WireContractProfile input, output;
    WirePolicyProfile policies;
    WireGeometryLimits geometry;
    WireMemoryLimits memory;
};
struct NumericControlArena { /* caller-owned spans for every variable record */ };
struct NumericControlCodecLimits { /* bytes, counts, text, depth, total budgets */ };
class NumericControlCodec {
    // proposed methods: encode(message, output, scratch, limits)
    // decode(input, destination, arena, limits)
    // explicit result/status + required sizes; no allocating overload
};
```

`TaggedUnion`, profile structures and optional views above denote DTO structure,
not a request for a generic tensor framework. R's published live headers are
`data_contract.h`, `data_frame.h`, `numeric_policy.h`; they are not frozen N2 DTOs.
Their syntax compilation does not establish full numeric GREEN. Shared validation belongs below the
two codecs; codecs validate structural/presence/bounds constraints, then the
application/domain validates supported combinations and policy meaning.

Numeric contracts represent Int8/UInt8/Int16/UInt16/Int32/UInt32/Int64/UInt64/
Float32/Float64. Scalar rank0 has one value and one plane; vector rank1 and matrix
rank2 have explicit axes. History is frame-major matrix per channel, oldest to
newest, newest age zero; channel is a plane, never an implicit rank3 axis.
Interleaved planes may share backing bytes with disjoint logical element addresses.
Wire profiles specify layout/storage width/endian/valid bits/alignment/stride bounds;
actual offsets and extents belong to DATA. Native encoding is resolved to explicit
endian before serialization, and IEEE widths are explicit.

Packed signed PCM24 has logical Int32/storage width3; valid24-in32 has width4 and
declared LSB sign extension or MSB low-zero placement. PCM32 is distinct.
Representation includes normalization/bias rules; UInt PCM silence is not numeric
zero unless its declared representation says so. Metadata is not a conversion.
Coordinates use real grids/timebases or explicit index axes, not invented Hz/seconds.
`filled_count` uses exact UInt64 by default; explicit UInt32 requires a bounded
capacity proof. Byte is a UInt8 alias, not another dtype.

Structured is a distinct tagged branch with schema identity/version/encoding and
bounded bytes. It is not a UInt8 vector and is not arbitrary JSON in a processing
parameter. Known device adapters validate their payload schema (including existing
WLED identities/versions); transport recognition alone does not authorize execution.
Unknown schemas may be preserved by the document adapter but cannot be applied
without matching support. No schema upload/registry service is introduced here.

Converter persists both contracts and all explicit policies from D/R: TowardZero
or NearestTiesEven, Reject or Saturate, RejectInexact or AllowDeclared, nonfinite
Reject/ReplaceFinite/ClampFinite, and Identity/IntegerAffine/DyadicAffine/
PcmNormalize/PcmDenormalize/Binary64Affine scaling. Exact literals preserve sign,
width and bits; no UInt64 is routed through double. Dyadic exponent bounds and
Binary64Affine restrictions follow advertised implemented combinations, not the
mere presence of the fields. Saturation/replacement requires D's declared loss
permission. Finite policy replacements are checked semantically; DATA may preserve
IEEE nonfinite bits under the declared value domain. Legacy finite-only fields
retain their old validation. Root's direct decision: preserve inactive gain,
offset and replacement fields exactly on replay/equality/wire/storage; never
require absence, reset or silently erase them. Always check structural enums,
literal representation and bounds; apply semantic range checks only to fields
active for the selected ScaleMode/NonFinite. Inactive values do not enable
behavior. An unsupported active policy must be refused.

Capabilities are complete supported rows, not independent lists whose Cartesian
product is inferred. Each row relates operation/backend, input/output profiles,
permitted policy combinations, value domain, geometry and memory/stack bounds.
Policy profiles must not imply arbitrary combinations of individually listed modes.
Float64 storage does not promise Float64 FFT or hardware arithmetic. An epoch's
capability snapshot is immutable. Old `max_types=9` is neither a numeric dtype
ceiling nor evidence of support for any new combination.

## 3. Identical CBOR/Protobuf semantics and exact64

Both codecs serialize the same DTO, required-presence rules and optionals.
CBOR maps and Protobuf fields get one reviewed field table; tags/field numbers are
assigned only after root approves the schema. Zero required values remain present;
an absent diagnostic is distinguishable from a diagnostic equal to zero.
Unknown/duplicate mandatory fields, invalid tags, malformed UTF-8 where text is
required, invalid bounds and unsupported schema version receive explicit errors.
Unknown optional extension policy must be identical in both codecs, never inherited
accidentally from nanopb's skip behavior versus CBOR traversal.

| Value | CBOR proposal | Protobuf proposal |
|---|---|---|
| signed64 literal/time ticks | integer, checked negative magnitude | `sint64` |
| unsigned64/count/size/revision | unsigned integer | `uint64` |
| Float32 exact literal | tagged unsigned 32-bit bits | `fixed32` |
| Float64 exact literal | tagged unsigned 64-bit bits | `fixed64` |
| optional error plane/element | absent or unsigned index | explicit presence + unsigned index |
| numeric DATA samples | T-owned explicit byte representation | T-owned explicit byte representation |

INT64_MIN decoding must use checked unsigned magnitude, not negation overflow.
Float-bit literals preserve negative zero/subnormals; finite replacement checks
do not globally canonicalize their bits. R's live `numeric_policy.h` stores
`variant<int64_t,uint64_t,float,double>`; K extracts each float alternative's own
bits without converting Float32 or integers through double. D's native SIZE_MAX error-index sentinel
maps to absence, not an unreviewed wire ordinal. Narrow dtypes are validated by
their declared dtype when converting an exact literal into a parameter.
No per-sample control variant/boxing is added. Decode ownership includes contract,
plane/channel/axis/profile/schema strings and descriptors, not only payload bytes.

## 4. Bounded storage and generated-code limits

Preserve the legacy operational guarantee in the new API: validate/count without
mutation, report exact required capacities, then commit into caller arena.
On error produced/consumed are zero and destination/arena remain unchanged;
encode output remains unchanged on failure. Scratch is disposable. Caller-owned
input/output/scratch/arena are stable and disjoint for the call. No hidden heap,
exceptions/RTTI, mutable global arena or allocating convenience path.

Explicit limits cover wire bytes, graph nodes/edges/contracts, total capability
rows/profile references, total planes/channel identities, schemas, coordinate
entries, policy literals, per-string UTF-8 bytes and aggregate text, aggregate
descriptor bytes, scratch bytes and nesting depth. Rank axes have fixed maximum2;
four literal tags are fixed. Planes/custom schemas/rows are bounded caller spans,
not large stack arrays. Every count, multiplication, offset and sum is checked
before conversion from uint64 to host size_t, including 32-bit targets.
Layout proof uses storage width, offset and capacity strides; logical overlap,
alignment, zero extents and rows0 History are validated according to R's contract.

Generated nanopb uses bounded callbacks for variable collections/text, fixed
small records for rank/tag/presence, no FT_POINTER/PB_ENABLE_MALLOC. Generator
options encode the selected envelope; parser limits and advertised capabilities
must agree with it. TinyCBOR nesting and Protobuf callback recursion are bounded.
New numeric schema/generated files are checked in with reproducible generation
and retained existing vendor pins; ordinary SDK build still needs no generator.

No new universal max_planes or large desktop default is assigned in N0. Root/R/T/S
must approve concrete host and device envelopes before N2: per-contract planes,
aggregate planes across graph and capability rows, custom schemas/text/coordinates,
wire bytes, arena and scratch. Required arena accounting is the checked sum of
aligned record storage plus owned string/coordinate bytes. Include both old and
candidate graph resources, owned async events and adapter reserves in preparation
peak; a control-only arena count does not establish application peak memory.
Measure new compound callback stack frames and actual ESP task nesting separately;
the old K1 stack probe is not a new-schema/device-stack proof.

## 5. Independent version axes and legacy bridge

| Axis | Existing boundary | N0 proposal requiring root approval |
|---|---|---|
| processing DTO | processing v1 | v2 candidate: numeric contracts + Converter |
| project document | schema1, graph2 | P-reviewed schema2/graph3 candidates, one file |
| control message | schema1 | separate schema2 candidate negotiated per peer |
| DATA representation | existing framing/FrameType IDs0..8 | T-defined version/feature negotiation |
| structured payload | each stable schema/version | unchanged unless that schema changes |

Versions are independent, not assumed to advance together. Capability/features,
dtype/layout/semantic/Converter IDs, structured encoding IDs and field numbers
remain proposals until root assigns them. No new numeric payload is emitted to a
peer merely because it accepts old framing or old control. Existing framing and
legacy schema1 golden bytes remain immutable; new control schema cannot overload
old FrameType IDs or borrow their enum ordinals for dtype.

A legacy bridge accepts only a proven exactly representable old contract and
parameter set: known legacy Float32 semantics, old geometry/channel layout and
supported commands. New metadata may be erased only when it is derivable without
changing meaning. Integer payloads, explicit Converter, unknown Structured and
unrepresentable layout/coordinates/policies are refused, not coerced. Document
downgrade likewise requires an exact inverse for every persisted field. Failed
negotiation/bridge/migration leaves revision, draft/applied state and saved file
unchanged; it never silently falls back to Float64 or UInt8 transport.

## 6. One JSON file: exact encoding and P seam

Root-approved direction: versioned typed decimal strings for exact signed/unsigned64
known fields and exact literal tags. Example UInt64:
`{"dtype":"uint64","decimal":"18446744073709551615"}`;
Int64: `{"dtype":"int64","decimal":"-9223372036854775808"}`.
Grammar is canonical ASCII decimal: no plus, leading zeros except zero, integer
negative zero, whitespace, exponent or fractional part. Checked accumulation
handles the signed minimum without narrowing through QJsonValue::toDouble.
Float literals use `{"dtype":"float32","bits":"80000000"}` or float64
with exactly16 lowercase hexadecimal digits (Float32 exactly8, also lowercase).
This preserves
negative zero and exact policy bits. Finite-policy validation remains separate.
Use exact rational grid/timebase integer components and typed decimal64 for their
64-bit fields. Runtime token/history state is not added to the project file.

Complementary P implementation choice: a lossless parser retaining
numeric lexemes and checked integer conversion before Qt projection. Merely
reading raw numbers into QJsonValue and converting later cannot prove exact64.
For legacy raw large integer tokens, migrate from retained source lexemes or refuse
ambiguous conversion; do not infer the user's intended integer from a rounded double.
Duplicate required numeric keys must be rejected before Qt's last-value projection.

Typed known fields alone do not guarantee preservation of unknown raw numeric
lexemes during whole-file Qt serialization. Root requires that unknown persisted
numeric values never silently round: P must retain lossless lexeme/tree storage or
return a diagnostic with no overwrite when preservation cannot be proved. This
is a concrete blocker to claiming arbitrary unknown64 round-trip; semantic
unknown-field preservation also remains required.

P extends the existing `project_processing_dto` / `processing_dto_document` seams
with version dispatch and R's DTO. Keep one physical document containing applied
pipeline_graph, draft scene_graph, ui, presets and app/device settings; no competing
numeric file/envelope. Preserve unknown/nested opaque fields, stable IDs, explicit
defaults and presentation settings; malformed local drafts remain preserved but
cannot be applied as a valid graph. Project display merge still uses graph/node/
kind identity and unique unchanged edge endpoints. Failed/future-version reads do
not authorize overwrite. Migration builds a candidate and validates all relevant
parts before commit; QSaveFile/direct-fallback-disabled atomic save remains P's
boundary. Unsupported/lossy up/down conversion returns a typed error and preserves
the existing file and active state. Qt JSON implementation belongs to P, not K.

## 7. Concrete N1/N2 work and acceptance

| Owner | Proposed files / responsibility |
|---|---|
| R, D (N1) | R live `libs/signal_contracts/include/signal_contracts/data_contract.h`, `data_frame.h`, `numeric_policy.h` and corresponding sources; resolve application processing DTO/support headers; D `numeric_convert`, `numeric_reduce`, `kernel_support`; K consumes reviewed names |
| K (N2) | new `libs/bdsp_protocol/include/bdsp_protocol/numeric_control_message.h`, `numeric_control_codec.h`, `numeric_control_limits.h`; new shared validator and CBOR/Protobuf sources; `schema/numeric_control.proto`, `.options`, separate generated numeric files and targeted codec/golden tests |
| T (N2) | `libs/bdsp_protocol/include/bdsp_protocol/stream_codec.h`, `src/stream_codec.cpp`; `adapters/desktop/include/pipeline/port_delivery_codec.h`, `runtime/src/port_delivery_codec.cpp`, `runtime/src/control_session.cpp`; proposed new numeric DATA descriptor files selected by T; owned row-event/query seam, endian/layout serialization, exact legacy adapter tests |
| P (N2) | `adapters/desktop/src/graph_document.cpp`, `include/config/config_migration.h`, `gui_config/src/config_manager.cpp`; `tests/graph_document_adapter_tests.cpp`, `config_migration_tests.cpp`, `project_atomic_save_tests.cpp`, `project_display_settings_tests.cpp`; exact parser/typed literal helpers, one-file preservation and atomic refusal |
| S/root | negotiation/capability composition, schema/feature/ID assignment and device memory envelope |

K tests through both public codec APIs: all ten contract dtypes, rank0/1/2,
planar/interleaved profiles, PCM packed24/valid24/32 distinctions, Structured,
Converter policies and non-Cartesian capability rows; equivalent normalized DTOs
after each codec. Exact values include ±(2^53±1), INT64_MIN/MAX, UINT64_MAX,
signed/unsigned boundaries, Float32/64 negative zero/subnormal bits and invalid
policy literals. Adversarial tests cover absent-vs-zero, duplicate/unknown tags,
overflow/depth/planes/text/schema limits, one-byte-short output/arena, source
release after decode, no allocation and unchanged destinations on failure.
Legacy schema1 byte goldens must remain exact. Reproducible generation, no-Qt
portable SDK/installed consumer and targeted integration checks are future N2 work.
After code integration the application must build; DOC-only N0 runs no build/tests.
Independent acceptance remains separate from authors' checks.

P tests exact64 source/save/reload, legacy lexeme migration/refusal, unsupported
downgrade, future version, unknown/draft/applied/UI/preset/default preservation and
atomic save failure. T tests byte-accurate payloads, geometry/stride/byte-order
bounds, async owned metadata and row/query lifetime. Test signals use 5–10 changing
tones inside the pipeline without speaker output. Native GUI/graphics and broad
stress/full suites require the project's separate authorization; no local Linux run.

## 8. Open choices before N1 interface freeze

These decisions are needed for the affected interface freeze/N2 implementation;
unassigned transport versions/IDs do not block N1 primitive foundation work.

1. Root assigns processing/document/control/DATA versions, feature bits and stable
   registries; confirms whether schema2/graph3 candidates are needed for JSON.
2. R/D finalize processing DTO/support names, rational/dyadic bounds and
   support-profile matching against live numeric headers. Inactive-field
   preservation is already decided by root, as recorded in section2.
3. R/T/S approve maximum/aggregate planes, coordinates and schema/text bounds,
   capability-row compression, owned History row/query descriptor and gap behavior.
4. P provides the lossless unknown/raw64 preservation or diagnostic-no-overwrite
   mechanism and concrete migration/refusal rules. Typed decimal known64 and
   lowercase fixed-width float bits are root-approved direction. K does not
   implement Qt seams.
5. K/S/I choose concrete arena/generated limits and ESP stack/memory envelopes;
   baseline K14 buffers are not assumed sufficient for compound numeric contracts.

Compact T questions: which negotiated DATA version/features carry numeric
contract identity and actual layout; what row-event/query descriptors own all
channel/schema/coordinate metadata; what per-frame/aggregate limits and gap/empty
History rules are enforced? Compact P questions: how are unknown numeric lexemes
retained before QJson projection, when is save refused, and which independent
document/graph versions permit the new known-field encoding and exact migrations?

These choices prevent implementation from inventing compatibility or allocating
unbounded descriptors. This document authorizes no numeric implementation, ID
assignment, vendor/generated edit or expansion of historical PASS scope.

## 9. N2 CONTROL implementation and author source freeze

2026-10-07: **AUTHOR GREEN / SOURCE FROZEN for numeric CONTROL codecs only**.
This section supersedes the N0 implementation-not-started status for the files
listed below. Root accepted the repaired owning domain's bounded independent
review and explicitly authorized both codecs. Independent numeric CONTROL review
and I's application/SDK integration remain pending; no runtime/hardware completion
or expansion of historical K14 acceptance is claimed.

New owned files: `numeric_control_message.h`, `numeric_control_codec.h`,
`numeric_control_limits.h` under `libs/bdsp_protocol/include/bdsp_protocol`;
`src/numeric_control_message.cpp`, `src/numeric_control_codec.cpp`;
`schema/numeric_control.proto`, `schema/numeric_control.options`;
`generated/numeric_control.pb.h`, `generated/numeric_control.pb.c`;
`tests/numeric_control_codec_tests.cpp`; this appended handoff. Build artifacts
are isolated in `build-refactor-numeric-control`. No existing CONTROL/schema1,
vendor/tool, domain/DSP/DATA, CMake/manifests, JSON/desktop/ESP, configuration or
output file was edited by K. R and K14 frozen hashes were checked unchanged.

### Actual API and canonical mapping

The actual namespace is `bdsp_protocol::numeric_control`, `Message`, `Graph`,
`Node`, `Port`, `Parameters`, `CborCodec`, `ProtobufCodec`, `Limits`, `Result`.
Both implement `Codec::encode(message,output,scratch,limits)` and
`decode(input,arena,staging,message,limits)`. Every variable string/collection/
extension in a decoded message lives in the destination arena. Value alternatives
contain their exact literals/policies, not reference-counted domain owners.
Caller storage is stable/disjoint, arenas aligned to `max_align_t`, and must
outlive views. Encode scratch and decode staging are disposable on refusal.

Decode counts the bounded structural footprint without writes, checks staging,
materializes and validates the complete staged message, checks destination arena,
then repeats the validated traversal into destination and publishes the message.
All errors leave destination arena/message unchanged with consumed/produced zero.
With insufficient staging, required sizes describe the structural footprint, not
domain admission; once staging is sufficient full validation precedes destination
capacity reporting. Required scratch means wire bytes on encode and arena bytes
on decode. No hidden allocation, exceptions, RTTI or PB malloc is used.

This codec performs structural contract/presence/bounds/version validation only.
The cold T/application adapter must map these views into the **same** owning
`domain::NumericProcessingGraph` before calling the canonical resolver/admission:

| Wire view | Owning domain mapping |
|---|---|
| Graph identity/versions/edges | exact NumericProcessingGraph fields and existing Edge endpoints |
| Node id/kind/enabled | exact ConfiguredProcessingNode fields, no new kind registry |
| Port id/direction/DataContract | ProcessingPort and bounded `own_processing_contract`, never a dangling view |
| Endpoint/Transform values | same named key/value alternatives, copying strings in the cold adapter |
| Converter/Route/History | actual domain parameter value structures reused directly |
| Reduction | same operation/channels/rows/policies, owned selected-channel vector built cold |
| Literal | actual NumericLiteral tag and own float bits; no integer64/double bridge |

Owning vectors/strings/contracts may allocate at that cold boundary and need its
explicit construction/retained-old/candidate budget and failure handling. That
mapper is not implemented or advertised as zero-allocation by these codecs.
Domain resolution rejects unknown/presentation/adapter processing keys and actual
capability admission rejects unsupported active policies/combinations before
preparation. A structurally decoded node is not executable authorization. There
is no JSON, settings document, frame payload, history snapshot or executor here.

### Exact field/ID/version behavior

The checked-in proto2 schema has explicit typed Message/Command/Reply/Graph/Node/
Port/Contract/Policy/Parameter/Capabilities/etc. messages. Shared visitor tags
match Protobuf field numbers and CBOR map keys. Generated nanopb descriptors
control protobuf field/wire types; official nanopb tag/varint/svarint/fixed/string
primitives traverse the typed callback collections without creating an owning
generated object tree. CBOR uses TinyCBOR maps/arrays. Neither wraps another codec
nor carries a custom universal binary blob. Generated callback fields use no
FT_POINTER or PB_ENABLE_MALLOC; normal builds use checked-in C descriptors.

Message tags1..9 are schema, client, session, epoch, request, expected_revision,
command, reply, extensions; exactly one command/reply is present. Command IDs1..9
explicitly name Capabilities, State, Prepare, Apply, SetParameters, Start, Stop,
Persist, Release. Parameter alternative IDs1..6 explicitly name Endpoint,
Transform, Converter, Route, Reduction, History. Literal IDs1..4 explicitly name
Int64, UInt64, Float32Bits, Float64Bits; active literal fields are sint64 tag2,
uint64 tag3, fixed32 tag4, fixed64 tag5. These named tables never cast domain enum
or variant ordinals into IDs. Existing DATA symbolic tables are reused for all
shared contract/representation/axis/value-domain/direction symbols. Native endian
is refused until an explicit cold representation resolution; no competing table
or legacy FrameType mapping was created.

Message control2 is checked in both real decoders before interpreting its body.
Canonical graph3/processing2 is independently checked; document/control/DATA
metadata in an already canonical local graph is preserved without gating that
graph. Actual capabilities advertise control2/DATA4 and a positive codec mask.
Negotiation requires both control2 and a positive common supported codec mask,
preferring CBOR. There is no implicit legacy fallback. Numeric DATA4 framing IDs
remain T's authority; K assigns no additional framing IDs. Structured schema
versions remain independent. Schema1 bytes/goldens/files are unchanged.

Capabilities rows encode one complete configured node combination, kernel/backend
identity, value domain and exact plan/payload/scratch/stack/peak resource bounds.
Node contracts encode bounded geometry; policies encode exact selected settings.
Rows do not infer combinations from separate dtype inventories, and encoding a
row does not discover or certify native DSP support. Production row construction
must use the accepted actual capability query adapter.

Unknown top-level/nested schema fields and duplicates refuse in both codecs.
Allowed unknown extensions use explicit records with unique tags1024..32767 and
owned binary bytes; the mandatory/high-bit namespace refuses. Unknown optional
extension bytes including NUL/non-UTF8 preserve exactly. Absent diagnostics remain
distinct from zero, and signed diagnostic NodeId keeps its exact int32 value.

### Limits, receipts and remaining risks

All limits are explicit and zero restrictive, with depth hard ceiling32.
`desktop_limits()` supplies wire/arena524288 bytes each, text131072 bytes,
records8192/nodes128/edges256/ports2048/parameters2048/channels16/rows256.
`device_limits()` supplies wire/arena16384 bytes each, text2048 bytes,
records128/nodes8/edges16/ports32/parameters32/channels2/rows4. Extensions have
aggregate byte/count budgets. These are transport envelopes; independent maxima
are not promised simultaneously. Aggregate record/arena/text budgets can refuse
a message before commit. Caller-supplied staging doubles decode arena storage.
On this MinGW64 host sizeof(Node)=192, Port=256, Parameter=40, CapabilityRow=272;
device arena+stage32768 bytes, desktop1048576 bytes. Exact per-message arena size
comes from preflight. These profiles neither admit graph kernels nor establish
ESP task stack/memory acceptance.

Targeted actual-source command: PowerShell
`& ./build-refactor-numeric-control/run.ps1`. Uses existing MinGW13.1, C++20,
`-fno-exceptions -fno-rtti -UNDEBUG -Wall -Wextra -Werror -O1 -static`, actual new
sources plus readonly domain/foundation/DATA sources, and actual pinned vendor C
and generated sources. Compiler failure stops execution; no stale executable is
run. `before.json`/`after.json` cover compilation inputs, `status.log` reports
build0/run0/source_equal=True. Public RED stub receipt: red-build.log/red-run.log/
red-status.log build0/run1 with two failed codec expectations.

Final author suite: **1750 checks, failures0, observed C++ allocations0/C allocations0**.
Includes all10 dtypes × rank0/1/2, planar/interleaved metadata, PCM packed24 LE/BE
and valid24-in32 placements/endian, Structured, all6 parameter alternatives,
all scaling modes and inactive policy bits, Int64 limits/UInt64max/>2^53, NaN
payload/negative-zero/subnormal literal bits, all commands and correlated reply/
state/capability rows, absent-vs-zero diagnostics, signed NodeId/PortId limits,
owned source-release lifetime, explicit extension preservation and duplicate/
mandatory/unknown/malformed/version/depth/record/text/arena/staging/output refusal.
Independent hand-written state message and literal byte goldens are checked in
the tests, not derived from either encoder. Allocation observation wraps C malloc/
calloc/realloc and C++ new only during codec calls. `undefined-symbols.log` audit
of the new production objects plus vendor/generated objects finds no allocator,
exception or RTTI undefined symbols. These are author checks, not independent PASS.

Reproduction commands (no installs/downloads/tools edits):
`C:/Python/Python314/python.exe -S build-refactor-numeric-control/generate.py --output-dir build-refactor-numeric-control/gen-a`
and the same command with gen-b. The artifact runner executes the existing pinned
`tools/control_codegen/run.py` in memory with only the schema/options paths changed
to numeric_control. It retains the original Python/protoc/protobuf/nanopb hash
checks and temporary descriptor handling. Both outputs match checked-in files:
header SHA256 `B965E8CC725573F007F0DCDF198108FF642B588F1E73B53CA44E197FE935349C`,
C SHA256 `DB1D29F6541896EF71EF9D6CF4FD3E2E7BC028802E6C04D04572B3CF2955E622`.
`source-freeze.json` records all11 owned source/test/doc hashes.

Host `-fstack-usage` receipts: encode_impl848 bytes, decode_impl1616,
Reader::object992, Reader::sequence1056, bounded CBOR recursion112 per frame.
Nested visitors/calls add to those frames; they are not a task-peak measurement.
Device call-chain/stack must be measured or reduced before firmware admission.
I owns later CMake/SDK exports/source manifests and application build/short affected
checks after independent review. T/application own allocating domain mapping,
session revision/token/resource commit and actual capability generation. P owns
JSON migrations/persistence. None is claimed complete by this codec freeze.

## 10. Independent zero-mask finding: narrow repair

2026-10-07: independent module review was **FAIL**, with one P2 finding:
capabilities `codec_mask=0` was accepted by both codecs while negotiation refused
it. Fully read `build-gate-numeric-control-review/review-report.md`; its original
832-check batch/report/oracles and failing receipts remain unchanged. The previous
author GREEN did not cover this defect and does not supersede independent FAIL.

Only production change: shared `caps()` requires `codec_mask != 0` in the existing
version/known-bit condition. Masks1/2/3 remain valid; unknown bits remain refused
with the existing UnsupportedVersion status. No public API, schema, generated
file, vendor, wire representation, DATA/domain or integrated application closure
changed. Test changes add `codec_mask_regression`, available as `--codec-mask`
and included in the ordinary suite for future regression coverage.

Focused regression uses both public codecs for masks0/1/2/3/4/5/8: shared validate,
empty-buffer measure, encode and decode. Invalid wire fixtures are made by changing
only the scalar in a valid capabilities encoding, so decode refusal is not hidden
behind encoder refusal. All invalid cases assert produced/consumed zero and full
destination wire/arena plus message sentinels unchanged; positive masks round-trip.
The test also observes no C/C++ allocations during these calls.

`& ./build-refactor-numeric-control/run-mask.ps1 -Receipt mask-red` compiled the
actual unchanged validator with the new regression: build0/run1, inputs_equal=True,
196 checks/24 failing zero-mask expectations. This is behavioral RED, not a compiler
startup failure. `-Receipt mask-green` compiles actual repaired sources with strict
existing C++20/no-exceptions/no-RTTI flags and runs only the affected case:
**build0/run0, inputs_equal=True, 198 checks/failures0, C++0/C0 allocations**.
Pinned existing C/generated objects are reused; no generation/vendor/full suite,
application or shared Ninja run was performed for this repair.

`pre-mask-freeze.json` retains the prior eleven-file snapshot; updated
`source-freeze.json` changes only shared message.cpp, author test and this doc.
Fresh independent rereview of the unchanged oracle remains required. K reports
the repair/freeze to root; reviewer dispatch waits for root's concurrency slot.
No whole V4/application/device acceptance is claimed. **REPAIR AUTHOR GREEN /
SOURCE FROZEN, K IDLE after handoff**.

## 11. Native processing descriptors: completed CONTROL boundary repair

2026-10-07. Actual R Window/WindowFunction/StereoFFT contracts use Native storage.
The real T GraphProjection called CONTROL validation and refused these graphs
because the DATA-only symbol table has only explicit byte encodings. This blocked
remote preparation, advertised Native capabilities and E's persistence projection.

Root approved CONTROL descriptor value0=`NativeDescriptorEncoding`; fixed values
1..4 still use the shared DATA table. A small CONTROL-only descriptor helper handles
zero and delegates other values; unknown values refuse. No graph is rewritten to
LE, no Native payload bytes are serialized, no schema/field/generated change was
needed. Both CONTROL codecs and capability rows use the same descriptor mapping.
DATA4 remains unchanged and refuses Native payload representation.

The ready public checkpoint is numeric_control_message.h plus the shared
representation helper in numeric_control_message.cpp. Tests reconstruct R's actual
Native DSP contract chain, call the canonical domain resolver, real T projection
and owning mapper, and exercise both codecs for Prepare and the applied graph used
by persistence/state. Native UInt64 advertised endpoint contracts also round-trip.
All descriptor fields/coordinate/clock/channel metadata retain exact contract
identity; decoded cold owners survive arena overwrite. The affected batch covers
all five CONTROL encodings, unknown-symbol atomic refusal, short output/arena,
DATA FixedLE positive versus Native encode refusal, and new-format literal/state
and positive-mask regressions. Compilation uses readonly real T/DATA sources;
no adapter/runtime/device files were edited by K.

Receipts already completed before the new user workflow override: run-native.ps1
native-red build0/run1, unchanged product, 320 checks/8 Native failures;
native-green build0/run0, inputs_equal=True, **424 checks/failures0, C++0/C0
allocations during codecs**. Cold domain/T ownership allocation is outside that
observation. An intermediate DATA-positive fixture omitted required envelope and
sequence metadata; corrected fixture passes with FixedLE and rejects Native.
That fixture failure is not claimed as product RED. No regeneration/vendor or
full-suite run was performed. Source/API handed to I for the V1 dependent library
build without treating its result as Native/V4 acceptance.

The latest direct user override now makes the canonical numeric pipeline the
only path, removes legacy-support obligations, and cancels intermediate TDD/test
runs. Earlier compatibility and test-policy text here is historical authority,
not a continuing requirement contrary to that override. No additional test run
will be made by K before the connected system's final checks. Current new-system
regressions remain; user config/output and files outside K ownership are untouched.
E/T receive this ready boundary handoff for continued implementation/build.
Independent acceptance remains pending; no whole runtime/persistence/device PASS.
