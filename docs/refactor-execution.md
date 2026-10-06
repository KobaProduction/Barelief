# Architecture migration execution ledger

Baseline: `3de4de8`. Product and repository names remain unchanged. Local
`config.json` and `output/` are user-owned and excluded from migration commits.

Reference (verified commit, not file blob):
https://github.com/ArthurKoba/ai-agent-workflow/blob/9bc1b1ff27f7204fa946253a4d5612fd80102e8f/skills/software-engineering/clean-architecture-ddd.md

## Required final gates

1. Shared Qt-free executor used by GUI, CLI and distributed processing; remove
   the old executor after adapters migrate.
2. C++20 signal contracts, DSP kernels, dataflow runtime and BDSP protocol targets
   build without desktop or ESP-IDF dependencies. SDK install/export and external
   consumer compile; embedded profile supports no mandatory exceptions/RTTI.
3. Caller-buffer codecs preserve existing wire bytes. Transport-neutral framing
   supports fragmented input, partial output, bounded queues and TCP/UART.
4. Prepared execution has bounded memory and no allocation, JSON, socket blocking
   or complete-history copies per frame. History uses a contiguous ring.
5. Preserve numerical behavior, graph/config IDs, unknown JSON fields, typed zero
   versus unavailable, bypass versus stop, compatible rebind and history.
6. Desktop Serial adapter and ESP-IDF component/example; no SDK installation.
7. Address known resize bugs; preserve rendering, camera, theme and localization.
8. Independent architecture, runtime/protocol and desktop reviews; final short
   automated suite and desktop rebuild with runtime DLLs. Native/hardware checks
   remain explicitly unverified when no desktop/device is available.

## Shared interface decisions

Portable headers use existing semantic FrameType values in their existing order.
Canonical header: `signal_contracts/frame.h`, namespace `signal_contracts`.
FrameView is non-owning; retained/queued frames use BufferHandle from a bounded
pool. Metadata includes sequence/generation/epoch/revision/time/contract and
availability/discontinuity. Stereo channels are distinct spans; history layout
is [frame][band] with a ring descriptor, never transposed by changing labels.

GraphDefinition is independent of Qt/JSON and contains stable node IDs, kinds,
typed parameters and edges. Preparation validates semantic ports, topological
order and RuntimeLimits and computes MemoryRequirements. PreparedGraph owns the
immutable plan; RuntimeContext owns state and buffers, allocated in preparation.
Processing returns explicit status and output views valid until the next tick.
GUI/network owned snapshots are created outside processing. Error statuses do
not require exceptions. Existing desktop owned APIs may bridge temporarily.

Codec encode/decode receive spans and return status plus required/written size.
Existing allocating encode/decode wrappers remain wire-compatible temporarily.
BDSP framing is reused, not replaced. Byte transports expose state, overflow and
partial read/write counts. They do not perform DSP.

## Ownership and sequence

Coordinator owns root/CMake integration, tests/CMakeLists.txt, composition roots,
this ledger, SDK packaging and local commits. Only coordinator runs common
build directories. Workers are not alone: never revert others' edits.

Slice 1 parallel ownership:

- Contracts/protocol: libs/signal_contracts, libs/bdsp_protocol,
  src/pipeline/include/pipeline/typed_frame.h and stream_codec.h,
  src/pipeline/src/stream_codec.cpp, new contract/codec tests.
- DSP/memory: libs/dsp_kernels, src/dsp_core (migration forwarding header permitted),
  new DSP resource/backend tests. Do not edit contracts or common CMake.
- Domain/application: domain, application, new domain/application tests. No Qt,
  JSON, networking or DSP execution implementation here.

Slice 2: executor owns libs/dataflow_runtime and graph implementation migration;
desktop adapters own adapters/desktop and platform implementation extraction.
Slice 3: presentation/Qt and resize fixes; ESP-IDF SDK/example; app integration.
At most three working chats simultaneously. Before initial acceptance of EACH
implemented slice, create a fresh independent reviewer chat. Do not forward
implementation conversations or author conclusions. Reviewer reloads project
rules, pinned architecture reference, requirements and current code independently;
inspects actual diff and evidence, reports strict blocking/nonblocking findings.
Review chats are read-only; findings return to owners for repair and re-review.
Final three-specialty acceptance remains an additional gate, not a replacement
for these primary slice reviews. Coordinator accepts only after findings close.

Each slice report lists requirements, actual diff, commands/results, contract
changes, risks and status. Reports are evidence to inspect, not acceptance.
TDD: new behavior red -> green -> refactor; old behavior characterization before
migration. File moves do not need artificial structure tests.

## Baseline dependency map

portable_dsp is Qt-free; portable_stream_codec is Qt-free but owns vectors and
throws on encode. GraphProgram and evaluation context currently depend on Qt
and Analyzer; AudioPipeline combines capture, runtime, networking and WLED.
GUI/CLI share that desktop pipeline. Core is actually Qt/BDSP infrastructure.
BUILD_DSP_ONLY currently excludes runtime/codec. These are migration targets,
not acceptable final portable SDK boundaries.

## Current status

Foundation contracts and ownership published. Baseline portable_dsp,
portable_dsp_legacy, portable_dsp_backend and pipeline_graph passed (2.49 s).
Domain headers agreed: domain/graph_definition.h, domain/graph_validation.h,
application/graph_lifecycle.h. DSP target/header: dsp_kernels,
dsp_kernels/dsp.h; portable_dsp and dsp_core/dsp.h are temporary compatibility
aliases. Typed stream transport, typed graph runtime, three distributed cores,
config resolver and disconnect continuity baseline also passed (29.14 s).
Canonical kinds mirror existing definitions;
SurfaceView alias remains document-adapter responsibility. No slice
accepted yet. Tokens issue remains undefined in backlog. No ESP-IDF installation
permitted; SDK presence and cross-build evidence must be recorded separately.

Coordinator integration evidence (foundation; not full completion): desktop GUI,
CLI and both registry binaries rebuilt successfully, Qt/MinGW runtime deployed.
BUILD_PORTABLE_SDK configured and built without Qt, miniaudio or OS network
libraries; eleven portable tests passed. Eight new desktop foundation tests
passed. Installed SDK external consumer configured, linked and ran successfully
using only installed package targets and no Qt. Runtime is still the old adapter
implementation until slice 2 migrates it; current SDK is foundation only.

Fresh primary reviewer: 01a10eba-3bae-79c2-ae45-42b9b0b390ac (read-only).
Executor worker: 01a10ebd-0485-7531-9a6d-9ee062fa7433 (runtime + graph bridge).
No implementation slice accepted/committed before independent review closes.

Slice 1 accepted after fresh primary review and narrow independent re-review of
newest_override. Reviewer reproduced and closed two P1s (malformed descriptor
crash; missing BDSP installation) and a P2 (Network type alias). Current portable
verification script rebuilt all portable foundation targets, ran 11/11 tests,
installed dependencies/public package and built/ran the installed-only consumer.
No Qt6/miniaudio/ws2_32 dependencies appear in the portable build graph.
Acceptance excludes runtime integration, physical/native graphics and ESP SDK.

Desktop platform worker: 01a10ec1-f2f8-7f02-bcef-6b34971f9edd. Qt SerialPort is
present; canonical platform implementation moves and shared TCP/UART framing are
in progress. Framework includes and stable namespaces stay adapter-facing.

Primary acceptance of every implementation slice belongs to a new independent
Codex chat. It loads AGENTS.md, the architecture reference and current sources
from scratch, without writer conversations or conclusions. Findings return to
the owner, and changed portions require independent re-review before the
coordinator accepts the slice. Final reviews are additional to this gate.

Slice 2 integration in progress: common runtime target and public SDK export
registered; desktop platform sources relocated; Qt presentation source manifest
integrated. Focused offscreen resize cases cover selection, narrow controls and
single-area/footer geometry. Desktop TCP/UART adapter and legacy wire tests are
registered separately. PacketStream exact legacy checksum diagnostics followed
red-to-green verification; the portable packet test passes. Neither slice 2 nor
Qt resize changes have received independent acceptance yet.

Fresh desktop/platform primary reviewer: 01a10ecf-74b6-7820-8cf4-aedeff9b3a7c.
Read-only independent scope is platform extraction, shared TCP/UART framing and
fragments, lifetime/resource/error compatibility and targeted tests. Runtime and
Qt remain unfinished and excluded from this acceptance gate.

Portable integrated check now passes 12/12 short tests including runtime. SDK
installed-only consumer links dataflow_runtime and executes window -> FFT -> Mel
with no Qt and exceptions/RTTI disabled. Initial consumer failures exposed a
missing explicit stream ID in its draft and insufficient samples for the
preserved 800-sample cadence at 48 kHz; fixture corrected without changing
production cadence. This proves package integration, not desktop hot-path or
full runtime acceptance.

Desktop coordinator integration rebuilt the moved platform targets and ran
aggregator_network, typed_stream_transport, desktop_byte_transport and
desktop_packet_socket: 4/4 passed (18.03 s). Primary independent review remains
open; these checks do not accept hardware or complete Serial application wiring.

GUI composition root now lives in apps/desktop_gui/src/core_application.cpp,
with desktop_gui_composition as a separate target. Qt presentation's
core_app_components no longer contains QApplication composition or file I/O.
ConfigManager is a separate desktop_gui_configuration adapter under
adapters/desktop/gui_config. Stable class/include names have temporary forwarding
headers. GUI configuration DTOs still share a presentation header and require
separation before final cleanup; framework dependencies remain outside portable
targets. Coordinator owns this integration and its follow-up checks.

Desktop primary review closed a P1: PacketStream kept parsing a coalesced read
after callback-triggered abort/reset. Root added a reset-generation fence and
portable red-to-green regression; independent localhost probes verified no stale
Ack/Data or packet 2 after cancellation and normal fresh reconnect. The narrow
desktop/platform scope has no open P1/P2. Full runtime/engine remains outside
that acceptance; concurrent review builds did not stabilize its changing ABI.

Fresh Qt primary reviewer: 01a10ed7-dd3a-76a3-855d-50e57abe6e7b. It independently
loads rules and checks sources and focused offscreen resize/navigation cases.

Desktop history demand regression: a processing tick allocated 14,796 bytes
with 8 retained rows and 571,212 with 400 rows (RED). Borrowed-ring integration
now passes the same processing-cost test plus continuity and typed runtime.
An engine disconnect-clear regression remains under coordinator repair; no
broader runtime or final acceptance is claimed.

Runtime primary reviewer: 01a10eda-e05d-7ae3-aea6-1a39646be6e2. Runtime/API and
GraphProgram scope is independent from ongoing AudioPipeline integration.
After synchronized rebuild, engine_transport and silent_test_signal pass;
engine_typed_views still reproduces missing immediate disconnected view marker.
Qt reviewer found two damaged Unicode literals during extraction; the original
owner is assigned the precise repair before independent re-review.

Owner delivery for the two literal repairs failed transiently twice. Coordinator
applied only the exact close symbol and status separator with Unicode escapes;
fresh Qt reviewer is asked to recheck the resulting text independently.
History processing guard after migration measures 3,108 bytes at both 8 and 400
retained rows; numerical/continuity tests remain separate requirements.

Disconnected flat-view notification now passes engine_typed_views together with
history continuity (2/2, 0.82 s). Root expands the demand regression to a selected
Mel output subscription, so unrelated history is not copied just because a
network sink exists. NetworkRuntime subscribes only to its actual output nodes.
This application integration awaits targeted checks and independent acceptance.

Fresh runtime reviewer independently reproduced four blocking edge cases:
producer_epoch mismatch in marker_current, invalid source ingress types,
unavailable processing without clearing state, and non-advancing retained history
after producer deletion. They returned to the original owner for TDD repair and
independent re-review. Portable green tests do not close these findings.
ESP-IDF recheck: IDF_PATH absent, C:/Espressif absent, idf.py unavailable; do not
install SDK or claim ESP cross-build/hardware acceptance.

Filtered subscription demand test followed missing-API RED to GREEN: only
requested Mel node is delivered, history processing allocation is independent
of ring length. Integrated typed runtime, engine views, continuity and demand
tests pass 4/4 (6.32 s). Existing unrestricted subscriptions remain compatible.
Fresh Qt reviewer closed encoding findings with actual QToolButton/QLabel text;
it found another narrow title-width P2 in an untested node. Coordinator added
shrinking title policy and expanded narrow fixture to every default node in both
themes. Independent re-review and root focused Qt run are pending.

The expanded narrow case reproduced a zero-width history_size field outside its
form. Shared ParameterForm now derives wrapping from label/field minimum hints,
using full rows at narrow widths and compact rows when space permits. Root's
three focused resize cases pass 3/3 (2.19 s), including all default nodes in both
themes. Fresh Qt reviewer also confirmed presentation no longer includes/links
the concrete ConfigManager adapter; file I/O is composed only in desktop root.

Fresh runtime re-review closed four original findings and follow-on resume/window
compatibility defects using its own probes and freshly compiled characterizations.
Large-block aggregate ingress limits and failed rebind transaction remain open.
Root Apply now prepares a candidate context before publishing graph/epoch/revision;
it rejects status failures without replacing the working desktop graph.

Qt primary acceptance is closed independently in its declared scope: five
focused cases pass and actual Unicode text, presentation-only header consumer,
all-node/light-dark/live-preview probe pass. Full native graphics remains excluded.
Embedded SDK worker: adapters/esp_idf, examples/esp32_streamer, optional portable
embedded_support helper and focused tests/docs only. No installs or common Ninja;
ESP cross-build will be explicitly unverified without the SDK.

Runtime primary acceptance is now independently READY for the current Apply-time
state refresh implementation: all original findings, follow-ons and pending
Prepare/tick/Stop/Apply lifecycle probes are closed. Latest state survives pending
Apply, stopped history stays cleared, old sequences remain fenced, and compatible
capacity/topology changes preserve the newest rows. Review excludes desktop
AudioPipeline/EngineClient integration and final numerical/profile matrix.

Desktop integration worker: 01a10eef-46ff-7990-a741-f690519daeb9 owns actual
application lifecycle composition, AudioPipeline/EngineClient integration and
explicit Serial endpoint wiring. It must first diagnose the fresh failing
 typed_graph_runtime test; no desktop acceptance is inferred from portable review.
Independent primary review will be a NEW read-only chat after this slice.
Coordinator registered embedded_support and its host contract tests in shared
CMake. ESP component/example remain in progress and cross-build unverified.

Portable shared build now passes 13/13 quick tests (1.65 s), including the
embedded host contracts. SDK installed into build-sdk/prefix and external
consumer rebuilt/runs with installed embedded_support + runtime, without Qt.
Desktop worker may extend GraphExecutor only with a serialized mutable-context
accessor and explicit working-state clone/copy: pending bundles must never be
accepted by another executor. That API delta requires fresh independent review;
previous runtime acceptance does not cover future edits.

Phase 4 cleanup started with four unused QtNodes arithmetic/demo models. The
actual GUI registry registers only AudioNodeModel definitions; reference search
found no consumers outside demo implementations and unused includes. Removed
demo presentation sources/headers, old forwarding headers, manifest entries and
editor unused includes. Composition owner removes its unused demo includes and
obsolete load special-cases; unknown saved draft fields remain preserved.

Final-test fixture audit found legacy_startup_shutdown used the physical default
capture. Compatibility Application now accepts optional source-factory injection;
legacy smoke supplies the existing synthetic 8-tone source. Production default
is unchanged. Its focused build/test remains pending until desktop source edits
are stable. Native capture must not be inferred from this synthetic smoke.

Embedded implementation is frozen for NEW independent primary reviewer
01a10ef4-2a2b-7e12-8d5a-eae45784baac. Reviewer reloads rules/reference/code
without author conversations and performs its own private probes. Coordinator
included ESP source-package install fragment in SDK composition so installed
share/PortableDSP/embedded contains usable common source topology/components.
ESP-IDF native build, optimized external backend and hardware remain unverified.

Physical capture lifecycle registration is now explicit ENABLE_DEVICE_QA=ON,
OFF by default; helper executable remains available for manual hardware checks.
Default core_startup_shutdown uses the synthetic fixture with autosave disabled,
rather than default physical source or user configuration. Final automated suite
runs all registered correctness tests with native/device QA disabled, separately
listing omitted physical checks. These fixture changes need focused revalidation.

Phase 4 desktop analysis adapter worker: 01a10ef6-5376-7830-98f2-7284a2fddd56
owns only canonical Analyzer/AudioWindow/FFT/Spectrogram headers and sources plus
dedicated source manifest/documentation. All math/signatures/locking are preserved;
current direct DSP wrappers are compatibility APIs, not a new executor. Original
cpp removal waits for coordinator CMake rewire to avoid concurrent integration
builds observing missing files. NEW independent primary review follows the move.

Coordinator extracted deploy_desktop_runtime(target) into shared CMake and
registered it in each composition root (GUI, console, registry, compatibility
visualizer). This closes the old headless-only gap where build-tree DLL deployment
ran only when GUI existed. Final executable import/DLL checks and native launch
remain separate gates; configuration is not proof of deployed runtime files.

Embedded independent primary review returned NOT READY with two actual probes:
P1 saturated example queue drops unavailable marker while retaining old data;
P2 subscriber sequence 1 -> 3 lacks discontinuity. Findings returned to SDK owner
for TDD repair. Existing host/component/install tests were green but did not cover
these cases; no SDK acceptance claimed until independent re-review closes them.

Root canonical analysis build + audio_sample_cadence passed 1/1 (0.17 s).
Fresh BUILD_GUI=OFF/BUILD_TESTING=OFF registry server/client both build and deploy
Qt6Core/Network/SerialPort plus all three MinGW runtime DLLs successfully in
build-headless-verify/bin. The initial wrong cached dependency override was
corrected against authoritative build-gui CMakeCache; no network fetch/install.
Actual desktop Serial integration uses typed Subscribe/Ack/Data IDs11-14, matching
ESP example, not engine-control IDs5-10. New desktop_serial_stream test registered
for the caller-owned fake-device channel through subscriber/shared runtime.

Analysis extraction frozen after original cpp removal; fresh independent primary
reviewer created for canonical adapter, dependency wiring, public compatibility,
numerical/cadence/queue preservation. Author comparisons are evidence to inspect,
not acceptance. Review scope excludes concurrently changing desktop lifecycle
and Serial integration. At most three active worker/review chats remain enforced.

New read-only verify-desktop-runtime.ps1 walks actual PE imports recursively,
including deployed dynamic plugins, resolving local DLLs and Windows system
contracts without relying on Qt/compiler entries in shell PATH. Fresh headless
registry artifact passes. This is deployed-file evidence, not native graphics,
capture/device, or application behavioral acceptance.

SDK independent re-review is READY in declared scope: own saturated-marker
probe now stops run and actual transport-task purge/cancel removes pending data;
own sequence-gap probe marks discontinuity and verifies continuity, explicit
flag and old-sequence rejection. Reviewer repeated private root SDK build/install,
installed component/example 3/3, and installed-only consumer exit0. Hardware,
native cross build and external optimized backend remain explicitly unverified.
New architecture-map.md describes actual responsibilities/dependency direction;
current desktop lifecycle/Serial integration still requires its own primary gate.

Root rebuilt core_app, pipeline_cli, core_smoke, legacy_smoke and Serial fixture
against actual GraphExecutor/lifecycle integration. New synthetic startup and
UART typed-stream checks pass 3/3 (1.51 s); no device/native tests launched.
verify-desktop-runtime.ps1 confirms all four desktop executables plus 33 local
PE/DLL/plugin dependencies in build-gui/bin, including Qt6DataVisualization.
This is a usable rebuilt artifact; whole migration is not accepted until fresh
application review, cleanup, final independent reviews and full short suite.

Desktop analysis independent primary acceptance is READY. Fresh reviewer built
actual manifest/target and separate HEAD baseline; own broad exact numerical and
worker/queue probes match, public layout/enums/signatures/alias consumer passes.
Current required compatibility wrappers retained, no second graph executor.
Review excludes current application/distributed/Serial integration and final gates.
New gui_serial_runtime_options test registered for runtime-only serial config
without mutation of persisted document or physical capture.

Phase4 compatibility cleanup moves the unchanged dsp_core namespace alias into
libs/dsp_kernels/include/dsp_core/dsp.h, so it is provided by the canonical target
and installed SDK. portable_dsp remains a compatibility target name linking only
dsp_kernels; it no longer adds a source-tree include bridge. Installed consumer
now includes the alias header as well; fresh SDK consumer validation is pending.

Latest root SDK verifier passes 13 portable tests (1.96 s), installed-only
consumer including canonical dsp_core compatibility header, and installed-source
component/example smoke 3/3 (0.11 s). The script now includes component smoke as
an explicit host check and states that ESP-IDF cross/hardware remain unverified.
AGENTS documents portable dependency direction, single executor/lifecycle,
prepared buffers/borrowed lifetime, TDD/characterization and independent gates.

Root latest actual network Apply auto-start case passes 1/1 (0.15 s), including
explicit Stop intent. Manual acceptance checklist now explicitly lists native
Windows surface/theme/workspace, physical capture, real ESP/UART/TCP and optimized
backend checks. These are not claimed by synthetic/offscreen/component evidence.
Desktop owner is fixing async fixture synchronization and replacing a remaining
physical-default EngineServer test source with injection before final freeze.

Fresh independent desktop application reviewer: pending assigned thread. Initial work is rules/contracts/checklist; acceptance waits for final source freeze and canonical adapter relocation. Automatic fixture audit is separate author work. Root has enabled Release test assertions across the tests directory and added a used source-factory seam at GUI composition/presenter boundaries; EngineClient and synthetic fixtures are being completed before review.

Primary application reviewer: 01a10f0a-ccfb-7c11-ae6e-f5439444ed74. Production source freeze communicated; review is independent and not final gate. Root relocated remaining desktop runtime/GraphProgram/EngineClient and owned stream wrappers to adapters/desktop/runtime and canonical public include paths. Removed verified obsolete relative forwarding headers and rewired canonical includes; public header names remain stable. Automatic fixture audit uses a synthetic factory injected through actual GUI composition, preserving selected source identity. Release assertions are enabled for all test targets; previous assertion-elided graph_preparation result is withdrawn, actual characterization exposed a sequence-counter restore regression, repaired with a real red/green check. Subscription flags are now checked before typed/legacy/audio delivery; independent acceptance is pending.

Phase4 canonical build wiring completed: CMake definitions now reside in adapters/desktop platform/runtime/analysis/configuration and presentation/qt, compatibility visualizer composition in apps/compatibility_gui. Historical target names remain; no second executor or algorithm introduced. Fresh SDK verification after cleanup succeeds: portable suite 13/13, installed external consumer, installed-source component smoke 3/3. Root GUI inventory has 63 automatic tests; ENABLE_DEVICE_QA and ENABLE_NATIVE_QA both OFF; no native/soak/device-capture test is registered. Complete desktop rebuild is running before executing these automatic tests. Integration author reports frozen source and 13/13 focused private checks; primary review remains independent/pending. Fresh fixture primary reviewer01a10f0d-ef4e-70b1-8be4-8b9922abaa2d separately checks assertions, injection and preserved intent.

Independent desktop application primary review reproduced two blockers with its own probes: local Subscribe(0) delivered queued and post-ACK views; Apply source reconfiguration restored an old snapshot and lost an independent NetworkInput history frame accepted during source Stop. Findings are in docs/review-desktop-application.md and returned to the owner for TDD repairs; application slice NOT accepted. Independent fixture review found missing first-run bootstrap coverage; root restored EXPECT_FIRST_RUN registration with synthetic injection, distinct from TestSignal smoke. Re-review and synchronized targeted tests are required.

Complete canonical desktop build succeeded; PE verifier confirms four executables and 33 local dependencies. Subsequent targeted fixture checks passed 7/11, exposing four GUI failures, so no broad GUI acceptance is claimed. Independent fixture primary accepts injection/assertions and repaired first-run coverage in its scope (docs/review-test-fixtures.md). Root TDD reproduced an effective-processing identity defect: editor explicit defaults/unknown UI fields were compared as raw JSON. GraphProgram now compares normalized domain parameters/enabled state while preserving document fields; new equivalence/disabled characterization and actual silent GUI smoke pass 2/2 (2.68 s). Three remaining UI fixture/behavior failures are assigned for source-based analysis, not waived. Manual native/device helper targets are excluded from ordinary builds when both QA options are OFF, remaining explicitly buildable. Application reviewer is independently rechecking source restart/local subscription repairs and the new identity delta.

Independent application re-review closes original Subscribe/source-history/normalized-default probe failures, but adds a real steady-state ingress allocation finding (13 C++ allocations/2172 bytes in unchanged unsubscribed Mel feed). Owner is implementing a bounded/prepared desktop cache/negotiation fix, so primary gate remains open. Root exposed a separate floating-render defect: inactive tab detached through native QDock setFloating was visible yet retained stale render exposure. Deferred topLevelChanged refresh added; per-node direct-floating/typed-zero cases now proceed, remaining remote GUI readiness failure is under targeted analysis. No final three-chat review or broad full-suite acceptance yet.

Additional GUI Stop fence: snapshot Analyzer=Stopped now clears views even when a different client/server initiates Stop, keeping explicit Stop distinct from unavailable markers that retain typed zero geometry. Root repair is based on the existing remote GUI Stop characterization; target regression/review pending. Desktop feed_typed const-reference synchronous adapter signature authorized by coordinator to remove avoidable ownership copy; caller lifetime is limited to processing and any retained cache must own copies. SDK wire/API untouched. Owner also audits new clone/restore bridges for removal after their sole AudioPipeline use disappeared.

Next integration gate: allocation owner reports zero steady-state allocations for unchanged unsubscribed Mel and runtime history, with ownership/renegotiation regressions still finishing before source/header freeze. Two new clone/restore APIs had no production users after removal of the old source-restart bridge; coordinator authorized removing only those unpublished APIs and clone-only self-tests, retaining prepared-bundle owner/transaction tests and actually used context/limit APIs. UI fixtures run only against a consistent archive+header snapshot; paused while feed_typed changes to synchronous const-reference and cache layout changes. Rebuild all affected desktop targets after freeze, then re-run exact failed UI cases and obtain fresh Qt primary review. Existing fixture primary is accepted only for injection/assertions; application and global GUI gates remain pending.

New independent Qt post-integration primary reviewer: 01a10f22-a176-7351-894a-8ca00659c1b1. Fresh rules/reference/source reading, own short offscreen probes; no author conversation/conclusions. Focus is direct floating exposure, explicit external Stop versus unavailable, hidden rendering and actual current UI fixture semantics after canonical composition migration. This primary gate is separate from the final NEW three-specialty review chats. Owner allocation report now zero/no-demand with ownership/geometry/external-History guards and 11/11 targeted checks; independent application re-review still decides acceptance.

Root final synchronized rebuild completed after const-reference/cache/API cleanup; ten relevant checks pass 9/10 in28.86s. GUI direct native floating and external server Stop cases now pass, as do first-run, silent bootstrap, workspace persistence, presenter, allocation/history guards, preparation and transport. Only settings_interactions fails on an obsolete success-message expectation: product intentionally clears successful scheme status per user request; fixture must require accepted ACK/actual applied graph and keep invalid-draft diagnostics, not invent a success banner. Fixture owner is making this exact correction and isolating WLED fixture traffic to loopback. New Qt primary independently reviews actual final source and tests; no full acceptance yet.

Independent desktop application primary review is READY after its own remote-fields TCP/BDSP reproducer passed; recorded source ownership/zero-allocation/Apply continuity checks remain accepted. New independent Qt primary found a distinct late valid TypedViews packet17 receive fence: confirmed Stopped GUI can be repopulated because packet17 lacks packet8's Running check. Source owner receives this exact regression for TDD repair; Qt primary remains NOT READY pending own rerun. This demonstrates independent acceptance is a required repair gate, not approval of author reports. Current UI/runtime fixtures isolate WLED to explicit loopback while retaining real worker/ACK/packet lifecycle checks. No final three-chat gate or completion claimed.

Root corrected runtime fixture composition after actual failures: direct view-signal consumer explicitly requests Views/TypedViews instead of relying on hidden GUI delivery; all original Stop/unavailable marker/stall assertions remain. WLED restart uses Stop then explicit loopback Start, matching the idempotent Start contract and original restart intent; five cycles retain ACK, new destination packet, old destination cancellation, double Stop and disabled-restart silence checks. Actual current runtime_regressions passes 3.80 s. Late17 transport test passes 10.09 s; independent Qt own replay remains the acceptance gate. Latest SDK verifier passes portable13/13, installed consumer and installed component3/3; a separately built C++20/no-exceptions/no-RTTI FFT512 profile passes13/13 (0.43 s). These are short host checks, not hardware/native acceptance.

Qt independent primary re-review is READY: original own late17 replay now delivers0/empty whileStopped, own Recovering unavailable marker stilldelivers; own exact loopback WLED restart block passes. Full native/hardware acceptance is excluded. Primary gates are closed for the frozen implementation; final fresh three-specialty reviews are next. Canonical relocation, runtime integration and test migration have interdependent target/header changes, so coordinator records the accepted integrated slice as one coherent local commit rather than deliberately broken intermediate snapshots. Foundation remains separately committed; subsequent final-review repairs/results receive separate commits. Final review assignment checklist is docs/final-review-brief.md.

Accepted integrated migration committed locally as2236971. User config.json/output remain untracked and untouched. Final reviewers are NEW independent chats: architecture/SDK01a10f32-f211-7673-92c8-5348ac18969e; runtime/protocol01a10f33-5752-75f0-832e-6bbeee94c280; desktop/regressions01a10f33-6365-78d0-9c48-a9c753d4d16b. Fresh rules/fullreference/currentdiff/probes; read-only source, separate build directories, no author/reviewer conversations as evidence. At most three active. Their acceptance, final full short suite, DLL check and coordinator audit remain open.

Final architecture reviewer independently reports NOT READY A1: Presenter constructs/owns concrete EngineClient and dynamic endpoint creation remains in presentation. A new repair chat01a10f39-a121-73a0-8560-d8ae51e61005 owns an abstract Qt-facing command/snapshot port and injected provider assembled in desktop GUI composition; portable application remains Qt-free. Final runtime independently reports R1 lost producer/generation fence after unavailable, R2 wrong negotiated stream_id accepted, R3 stale sequence permitted after gap/unavailable. Original private probes and baseline positive results remain separate evidence; fixes and own re-review are required. Final desktop scope remains running. No full migration acceptance claimed.

Final desktop reviewer frozen2236971 reports D1: graph_settings_theme incorrectly rejects QtNodes WidgetShortcut Copy/Cut/Paste despite requested editor clipboard shortcuts. Root added exact standard-key exception ONLY for WidgetShortcut; global unexpected Ctrl remains prohibited, Save/Open unchanged. Source test fix is pending consistent integration build and independent narrow rereview. Runtime R1/R2/R3 repair assigned to existingdesktopowner01a10eef...; newA1Qt-facing port owner01a10f39... has non-overlapping header/DTO/composition ownership. Root owns neutral ports CMake and fixture target integration; SDK early-return remains before Qt, owned codec's TypedFrame must remain Qt-free. Wait for stable headers before linking/building; no old-archive/new-header evidence. Final desktop/root full short suite not accepted yet.

A1 header/DTO/interface extraction is frozen for consistent private builds. Root wired qt_engine_ports (Qt-facing INTERFACE only) in desktop Qt branch and pure owned_frame include for codec wrappers without Qt; portable SDK verifier after extraction passes13/13 +installedconsumer +component3/3 (0.12 s). Portable application/domain/executor remain unchanged. Runtime author independently obtains RED for R1/R2 and separate R3 before production repairs. GUI fake endpoint/provider test and existing characterization builds are in progress. Common desktop build/test and independent final re-review remain pending; do not claim new-port migration accepted from the old2236971 artifacts. No live common Ninja remains.

A1 final architecture independent re-review closes the concrete-client finding with its own fake provider/endpoint consumer and include-trace probe; portable extraction checks pass. Root common desktop rebuild succeeds for that snapshot. Runtime targeted integration then reveals restart-identity and invalid UART fixture issues before acceptance: local producer_epoch repeats across publisher incarnations, and an unavailable fixture omitted its negotiated stream ID. Runtime freeze reopened only for these verified defects. Coordinator approves deterministic opaque publisher-incarnation scoping of inherited producer identity, retaining generation/timestamps/source sequence; unknown disconnect markers reuse last route identity. Existing three-core restart characterization must pass, and final runtime independent probes must review this policy, not just the earlier R1/R2/R3 cases.

Final architecture A1 and final desktop D1/port integration are independently READY within their recorded scopes. Own neutral include/fakeprovider checks and actual shared settings/theme case pass; final desktop excludes the two runtime body refinements, which remain subject to final runtime review. Root records accepted GUI boundary/test guard as a separate local commit; runtime bodies/tests are intentionally excluded until independent acceptance.

Final runtime repair author freezes desktop bodies after 5/5 targeted checks including actual three-core source restart (32.58 s). Root consistent desktop rebuild succeeds and starts all 64 short automatic tests; independent runtime reviewer recompiles its original and separate boundary probes. Independent SDK Session probe reproduces a new blocker: generation2 unavailable followed by generation1 data is delivered. The coordinator assigns a separate bounded Session fence repair to the runtime owner; no acceptance is inferred from earlier SDK green tests. Current desktop source remains frozen while its independent probes run. Headless rebuild is running; complete migration remains active until this new finding and final integrated checks close.

Root complete short desktop CTest passes **64/64, 111.18 s** against frozen desktop repair bodies, with assertions enabled and native/device QA OFF (`build-gui/final-automatic-tests.log`). GUI rebuild and headless rebuild both succeed. Deployment verification passes four GUI-profile executables/33 local PE dependencies and three headless executables/12 local dependencies, including dynamically loaded plugins. User config SHA256 remains 9EE158D2264D7D1D64F54A238D347AE492548857D7495FD6B2AC7FC315FD98BE. This is evidence for that snapshot, not closure of the independently reproduced SDK finding; SDK changes require rebuild/retest and independent runtime/portable acceptance before completion.

Independent final runtime re-review closes R1/R2/R3, but records executed RED findings R4–R7: SDK stale-generation/retired-producer callbacks, portable newer-generation marker blocking freshseq1, desktop retired-producer replay on one live connection, and direct portable same-identity replay permitted by discontinuity. All are returned to the owner with narrow source/test ownership. SDK/desktop adapters use bounded16 retired identity tuples (publisher incarnation + inherited producer); portable runtime retains its existing bounded16 producer ring. No unbounded anti-replay retention is required. Fresh generation resets its sequence baseline; discontinuity alone does not authorize old sequence. The independent architecture reviewer prepares a narrow resource/SDK re-review, while final runtime re-review waits for the complete repaired source freeze. Earlier full suite is not evidence for these moving changes.

Final frozen R4–R7 delta is independently READY in both runtime/protocol and architecture/resource scopes. Original independent RED probes now pass unchanged; bounded16 retirement, unknown markers, fresh generation/producer and discontinuous replay are checked separately. Independent Session construction/handshake/48 producer transitions/stop/begin records zero new/malloc/calloc/realloc. Root consistent final GUI/headless builds pass; final complete desktop suite **64/64, 113.17s**, no-Qt SDK **13/13, 1.38s**, actual installed consumer and installed-source component **3/3, 0.10s**, smaller FFT512/no-exceptions/no-RTTI profile **13/13, 0.32s**. Both deployment/import checks pass. Final coordinator requirement/evidence audit is [refactor-acceptance.md](refactor-acceptance.md); native/hardware/cross-build exclusions remain explicit. The accepted runtime repair and reports are recorded locally, with no publication and user config/output preserved.
