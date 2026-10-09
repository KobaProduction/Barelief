# N1b owning processing domain handoff

2026-10-07, Europe/Moscow. **F1–F4 REPAIR AUTHOR GREEN / NEW SOURCE FREEZE for the four granted files. Independent repeat review and I integration pending.** The initial independent domain review failed; its report/oracles are unchanged. This does not close the complete N1b or canonical runtime migration.

AGENTS.md was fully reread. The complete pinned [Clean Architecture / DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md) edition was already fully read in this chat. Implementation/review handoffs must carry that link and full-reading requirement. The immutable foundation re-review report was read; R6 F1/F2 are independently closed in that bounded scope.

Owned source scope:

- `domain/include/domain/processing_contracts.h`
- `domain/src/processing_contracts.cpp`
- `tests/numeric_processing_contracts_tests.cpp`
- this document

All existing graph definitions/validation, R6 foundation, DSP, runtime/application, Qt/adapters, CMake/manifests, schemas/generated sources and user config/output remain readonly to this author. No subagents, installations, new chats, branches, commits, archives, common Ninja, native GUI, network or audio checks were used. D owns numerical arithmetic/support; I owns integration and the shared application/SDK build.

## Actual public DTO and ownership

`OwnedProcessingContract` snapshots the accepted `signal_contracts::DataContract`, without another dtype registry. Numeric shape/semantic/quantity/unit/representation, regular/explicit coordinate **descriptors**, and clock ratio/sample rate are copied by value. Channel IDs and clock-domain bytes are copied into owned allocations. Structured schema/version/encoding/payload bounds are owned, with schema/encoding text rebased by `view()`.

Private immutable Storage uses an intrusive atomic reference count. Copy/copy assignment share immutable ownership without allocation; move transfers it and clears the donor. A returned DataContract is a borrowed value whose spans remain valid while at least one snapshot owner remains alive. Empty owner is explicitly false; its default view is not admitted. Concurrent mutation of a graph/owner while it is borrowed is outside the API contract.

`own_processing_contract(source, FrameLimits, max_owned_bytes, out)` first validates the actual foundation contract and checked sizes, then performs nothrow allocations. The ledger charges `sizeof(Storage)` + actual channel array + text bytes, and simultaneously retained `out.owned_bytes()` before replacement. Every refusal retains out. Additional snapshots retained independently by the caller need their own aggregate profile accounting. The ledger excludes allocator headers and unrelated external owners; it is not a measured process peak.

This owner contains **contract metadata**, not actual frame coordinate arrays, payload, ring/mask, stream stamps or asynchronous row storage. AxisCoordinates are owned by value; actual ExplicitFloat64LE samples belong to the frame/runtime/delivery boundary. R6 OwnedDataFrame remains the reviewed frame snapshot owner. Required compact row ownership is still a separate runtime phase.

`ConfiguredProcessingNode` owns its kind string, typed parameters and input/output ProcessingPort vectors. Each ProcessingPort owns an immutable contract. Node/graph copies deep-copy strings/parameter vectors and share immutable contract snapshots; normal container construction/copy is a caller-controlled preparation operation, not a nothrow graph-builder API or processing callback. Graph/node admission bounds and charges retained capacities before approving them. A caller preparing a candidate must also bound construction and account any old graph through the resource profile. Container allocation failures outside `own_processing_contract` are not translated by this slice.

Typed parameter alternatives are EndpointProcessingParameters, TransformProcessingParameters, ConverterProcessingParameters, RouteProcessingParameters, ReduceProcessingParameters and HistoryProcessingParameters. Named processing values are `variant<NumericLiteral,bool,string>`; NumericLiteral is exactly R's Int64/UInt64/Float32/Float64 tagged variant. No integer64 value is stored in a double. `same_processing_parameters` compares literal tag and own IEEE bits, including inactive NaN payload and negative zero. Named field order is not identity; duplicate field names refuse. Explicit absent/default distinction remains stored.

## One descriptor authority and stable endpoint identity

`resolve_processing_node` publishes a borrowed ResolvedProcessingNode only on success. Runtime, editor and adapter consumers must call this resolver rather than reproduce port inference. Existing node kind strings/roles are obtained from the existing domain registry; a switch on named NodeKind constants selects operation responsibility. Existing enum order has no meaning. The explicit new kind is `Converter`. No wire/config enum IDs are assigned by these enum ordinals.

Canonical endpoint IDs remain `NodeId=int32`, `PortId=uint16`; Edge reuses the existing exact numeric structure. Named constants map input/output/history to 0 and filled-count output to 1. Source has input count 0; Sink has output count 0. Splitter/Switch outputs retain the configured consecutive numeric IDs 0..N-1, independent of vector storage order. Graph link lookup uses exact numeric IDs, never display strings. No existing IDs are renamed or renumbered.

`processing_ports_compatible` requires the proper directions and exact `same_contract`, including dtype, semantic, rank/extents/axes, channel order, layout/encoding, quantity/unit, coordinates/clock and structured schema/version/encoding. No implicit cast, conversion, packing, merge or resampling is inserted.

Implemented resolver rules:

| Operation | Actual configured-domain rule |
| --- | --- |
| Source / Sink | Role from existing registry; pure sinks have no output. Dedicated plots retain PCM/Window/Windowed/Spectrum/Mel/History/Scalar semantics. Network endpoints accept an explicit exact contract. Presentation and adapter parameters do not enter processing |
| Converter | Exactly one numeric input/output; preserves semantic/shape/channels/clock/coordinates/quantity. Dtype/representation change is explicit; PCM normalize/denormalize may declare unit change. All policy enums/literal structures checked; active arithmetic validity belongs to D support query |
| Splitter / Switch / Gate | Same exact contract at every output, including Structured. Splitter/Switch 1-to-N, 2<=N<=8 independently of looser caller port limits; selected index <N. Gate 1-to-1 with explicit open flag. Known-silence execution is not implemented here |
| ScalarReduce | Named MaxAbs/SignedMean/RMS, AllChannels/BothChannels/SelectedChannels, WholeValue/LatestHistoryRow. BothChannels requires exactly two; selected IDs must be unique and present. History requires LatestHistoryRow. Result is one Scalar with matching quantity/unit/clock; Mean/RMS require declared Float32/Float64 result |
| Spectrogram accumulator | Vector to History Matrix[Time,source-axis], exact source dtype/channels/representation/clock/element semantic/source coordinates. Rows 0..declared capacity. Port 1 is separate exact UInt64 Scalar FilledFrameCount/Frames with matching clock; UInt32/float count refuses |
| WLEDEncode / WLEDOutput | Mel input to root-approved Structured `wled-audio-sync`, schema1, `audio-sync-44-le`, exact44 bytes; sink consumes that exact schema. Custom schemas are not inferred as WLED |
| Ordinary transforms | Explicit Window Scalar/Vector PCM->Vector Window, WindowFunction Window->Windowed, StereoFFT Windowed->Spectrum, SpectrumMerge Spectrum->one channel Spectrum, SpectrumGain/Bark Spectrum->same Spectrum, Mel Spectrum->Mel. Scalar input is admitted only for Window with PCM semantics; other transforms require Vector. Rank, dtype, representation and clock checked; preserving operations compare exact contracts after their named semantic change. Actual geometry/backend/policies require capability admission |

Endpoint/ordinary transform fields reuse registered domain processing keys and their existing type/range rules; presentation, opaque, adapter and legacy `data_type` fields refuse. The latter is represented by the actual explicit port contract, not a second FrameType authority. Outer enabled is not duplicated in a parameter field. `resolve_processing_parameter` returns explicit typed values or the existing registered processing default as a borrowed/copy value, without allocation or rewriting persistence. TestSignal shared domain invariants use this same default resolver: minimum_hz < maximum_hz and maximum_hz*(1+sweep_depth) < sample_rate/2. Backend-specific geometry and implemented combinations remain part of the capability adapter.

Reduction selected-channel projection and History latest-row compact gather are **not** existing D support by implication. D's reduction query consumes its actual source contract; the future preparation adapter must explicitly prepare bounded projection/gather and refuse unsupported populations. There is no second executor in this slice.

## Concrete capability and resource admission

`NumericDeviceCapabilities` is a pure domain capability port: its Query receives the complete ResolvedProcessingNode and explicit ProcessingResourceProfile. It must query actual implemented combinations and active policies, returning supported plus exact plan/payload/scratch/stack requirements. Individual kind/dtype inventories and Cartesian products do not authorize a node. No domain dependency on DSP, Qt, JSON, operating-system resources or ESP-IDF was added. D's `conversion_support` / `reduction_support` belong behind this adapter.

`admit_processing_graph` validates versions, node IDs/uniqueness, the common resolver, all edges/exact port contracts, single incoming edge per input and acyclic topology before capability queries. Missing input connections remain legal disconnected topology; known silence/unknown availability belongs to runtime. Disabled nodes still require valid supported contracts. A null query supports no nonempty graph. Query implementations must preserve the borrowed graph and working state; domain cannot make an arbitrary external callback pure.

Caller supplies disjoint workspace >=2*N size_t elements. The first N are temporary indegrees; the second N contain node indices in topological order on success. Scratch can change on refusal; it is not a published executor plan. Admission output is assigned only after complete success. The function receives no working graph/token/lifecycle/executor object, so a rejected candidate cannot replace any working graph here. Runtime must keep this candidate-only preparation boundary when integrating.

FrameLimits and all admission/profile limits are explicit; zero is restrictive. DTO footprint charges graph/node objects, strings including conservative capacity+terminator, vector **capacities**, parameter allocations and owned contracts. Shared immutable snapshots are conservatively charged per port; small-string internal bytes can be conservatively charged again. Checked multiplication/addition refuses overflow. Per-node and aggregate DTO limits precede authorization.

Plan/payload/scratch requirements sum; serial execution stack uses the maximum. Peak admission charges retained working bytes + candidate DTO footprint + 2*N workspace + reported plan/payload/scratch + maximum stack. One-byte-short peak rejects. The actual future adapter must include strided pools, incremental rings, staging, retained delivery/frame metadata and target call chain. A deliberately incomplete callback would report an incomplete budget; this portable seam is not an implemented whole-runtime memory estimate or measured ESP/desktop peak.

Local canonical admission requires graph3 / processing2 through `processing_versions_supported`; document/control/DATA edition fields do not gate an already constructed local graph. External adapters must independently check their real edition **before** decoding, negotiation or delivery. `processing_boundary_version_supported(ProcessingBoundary,edition)` preserves current document2/control2/DATA4 membership/refusal separately. It does not authorize an external format merely because a local graph is accepted, nor does local admission call this boundary helper. This slice has no JSON, wire codec, negotiation or old-version lowering implementation. Accepting exactly representable legacy external formats requires a later explicit tested adapter; unsupported canonical graph/processing versions still refuse. Structured schema version is independent of these axes.

## Author checks and source freeze

Direct actual-source Windows build under `build-refactor-numeric-domain/run-domain.ps1`: existing MinGW13.1, C++20, `-fno-exceptions -fno-rtti -UNDEBUG -Wall -Wextra -Werror -static`. Inputs are new tests/cpp + readonly graph_definition.cpp/data_contract.cpp/frame.cpp and their headers. No archive/stub ABI or stale executable is used after implementation; compiler failure stops execution. The script records all ten project input SHA256 values before and after.

| Evidence | Actual result |
| --- | --- |
| red-build/run/status.log | Stub direct build0/run1, 152 failing expectations covering new boundaries |
| green1-build/run/status.log | Initial implementation build0/run0 |
| boundary-red-build/run/status.log | build0/run1, footprint and wrong WindowFunction semantic accepted before added rules |
| owner-peak-red-build/run/status.log | build0/run1, one-byte-short retained-old+candidate owner budget accepted before peak fix |
| final build.log/run.log/status.log and before.json/after.json | Current strict build0/run0, sources_equal=True |

An initial compile rejected an explicit self-move expression under Werror; the test uses an alias pointer to exercise the same behavior without the warning. The reserved-port boundary initially assumed 1000 ports exceeded32768 bytes; sizeof(ProcessingPort)=16 made that assumption false, so the test reserves3000. The final overflow test reconstructs a fresh source instead of relying on nonbinding `shrink_to_fit`. These oracle corrections do not weaken ownership/refusal behavior. Failed patch-tool attempts changed no source; later complete patches supplied the implementation.

Targeted tests exercise all ten dtypes x Scalar/Vector/Matrix metadata ownership; destroyed donor/channel/clock/schema storage; shared copy/move/self-assignment; exact UInt64max/>2^53, IEEE inactive NaN payload and +/-0 policy identity; wrong dtype/layout/clock links; reordered/duplicate/missing numeric port IDs; source/processor/pure-sink roles; Converter semantic preservation; routing outputs/index; selected/both channel population and latest History row; typed UInt64 count; WLED schema vs numeric; unknown/display parameter refusal and registered default resolution; duplicate input/cycle/node IDs; conservative unsupported versions/combinations; null or selected capability/resource refusal; checked sum overflow; retained-old owner and DTO capacity/peak budgets.

Test-only allocation interception refuses each of the three actual owner allocations; each returns exhausted and preserves the old contract. Threshold3 succeeds. Copy/move of immutable snapshots and graph admission with the test capability query observe zero C++ heap allocations. These are local author observations of the named operations, not a processing-runtime/hardware/noalloc acceptance claim.

`source-freeze.json` contains full hashes for the three new source/test files and this doc; `after.json` separately covers actual readonly compilation dependencies. These files are frozen for fresh independent review. Author GREEN does not replace that gate.

## Consolidated K / T / P / I / runtime seams

- K/T/P consume `NumericProcessingGraph`, ConfiguredProcessingNode/ProcessingParameters/ProcessingPort and OwnedProcessingContract from this one header. Codecs own decoded DTOs; persistent/transport enum tags are explicitly mapped to named alternatives, never inferred from variant/enum order. Keep numeric NodeId/PortId edge endpoints; preserve typed literal bits, every inactive policy field, stable node kind strings and independent version fields. No idle chat was awakened for acknowledgment; root consolidates this handoff.
- K control capability encoding needs complete implemented contract combinations or a lossless representation of the actual query profile. A serialized dtype inventory does not encode this callback. Concrete table compression/adapter support must be assigned separately.
- T DATA/frame ownership keeps actual frame coordinates, masks, payload and stream stamp separate from this contract owner. Compact last-row delivery and gap semantics remain required. WLED identity here matches T's root-approved identity.
- P preserves one physical project file, unknown lexemes/explicit defaults and applied/draft distinction. Conversion from lossless source nodes into this owning DTO and exact reverse mapping is N2; this source does not parse or overwrite JSON. Presentation/adapter parameters remain outside processing.
- I integrates the new header/source/test into the existing graph_domain target and SDK exports/manifests, then rebuilds apps/portable/installed/ESP sources and runs short affected checks after the agreed review/freeze sequence. This author did not change CMake or claim application integration. Existing NodeKind declaration/registry and foundation interfaces are unchanged.
- R's separately authorized runtime phase must adapt the **existing** PreparedGraph/GraphExecutor, construct the actual D capability query, compile canonical schedules/state/pools, implement typed silence, compact History row gather, incremental rings and affected-only transfer/reset. It must retain working graph/token on preparation failures and demonstrate final callbacks do not allocate/copy the whole history. None of those runtime/application actions is implemented or accepted by this domain slice.

## Independent FAIL F1–F4 narrow repair

Fully read `build-gate-numeric-domain-review/review-report.md` and the complete immutable `review-oracles.cpp` before repair. Current AGENTS was reread; the already fully read pinned architecture edition was not reread. All repair edits stayed within the same three new domain/test files and this handoff. Runtime, D/T files, old domain sources and CMake/manifests remain untouched by this author.

- F1: `processing_versions_supported` now checks only graph3/processing2. The new named boundary helper preserves independent document2/control2/DATA4 refusal at adapters. Local tests vary each external axis through0/1/99 without changing the canonical graph; unsupported graph/processing editions still refuse. Each external boundary helper independently refuses0/1/99 and unknown boundary enum.
- F2: Window's common structural relation admits PCM Scalar or Vector input to Window Vector output. No other transform gains Scalar input; wrong generic Scalar semantics refuse. A supporting test backend admits the declared Scalar accumulator; an unsupported backend refuses without publishing new admission. This does not implement Scalar accumulation in DSP/runtime.
- F3: after existing typed per-field validation, TestSignal resolves minimum/maximum/rate/sweep through `resolve_processing_parameter` and checks ordering plus strict swept Nyquist. Validated integer values are bounded <=192000, so temporary double comparison is exact for those integers and does not rewrite their stored tag. Float literals retain their stored bits. Absent and explicit values remain unchanged; invalid ordering/equality/default-rate/default-sweep/explicit-rate cases refuse before capability query.
- F4: Splitter and Switch enforce2..8 regardless of max_ports_per_node=16. Tests include2/8 positives and1/9 negatives for both kinds; Gate1->1 and selected-index rules remain unchanged.

New behavioral repair RED: `repair-red-build/run/status.log` records strict direct build0/run1, 23 failing expectations before product fixes. One additional test expected a StereoFFT semantic failure while carrying Window's registered `size` parameter; that legitimately failed parameter validation first. The test now clears Window-only parameters before asserting the wrong semantic relation, without weakening the Scalar restriction. The initial repair test compile had a misleading-indentation warning, preserved in `repair-first-build-failure.log` / `repair-first-status.log`; no stale executable ran. An artifact-copy command following that failed build initially copied an older run log; the final repair-red-run.log was replaced by the actual compile0 behavioral RED run and is the only claimed repair RED receipt.

After repair `run-domain.ps1` compiles the actual current implementation with the original strict flags: build0/run0 and all ten project input hashes equal before/after. Final `build.log`, `run.log`, `status.log`, `before.json`, `after.json` are the current receipt.

`run-unchanged-oracles.ps1` in this author's artifact directory compiles the ORIGINAL reviewer fixture directly, writes only author artifacts and runs versions/scalar-window/domain-rules/fanout/positive. Build0, all five run0; `unchanged-oracles-status.log`, `unchanged-*.log` and `oracle-before.json` / `oracle-after.json` record it. Reviewer fixture SHA256 stays `B178DFD6C7450CDFF31DEEF0CEECA2E2A930ED3D4B7356E6EE4BB847E1D28805`. The initial review's failing logs and oracles were not edited. The positive mode also repeats the original independent ownership/contract/resource assertions; this is an author repeat, not independent acceptance or a new broad stress campaign.

`pre-repair-freeze.json` preserves the initial failed source manifest. Updated `source-freeze.json` contains the repaired header/source/test/doc hashes. The independent reviewer must recheck the new freeze; the author's unchanged-oracle GREEN does not itself close F1–F4. I owns later application/SDK integration and rebuild. No whole N1b/runtime/device completion is claimed.

**REPAIR SOURCE FROZEN / R IDLE after reporting. Independent domain acceptance, integration and the required remainder of N1b stay open.**
