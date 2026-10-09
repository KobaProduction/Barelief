# Numeric runtime handoff — 2026-10-07

Current author implementation and FINAL targeted evidence. Shared production rebuild
and independent acceptance of the revised runtime remain I/reviewer responsibilities.
Historical checkpoints do not accept the current whole block.

## Execution

PreparedGraph / GraphExecutor is the single canonical numeric executor. Legacy GraphDef,
FrameView executor, legacy preparation and migration paths were removed. Plans own
contracts/topology; contexts own prepared buffers/rings/workspaces. NumericRuntimeProfile
is required at executor construction; prepare takes bounded application NumericRuntimeLimits.

Implemented all ten dtypes through real conversion/reduction, ordinary Matrix, typed
routing, structured packets, pure sink Input delivery, History/count, compact row events,
port queries, full snapshot queries and compatible control-boundary state refresh. Disabled
Converter/Reduce emit typed silence; disabled History appends a zero row and retains
older rows. Disabled History Converter updates only new rows with zero. History ordinal
processing is independent of callbacks and GUI visibility.

Float32 native planar DSP uses Window/WindowFunction/FFT/Bark/Merge/Gain/Mel kernels.
Window/FFT/Bark handle declared planes, Merge requires stereo, Gain/Mel mono. Stable Merge
modes remain Mean0/Maximum1/RMS2. Regular Hertz spectra must match sample_rate/FFT_size;
explicit Hertz spectra use their actual finite increasing coordinates for Bark/Mel.
Elementwise transforms retain explicit coordinates. WLEDEncoder uses bounded wled_protocol.

## R2 repairs and ownership API

Whole History conversion is supported by the shared numeric conversion kernel, including
independent physical input/output ring origins, typed masked silence and all ten dtypes.
Canonical runtime History Converter keeps its own destination ring and converts only
changed rows via a prepared compact gather and row kernel. Initial/rebase imports process
new supplied rows once; each steady append processes its new row only. Coordinates are
stored in prepared buffers; mirrored logical time storage avoids shifting the history.
Local History supports explicit feature coordinates and explicit Seconds/Frames time
coordinates derived from the real stamp clock / accepted row ordinal.

External History is supported through process_numeric_owned_ports. Final signature on
GraphExecutor: NodeId, const OwnedDataFrame&, DeliveryStamp, NumericRuntimeCallbacks={},
optional<uint64_t> history_ordinal={}, shared_ptr<const void> retention_owner={}.
PreparedGraph additionally takes RuntimeContext after the frame. For History, the ordinal
and owner are mandatory. DATA4 Message.history.ordinal supplies the real row ordinal;
the owner pins BufferPools/slots/storage. Handles are released before the arena owner.
Public borrowed process_numeric_ports rejects History before state mutation. Accepted
ordinal deltas determine append/batch/full-lap work; ring/count must match the declared
delta. Producer changes or stamp.discontinuity explicitly declare snapshot replacement.
No row ordinal is guessed from frame shape or transport sequence. Arena physical budgets
remain part of adapter composition; runtime charges a bounded retained input frame.

## Application

GraphLifecycle performs numeric transactional prepare/apply/connect/disconnect/start/stop.
ProcessingControlService owns one reservation, owner/token/TTL, CAS revisions, exact owned
request replay and explicit persistence. DTO.numeric and capabilities.numeric are required.
Runtime ports require no-allocation working_numeric_snapshot(), retaining an immutable
DTO-only owner. Requests are staged before dispatch; replies retain graph DTO ownership after
Apply. Control budgets use the compiled control_snapshot_bytes() DTO/session ledger;
composition supplies replay/scratch/aggregate reservations.

## Final targeted verification

Own directory build-refactor-numeric-runtime; no shared Ninja or application/GUI/audio run.

- run-final-scenario.ps1 builds current production sources + numeric_runtime_scenario_tests:
  all ten dtype routing, exact UInt64 >2^54, actual six-tone Float32 DSP, typed silence,
  detach/compatible history/Stop, failed prepare/resource/capability bounds, same executor
  + lifecycle + two-client control/replay/parameters/persist/start/stop/TTL/foreign-owner,
  local incremental History Converter, disabled preservation, external owned History,
  arena lifetime, explicit ordinal full-lap/batch, all ten whole-History kernel dtypes,
  explicit local feature/time coordinates and zero-row preservation. Allocation probes
  cover processing. Latest final status is recorded in final-scenario-status.log.
- run-gather.ps1: build0/run0, sources_equal=True, numeric gather failures0.
- final-conversion-tests.exe: strict build0/run0, exact integer/IEEE conversion oracles PASS.
- Latest runtime/lifecycle/control and numeric conversion sources compiled with C++20,
  -fno-rtti, -Wall -Wextra -Werror. Combined scenario only suppresses pre-existing signed
  comparison / indentation warning-as-error in other DSP sources, not test expectations.

Legacy-only dataflow_runtime/node_routing/application_lifecycle/processing_control fixtures
are excluded by I instead of restoring obsolete APIs. Current scenario retains their
useful canonical invariants; independent reviewer oracles are separate and not modified.

Actual restrictions: native DSP requires Float32 or an explicit converter; arithmetic
follows configured kernel support. history_enabled=false rejects History consistently.
Stack reservation is conservative reference accounting, not a native ABI measurement or
embedded hardware certification. Independent acceptance and shared rebuild remain pending.

## Stock-device plan compactness repair

After I's actual stock spectral profile admission exposed the per-node kernel-plan cost,
PreparedConversion, row PreparedConversion and PreparedReduction are cold-owned optional
objects on their actual node kinds. Ordinary nodes no longer embed those large snapshots.
The capability ledger still charges the dynamic kernel objects where used. No stock budget
was widened and no stack declaration changed. Revised current-source scenario build0/run0;
runtime.cpp also compiles separately with strict -Wall -Wextra -Werror. I must rerun actual
stock profile/shared application and SDK admission against this source revision.

## Disabled ordinary Sink repair

Additional independent RED found ordinary disabled Sink forwarding input values. It now
publishes typed silence while preserving admitted actual extents and coordinates; bounded
Structured inputs use the prepared silence packet. History sinks keep their existing
per-row mask behavior. The current all-ten-dtype runtime scenario includes disabled Sink
expectations. Latest revised final scenario build0/run0, failures0; acceptance remains
independent. Shared rebuild must use this revision after the compact-plan repair.

## Admission dimension coherence repair

The domain ledger separately bounds reported node plan resources and owned graph DTO
metadata, then includes both in max_peak_bytes. Runtime erroneously compared their sum
plus global4096 to the node-plan ceiling, rejecting an otherwise bounded stock profile.
That duplicate dimension comparison is removed: resources.plan_bytes retains the admitted
node plan value, MemoryRequirements.state_bytes contains all owned plan/DTO bytes, and
peak accounts them together. The global4096 reservation is now present in returned runtime
capability profile and preserved by GraphLifecycle request limits; E preserves that profile
in its device capability wrapper. The later compact replay repair below supersedes the
previous whole-plan replay charge. No capability
ceiling is increased. A final exact node-plan bound / one-byte-below refusal oracle passes;
current full author scenario build0/run0. I still owns stock/shared rebuild acceptance.

## Compact ordinary-ESP32 runtime block (2026-10-07, author READY)

PreparedGraph creates a separate immutable graph DTO owner during cold preparation.
GraphLifecycle replay returns that owner without allocating and without retaining a retired
PreparedGraph, its kernels or RuntimeContext. processing_graph_owned_bytes() applies the
same full DTO capacity ledger before application staging. control_snapshot_bytes() computes
the compiled session/replay/staging bound without charging processing kernels per session.

Immutable byte-zero input buffers and Source outputs share a prepared read-only pool;
unsigned PCM midpoint silence keeps independent typed storage. Equal-sized old/new plans
share this pool during atomic replacement. Native Float32 planar Window emissions borrow
the rolling samples until synchronous descendants/observers return; disabled Window emits
the immutable pool and leaves rolling state intact. FFT::process_plane() uses private FFT
scratch directly from/to numeric views, eliminating N + N/2 float intermediates. Merge/Gain
no longer charge nonexistent generic DSP payloads. Mutable old/new kernels stay independent.

Cold sizing evidence: build-refactor-numeric-runtime/memory-probe-run.log, direct strict
host build0/run0, stock stereo FFT2048/block256/Mel8/History OFF, DTO cap8192, frame cap16384:
SINGLE state37649 + buffer74376 + scratch40944 = 152969 B; admission177673 B includes
stack24576 + topology128. ATOMIC admission314258 B; shared silence pool16384 B is counted
once. C++64 requested heap SINGLE115687 B / ATOMIC214950 B excludes vendor C table malloc
and allocator headers. Retaining the old DTO while dropping its context/plan expires the
old plan and reduces requested heap to118166 B. This is cold allocation sizing, not native
heap or processing acceptance. Existing scenario proofs predate this compact revision.

Actual Xtensa ESP32 ABI object native-memory-abi.o (elf32-xtensa-le): PlanNode328,
State984, NumericBuffer80, DataContract224, DataFrameView144, PlaneLayout12,
NumericProcessingGraph68, ConfiguredProcessingNode192, ProcessingPort12,
ProcessingParameter56, Edge12, ControlSessionSlot472, size_t4, string24,
PreparedConversion1656 B. Exact stock node-plan sum from current query formulas:18752 B
(host26344). With nodes8/DTO8192/identity64, native control snapshot bound16985 B
(host17153). Strict no-exceptions/no-RTTI native compilation passes processing_control,
graph_lifecycle, processing_contracts and dsp.cpp; runtime compiles in the ABI object.
Native DTO capacities/allocator overhead/actual device heap are not measured by this object.

I owns shared Ninja/device integration and actual ordinary320KiB TCP/UART full2048 boot,
processing/control/persist/reboot/atomic-refusal checks. The constrained profile can truthfully
admit frame payload16384 B; extended4096 retains32768 B. Insufficient old+new budget must
reject before mutation. Independent review and FINAL processing tests remain pending for
this compact revision; no whole V2/V4 acceptance is claimed.
