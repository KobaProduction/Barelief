# Control codec contract / K3 dependency handoff

2026-10-07, Europe/Moscow. **Actual CBOR/Protobuf implementation and generated
payloads AUTHOR GREEN / FROZEN for I integration and independent review.**
Coordinator's current dispatch explicitly permits this independent portion while
Gate 2 desktop review is pending. It does not authorize network integration.
Baseline b18cfbf3c2ac62b4baf022de5348f5a29d46e2cc; other dirty files are not acceptance.

Read: AGENTS.md, implementation-plan.md, ProposedSlice3 in baseline-and-ownership.md,
architecture-map.md, typed-node-routing.md, accepted processing_control/graph/domain
contracts, and the full [pinned responsibility reference](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).
The application owns sessions, admission, replay, revision, preparation, TTL and
transactional lifecycle. A codec validates representation and copies data only.

## Files and boundaries

K3 authored only:

- libs/bdsp_protocol/include/bdsp_protocol/control_limits.h, control_message.h,
  control_codec.h;
- libs/bdsp_protocol/src/control_message.cpp, control_cbor_codec.cpp,
  control_protobuf_codec.cpp;
- libs/bdsp_protocol/schema/control.proto, control.options;
- libs/bdsp_protocol/generated/control.pb.h, control.pb.c;
- tests/control_cbor_codec_tests.cpp, control_protobuf_codec_tests.cpp,
  control_codec_golden_tests.cpp;
- this document.

Actual codec bodies and generated payloads were added only after verified I
dependency/generator handoff. No stub or handwritten replacement codec was added.
Existing framing, registry/data IDs,
desktop, application/domain, CMake, vendor, pins, licenses, codegen runner,
config.json and output remain outside K3 ownership.

## Payload schema and mapper handoff

control.proto is the exact field-number inventory. CBOR uses the same numbers
as integer map keys; this is a payload schema, not a BDSP registry assignment.
Root fields: 1 schema_version=1, 2 identity {client,session,epoch}, and exactly one
of 3 command / 4 reply. Every declared required scalar is present on the wire,
including zero, false and empty strings. Omitted optional diagnostic indices are
distinct from present zero. Empty repeated protobuf fields mean empty lists;
CBOR graph/capability lists are definite arrays, including empty arrays.

Commands use explicit IDs, not C++ variant indices:

| ID | Accepted application command | Required command-specific fields |
| --- | --- | --- |
| 1 | GetCapabilities | none |
| 2 | GetState | none |
| 3 | PrepareProcessing | processing |
| 4 | ApplyProcessing | token, graph_identity |
| 5 | ReleasePrepared | token |
| 6 | SetProcessingParameters | node, parameters, enabled |
| 7 | StartProcessing | none |
| 8 | StopProcessing | none |
| 9 | ExplicitPersist | graph_revision |

All commands also carry request_id and expected_revision. Extraneous fields are
rejected, never silently dropped. Zero request/token/session values are losslessly
represented; application decides their admission. Header and epoch are preserved,
not synthesized by codec. TTL expires_at_ms remains in the host clock domain.

Reply preserves accepted/error/status, legacy ValidationResult error/node/edge,
both diagnostic locations, complete ControlState, prepared_token/expires_at_ms,
memory requirements and optional effective capabilities. Internal topological
order is not a reply field. Capabilities preserve every numeric field of accepted
DeviceCapabilities, RuntimeLimits and ControlLimits plus explicit_persist.
No mapper may truncate uint64 budgets to platform size_t without a checked bound.

Explicit payload IDs: ControlError 0..12, Status 0..10, GraphError 0..13,
Lifecycle 0..3, direction Input=0/Output=1, existing FrameType 0..8. The named
enumerators in control_message.h are authoritative for this schema proposal;
T3 must use explicit mapping rather than assuming application enum/variant order.
New BDSP control message IDs remain unassigned. Legacy1..17 and DATA18..20 are
read-only. T3 must coordinate registry/version negotiation separately.

Nodes keep signed int32 document identity and canonical kind_id text; edges keep
both uint16 port indices. Graph processing_version is its sole version authority.
Parameters have twelve named alternatives, including explicit NoParameters.
All registered views use NoParameters; FFTView channel/merge, layout, dock data,
history and generic JSON/opaque fields have no representation. Network endpoint
fields are explicit accepted NetworkParameters, not an extension dictionary.
An unknown kind name remains text for mapper/domain refusal; known-kind parameter
family mismatch is a boundary error. Numerical ranges, compatibility, cycles,
duplicate node identities, device support and runtime memory admission belong
to application/domain. No codec calls ProcessingControlService or mutates it.

## Codec interface, ownership and refusal

ControlCodec has interchangeable CBOR/Protobuf implementations with noexcept
encode/decode, caller buffers and explicit ControlCodecLimits. No Qt, OS, RTTI,
exceptions, heap ownership or application dependency in its headers/implementation.
select_control_codec(mask,mask,version,version) prefers CBOR; protobuf is selected
when it alone is common. Schema mismatch/no common known bit returns no selection.
Masks 1=CBOR, 2=Protobuf are format capability bits, not framing message IDs.

Encode validates and measures, checks destination and scratch, encodes into caller
scratch, then commits destination. Scratch is disposable workspace; destination
is unchanged on any error. Decode validates/counts without writes, checks caller
arena, then materializes views owned by that arena and publishes the destination.
Caller source, destination, scratch and arena regions must be disjoint and stable
during the synchronous operation. Incoming bytes can be reused immediately after
successful decode; arena and views live together. Async queues require retention
of this owning arena, never a borrowed view into transport bytes.

Failure has consumed=produced=0 and preserves destination and all arena storage.
Successful decode consumes the entire payload. Buffer-too-small after complete
valid preflight reports exact required wire/scratch or arena counts, including
text bytes without NUL terminators. Malformed/unsupported/limit failures promise
no required capacities. Counts use checked size_t addition; all wire integer
fields are explicit uint32/uint64/int32, with checked uint16 narrowing for ports.

Defaults: wire512KiB, nodes128, edges256, graph identity256B, individual text256B,
total text128KiB, capability kinds26, receive/publish types9 each, depth16. A hard
maximum of 32 bounds CBOR stack traversal even when caller max_depth is larger.
Depth counts conceptual schema maps/arrays, including empty NoParameters; both
formats enforce the same thresholds. Limits
are caller configurable and independent of application runtime/replay limits.
validate_control_message checks DTO structural/text/count constraints; actual
wire byte count and serialized nesting depth are codec-specific checks. No opaque
extension budget exists in v1 (zero allowed extensions). Protocol unknown fields
are explicitly refused rather than silently treated as processing parameters.

CBOR uses definite maps/arrays, ascending integer keys and minimal integer encoding;
double fields use finite binary64, preserving signed zero. Decoder may accept
unordered known keys, but rejects duplicate/unknown keys, missing required fields,
wrong types, indefinite containers, tags, overflow, excessive depth and trailing
items. UTF-8 excludes NUL, overlong encodings, surrogates and out-of-range codepoints.
Protobuf uses nanopb generated descriptors/runtime; no PB_ENABLE_MALLOC/FT_POINTER.
Finite fixed64 doubles and full uint64 values preserve the same semantics.
Schema-aware preflight must refuse duplicates, unknown tags, wrong wire types,
invalid bools, missing required/conditional fields, multiple exclusive bodies,
integer overflow and trailing malformed bytes. Stock nanopb last-value-wins and
unknown-field skipping are insufficient alone. Repeated fields are exempt from
singular duplicate rejection. Node/edge/string callbacks use bounded caller arena.

## Initial I request and environment record (fulfilled dependency handoff)

At initial API handoff, no TinyCBOR/nanopb source/header/generator cache was found
by filename inventory in this workspace. protoc was not on PATH; Python resolved to
C:/Python/Python314/python.exe and importlib.find_spec('nanopb') returned None.
This is a bounded workspace/PATH observation, not a whole-machine cache claim.
Compiler was discovered from existing
build-refactor-integration/portable/CMakeCache.txt: CMAKE_CXX_COMPILER currently
C:/Qt/Tools/mingw1310_64/bin/g++.exe. No tool installation/download was performed.

I owns acquisition, exact immutable upstream revision/content hashes/licenses,
runtime build wiring and tools/control_codegen runner. Requested handoff:

1. **TinyCBOR v7.0** verified upstream revision, public cbor.h/include path,
   complete encode/parser/error/validation source dependencies and license/pin
   inventory. Confirm the actual requested release identifier upstream; K3 does
   not silently substitute a similarly numbered release.
2. **nanopb 0.4.9.2** verified runtime pb.h/pb_common.h/pb_encode.h/pb_decode.h,
   pb_common.c/pb_encode.c/pb_decode.c and generated-descriptor compatibility;
   generator sources, nanopb.proto/options support and immutable hashes/licenses.
3. Pinned compatible protoc + generator Python/environment dependencies, their
   hashes/versions and reproducible separate command using control.proto AND
   control.options, output libs/bdsp_protocol/generated/control.pb.h/.c. Only K3
   writes generated payload files, in a coordinated runner slot. Regeneration
   twice must match bytes. No source hand generation or generator at normal build.
4. Later CMake targets: control_message + actual codec bodies/generated C/runtime,
   no-Qt/no-exception/no-RTTI compile, assertions enabled for the three targeted
   tests, no PB_ENABLE_MALLOC, installed public control headers; generator/tools
   must not be a normal build/install dependency. Whole app and installed consumer
   checks happen through I after actual codec source freeze, not in this phase.

## Initial schema/API evidence and risks (historical)

Initial control contract test link was RED exit1 (missing validation/selection
functions). Actual validation implementation plus accepted domain/frame sources
then compiled/run GREEN exit0. Command:

```powershell
$taskCompiler = ((Get-Content build-refactor-integration/portable/CMakeCache.txt |
  Where-Object { $_ -match '^CMAKE_CXX_COMPILER:' }) -split '=',2)[1]
$env:PATH = (Split-Path $taskCompiler) + ';' + $env:PATH
& $taskCompiler -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror `
  -Wno-missing-field-initializers -I libs/bdsp_protocol/include `
  -I application/include -I domain/include -I libs/signal_contracts/include `
  tests/control_codec_golden_tests.cpp libs/bdsp_protocol/src/control_message.cpp `
  domain/src/graph_definition.cpp libs/signal_contracts/src/frame.cpp `
  -o $env:TEMP/control-contract-k3.exe
& $env:TEMP/control-contract-k3.exe
```

Output: control wire contract validation/semantic IDs passed. The warning exception
is for existing domain aggregate initialization; control_message.cpp separately
compiles with -Wall -Wextra -Werror and no exception. No shared Ninja build ran.

At that API-only phase both codec fixtures separately compiled -c exit0 under
those portable flags. Their full links were intentionally RED exit1,
undefined actual codec methods/vtables;
this is dependency/API RED, **not executed behavioral refusal evidence**.
Fixtures pin literal state-command bytes, all truncation prefixes, duplicate and
unknown fields, short destination/scratch and graph arena atomic refusal, source
reuse after decode, uint64 maxima, diagnostic absent-vs-zero and signed zero.
C++ operator-new guard will detect C++ allocation when codecs are implemented;
it does not prove absence of vendor malloc (I flags/source audit still required).
The implementation phase extended semantic parity across all parameter/command/
reply/capability branches and bounded malformed/depth/overflow cases below.

Likely mistakes and checks: (1) schema/contract drift: explicit field inventory,
canonical view checks and literal goldens; require generated descriptors/parity
checks after I handoff. (2) admission mutation: codec has no service/runtime port,
fixtures assert all arena/destination sentinels survive rejection. (3) overbroad
testing/acceptance claims: standalone contract check and compile-only codec fixtures,
no shared builds/network/UI/full suite, author GREEN never independent acceptance.

That was the initial **CONTRACT/SCHEMA HANDOFF / IDLE** record. The current
implementation and evidence are recorded below; integration and acceptance are
not inferred from either author's phase.

## Actual implementation / consolidated frozen handoff

Coordinator authorized codec implementation after I verified TinyCBOR v7.0
commit 6442e749ca811e24afad1551338a45c75e09808f and nanopb 0.4.9.2 peeled commit
160d4f09e5fabb2b66aa2dea32d4f38ace2c4b3f. I supplied verified protoc36.2,
protobuf7.36.2 pure Python wheel and Python3.14.7 executable pins. K3 did not alter
vendor, manifests, runner, CMake or any other owner's source.

control_message.cpp now owns a std-only schema visitor. TinyCBOR uses its actual
encoder/parser/validation APIs. Protobuf encoding uses actual generated structs,
callback fields, descriptors and pb_encode; decoding uses generated descriptors
with nanopb tag/varint/fixed/substream primitives for strict preflight, followed
by actual generated pb_decode before arena materialization. This rejects the
unknown/singular-duplicate/default cases that stock permissive decode alone would
accept. There is no alternative byte-format implementation or Qt/JSON schema.

Callback records copy text views and sequence data pointers into fixed storage;
they never point at temporary DTO wrapper members. Graph nodes/edges are handled
one at a time. Decoder preflight counts and validates all fields without writes;
with admitted disjoint storage and immutable source, materialization repeats the
same deterministic traversal and publishes the final DTO. Arrays and every text
view are then owned by caller arena. Scratch is caller-owned disposable encode
workspace. Temporary schema objects, field tables and callback records use fixed
stack storage; this is not a claim that all memory is in the supplied scratch.
Firmware task stack/integration remains a composition responsibility, not tested
hardware evidence.

Generation commands executed by K3 through the pinned I runner:

```powershell
& C:/Python/Python314/python.exe -S tools/control_codegen/run.py --check-tools
& C:/Python/Python314/python.exe -S tools/control_codegen/run.py --output-dir build-control-codecs-k3/gen-a
& C:/Python/Python314/python.exe -S tools/control_codegen/run.py --output-dir build-control-codecs-k3/gen-b
# Compare gen-a/gen-b bytes before writing the checked-in payloads.
& C:/Python/Python314/python.exe -S tools/control_codegen/run.py --output-dir libs/bdsp_protocol/generated
```

All commands exit0. Both scratch outputs and checked-in outputs match:

- control.pb.h SHA256 75D7D2FB1CD3E15B8EF2B6974B894739AB332797FC9436EE253861C0927E90EB;
- control.pb.c SHA256 0F608AEC98028B2AE0CF2BCD743C944A78EF7FF90BC8414D22779F4DE01159D6.

Consolidated exact check: `& ./build-control-codecs-k3/run.ps1`, exit0. This own
artifact script discovers the existing compiler, verifies every runtime manifest
source hash, verifies all generated copies, compiles C99 runtime/generated C and
C++20 codec sources with -fno-exceptions/-fno-rtti/-Wall/-Wextra/-Werror, then
builds/runs precisely the three assigned tests. Golden fixture is warning-clean
separately; the existing domain source retains its documented aggregate warning
exception. No shared Ninja, whole application or network test was run.

Actual log: build-control-codecs-k3/results.log. Frozen inventory:
build-control-codecs-k3/source-hashes.txt. Test results:

| Assigned executable | Current author result |
| --- | --- |
| control_cbor_codec_tests | exit0 literal golden / atomic short buffer / arena ownership PASS |
| control_protobuf_codec_tests | exit0 literal golden / atomic short buffer / arena ownership PASS |
| control_codec_golden_tests | exit0 58 semantic cases / full resource and malformed matrix PASS |

Every semantic case goes through both codecs, checks every DTO field explicitly,
erases the incoming encoded bytes to check ownership, crosses into the other
codec, and returns to identical original bytes. Cases cover nine commands with
full uint64 and zero/default values, all twelve processing parameter alternatives
with distinct and default values, all 26 canonical node kinds, full and empty
capability lists, every control/status/graph error and lifecycle value,
diagnostic direction/port/edge absent-vs-zero/max, finite double signed zero,
UTF-8 identity, graph/no graph, and capabilities/no capabilities.

Refusal matrix tests all six arena capacities, nine transport limit classes,
every prefix of a required-tail reply, every depth threshold 0..10 in both
formats, malformed required/default presence, simultaneous parameter alternatives,
uint32 overflow, NaN, invalid bool, invalid UTF-8/NUL and an oversized caller depth
with a hostile nested CBOR payload. Individual literal fixtures additionally
cover unknown tags/keys and singular duplicates. Full destination values and all
arena bytes must match sentinels after refusal. Tests assert caller output remains
unchanged on encode refusal. C++ operator-new guard plus linker guards for
malloc/calloc/realloc execute throughout the consolidated matrix. Production
runtime/generated/codec object undefined-symbol audit finds no allocator,
exception throw/personality or RTTI dependency.

Work on mistakes: initial API-only RED was insufficient evidence for real codec
behavior, so the final pass uses actual libraries and a field-complete bidirectional
matrix. Compilation exposed a shifted parameter-alternative traversal and an
untyped PB_OSTREAM_SIZING initializer; both were fixed before tests. The CBOR graph
test was behavioral RED because text chunk iteration lacked initialization; the
TinyCBOR API usage was corrected. Reviewing depth limits exposed a missing encode
guard for empty Protobuf NoParameters and unbounded caller-controlled CBOR nesting;
both are now covered by full threshold/hostile-depth regressions. No broad tests,
fake codec, repeated whole build or cross-owner fixes were used.

I integration request after this freeze: register three codec sources and
generated/control.pb.c with its private generated include directory and actual
runtime dependencies; register the three tests (golden also needs accepted domain
descriptors/frame source); assertions enabled. Ordinary configure/build/install
must not invoke runner/Python/protoc or download. I owns whole application build,
installed SDK consumer checks and packaging of required runtime dependencies.

Status: **K3 AUTHOR GREEN / SOURCE FROZEN / IDLE**. No new control registry IDs,
network/service mapping, application mutation, independent acceptance or Gate2
decision. Coordinator schedules I integration and independent codec review.
