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
