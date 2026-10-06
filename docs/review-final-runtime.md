> Latest final acceptance (2026-10-06): **READY for assigned runtime/protocol scope** on the frozen hashes in the final section. All executed R1–R7 findings closed by independent unchanged probes; 12/12 targeted tests and SDK host test passed. Earlier RED and NOT READY sections below are retained historical evidence. Hardware/native cross-build remains unproven.
Verdict: **NOT READY** at `22369719cd00c6e57fafdc166f42a842aa5f161c`, against baseline `3de4de8`. Reviewed on Windows, 2026-10-06. Three independently reproduced defects remain. Passing existing tests does not close them.

Production and shared tests were read-only. Private build and probes are in `review-final-runtime/`; this document is the only authored repository document. No author/previous reviewer reports or conversations were used as evidence. Read project AGENTS.md, architecture.md, architecture-map.md, network-stream-v2.md, final-review-brief.md and the pinned clean-architecture-ddd-reference. The reference was fetched from the exact `9bc1b1ff27f7204fa946253a4d5612fd80102e8f` raw GitHub URL and equals the local reference after removal of its snapshot comment and CRLF/outer-whitespace normalization.

## Findings

### R1 — P1: desktop unavailable marker loses generation/producer fence

Location: `adapters/desktop/runtime/src/audio_pipeline.cpp:982–985`, with `clear_ingress` at 947. `feed_typed` tests whether a marker is current, then clears the ingress rather than passing the marker through the executor. `clear_ingress` clears state/history but does not advance the producer identity or generation. Consequently the accepted marker's newer generation is not remembered. This violates network-stream-v2's requirement that old data cannot revive dependent history after an unavailable marker, and the requested current-state fencing.

Own reproducer: `review-final-runtime/independent_probe.cpp`, first scenario. Apply NetworkInput(Mel) → Spectrogram(capacity 4) → SpectrogramView. Feed epoch 100 / generation 1 / sequence 1. Feed unavailable marker with the same epoch and generation 2. Then feed generation 1 / sequence 2 through the synchronous ingress API (the network adapter queries the current ingress serial immediately before delivery, so a new serial is not a producer-generation fence).

Expected: the history stays unavailable; generation 1 is rejected. Actual:

```text
after generation-2 marker available=0
generation-1 frame after marker available=1 history=1
```

This is an adapter defect, not an unavoidable limitation of the portable executor. The independent portable probe passes the same generation transition through GraphExecutor: the marker returns unavailable and the older generation returns would_block. See runtime.cpp:493–499 (accepted marker updates generation) versus the desktop shortcut. Preserve the accepted marker's producer/generation semantics and reject older generation data; do not lower the contract to the existing green test coverage.

### R2 — P2: subscriber accepts a payload for a different stream

Location: `adapters/desktop/platform/pipeline/src/stream_transport.cpp:252–256`. The completed frame is checked for decode success and type, but not `f.stream_id == id`.

Own reproducer: same independent probe, TCP loopback scenario. A descriptor-owning test server acknowledges the exact `chosen` / Mel subscription using message 12. It then sends a well-formed message-13 frame of the expected type with stream ID `wrong`.

Expected: explicit rejection/recovery and no data callback, because the negotiated stream identifies the routing input. Actual:

```text
requested=chosen delivered=wrong count=1
```

The same `receive` method serves TCP and attached UART channels. The embedded Session already checks stream identity (`session.cpp:85`), so current desktop and embedded acceptance disagree even though they share the byte/payload codecs. This check was also absent in baseline; it remains an acceptance defect in the migrated canonical adapter, not a newly attributed wire-format regression.

### R3 — P1: unavailable/gap disables stale-sequence rejection

Location: `adapters/desktop/platform/pipeline/src/stream_transport.cpp:258–268`. An unavailable marker sets `gap=true`, and the next data packet is rejected for a non-increasing sequence only when both `!discontinuity` and `!gap`. Old data therefore becomes deliverable precisely after the clearing marker; the adapter also labels it discontinuous, allowing the executor's discontinuity exception to accept it.

Own reproducer: same live loopback connection and generation. Deliver valid `chosen` sequence 11, then unavailable with sequence 0, then `chosen` sequence 10. No reconnect or new producer epoch/generation occurs. The earlier independent wrong-stream diagnostic has contributed one callback; before the stale frame there are three total callbacks. Expected: stay at three; the stale sequence produces no callback. Actual:

```text
frames after same-epoch stale seq10 following seq11/unavailable=4 (expected 3)
```

Requirement: network-stream-v2 explicitly states that old data cannot revive history after its unavailable marker. Gap/discontinuity is loss information; it must not authorize a backwards sequence in the same identity. A real producer restart uses a new epoch/generation. The embedded Session rejects non-increasing sequence regardless of gap (`session.cpp:87`). This desktop guard also existed in baseline; final acceptance must still close it rather than bless it because it was inherited.

## Independent checks and positive evidence

- PreparedGraph/GraphExecutor executes the desktop and embedded graphs; AudioPipeline composes GraphLifecycle and GraphExecutor, GraphProgram delegates to PreparedGraph, GUI models do not form another scheduler. CLI is built from the same desktop adapter. The distributed test launches three actual private-build CLI processes, rather than merely comparing mock clients.
- Portable processing source contains no Qt/JSON/socket/OS-thread operations. Plan workspaces and adjacency are prepared before ticks; local history writes one row to a contiguous bounded ring. Compatible state/history copy happens during preparation/Apply, including refreshing current state at Apply. It is not per-tick whole-history copying.
- Own `portable_probe.cpp`: 120 ticks of eight changing in-memory tones, antiphase stereo, full Window → Hann → FFT → Bark → RMS → Mel → history chain. Asserted output validity, 120 history outputs, eight retained rows. C++ new plus linked malloc/calloc/realloc guards measured **zero allocations**, including the first prepared tick. Compatible gate Apply retained the ring and appended typed zeros. Portable newer-generation unavailable marker rejected older data.
- Own `wire_probe.cpp`: exact equality with the actual baseline codec extracted using `git show 3de4de8:src/pipeline/src/stream_codec.cpp`, with only namespace qualification changed to coexist in one executable. Fourteen cases: all seven typed payloads and each unavailable marker, including provenance, flags, negative timestamp, scalar metadata and payload values. All bytes equal.
- Own portable framing probe: maximum 65535-byte pseudorandom payload matched pinned upstream COBSZPE bytes exactly, received in varying 1–97 byte chunks. Packet encoding/feeding measured zero allocations.
- PacketStream owns outgoing bytes in caller buffers, supports partial writes, bounded incoming storage and reset cancellation. Qt TCP/UART use QtPacketChannel and the same PacketStream; ESP transports adapt nonblocking TCP/UART to ByteTransport/Session. Fragment assembly uses caller-owned bounded storage. Desktop retry/Stop resets assembly and fences channel bindings; mailbox/socket budgets, unavailable purge and selected output subscriptions are present.
- Desktop ownership is distinct from portable hot-path guarantees: evaluate_view/typed callbacks create owned snapshots; requested histories are materialized at delivery/control boundaries. An external desktop History ingress flattens/copies the incoming owned matrix and retains its own bounded owner. These adapter copies do not demonstrate allocation-free processing for all subscribed desktop paths. No borrowed FrameView was found retained in an asynchronous queue.
- Local EngineClient uses bounded latest delivery and subscription generations; delivery checks current engine epoch/revision. Remote v1 views/audio use current epoch/revision/generation/status and subscription filtering; typed views use current epoch/revision/status. Existing own-executed engine tests cover blocked delivery, unsubscribing and Stop. These positives do not supersede R1–R3.

## Commands and results

Toolchain: `C:/Qt/6.10.2/mingw_64`, GCC 13.1.0 from `C:/Qt/Tools/mingw1310_64`, bundled CMake/Ninja. BDSP source HEAD independently verified as `2c68056dfaeffba2f34fc64eee9bc9e331e83c7c`.

```powershell
$env:PATH='C:/Qt/Tools/mingw1310_64/bin;C:/Qt/6.10.2/mingw_64/bin;C:/Qt/Tools/CMake_64/bin;C:/Qt/Tools/Ninja;'+$env:PATH
cmake -S . -B review-final-runtime -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O2 -DNDEBUG -UNDEBUG' -DCMAKE_C_FLAGS_RELEASE='-O2 -DNDEBUG -UNDEBUG' -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/mingw_64 -DBUILD_GUI=OFF -DBUILD_LEGACY_APP=OFF -DFETCHCONTENT_SOURCE_DIR_BDSP=C:/Users/Koba/CLionProjects/qt-visualization/cmake-build-release/_deps/bdsp-src -DFETCHCONTENT_SOURCE_DIR_MINIAUDIO=C:/Users/Koba/CLionProjects/qt-visualization/cmake-build-release/_deps/miniaudio-src
cmake --build review-final-runtime --parallel 4 --target dataflow_runtime_tests bdsp_protocol_tests codec_buffer_tests application_lifecycle_tests stream_fragment_tests graph_continuity_tests graph_preparation_tests desktop_history_demand_tests engine_transport_tests engine_typed_views_tests stream_transport_tests desktop_byte_transport_tests desktop_packet_socket_tests desktop_serial_stream_tests distributed_transport_tests network_runtime_tests
ctest --test-dir review-final-runtime --output-on-failure -R '^(dataflow_runtime_tests|bdsp_protocol_tests|codec_buffer_tests|application_lifecycle_tests|stream_fragment_tests|graph_disconnect_continuity|graph_preparation_transaction|desktop_history_demand|engine_transport|engine_typed_views|typed_stream_transport|desktop_byte_transport|desktop_packet_socket|desktop_serial_stream|distributed_three_cores|network_graph_apply_start)$' -j 1
./review-final-runtime/reproduce.ps1
```

Configure and all 149 build steps passed, including pipeline_cli and native DLL deployment for that CLI. Selected CTest: **16/16 passed, 36.71 seconds**. Full log: `review-final-runtime/Testing/Temporary/LastTest.log`. Compiler commands independently inspected: `-O2 -DNDEBUG -UNDEBUG`; assertions are active. The independent probe executables exit zero after printing observed diagnostic results; their exit code is not an acceptance PASS. Their RED expected/actual differences are recorded above. Portable/wire probes use active assertions and passed. Reproduction script records the exact manual compile/link commands; baseline retrieval/namespace adjustment is included.

## Checked hashes

Git blob hashes of the checked canonical source (same HEAD; not hashes inferred from another report):

| File | Blob |
| --- | --- |
| libs/dataflow_runtime/src/runtime.cpp | 8be3b481b2375bd1f13f322799171870b76d17ce |
| libs/bdsp_protocol/src/byte_stream.cpp | 12be3c62e87c5b7919eb6f65fd69f6c1a793ded2 |
| libs/bdsp_protocol/src/stream_codec.cpp | 6cfc6aff1293cfbc670694ffe283a8af93b1ee98 |
| libs/bdsp_protocol/src/stream_fragments.cpp | b03c23b0ad10df81f63b21dcdc9d77e251f87c41 |
| libs/embedded_support/src/session.cpp | ce62371a8d3c137df0f1b3e82148c370513c7bfb |
| adapters/desktop/runtime/src/audio_pipeline.cpp | 8fdc2cd1c29e6184fa63a6b063b53aa0d0bb633d |
| adapters/desktop/runtime/src/engine_client.cpp | 78952846267fc1e0a3e99b26c0c5f93f5b81a862 |
| adapters/desktop/platform/pipeline/src/network_runtime.cpp | 87a163c9bedc8fce18bde0c27178c848739c8c2d |
| adapters/desktop/platform/pipeline/src/stream_transport.cpp | a3e3237f76e10826c59d5c657aea0390a465c493 |
| adapters/desktop/platform/core/src/qt_byte_transport.cpp | 9cbde98a7dd1feefafefe47d4fdcf7f8c21da63b |
| adapters/esp_idf/stream_adapter/src/byte_transport.cpp | 242af5aeed60bcb0f78714e449aa84e01d1f3f3a |

Private sources and logs remain in the private build for owner reproduction. No new branch, commit, push, SDK installation, shared Ninja, native GUI, physical serial/audio/WLED or stress run. User config.json/output were preserved. Native ESP-IDF cross compilation, optimized hardware backend, physical serial error recovery and native graphics are **not proven**. This verdict covers only assigned runtime/protocol scope; it is not acceptance for architecture or Qt reviewers. An independent re-review of the repaired scope is required before changing this verdict.

Private probe SHA256:

- independent_probe.cpp: E8E58F0FC5049FBC1C815BD540937CC49025CF496F766D041965E29AD51622B0
- portable_probe.cpp: 31D613D95850A43D1EF0B3777F5036B08BE9317EF8D7E89EA07E63DC03C1F751
- wire_probe.cpp: 49ECE5349833E5921355B883C61CD934C1AEEC4648B33A4F43BF45AAC9E8928F

The saved reproduce.ps1 was executed after authoring: all three probes compiled and reproduced the recorded outputs. Final production/shared-test diff was empty, HEAD unchanged. Concurrent edits to .gitignore, coordinator ledger and architecture report belong to other workers and were not reviewed or reverted.


## Re-review preparation (2026-10-06, HEAD 4f9bc5d)

Coordinator reactivated this reviewer while runtime repair bodies are still moving. No build or execution against those moving bodies was performed. Original R1–R3 RED evidence and original independent_probe.cpp are retained unchanged. No repair/author report was read.

Prepared independent private sources:

- desktop_boundary_probe.cpp: isolated connections for wrong stream, stale sequence after marker, discontinuous duplicate, newer generation marker/lower-generation data, direct AudioPipeline generation fence/provenance, publisher stop/listen incarnation and unknown marker reuse. Isolation is needed because the unchanged original combined diagnostic intentionally sent the wrong-stream case first; successful rejection closes that connection before its later sequence case.
- session_boundary_probe.cpp: real bounded SDK Session framing with 7-byte reads/5-byte writes; newer-generation unavailable marker followed by lower-generation data, duplicate with discontinuity, legitimate new producer and retired producer replay diagnostics. It uses no Qt/OS/device transport.

SDK source inspection at blob ce62371a8d3c137df0f1b3e82148c370513c7bfb: session.cpp:87 only filters sequence for equal transport epoch and equal generation. It has no lower-generation check, and does not consider producer_epoch in that guard. This is a pending independent probe, not an executed RED yet. The acceptance requirement remains old-frame fencing across receiver boundaries: a newer-generation unavailable marker must not admit older-generation data for the same producer. Gap/discontinuity cannot lower that fence. A legitimate new producer remains eligible for a fresh sequence/generation; this must be distinguished from replay of a retired producer.

Freeze notification and independent recompile/rerun are outstanding. Verdict remains NOT READY from original review; no provisional acceptance is granted for moving source.

## Executed SDK blocking evidence (frozen Session blob ce62371a8d3c137df0f1b3e82148c370513c7bfb)

**R4 — P1, NOT READY: SDK receiver does not fence older generations or retired producer identities.** Source: libs/embedded_support/src/session.cpp:86–93; session.h retains only rx_epoch_/rx_generation_/rx_sequence_, no producer identity/retired set. This is the Session callback boundary, prior to any optional consumer GraphExecutor. A consumer/queue must not receive a stale frame as a new discontinuous delivery after a newer-generation marker.

Own session_boundary_probe.cpp was compiled against private-build embedded_support and portable libraries with -std=c++20 -O2 -UNDEBUG. Transport uses 7-byte reads/5-byte writes, exact Ack and encoded BDSP typed frames. Actual executable assertion failed (exit 1); log session-boundary-probe.log:

```text
SDK new-gen marker then lower-gen data: available=2 expected=1
SDK duplicate same generation with discontinuity: before=3 after=3
SDK new producer same transport epoch: available=2 expected=2
SDK retired producer replay: before=2 after=3 expected unchanged
Assertion failed: lower_blocked, file session_boundary_probe.cpp, line 34
```

Case 1: transport epoch100, producer500, generation1/seq10; unavailable generation2/seq0; old generation1/seq11. Expected one available callback plus marker; actual second available callback of older generation. Case 2 distinguishes a duplicate with discontinuity (correctly blocked). Case 3 admits a genuinely new producer501 with fresh generation/sequence (must continue to work). Case 4: one live connection, transport epoch100, producer500/gen1/seq1, producer501/gen1/seq2, then retired producer500/gen1/seq3; expected unchanged two available callbacks, actual three. No reconnect or identity retirement timeout is needed for either RED.

Retirement expectation is bounded, not infinite: the prepared executor's existing 16-entry retired producer ring is the reference. The current and recently retired identity must be fenced for this short 500→501→500 sequence; no requirement to retain unlimited historical identities is introduced. Generation monotonicity is evaluated within a producer identity, so guarding every lower generation without considering producer identity would incorrectly reject legitimate new producers. Marker generation0 remains an unknown-generation marker, not permission to forget an already known generation. Transport epoch and inherited producer_epoch must both participate in receiver identity; new transport incarnation may legitimately restart sequence.

The supplied freeze hashes were independently verified; SDK Session itself remained at the original source blob. Coordinator then assigned Session repair separately; no additional SDK build was started against its moving repair source. Desktop re-review continues against its separately frozen bodies.

## Frozen desktop re-review — completed, NOT READY (2026-10-06)

Checked HEAD 4f9bc5d71dea9eb9641628c7046a443c1554c2fd with frozen uncommitted repair blobs:

- audio_pipeline.cpp: 0516403160f049fe10c2127f49ccbe7ecd4dd545
- stream_transport.cpp: a40f432be2dd865f25e8fd97ab9acd86e3369831
- typed_graph_tests.cpp: 80738a91a8986d9b7b239a22217849797d13a498
- stream_transport_tests.cpp: 517b4df47cd67ca8295ecac107b8345da33417a0
- desktop_serial_stream_tests.cpp: 352d3a5ff0038dfbc8f70d5bc6aea0b571fc38aa
- dataflow_runtime/runtime.cpp: 8be3b481b2375bd1f13f322799171870b76d17ce (unchanged portable source).

**Original desktop R1/R2/R3 are closed for these frozen blobs.** The original diagnostic source was recompiled unchanged. Its first two observations now show generation1 after generation2 marker stays unavailable and zero wrong-stream callbacks. Its combined later sequence count is not an isolated assertion because wrong-stream rejection changes the connection; isolated independent connections verified R3, including stale and discontinuous duplicate rejection, then legitimate sequence12 recovery. Own desktop_boundary_probe also verifies newer-generation marker rejects lower generation, including a discontinuous old frame.

Own publisher probe passed: stop/listen changed publisher epoch and scoped producer identity, unknown marker reused the last route identity, and source_sequence=91, timestamp_ns=-456 and generation stayed unchanged. Own AudioPipeline provenance probe passed with distinct transport epoch100 / producer500. Using discontinuity=true to continue this probe does NOT close R5 below; the ordinary fresh sequence remains a RED.

### R5 — P1: accepted newer-generation marker leaves the previous generation's sequence fence

Source: libs/dataflow_runtime/src/runtime.cpp:493–499 and 461–465. In the same producer, unavailable generation2 sets origin.generation=2 but leaves origin.sequence from generation1. A subsequent legitimate generation2 sequence1 then appears non-increasing and is rejected unless it redundantly carries discontinuity. Data delivered directly with a new generation (without the marker) bypasses the old sequence check, so the marker changes legitimate restart admission.

Own independent generation_resume_probe.cpp, portable GraphExecutor/GraphLifecycle only, no Qt/OS/socket: NetworkInput(Mel) → history → view; epoch100/producer500/gen1/seq10; unavailable gen2/seq0; gen2/seq1, ordinary available frame. Expected Status::ok and one history row for the new generation. Actual exit1 with active assertion:

```text
portable gen1 seq10 -> unavailable gen2 -> gen2 seq1 status=7 expected=0 history=0
Assertion failed: status==Status::ok, file generation_resume_probe.cpp, line 6
```

Status7 is would_block. The initial desktop_boundary_probe independently failed the matching AudioPipeline resume availability assertion at line32; its source before the diagnostic continuation switch is retained as desktop_boundary_probe_before_resume_mode.cpp. Sequence monotonicity belongs to a generation within producer identity. The same-generation unavailable case must retain its existing fence, while a genuinely advanced generation must admit a fresh sequence; guarding old generation data remains required.

Compile/run (private directory, GCC13.1, active assertions):

```powershell
g++ -std=c++20 -O2 -UNDEBUG -DBDSP_STATIC_DEFINE -I../libs/dataflow_runtime/include -I../application/include -I../domain/include -I../libs/signal_contracts/include generation_resume_probe.cpp lib/libdataflow_runtime.a lib/libgraph_application.a lib/libgraph_domain.a lib/libsignal_contracts.a lib/libdsp_kernels.a lib/libdsp.a -o bin/generation_resume_probe.exe
./bin/generation_resume_probe.exe
```

### R6 — P1: desktop receiver admits a recently retired producer replay

Source: adapters/desktop/platform/pipeline/src/stream_transport.cpp:280–289, current-producer-only identity guard. No retired tuple is retained. This finding concerns actual accepted data callback at the NetworkSubscriber boundary, not reconnect status or a frame later rejected by an optional GraphExecutor.

Own isolated Wire: one live loopback socket, exact chosen/Mel handshake; transport epoch100/gen1 fixed. Send producer500/seq1, producer501/seq2, then retired producer500/seq3. All are valid available typed frames; no unavailable/disconnect/reconnect in this case. Expected two data callbacks, the recently retired identity ignored. Actual three callbacks and active assertion failure:

```text
desktop retired producer replay: callbacks=3 expected=2
Assertion failed: w.frames.size()==2, file desktop_boundary_probe.cpp, line 34
```

The first two callbacks confirm genuine producer transition is allowed; the third is a data callback from the old identity. Reproduction: review-final-runtime/reproduce-boundary.ps1 compiles desktop_boundary_probe.cpp and runs --remaining. That switch only enables discontinuity on the earlier generation-resume diagnostic so independent later publisher and retirement cases execute; it does not alter this retirement case or its assertions. Log: desktop-boundary-remaining.log. Bounded recently retired identity retention (reference executor ring16) suffices; no infinite identity history is requested. Identity comprises publisher transport incarnation plus inherited producer identity, rather than generation alone.

### Re-review build/test result

Private incremental build succeeded (85-step plan) including actual pipeline_cli. Twelve targeted tests passed, **43.06 seconds**, exact set: bdsp_protocol_tests, dataflow_runtime_tests, engine_transport, typed_stream_transport, typed_graph_runtime, distributed_three_cores, network_graph_apply_start, engine_typed_views, graph_disconnect_continuity, graph_preparation_transaction, desktop_history_demand, desktop_serial_stream. Actual three-core test source independently read: source CLI killed/restarted, live B/C processes confirmed, unavailable remains clearing while source down, independent B stream continues, source rediscovered and new control epoch awaited, restart ACK and ≥30 numeric sequence comparisons asserted. No production/shared tests were authored by this reviewer.

Recompiled original portable_probe: zero allocations over 120 eight-tone ticks, compatible gate ring continuity, portable old-generation rejection still passed. Recompiled wire_probe: baseline14 exact byte cases still passed. Private manual include paths were updated for the independently accepted presentation/ports header move; initial probe include-path compile failures were private harness maintenance, not production findings. No body assertions were disabled or source contracts weakened.

Build command: same private CMake toolchain/configuration as initial report, cmake --build review-final-runtime --parallel4 targeting typed_graph_tests stream_transport_tests desktop_serial_stream_tests distributed_transport_tests graph_continuity_tests graph_preparation_tests desktop_history_demand_tests engine_transport_tests engine_typed_views_tests network_runtime_tests embedded_support dataflow_runtime_tests bdsp_protocol_tests. CTest used an anchored exact-name regex for the twelve cases above, -j1, --output-on-failure. Probe exact compile/link commands are saved in reproduce.ps1/reproduce-desktop.ps1/reproduce-boundary.ps1. Executable diagnostics with assertions can abort; script pipeline exit0 is NOT evidence of passing acceptance. Per-executable assertion outputs and expected/actual comparisons govern this verdict.

Overall runtime/protocol remains **NOT READY**: original R1–R3 closed for frozen desktop hashes, SDK R4, portable R5 and desktop R6 blocking. SDK repair was assigned separately after the executed R4 RED; no claim is made about its subsequently moving source. No other reviewer's report was read, no production repair was made, no native/device/stress/SDK-install run. Hardware and native cross-build remain unproven. Independent re-review is required on the next fixed hashes.


### R7 — P1: portable discontinuity flag authorizes same-identity replay

At coordinator's explicit additional boundary request, independently probed the portable executor without SDK/desktop guards. Source libs/dataflow_runtime/src/runtime.cpp:461–465 at compiled blob 8be3b481b2375bd1f13f322799171870b76d17ce contains the !input.metadata.discontinuity exemption for non-increasing sequence.

Own portable_duplicate_probe.cpp: the same Mel→history graph as generation_resume_probe, epoch100/producer500/generation1. First seq10 accepted. Then exact same seq10 with discontinuity=true, then backwards seq9 also with discontinuity=true. Expected each Status::would_block (7), unchanged current state; actual each Status::ok (0). Each accepted discontinuity clears and refills the ring from replayed data (history1). Executable failed its active duplicate assertion:

```text
portable same producer/gen discontinuous duplicate seq10 status=0 backwards seq9 status=0 expected=7 history=1
Assertion failed: duplicate==Status::would_block, file portable_duplicate_probe.cpp, line 6
```

No generation/provenance/producer transition in this case. Discontinuity communicates lost continuity; it must not authorize a stale sequence for a current producer+generation. Genuine new generation/producer remains an independently required admission case, including R5's newer-generation marker→freshseq1. A receiver adapter guard does not prove that GraphExecutor direct ingress is fenced. Expected policy is shared at this portable receiver boundary, not a demand for a transport-only exception.

Compile/run from review-final-runtime (GCC13.1):

```powershell
g++ -std=c++20 -O2 -UNDEBUG -DBDSP_STATIC_DEFINE -I../libs/dataflow_runtime/include -I../application/include -I../domain/include -I../libs/signal_contracts/include portable_duplicate_probe.cpp lib/libdataflow_runtime.a lib/libgraph_application.a lib/libgraph_domain.a lib/libsignal_contracts.a lib/libdsp_kernels.a lib/libdsp.a -o bin/portable_duplicate_probe.exe
./bin/portable_duplicate_probe.exe
```

Probe SHA256 at completed re-review:

- session_boundary_probe.cpp: 972A852BA26C54B55ADF000FB5EEBC60AD10A7F40F8E2D37B1F4A6551E52E204
- desktop_boundary_probe.cpp (includes continuation diagnostic mode): E74F87F90DE49F0B4B3D44A3B384621574F06A4AAA17A22EE1B2612421F600A7
- generation_resume_probe.cpp: 75FA66B6AB65F2A244034324C4BE5FAC04A42C7A789ABD0859A507B3056BE64A

Final re-review verdict: **NOT READY**, blocking R4/R5/R6/R7. Original R1/R2/R3 closed for frozen desktop hashes. All newly identified findings are actual independently executed REDs. Later owner repair source is outside these compiled hashes and requires a fresh independent verification.


## Final frozen independent acceptance — READY (2026-10-06)

**READY for assigned runtime/protocol scope** on the final frozen working tree based on HEAD 4f9bc5d71dea9eb9641628c7046a443c1554c2fd. All original executed R1–R7 findings are closed on the hashes below. Initial RED findings and intermediate NOT READY verdicts are preserved above as historical evidence, not current blockers. No author report or other review report was used as acceptance evidence.

Final Git blob hashes were independently computed before the private rebuild and repeated after probes; they match:

| Frozen source | Git blob |
| --- | --- |
| adapters/desktop/runtime/src/audio_pipeline.cpp | 0516403160f049fe10c2127f49ccbe7ecd4dd545 |
| adapters/desktop/platform/pipeline/src/stream_transport.cpp | f6e803f7c69f546eeb58413be326be5b5c334bf0 |
| libs/dataflow_runtime/src/runtime.cpp | 34ee46af4e62fdc0c220bbefc7cc6613bec1db64 |
| libs/embedded_support/src/session.cpp | 1786c4d1e60c491903436aa60148b050ca61a7a5 |
| libs/embedded_support/include/embedded_support/session.h | 4273fed6335b9f636405296fd370d7a89f2854e9 |
| tests/dataflow_runtime_tests.cpp | 22842a35d0b55df0d9aeb83b791d9350dfeac047 |
| tests/stream_transport_tests.cpp | a0e281594d02426a63cdc378085fa3845c874da2 |
| tests/embedded_sdk_tests.cpp | 20b9e2b62077f0a5b3736317e54878dcd76ab477 |

### Unchanged independent regression probes

The previously RED probe sources were not weakened or changed for this rerun. SHA256 matches the preceding re-review hashes. desktop_boundary_probe ran without --remaining: ordinary gen2/seq1 resumes without setting discontinuity. All active assertions passed.

```text
R2 isolated wrong-stream callback count=0
R3 isolated stale/duplicate blocked; later seq12 resumes
desktop newer-gen marker blocks lower generation including discontinuity
R1 isolated marker generation fence and producer/time/source provenance pass
publisher incarnation scopes producer, unknown marker uses last route identity, provenance unchanged
desktop retired producer replay: callbacks=2 expected=2
SDK new-gen marker then lower-gen data: available=1 expected=1
SDK duplicate same generation with discontinuity: before=2 after=2
SDK new producer same transport epoch: available=2 expected=2
SDK retired producer replay: before=2 after=2 expected unchanged
portable gen1 seq10 -> unavailable gen2 -> gen2 seq1 status=0 expected=0 history=1
portable same producer/gen discontinuous duplicate seq10 status=7 backwards seq9 status=7 expected=7 history=1
```

R4: Session callback boundary rejects old generation and recently retired producer while accepting genuinely new producer; equal-generation sequence fences remain intact even with discontinuity. R5: newer-generation marker resets only that generation's sequence admission so a freshseq1 is accepted. R6: desktop NetworkSubscriber accepted callback count stays2 for the same live publisher/500→501→500 replay. R7: portable direct ingress blocks both duplicate and backwards sequence with discontinuity; this is not merely an adapter guard.

Source inspection independently confirms fixed inline 16-entry publisher+producer tuple rings in Session and NetworkSubscriber; no growing retirement container. stop/begin or explicit subscriber rebind resets its binding state. Existing prepared executor uses its bounded producer ring. No infinite retirement promise is made.

Additional own session_retention_probe.cpp passed:

```text
SDK unknown marker retains generation/sequence fence; current newer frame resumes
SDK retained16 recent identity replay rejected; evicted oldest eligible, callbacks=19
SDK fresh publisher incarnation with same inherited producer allowed
```

This checks 18 fresh producers: recent501 replay blocked, oldest500 beyond retention eligible. Fresh publisher101 with the same inherited producer500 is admitted. Probe SHA256 F9B714BAC8B69314FE5EB9FD34AD4B0723BF20A38E4CEC1EAB98CADEA95342AE. The bounded retention window is explicit, matching the requested finite resource policy.

### Final own commands and results

Private toolchain/build configuration remains the one recorded above: GCC13.1, Qt6.10.2 MinGW, Release -O2 -DNDEBUG -UNDEBUG. Private rebuild succeeded (25-step plan reduced to24 after MOC regeneration), including pipeline_cli and deployed dependencies. No common Ninja was used.

Executed:

```powershell
cmake --build review-final-runtime --parallel 4 --target typed_graph_tests stream_transport_tests desktop_serial_stream_tests distributed_transport_tests graph_continuity_tests graph_preparation_tests desktop_history_demand_tests engine_transport_tests engine_typed_views_tests network_runtime_tests embedded_support dataflow_runtime_tests bdsp_protocol_tests
./review-final-runtime/reproduce.ps1
ctest --test-dir review-final-runtime --output-on-failure -R '^(dataflow_runtime_tests|bdsp_protocol_tests|graph_disconnect_continuity|graph_preparation_transaction|desktop_history_demand|engine_transport|engine_typed_views|typed_graph_runtime|typed_stream_transport|desktop_serial_stream|distributed_three_cores|network_graph_apply_start)$' -j 1
```

Exact private manual compile/link flags are saved in reproduce.ps1. Additionally recompiled generation_resume_probe and portable_duplicate_probe using their previously recorded commands; both executables exited0. Their final logs are generation_resume_probe-final.log and portable_duplicate_probe-final.log. Independent desktop and Session logs: desktop-boundary-probe.log, session-boundary-probe.log. The unchanged original combined diagnostic also reran; its sequence count2 is expected after fixing wrong-stream rejection and is not the isolated R3 result.

Selected CTest **12/12 passed, 43.28 seconds**. This includes actual three-core source CLI kill/restart with live intermediate cores, independent stream continuity, unavailable clearing, positive restart ACK and numeric comparison by source sequence; TCP stream and desktop UART/attached-channel tests also passed. These tests use synthetic in-memory sources and loopback, not physical audio/serial.

Own original portable_probe still passed zero C++new plus wrapped malloc/calloc/realloc allocations for120 eight-tone full DSP ticks, bounded ring/gate continuity, and maximum pseudorandom BDSP framing/partial feed. Original baseline wire_probe passed all14 exact byte comparisons with3de4de8. Header layout and payload bytes were not modified by identity fencing; publisher incarnation scoping changes the intended identity field value, not its wire position/encoding or source clock/source sequence/generation.

Independently compiled current shared tests/embedded_sdk_tests.cpp into the private bin as embedded_sdk_review_tests.exe and executed it (exit0), with -std=c++20 -O2 -UNDEBUG -fno-exceptions -fno-rtti. Linked the freshly built private portable libraries. This test's fixed-transport allocation guard and bounded identity-retirement tests execute with active assertions, plus sessions/partial I/O/reconnect/overflow/marker and synthetic SDK behavior. The no-exceptions/no-RTTI flags here apply to the test translation unit; this is not a new claim that the entire desktop-linked library build was configured with those flags. Native portable SDK component/build acceptance remains the architecture review scope.

### Limits and scope

No outstanding verified runtime/protocol defect remains within this assigned review at the final frozen hashes. Production/shared tests were never authored by this reviewer; only docs/review-final-runtime.md and private probes/build were written. No branches/commits/push, author reports, native GUI, physical serial/audio/WLED, stress run or SDK installation. User config.json/output preserved. Native ESP-IDF cross-build, optimized device backend and physical UART/hardware recovery are not proven by this review. This scoped READY does not replace the coordinator's final full-suite/build audit or the architecture/Qt reviewers' acceptance.
