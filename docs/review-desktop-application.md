# Independent primary review: desktop application/runtime migration

Status: **READY for this desktop application/runtime primary review scope. All reproduced findings are closed against the recorded repaired source. Separate Qt primary/final acceptance and hardware/native rendering remain outside this verdict.**

This is a primary review, not one of the three final acceptance reviews. Preparation date: 2026-10-06 (Europe/Moscow). Observed HEAD: `0387743d734b34f6c8bd86e0f84032dc3e2206e6`; requested comparison baseline: `3de4de8`. The working tree contains substantial tracked and untracked migration changes; HEAD alone does not identify the reviewed source snapshot.

## Authority and independence

Read AGENTS.md, docs/architecture.md, docs/architecture-map.md, docs/network-stream-v2.md, docs/refactoring-backlog.md, and the entire docs/clean-architecture-ddd-reference.md. The local reference identifies ArthurKoba/ai-agent-workflow commit `9bc1b1ff27f7204fa946253a4d5612fd80102e8f`. Its patterns are advisory; project contracts determine acceptance. No author conversations, author reports, or other chat conclusions were read. Existing test registrations were inspected only to plan targeted checks; passing author checks are not acceptance evidence.

The only authorized repository write is this report. No implementation edits, subagents, branches, commits, physical capture, playback, native GUI, Linux tests, soak tests, or shared build/Ninja operations.

## Checklist after freeze

| Contract | Independent evidence to obtain |
| --- | --- |
| One executor | Trace GUI local engine, CLI, distributed ingress and GraphProgram compatibility calls to GraphExecutor/PreparedGraph. Check that GraphEvaluationContext is not a second live scheduler. |
| Layer boundaries | Inspect actual target dependencies and headers: portable contracts, kernels, runtime, protocol, domain/application and embedded support have no Qt, miniaudio, OS sockets or ESP-IDF dependencies. Confirm canonical desktop sources and compatibility headers use the same implementations. |
| Prepared plan and mutable context | Establish PreparedGraph/RuntimeContext ownership, preparation limits and workspaces, execution bundle owner validation and frame-boundary activation. |
| Processing budgets | Examine and probe steady-state process allocation, container growth, history ring updates, JSON work and socket calls. Distinguish adapter snapshot materialization from prepared processing. |
| Transactional Apply | Inject invalid preparation and resource rejection while a graph runs. Verify unchanged working graph, latest state and history; compatible changes preserve history accumulated after preparation; incompatible changes affect only the relevant branch. |
| Prepared identity and limits | Verify prepared bundle ownership, atomic Apply and resource limits on the final API. Unused unpublished clone/restore APIs were removed after rollback repair; they are outside final acceptance scope. |
| Break, gate/bypass, Stop, unavailable | Exercise known geometry/clock silence continuity, unknown-contract unavailability, independent branches, reconnect, explicit Stop inhibition and source/device changes. Follow the latest continuity rules in architecture.md. |
| Delivery fencing | Toggle subscription flags with queued legacy/typed/history deliveries; Stop/restart, Apply, epoch change and unavailable markers cannot revive stale output. Check cancellation at consumer delivery, not only production. |
| Actual capture ingress | Use injected fake/synthetic IAudioSource through AudioPipeline. Check PCM channel/layout validation, sample-rate and block/source geometry transitions, discontinuity metadata, lifecycle reprepare and independent branch preservation. |
| Factory seam | Trace CoreApplication -> GuiPresenter -> EngineClient -> AudioPipeline factory injection. Verify its actual use by synthetic fixtures and unchanged production default construction. |
| Owned asynchronous data | Trace each FrameView handoff and retained network/GUI queue. Borrowed spans expire before subsequent process; retained snapshots own bounded storage. |
| TCP/UART and configuration | Verify common BDSP byte framing and typed payloads, bounds, explicit serial endpoint binding and runtime-only endpoint override. Unknown user JSON fields and explicit settings survive save/resolve. |
| Presenter/composition | Check commands/ACK fencing, local ownership/shutdown, remote behavior, runtime-only config copy, CLI common configuration and graph selection. |

## Planned isolated validation

Use a separate `review-desktop-application` build directory. Qt: `C:/Qt/6.10.2/mingw_64`; compiler: `C:/Qt/Tools/mingw1310_64/bin/g++.exe`. Reuse pinned dependency sources from `cmake-build-release/_deps`, without building that directory. Freeze notification must precede configuration/build and final source assessment.

Build the desktop application and only the targeted test executables needed for the inspected changes. Candidate CTest cases, subject to fixture safety inspection: `application_lifecycle_tests`, `dataflow_runtime_tests`, `graph_preparation_transaction`, `desktop_history_demand`, `graph_disconnect_continuity`, `default_runtime_start`, `typed_graph_runtime`, `engine_typed_views`, `desktop_byte_transport`, `desktop_packet_socket`, `desktop_serial_stream`, `gui_serial_runtime_options`, `gui_presenter`, `console_config_selection`, and `distributed_three_cores`. GUI integration is limited to explicitly requested fake/synthetic/offscreen paths. Run granular cases rather than a complete profile. All commands, exit results and limitations will be recorded after execution.

## Results and blockers

Production source freeze notification received; subsequent notifications described source/header and CMake relocations. Tests/fixtures remained under author changes. Initial isolated configuration succeeded, then the isolated tree was reconfigured after canonical CMake relocation. Both desktop application builds and the targeted test build succeeded; no shared build was used.

### P2 — local subscription changes do not fence delivered frames

`adapters/desktop/runtime/src/engine_client.cpp:539` (local view consumer; analogous audio consumer at line 546). The local mailbox consumer checks engine epoch/revision but never checks `d->subscriptions`. Subscribe updates the mask immediately and asynchronously passes it to AudioPipeline, but neither queued views nor later views are suppressed. The remote consumer does check Views/TypedViews/Audio, so the same command has different local and remote behavior.

Independent `review-desktop-application/subscription_probe.cpp` uses a local EngineClient with an injected synthetic factory, a NetworkInput(Mel) -> MelView graph and manual owned TypedFrame ingress. It queues a frame, submits Subscribe(0), waits for accepted ACK, then supplies another frame. Expected: neither queued nor post-ACK view delivery. Actual: `Subscribe(0) ack=1 queued_view_deliveries=1 post_ack_view_deliveries=1`; probe exits 1. No audio device or GUI is opened. Delivery must check the current mask, and flag changes must also fence optional fields retained by prior publications.

### P2 — desktop source reconfiguration restores an obsolete compatible state

`adapters/desktop/runtime/src/audio_pipeline.cpp:573` restores the snapshot cloned at line 537 after releasing `_evaluation_mutex` at line 542. While source Stop/restart executes on the owner thread, a valid independent network ingress can still process against the new graph. Restore then discards those newly accepted frames and reinstates the earlier history/sequence/view state. Apply's accepted working revision remains current, masking the rollback.

Independent `review-desktop-application/apply_state_probe.cpp` injects a fake IAudioSource whose Stop blocks on an atomic gate. A mixed graph has an independent NetworkInput(Mel) -> Spectrogram(8) -> SpectrogramView branch and a local TestSignal. Seed two remote frames, change only local source amplitude in another thread, wait until physical Stop is blocked, and feed remote sequence 3. It is accepted and visible in the working history. Release Stop and let Apply finish. Expected: all three independent frames retained. Actual: `Apply accepted=1 during_history_rows=3 after_history_rows=2 latest=1`; probe exits 1. No physical capture, native GUI or speaker output. Compatible state must be retained from the latest serialized working context rather than restoring a snapshot predating an unlocked interval.

### P2 — unchanged desktop ingress allocates negotiation state on every tick

`adapters/desktop/runtime/src/audio_pipeline.cpp:29` unconditionally copies `_ingress_specs` before checking whether the existing contract matches. The steady-state path consequently allocates a vector even when every buffer and contract is already prepared. The copy should be deferred until actual renegotiation; the repeated validation/search can inspect the existing specs directly.

Independent `review-desktop-application/allocation_probe.cpp` prepares NetworkInput(Mel) -> MelView, supplies three warmup frames, disables view subscriptions, and guards C++ operator new on a fourth unchanged frame. No user callback or typed subscriber is installed. Actual: `Steady-state feed_typed C++ allocations=13 bytes=2172 with no callbacks/subscriptions`; exit 1 against the requested zero-allocation desktop processing path. This number measures the complete adapter call and includes owned input/output snapshot copies; it is **not** attributed entirely to the spec vector. The source independently identifies the unconditional vector copy. `evaluate_view` also constructs owned snapshots/maps in the executor callback (line 772), so the desktop path is not allocation-free merely because bare PreparedGraph::process is.

Scope distinction: libs/dataflow_runtime/README.md explicitly excludes owned desktop compatibility snapshots from its allocation guarantee. The probe does not disprove that narrower portable guarantee. It does show the requested actual desktop hot path is broader than the existing guarantee, and the unnecessary preparation-state copy remains even if snapshot ownership is accepted as an adapter exception. This finding does not claim proportional full-history copying: that requires separate demand/materialization checks.

### P2 — processing equality stopped normalizing omitted defaults

`adapters/desktop/runtime/src/graph.cpp:160` stores sparse JSON parameters directly; `same_processing_as` compares those raw objects at line 214. Baseline `3de4de8` populated missing registered defaults during compilation. AudioNodeModel still loads and saves defaults explicitly. Consequently a graph loaded into the editor differs from its own working graph solely through default representation. This affects matching-editor Settings patches in `presentation/qt/src/presenter/gui_presenter.cpp:440` and dirty/apply status decisions, beyond a fixture-only discrepancy.

Independent `review-desktop-application/defaults_probe.cpp` compiles GraphProgram::defaults() and a copy with each node's missing registered defaults added, exactly as AudioNodeModel does. Actual: `Sparse/explicit defaults domain_equal=1 same_processing_as=0`; exit 1. The two portable domain graphs are equal, confirming identical processing semantics. Preserve original JSON/unknown fields, but compare normalized processing parameters. CTest independently failed both `silent_gui_pipeline` (`spectra=20`, both graphs valid, synthetic backend correct, `same=0`) and `gui_presenter` (`Coalescing did not update matching editor or replaced saved draft`). The latter's fixture node 2 remains SpectrumMerge in the canonical defaults; this is not a node-ID migration error.

The standalone probes are temporary review build artifacts, not committed test or production edits. They were compiled with GNU 13.1, C++23, canonical public include directories and the independently built audio_pipeline/runtime/Qt libraries. `review-desktop-application/compile-probes.ps1` contains the exact compile/link commands and runs all four probes; all compilations succeeded and all four probes exited 1. Re-run only after an applicable source repair.

## Executed commands and results

From the repository root (PowerShell), configure:

```powershell
$env:PATH='C:/Qt/Tools/mingw1310_64/bin;C:/Qt/Tools/Ninja;'+$env:PATH
& C:/Qt/Tools/CMake_64/bin/cmake.exe -S . -B review-desktop-application -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/mingw_64 -DFETCHCONTENT_SOURCE_DIR_BDSP=C:/Users/Koba/CLionProjects/qt-visualization/cmake-build-release/_deps/bdsp-src -DFETCHCONTENT_SOURCE_DIR_MINIAUDIO=C:/Users/Koba/CLionProjects/qt-visualization/cmake-build-release/_deps/miniaudio-src -DFETCHCONTENT_SOURCE_DIR_QTNODES=C:/Users/Koba/CLionProjects/qt-visualization/cmake-build-release/_deps/qtnodes-src -DBUILD_LEGACY_APP=OFF -DENABLE_NATIVE_QA=OFF -DENABLE_DEVICE_QA=OFF
```

Exit 0. After CMake relocation, `cmake.exe -S . -B review-desktop-application` also exited 0. Build command (run once before and once after relocation, both exit 0):

```powershell
& C:/Qt/Tools/CMake_64/bin/cmake.exe --build review-desktop-application --parallel 3 --target core_app pipeline_cli application_lifecycle_tests dataflow_runtime_tests graph_preparation_tests desktop_history_demand_tests graph_continuity_tests default_runtime_tests typed_graph_tests engine_typed_views_tests desktop_byte_transport_tests desktop_packet_socket_tests desktop_serial_stream_tests gui_presenter_tests console_config_tests distributed_transport_tests
```

Targeted CTest command:

```powershell
$env:PATH='C:/Qt/Tools/mingw1310_64/bin;C:/Qt/6.10.2/mingw_64/bin;'+$env:PATH
& C:/Qt/Tools/CMake_64/bin/ctest.exe --test-dir review-desktop-application --output-on-failure -R '^(application_lifecycle_tests|dataflow_runtime_tests|graph_preparation_transaction|desktop_history_demand|graph_disconnect_continuity|default_runtime_start|typed_graph_runtime|engine_typed_views|desktop_byte_transport|desktop_packet_socket|desktop_serial_stream|gui_serial_runtime_options|gui_presenter|console_config_selection|distributed_three_cores|silent_gui_pipeline)$' -j 1
```

Exit 1: **13/16 passed in 20.88 seconds**. Failures: `silent_gui_pipeline`, `gui_presenter`, and `graph_preparation_transaction` (assertion at tests/graph_preparation_tests.cpp:166, compatible independent history during local source reconfiguration). PASS: application lifecycle, portable runtime allocation guards, desktop history demand, disconnected continuity, default source synthetic factory/Stop intent, typed graph, engine typed views, fake UART, byte transport, packet socket, runtime-only GUI serial options, CLI config preservation and real three-process synthetic distributed execution. Existing tests are independent executions here, not substitutes for the four separate reproducers.

Standalone reproducible command:

```powershell
& ./review-desktop-application/compile-probes.ps1
```

Each compile exits 0. The script records each probe's exit separately; its own final shell exit must not be interpreted as acceptance. Expected corrected result is exit 0 for each named probe. Probe sources and script remain in the ignored isolated build directory. No production code or repository tests were modified by this reviewer.

## Positive findings and limits

AudioPipeline actually owns GraphExecutor plus GraphLifecycle. Local EngineClient and CLI compose that AudioPipeline; distributed network/UART ingress calls its typed route. GraphEvaluationContext is a compatibility wrapper around PreparedGraph/RuntimeContext, not a separate numerical executor on AudioPipeline's live path. Portable plan/context separation, preparation limits, latest-state refresh inside GraphExecutor::apply, clone/restore bundle identity and working-state independence are present; targeted portable runtime/lifecycle executions passed. That does not waive the desktop restore defect above.

The optional source factory is passed by CoreApplication to GuiPresenter, reused when selecting a local device, forwarded through EngineClient, and used by AudioPipeline for primary and secondary sources. Production fallback remains default_audio_source_factory: TestSignal generates in memory; physical kinds construct MiniaudioAudioSource. Synthetic fixture executions exercise the seam without changing production defaults. CLI and GUI runtime serial overrides operate on configuration copies; the tested unknown fields and CLI documents survive. TCP/UART share QtPacketChannel/BDSP framing. Retained network deliveries own TypedFrame data; no borrowed asynchronous FrameView was observed in the inspected desktop path.

This is a primary review of the current working source, not final acceptance. Physical miniaudio capture, actual UART/ESP hardware, WLED devices, native graphics, Linux, install/package consumer deployment and long performance/soak behavior were not checked. Offscreen success cannot establish native OpenGL correctness. Source negotiation transitions through physical drivers remain unverified; synthetic/default, invalid resource, typed geometry and source-change cases provide only the stated software evidence.

## Reviewed working source identity

HEAD stayed `0387743d734b34f6c8bd86e0f84032dc3e2206e6`. Production source hashes remained unchanged across both build cycles and the probes (SHA-256):

| File | SHA-256 |
| --- | --- |
| adapters/desktop/runtime/src/audio_pipeline.cpp | 468ECB11E22A94D489C2A1EFE92B6E0BFDFCDABB379D5DB26AE7C0038BBEE771 |
| adapters/desktop/runtime/src/engine_client.cpp | 7E8B338304D5E964E8787FA499A0151A2C34E67217542E05D48490595C2A8B88 |
| libs/dataflow_runtime/src/runtime.cpp | 2BC024516E0C51A2A75BB9EF3F99C32569B6BF44E9BD4509BD66AAA23088C853 |
| application/src/graph_lifecycle.cpp | D054F4A71B79D09551AEBC47B9F44BA7D187724A0C18D24FB801ADC8D6A3980C |
| apps/desktop_gui/src/core_application.cpp | 042A5EF4E3E430E11E691CB1D792487D0103607786B8F3F960B64F38721A5850 |
| presentation/qt/src/presenter/gui_presenter.cpp | AAE725BECA606EDECDD26D376CDD7B25F446070AC30F8A0470C5FA9024558357 |

Test snapshot after the final build: tests/gui_presenter_tests.cpp `2A888636E7B81A323C6FC567DC5D91B9B6E037592F3BF65E67A4CCD5B44261F1`; tests/core_smoke.cpp `F93CAB800987F56440CE04527BCDDDAAE7043551F661384CCE87AC363A4CB2C9`; tests/graph_preparation_tests.cpp `E4240203AF60F5415EE9351E71F7CA8345DEF4213A999A3EEAB02DBB25CAA2FE`. Findings must be re-evaluated against repaired hashes before this primary gate can close.

## Independent repair re-review (same day)

The four original findings above describe the initial frozen source; their file/line references and RED outputs are preserved as historical evidence. Subsequent owner freeze notifications led to independent re-reading and rebuilding, not reliance on reported GREEN results.

`subscription_probe`: PASS, `Subscribe(0) ack=1 queued_view_deliveries=0 post_ack_view_deliveries=0`. The local consumers now gate their subscription bits, use per-view/audio mailbox generations, and mask disabled optional fields. Original local view finding closed for this checked scope.

`apply_state_probe`: PASS, `Apply accepted=1 during_history_rows=3 after_history_rows=3 latest=7`. AudioPipeline now preserves the live executor during scoped source reconfiguration instead of cloning/restoring an obsolete context. Original independent-history rollback finding closed for this checked interleaving.

`defaults_probe`: PASS, `Sparse/explicit defaults domain_equal=1 same_processing_as=1`, after independently inspecting the normalized-domain comparison in GraphProgram. Original default-equality finding closed for that equivalence; the matching GUI/graph rechecks also passed below.

`allocation_probe`: still RED, unchanged 13 allocations/2172 bytes. Unconditional specs copy remains at current audio_pipeline.cpp:30. Desktop owned snapshots are explicitly separated from the portable guarantee in the original finding; this is not a claim that the bare executor allocates. No acceptance waiver inferred.

Additional independent `capture_geometry_probe`: PASS. An injected fake IAudioSource delivers eight in-memory tones through its actual configured AudioHandler into AudioPipeline/Analyzer. It changes a primary PCM block from 800 samples/48000 Hz to 1600 samples/44100 Hz, verifies the new AudioView contract, preserves a seeded independent remote history, and rejects unequal channel lengths. Output: `Actual factory ingress rate/block transition=1 independent_history_preserved=1 malformed_channels_rejected=1`. No physical capture or speaker output.

### P2 — remote legacy packets retain fields disabled by the current subscription

Current `adapters/desktop/runtime/src/engine_client.cpp:659` onwards decodes a legacy ViewId packet, gates only Views/epoch/revision/generation/running, then updates cached history and forwards the complete decoded frame. It does not remove disabled History/Previews/Diagnostics fields, unlike the repaired local consumer. An old packet already sent under the previous mask can therefore repopulate data after those flags are disabled.

Independent `remote_fields_probe.cpp` uses a real local TCP/BDSP scripted peer, publishes a valid running snapshot, subscribes to Views+History+Previews+Diagnostics, sends a protocol-v1 ViewId containing all three optional fields, and changes the client to Views-only before pumping its event loop. Expected: a valid view with all three now-disabled fields absent. Actual: `Remote Views-only deliveries=1 disabled_diagnostics=1 disabled_previews=1 disabled_history=1`; exit 1. This is a buffered old subscription delivery, not a physical source or malformed protocol packet. Sanitize by the current mask before updating cache or invoking the handler; legacy/typed mode changes must also reject the no-longer-selected packet representation.

The new probe is included in compile-probes.ps1. Compile-remote-probe.ps1 isolates it. Its first two compilation attempts exposed missing BDSP and QtSerialPort include directories in the standalone harness; the harness flags were corrected, compilation succeeded, and the behavior reproduced. These harness errors are not product findings.

Repair-round observed production SHA-256: audio_pipeline.cpp `575584714950CF8E47E5BC3C5FE4F1E6E3B1E08C69644F32F26D1B8AE610405A`; engine_client.cpp `3A164FA6CBDF26E696919EAE34B811A6E5C1F4FB612927834D566DE0D88D6DF4`; graph.cpp `0D475F9CB871B2C48E88E31CF9D482B485A9F0B3CF491E81801F97DC7902D697`.

Additional repair CTest run: `-R '^(engine_transport|default_runtime_start|typed_graph_runtime|network_graph_apply_start)$' -j 1`: 3/4 PASS in 9.50 s; engine_transport failed without captured diagnostics at 3.54 s. A direct rerun with `QT_FORCE_STDERR_LOGGING=1`, `QT_LOGGING_RULES=*.debug=false` printed `Engine transport checks passed` and exited 0. Exact CTest recheck `-R '^engine_transport$' -j 1` with those logging variables passed in 9.82 s. The initial unexplained CTest failure is retained as a repeatability limitation, not promoted to a line-specific defect without a confirmed cause.

Final narrow rechecks after the normalized-domain repair and relinking core_app/core_smoke/GUI tests: `-R '^(silent_gui_pipeline|gui_presenter|graph_preparation_transaction|engine_typed_views|pipeline_graph)$' -j 1`: **5/5 PASS in 5.61 s** (0.79, 2.20, 0.07, 0.71 and 1.81 s respectively). No complete suite, native GUI or performance profile was run. The previously failed default-equivalence and rollback scenarios now pass under independent execution.

Current open findings are the preparation-state vector copy and remote legacy optional-field fencing. Original local Subscribe(0), obsolete compatible-state restore and processing-default equality findings are closed only within the recorded source/probe scope. A broader READY verdict is withheld; additional repaired production source requires a fresh targeted check, not reuse of this report's former PASS.


## Final allocation/API freeze re-review

The owner declared the final production/header freeze after the allocation repair and removal of two unused unpublished clone/restore APIs. Independent scoped searches find neither API in runtime/application/test sources. The final runtime header retains mutable context access used for serialized invalidation/snapshotting, and prepared-bundle identity checks remain exercised: foreign bundle rejection followed by acceptance by its owning executor at tests/dataflow_runtime_tests.cpp:530–536. The report checklist now describes this actual final API rather than requiring acceptance of deleted functionality. Earlier clone/restore descriptions are historical evidence for the superseded implementation.

Current negotiate_ingress compares the existing contract before copying specs (audio_pipeline.cpp:32–36). feed_typed accepts a documented synchronous const reference. process_without_delivery reuses owned per-view buffers when there is no selected delivery consumer, and latest_views creates independent requested snapshots rather than sharing the cache QMap. The idle path still copies payload values into owned bounded storage; this check establishes zero allocations on warmed unchanged geometry, not zero memory copying or zero allocations on every subscribed delivery path.

Independent isolated build: core_app, pipeline_cli, dataflow_runtime_tests, graph_preparation_tests, desktop_history_demand_tests and engine_typed_views_tests rebuilt successfully (35 build steps). No shared Ninja was used. The core_app deployment step warned that dxcompiler.dll/dxil.dll were not found; this build is not native graphics acceptance.

Recompiled own probes through review-desktop-application/compile-final-probes.ps1:

- subscription_probe: PASS, queued/post-ack delivery counts both 0 after Subscribe(0).
- apply_state_probe: PASS, concurrent independent history remains 3 rows and latest value 7 after Apply.
- allocation_probe: PASS, unchanged fourth Mel frame **0 C++ allocations / 0 bytes**. Original unconditional specs-copy finding closed for this source and idle scenario.
- defaults_probe: PASS, domain_equal=1 and same_processing_as=1.
- capture_geometry_probe: PASS, actual fake source ingress changes rate/block, independent history survives, malformed channels rejected.
- ownership_probe (review-authored): PASS for Mel and PCM. Caller data is overwritten and destroyed after feed_typed, latest_views retains its original values, and a held snapshot remains unchanged after the next input changes payload geometry (Mel 50 -> 32, PCM 80 -> 160 samples) and sample rate (48000 -> 44100). Latest snapshot reflects the new owned data and rate.
- remote_fields_probe: **RED unchanged**, Views-only still delivers disabled Diagnostics=1, Previews=1, History=1. engine_client.cpp is unchanged from the prior reproduced source. The script's final shell exit is not an aggregate acceptance result; each executable result is recorded separately.

Exact CTest recheck `-R '^(dataflow_runtime_tests|graph_preparation_transaction|desktop_history_demand|engine_typed_views)$' -j 1`: **4/4 PASS in 0.82 seconds**, with QT_FORCE_STDERR_LOGGING=1 and QT_LOGGING_RULES=*.debug=false. Independently running desktop_history_demand_tests also reports Idle Mel=0 allocations, Idle history=0, Idle History ingress=0; retained ring capacity 8 and 400 both allocate 3232 bytes in the selected subscribed delivery scenario. This confirms history materialization is independent of retained ring length for that checked route, and checks source-authored ownership assertions by execution rather than accepting author GREEN.

Final observed SHA-256:

| File | SHA-256 |
| --- | --- |
| adapters/desktop/runtime/src/audio_pipeline.cpp | 692F5A583EC287EDFCF714130AB68D7DA483791B315C0538C3A3693ECB039B72 |
| adapters/desktop/include/pipeline/audio_pipeline.h | 9EE05F4167FE93447A2764A0B95CB6225AD58803FCF64FE1B7B775B0C6841F76 |
| libs/dataflow_runtime/src/runtime.cpp | 63C99409DD865ABD83F5C14305E06CAE4D622DFABD2D4347DAB5676F514E3DF9 |
| libs/dataflow_runtime/include/dataflow_runtime/runtime.h | 8E71B04AA77BE555736A32AFAE607CDD9C1C4578B7EA1C37A9F2143661F5B9C0 |
| adapters/desktop/runtime/src/engine_client.cpp | 3A164FA6CBDF26E696919EAE34B811A6E5C1F4FB612927834D566DE0D88D6DF4 |

**Current verdict: NOT READY solely for the reproduced remote legacy optional-field fencing P2.** Allocation/API cleanup gate is closed within the recorded checks; remote client gate remains open. GuiPresenter Stop/exposure changes were linked during the required app build but receive no fresh behavioral acceptance here; their separate Qt primary review is still required. No physical hardware/native rendering acceptance is claimed.


## Final remote legacy repair re-review

The owner froze a narrowly repaired engine_client.cpp; no other production change was required for this follow-up. Independent inspection finds select_view_fields shared by local and remote legacy consumers. It clears fields using the current subscription mask before a remote packet can overwrite cache.history or reach the handler. The existing Views, epoch, graph revision, analyzer generation and running checks remain, and ID17 remains gated by Views plus TypedViews.

In the isolated review build, core_app, pipeline_cli, engine_typed_views_tests and engine_transport_tests rebuilt successfully. The original review-authored TCP/BDSP remote_fields_probe was recompiled and independently rerun: **PASS**, `Remote Views-only deliveries=1 disabled_diagnostics=0 disabled_previews=0 disabled_history=0`, exit 0. This closes the exact buffered old-mask defect previously reproduced as RED.

Targeted CTest `-R '^(engine_transport|engine_typed_views)$' -j 1` with the recorded logging environment: **2/2 PASS in 10.63 seconds** (9.91 and 0.68 seconds). The independently executed transport target additionally checks each optional field separately, base FFT/Mel preservation, a seeded history cache remaining unchanged after a disabled-history packet, flags=0 suppressing delivery, typed packet selection, and a protocol-v1 legacy fallback when TypedViews is requested. These are source-authored tests executed for this review, separately from the review-authored reproducer.

Clarification of the earlier representation-fencing recommendation: ID17 must be rejected when TypedViews is disabled; requesting TypedViews does not require rejecting every valid ID8 packet, because retained protocol-v1 compatibility supports a legacy fallback. Current scope accepts that fallback with current-mask optional-field filtering. No wire-format change was required.

Frozen repaired engine_client.cpp SHA-256 is `75FD8926BD706B529576957CA38CF6937FA7D888CEA776E41C68633CE9EED9CB`, unchanged between inspection and completed build/probe. Prior source hashes and RED results above describe older snapshots and are retained as history.

**Final verdict: READY for the recorded desktop application/runtime primary scope.** There are no open reproduced findings in this review. The allocation/owned-cache, concurrent Apply continuity, local subscriptions, default equivalence and remote optional-field defects all pass their recorded independent checks. This is not a substitute for the separate Qt review of GuiPresenter Stop/exposure changes, the three final acceptance reviews, or hardware/native renderer validation. The earlier intermittent engine_transport failure remains documented; this repaired-source run passed, and no new repeatability failure occurred in the targeted checks.
