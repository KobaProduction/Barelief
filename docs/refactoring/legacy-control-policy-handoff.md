# Legacy/control/runtime policy handoff

2026-10-07, Europe/Moscow. **APPLICATION LEGACY DELTA FROZEN; independent review pending.**
The original source-only proposal below was subsequently approved by coordinator;
the implementation/evidence section at the end records the narrow authorized delta.
Coordinator assigned this document only. Separate explicit exception permitted the
ParameterDescriptor initializer completion in domain/src/graph_definition.cpp;
that fix is frozen/reported separately and is not implementation of this proposal.

## Authority and evidence

Read AGENTS.md, the complete T3 desktop-control-contracts.md (especially7/8/10/12),
accepted application processing_control/graph_lifecycle headers and bodies,
current desktop callsites and HEAD b18cfbf equivalents. The complete
[pinned Clean Architecture/DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md)
was already read in this chat; same revision revisited. Implementation/review
handoffs must pass that link and full-reading requirement. Stable IDs remain.

Evidence below is **source inspection**, not executed legacy characterization.
No control tests/builds/sockets/native queue experiments ran in this phase.
The separate initializer exception has actual strict compile/domain-test evidence
in build-refactor-runtime-slice2/descriptor-initializers-{red,green}.log,
descriptor-initializers-equivalence.txt and descriptor-initializers-hash.txt.
No transport/source implementation edits are authorized by this analysis.

## Concrete baseline versus accepted current behavior

| Operation and source | Actual observed semantics / consequence |
| --- | --- |
| HEAD GraphLifecycle::prepare/apply, application/src/graph_lifecycle.cpp | Prepare clears pending before validation; Apply attempt discards pending even on failure. Current historical methods preserve this. Accepted transactional methods preserve prior pending on failed replacement/Apply. |
| HEAD GraphLifecycle::start/stop; current lifecycle tests | Start requires working graph, then sets requested=true before runtime.start; Stop sets requested=false before runtime.stop. Failed operations retain previous lifecycle state, but next Apply uses changed intent. tests/application_lifecycle_tests.cpp explicitly characterizes this separately from typed failure atomicity. |
| AudioPipeline::prepare_runtime, HEAD and current | Sets executor ingress_specs, calls runtime_limits (clears pending), historical Prepare immediately followed by Apply. On failure restores old specs/limits but cannot restore cleared pending. Current delivery pools add staging work; they do not fix the ownership bypass. |
| AudioPipeline::negotiate_ingress, HEAD and current | Unchanged contract returns success without preparation. Changed/unknown contract calls prepare_runtime on working definition and enlarges local max_frame_samples from ingress capacities. No old graph_revision increment here. With a typed token, this path would silently destroy its bundle. |
| AudioPipeline::apply_graph | Invalid GraphProgram returns before lifecycle Prepare. same_processing_as replaces presentation document, may bootstrap local sources, without graph revision increment. Changed processing uses prepare_runtime, then swaps document/increments graph_revision and local epoch; physical source restart follows commit and may throw. Failed precommit preparation preserves working document/epoch/history (existing graph_preparation_tests). A postcommit source failure is a different case. |
| AudioPipeline::start_analyzer | Sets analyzer_requested=true/explicit_stop=false and typed_running=true before lifecycle.start and allocation/physical Analyzer start. Ignores lifecycle Status. A later exception produces legacy rejected reply although requested/core state may already change. Async worker failure is separate ComponentStatus. |
| stop_analyzer | Early adapter flags/generations/mailbox fences/teardown, then lifecycle.stop unless reconfiguring_sources. Ignores lifecycle Status. Portable legacy Stop failure and physical cleanup failure must be characterized at their actual phase, not assumed identical. |
| start_capture/stop_capture | Capture request and explicit_stop are adapter fields. StopCapture calls lifecycle.stop, then lifecycle.start if typed_running; this stop/start is a source fence, not proof of a user processing Stop. Replacing it by StopProcessing would change semantics. |
| set_merge/set_display_channel | SetMerge edits ALL SpectrumMerge and FFTView nodes; no single implicit node ID. Both functions call apply_graph but ignore its bool/error. execute therefore may report accepted despite ignored graph rejection. Display fields must remain presentation-only after strict projection. |
| engine_client.cpp execute/collect, HEAD and current | Engine rev increments for every accepted nonquery/nonSubscribe, including display/capture/WLED and accepted no-op; rejected exception does not increment it. Wildcard UINT64_MAX bypasses old revision check. This is NOT service revision or graph revision. No ProcessingControlService exists in HEAD. |

Current typed service has one GLOBAL token and immutable capabilities; ownership
is checked before foreign Prepare validation. Own invalid replacement keeps token/
bundle/TTL. Apply validates owner/token/identity/base revision; success consumes
token and increments revision and graph_revision. Typed failed Start/Stop do not
change intent/state/revision/token. Accepted state-changing Start/Stop increments
revision only; no-op does not. Queries/Persist/Prepare do not increment revision.
Accepted typed Start/Stop can make an existing token's base revision stale: token
is retained, but Apply conflicts until release/reprepare/TTL. Preserve this rule.

Do not promote dirty draft behavior into acceptance. In particular, ignored Status,
postcommit physical failure, SetMerge's swallowed apply failure, and negotiated
limit enlargement need actual characterization and explicit policy decisions.
They are not permission to weaken typed contracts.

## Recommendation: narrow explicit legacy entry, same service

Full adaptation using only existing submit is impossible: no existing typed command
can record Start/Stop intent when the runtime call fails. Compensation by applying
a second graph or direct lifecycle calls would bypass ownership/revision and can
fail itself. Prepare/Apply failure cleanup CAN use existing release semantics;
it does not require another executor or a second prepared bundle.

Proposed application API (names exact for discussion, NOT implemented):

```cpp
// Immutable per admitted session, stored in existing ControlSessionSlot.
enum class ControlSessionPolicy { Transactional, LegacyCompatibility };
// Existing open_session(client) and submit(...) remain unchanged/default typed.
OpenControlSessionResult ProcessingControlService::open_legacy_session(
    std::uint64_t client) noexcept;
ProcessingReply ProcessingControlService::submit_legacy(
    SessionIdentity, CommandHeader, const ProcessingCommand&);
// Add to the SAME ProcessingControlRuntimePort, implemented by the SAME adapter:
virtual Status start_legacy() = 0;
virtual Status stop_legacy() = 0;
```

GraphLifecycleControlPort delegates these two hooks to existing lifecycle.start/
stop. No GraphLifecycle API change is needed. Desktop runtime adapter implements
these hooks with characterized physical phases around that SAME lifecycle.
Its typed hooks stage resources and preserve failure atomicity. It never calls
AudioPipeline's public command method recursively back into service.

Only trusted composition opens legacy sessions; a typed wire mapper cannot select
legacy policy. All sessions occupy the SAME caller slot span/max_sessions. Calling
the wrong mutation entry for a session returns unsupported before runtime effects.
Queries may use either entry; immutable policy prevents replay ambiguity. Reuse
the existing last-request exact replay cache, DTO ownership and bound checks;
do not introduce a legacy cache of prepared graphs. Existing typed behavior stays.

Allowed legacy-entry operations: capabilities/state, PrepareProcessing,
ApplyProcessing, ReleasePrepared, StartProcessing, StopProcessing. SetParameters
and ExplicitPersist use existing typed entry/session when explicitly needed;
legacy wire has no implicit save. Legacy SetMerge is one strictly projected whole
candidate so all actual processing targets change atomically; FFTView fields stay
outside ProcessingDTO. No extra validator or parameter registry.

| Legacy entry operation | Required service policy |
| --- | --- |
| Prepare | Foreign token → Busy BEFORE bounds/validation/runtime. A typed token cannot belong to a legacy session. Own LEGACY pending is released before attempting a new legacy candidate, so invalid replacement clears only that legacy draft. Then reuse bounded strict domain/transactional preparation, TTL/token generation. |
| Apply | Same owner/token/identity/base-revision/TTL checks as typed; refusal never consumes foreign/stale/mismatched token. An authorized own legacy runtime Apply attempt releases pending on failure. Success consumes pending/increments the same service counters. |
| Start/Stop | Recommend Busy while ANY token exists, before intent/resource effects. This new mixed-mode exclusion protects foreign typed reservations; legacy Prepare→Apply is serialized without an intervening external command. With empty slot invoke legacy runtime hook, snapshot actual before/after intent/state. |
| Revision on legacy failure | If hook fails but shared intent/state changed, increment authoritative service revision once, graph_revision unchanged; reply accepted=false with actual new state. This is an explicit LEGACY exception, never a typed rejected mutation. Otherwise failure leaves revision unchanged. Overflow preflight occurs before effects. |
| Replay/expiry/close | Same service cache and monotonic TTL. Exact completed retry returns stored historical reply without invoking hook/clearing another token. Expiry releases own pending; close cannot release another session. |

Legacy Qt ApplyGraph performs strict projection and desktop candidate staging,
then submit_legacy Prepare/Apply with distinct internal request IDs under one
serialized command boundary. Release its own remaining token on failed/cancelled
combined command; successful Prepare alone never publishes graph/layout/resources.
Malformed JSON/projection failure before service cannot clear a foreign typed token.
There is no externally visible legacy Prepare command in Kind0..10, so old pending
clearing does not justify stealing the new typed slot. Preserve legacy wire
request IDs/correlation separately from internal IDs; never send a guessed token.

## Desktop staging, ingress, revisions and epochs

T3 must split prepare_runtime into candidate stage and accepted commit via its
ProcessingControlRuntimePort adapter. GraphProgram/delivery pools/ingress config/
affected platform resources are ancillary to the one lifecycle bundle. Stage is
associated with current service owner/token/identity; it is not another prepared
graph/token registry. Failed own replacement keeps typed bundle AND its matching
desktop ancillary stage. TTL/release/close cleanup discards that stage too.
All fallible acquisitions/admission precede Apply; final accepted publication is
no-fail swaps. Accepted processing with later async device error stays accepted.
Do not copy today's postcommit exception into typed rejected-Apply semantics.

Ingress policy recommendation without another portable API:

1. Under the same serialized owner, unchanged geometry continues processing and
   does not touch service/pending/specs/limits.
2. Changed geometry while ANY token exists returns Busy/would_block. Keep token,
   TTL, working specs/context, resources/revisions intact. Bound one owned latest
   negotiation candidate per ingress; never save borrowed FrameView. Refuse/drop
   incompatible payload explicitly; no stale numeric transit or implicit Stop.
3. After release/expiry/Apply, re-evaluate node/contract against CURRENT applied
   graph (queued candidate may be obsolete). Stage ingress config; use a normal
   internal typed session Prepare(current strict DTO)→Apply in one serialized
   batch with fresh IDs. Same GLOBAL slot and executor; no runtime_limits bypass.
4. Failed preparation restores ancillary config and releases only its internal
   token if created; other owner cannot be released. Success uses normal service
   Apply accounting: revision+1 AND graph_revision+1, even when DTO equals working
   graph, because current service counts accepted Apply, not semantic inequality.

This recommends an observable change to old ingress-only graph_revision accounting.
If exact historical ingress graph_revision values are required, coordinator must
choose a separate *legacy wire projection counter* or authorize a host-only
service reprepare-applied operation with graph_revision unchanged. Do not silently
call lifecycle behind service or redefine typed Apply increments. No new host-only
operation is proposed as mandatory until that compatibility choice is accepted.

Likewise immutable caps.runtime cannot be silently enlarged by negotiate_ingress.
Use advertised effective ceiling and refuse over-budget geometry. Preserving old
dynamic enlargement requires an explicitly approved initial effective profile
that declares the negotiated maximum; no config rewrite/default increase is
authorized here. Current 4096-default versus negotiated larger blocks is a real
compatibility/admission decision, not solved by changing runtime_limits privately.

Counter reconciliation:

* service epoch = issued control-session generation, stable across graph edits;
  service revision R is sole processing consistency counter; graph_revision G is
  accepted Apply counter. Startup strict default graph is applied through the
  service before exposing clients; its initial R/G values must be agreed in goldens.
* Canonical _graph_revision/DeliveryStamp.graph_revision publish G from accepted
  service reply, never ++ a competing processing counter. Local delivery epoch E
  fences engine queues; preserve it on typed failure, advance on accepted plan
  commit and explicit Stop/device fencing according to approved host policy.
  E is not service epoch and must not overwrite incoming producer lineage.
* Legacy Engine rev V remains compatibility accounting for old accepted nonquery
  operations (display/adapter/no-op included). Legacy expected_revision checks V
  or wildcard; bridge then supplies CURRENT R under serialization, after global
  slot admission. V never authorizes token ownership or typed CAS. Mixed typed
  processing success must invalidate legacy clients' prior V (recommended V++
  on that accepted event); avoid double increment on a legacy-driven service event.
* Old rejected legacy wire operations keep V unchanged. Service R can advance on
  legacy failure with changed shared intent; reply state is authoritative. Two
  counters serve different protocols, not two working graphs/controllers.
* Legacy early analyzer/capture_requested/explicit_stop and generation fences are
  adapter state, not a second portable run intent. StopCapture should fence its
  source without issuing a user Stop+Start around a retained typed token. This
  callsite needs T3 refactoring/fault characterization, not a new portable field.

## Exact ownership delta and required evidence

Application owner, new explicit coordinator assignment: processing_control.h/.cpp
(legacy session policy/admission/replay/entry/counter exception); corresponding
processing_control_tests.cpp. GraphLifecycleControlPort two delegations in those
same files. Existing graph_lifecycle.h/.cpp remain unchanged unless actual tests
prove another gap. No runtime/DSP/signal change is required by this proposal.

T3: existing audio_pipeline.h/.cpp, engine_client.h/.cpp, engine_port/models and
new legacy_control_bridge/control_session files/tests from its assigned zone;
resource reconcile remains its NetworkRuntime ownership. P3 strict projection/
display preservation APIs are called, not rewritten. I alone handles registrations,
composition dependency/export/build and independent review integration.

Proposed tests, NOT RUN here:

* HEAD literal Command5/Reply6/Snapshot7 and Kind0..10/wildcard/read-only goldens;
  execute accepted no-op/display increments V, rejected exception does not.
* Actual bridge fault injection at prepare/apply/Analyzer construction/start/
  source restart after commit/capture factory phases. Distinguish core lifecycle
  Status, synchronous exception and asynchronous ComponentStatus. Preserve
  accepted existing graph_preparation/lifecycle expectations.
* Actual same service: typed A Prepare→legacy B valid/invalid/oversized Prepare
  Busy, legacy Start/Stop Busy, no runtime calls, A token/TTL/apply still valid.
  Wrong entry with typed-session ID cannot clear own typed pending.
* Own legacy invalid replacement/failed authorized Apply clears only own slot;
  typed invalid replacement/failed Apply retains token. Legacy failed Start/Stop
  intent followed by Apply, revision R exception and unchanged legacy V; typed
  failed Start/Stop all facts/counters/resources unchanged. Exact replay once.
* Ingress unchanged while token retained proceeds; changed contract Busy doesn't
  call ingress_specs/runtime_limits. After release/TTL rebind uses current graph,
  accepted counters/fences once; failed internal prepare preserves working state.
  Deferred node removal/type/device change and bounds refuse stale candidate.
* Typed resource-stage failure rolls back without publication; failed replacement
  ancillary stage remains paired with retained token. TTL/close releases resources;
  unchanged handles/producer lineage survive accepted reconcile.

Expected initial RED includes absent legacy entry/hooks and actual same-service
tests demonstrating pending loss through negotiate_ingress; compile-only missing
API is not behavioral acceptance. No hypothetical full/native test replaces these.

## Decisions still requiring coordinator acceptance

1. Explicit legacy session/entry and two runtime hooks; Busy for Start/Stop while
   any pending token; R increment on legacy failed-but-mutating hook.
2. Which historically ignored/swallowed desktop failures are retained in legacy
   reply projection (especially SetMerge and postcommit source restart). Source
   evidence alone cannot supply executed goldens; typed must remain atomic.
3. Startup and ingress-only G compatibility: ordinary service Apply accounting
   recommended; if old numeric wire values mandatory, name the projection or
   explicit internal-operation policy before implementation. Do not guess offsets.
4. Effective ingress ceiling compatible with immutable advertised caps and old
   negotiated enlargement. No silent default/limit change.

All other T3 wire ID/codec/resource/UART gates remain separate. This single
consolidated proposal removes ambiguity about required portable versus desktop
work; it does not accept Gate3 or authorize any implementation. IDLE after report.


## Approved application delta � implementation freeze, 2026-10-07

Coordinator approved only application/include/application/processing_control.h,
application/src/processing_control.cpp, tests/processing_control_tests.cpp and
this document. No GraphLifecycle/domain/runtime/numeric/desktop/CMake edits in
this delta. Prior domain initializer exception remains separately frozen.
Pinned DDD and AGENTS were reread before changes; implementation/review handoff
must still carry the pinned link and full-reading requirement above.

Public handshake:

```cpp
enum class ControlSessionPolicy { Transactional, LegacyCompatibility };
OpenControlSessionResult open_legacy_session(std::uint64_t client) noexcept;
ProcessingReply submit_legacy(SessionIdentity, CommandHeader, const ProcessingCommand&);
virtual Status ProcessingControlRuntimePort::start_legacy();
virtual Status ProcessingControlRuntimePort::stop_legacy();
```

Existing open_session/submit remain transactional by default. Policy is selected
once by trusted composition during admission; there is no policy mutation API.
Both modes share the existing caller-owned session span, max_sessions, exact
last-completed replay payload, global token/owner/TTL, working graph and R/G.
Wrong-entry mutations and legacy SetParameters/Persist return unsupported before
replay or runtime effects. Queries work through either entry. Adapters implementing
only the old abstract typed interface remain source compatible: default legacy
hooks explicitly return Status::unsupported_version (the existing status enum has
no generic unsupported member), mapped to ControlError::unsupported only for
legacy Start/Stop. No typed/historical fallback. GraphLifecycleControlPort overrides
both hooks to call existing lifecycle.start()/stop(), with no new executor or API.
Adding virtual methods requires rebuilding adapters; no binary ABI claim is made.

Legacy Start/Stop refuses ANY pending token before hook effects. With no token,
it invokes the historical hook even for an already matching intent/state: a
legacy failure remains observable. Revision increments once if actual shared
state/intent changed, including failed legacy hooks; graph revision is unchanged.
Rejected typed hooks retain their existing atomic contract and accounting. The
common revision overflow guard runs before either hook. Exact retry returns the
owned historical reply without invoking hooks or stealing a later foreign token.

Own legacy replacement discards only its own pending bundle before candidate
bounds/domain/runtime admission. Stale expected revision and token/TTL arithmetic
overflow are refused before that discard. A foreign token returns Busy before
candidate validation, including oversized/invalid candidates. Authorized own
legacy Apply runtime failure releases pending; token/owner/identity/base-revision
refusals retain it. Typed failure retention is unchanged. Successful Apply keeps
ordinary R+1/G+1 accounting. Close/expiry retain the existing ownership rules.
Bounded replies use the same exact replay cache; malformed/oversized requests
retain the existing early-return/no-cache behavior and allocate no oversized DTO
snapshot. No new replay cache or prepared storage was introduced.

TDD/evidence (own build-refactor-runtime-slice2 only):

* Interface scaffolding compiled, then the new behavioral test failed on legacy
  Stop while a typed token existed: build exit 0/run exit 3, assertion at the Busy
  oracle. Evidence: legacy-control-red-build.log, legacy-control-red.log. This is
  behavior RED, not a missing-interface compile failure.
* Minimum implementation and extended behavioral cases: strict direct C++20,
  -fno-exceptions -fno-rtti -UNDEBUG -Wall -Wextra compile exit 0 (no diagnostics),
  executable exit 0. Evidence: legacy-control-green-build.log,
  legacy-control-green.log. All pre-existing processing_control tests are kept.
* Target build processing_control_tests/application_lifecycle_tests exit 0; exact
  CTest regex `^(processing_control_tests|application_lifecycle_tests)$` passed
  2/2 in 0.06s. Evidence: legacy-control-target-build.log, legacy-control-ctest.log.
* New coverage: typed/legacy foreign Busy, invalid/oversized own replacement,
  stale/mismatched/foreign Apply refusal, failed own Apply consumption and replay
  with a later foreign bundle, entry-policy guards before replay, unsupported
  default hooks without typed fallback, shared admission limit, own/unrelated
  close, expiry/reconnect, TTL overflow, failed Stop/Start intent/R/G and exact
  retry/no-change failure, next Apply intent, and actual GraphLifecycle integration
  paired with the existing transactional/lifecycle regressions.
* R-counter exhaustion has the same common preflight as existing typed mutations;
  no artificial test setter/private-counter escape was added. TTL arithmetic
  exhaustion is exercised. Source inspection confirms the common guard precedes
  hooks. Desktop faults and physical resources remain T3 acceptance work.

Exact strict compile command (PowerShell, task-local compiler PATH required):

```powershell
$env:PATH='C:/Qt/Tools/mingw1310_64/bin;'+$env:PATH
& 'C:/Qt/Tools/mingw1310_64/bin/g++.exe' -std=c++20 -fno-exceptions -fno-rtti -UNDEBUG -Wall -Wextra -Iapplication/include -Idomain/include -Ilibs/signal_contracts/include tests/processing_control_tests.cpp application/src/processing_control.cpp application/src/graph_lifecycle.cpp build-refactor-runtime-slice2/lib/libgraph_domain.a build-refactor-runtime-slice2/lib/libsignal_contracts.a -static -o build-refactor-runtime-slice2/legacy-control-tests.exe
& ./build-refactor-runtime-slice2/legacy-control-tests.exe
& 'C:/Qt/Tools/CMake_64/bin/cmake.exe' --build build-refactor-runtime-slice2 --target processing_control_tests application_lifecycle_tests
& 'C:/Qt/Tools/CMake_64/bin/ctest.exe' --test-dir build-refactor-runtime-slice2 -R '^(processing_control_tests|application_lifecycle_tests)$' --output-on-failure
```

Memory/replay limits: MinGW x64 sizeof(ControlSessionSlot) 696 -> 704 (+8),
sizeof(ControlSessionPolicy)=4; sizeof(service)=416 and sizeof(reply)=280 unchanged
by the added methods. Measured old-slot replica omits only the new policy member;
artifact legacy-control-size.cpp/legacy-control-size.log. Existing budget formula
already includes sizeof(ControlSessionSlot), therefore admission accounts for +8
per slot (+16 for the two-slot fixture, +64 for default eight slots) without new
limits/defaults. Existing replay/scratch budgets, graph/string/caps ceilings and
DTO snapshots are unchanged; no per-call allocation or second graph is added by
the policy. Source hashes are in legacy-control-source-hashes.txt.

Numerical-foundation steering: FrameType, primitive type/shape/semantic metadata,
PCM24 packing, conversion policies, graph datatype validation, parameters and
working-frame memory contracts are all untouched. Existing cap/DTO/budget checks
are reused as-is; this legacy delta does not certify numeric-model completeness.

Status: **FROZEN / ready for NEW independent application review**. T3 may use this
API handshake after coordinator integration; I owns application/full-app build and
registration. No desktop behavior preservation is inferred from these portable
tests. Remaining desktop phase-B failure characterizations, ingress integration,
legacy V projection and numeric plan are outside this freeze.
