# Numeric runtime API

The continuous-processing changes described here completed integration and host
acceptance on 2026-10-09: release 98/98, SDK 32/32, installed component 6/6 PASS.
Evidence and hardware proof boundaries are tracked in
[the core fixes report](core-fixes-final-2026-10-09.md) and
`refactoring/progress-checklist.md`.

The portable core executes one canonical `domain::NumericProcessingGraph` through
`dataflow_runtime::PreparedGraph` and `GraphExecutor`. Desktop and embedded
compositions use the same sources and application ports. Qt, JSON, capture devices,
sockets and ESP-IDF resources belong to adapters and entrypoints.

## Contracts and capability admission

`signal_contracts::NumericContract` describes dtype, semantic, quantity, shape,
representation, channels, coordinates and clock. `StructuredContract` describes a
bounded schema payload. `DataFrameView` borrows storage; `OwnedDataFrame` holds
bounded buffer handles whose pools, slots and storage must outlive those handles.

`domain::resolve_processing_node` resolves registered parameters and port rules;
`admit_processing_graph` checks topology and queries implemented combinations.
`PreparedGraph::numeric_capabilities(profile)` supplies the runtime capability
query. Dtype inventories alone do not authorize a particular operation or policy.
The profile/context used by that query must remain alive. Device wrappers preserve
the returned resource profile while adding their own transport checks.

Registered kind names and port IDs remain stable. For example, `AudioSource`,
`TestSignal` and `NetworkInput` resolve to portable Source operations;
`Spectrogram` is the History processor and `SpectrogramView` is a pure sink.
These domain mappings do not give graph models platform I/O or DSP ownership.
The UInt64 FilledCount output remains a separate port. Display labels do not
rename node IDs, contract keys or wire symbols.

## Preparation, lifecycle and control

Construct `GraphExecutor` with `NumericRuntimeProfile`. Its `prepare_numeric`
accepts a graph, application `NumericRuntimeLimits` and a `PreparedExecution`
output. Limits cannot exceed the executor profile. Preparation resolves kernels,
owns immutable contracts/topology and allocates mutable context buffers before
processing. Kernel plans are allocated only on the node kinds that need them.

`PreparedExecution` pairs an immutable `PreparedPlan` with its `RuntimeContext`.
`GraphLifecycle` calls `NumericGraphPreparationPort` and `GraphRuntimePort` for
transactional Prepare, Apply and connect/disconnect. A failed Prepare
retains previous pending and working state. Apply validates executor/bundle/base
identity; only success consumes the bundle. Compatible latest working state is
refreshed at Apply. Successful Apply activates processing immediately. User control
has no Start/Stop authority; `shutdown_transactional` is reserved for owner teardown.
Composition serializes commits, processing and snapshot access; expensive desktop
preparation runs outside the processing lock and revalidates its base before commit.

`NumericProcessingGraph.execution_interval` is a reduced positive rational number
of seconds, bounded from 1 ms to 60 s, with default `{1,60}`. Analysis deadlines
follow physical signal time and retain fractional sample phase. Capture sample rate
and GUI refresh rate are separate concerns. The JSON boundary encodes the numerator
as exact Int64 and denominator as exact UInt64 decimal literals; absent intervals
use the default. The console accepts `--analysis-interval-ms` for an explicit override.

`ProcessingControlService` uses `GraphLifecycleControlPort` for the same executor.
`ProcessingDTO.numeric` and `ProcessingCapabilities.numeric` are required. The
service owns one global pending owner/token/TTL, CAS revisions, exact last-request
replay per session, registered parameter updates and explicit persistence.
Requests are owned before dispatch. `working_numeric_snapshot()` returns a
no-allocation shared owner of the immutable graph DTO for durable reply replay after
Apply. Persist uses the applied canonical graph and its graph revision.

## Resource dimensions

The domain separately bounds reported node/kernel `resources.plan_bytes` and
owned graph DTO metadata (`max_owned_metadata_bytes`). Both contribute to
`peak_bytes`, together with payload, scratch, stack, topology workspace and
retained working resources. `maximum.plan_bytes` bounds the node plan dimension;
it is not a second ceiling on the combined DTO/plan total.

`MemoryRequirements.state_bytes` includes all retained DTO/plan ownership;
`buffer_bytes` and `scratch_bytes` describe prepared processing storage.
Runtime capabilities reserve the global 4096-byte plan/context allowance and one
immutable byte-zero pool bounded by `frame.max_payload_bytes` in the
returned profile's `retained_working_bytes`. GraphLifecycle preserves that
allowance under request limits, and preparation includes the previous working
bundle in peak admission, sharing the same-sized immutable zero pool between old
and new bundles. Unsigned PCM midpoint silence retains its own typed buffer.
Replay retains only graph DTO ownership, so it releases retired processing kernels.
`control_snapshot_bytes()` charges the compiled session object, two bounded DTOs,
topology workspace and standalone parameter request bounds. Adapters separately
account their physical pools, queue capacities, arenas and transport buffers.
Stack reservations are conservative reference accounting, not native measurement.

## Processing and delivery

`GraphExecutor::process_numeric_ports` accepts ordinary borrowed numeric or
structured input. `NumericRuntimeCallbacks` exposes Output and pure sink Input
endpoints with exact port identity, contract, stamp, status and delivery fence.
Borrowed frames are consumed synchronously. Async adapters retain bounded owning
data before the callback returns; they do not queue a borrowed view.

Processing uses prepared buffers and workspaces. It does not allocate, expand
containers, parse JSON, perform network I/O or copy the retained history on each
append. Disabled ordinary computations with identical complete input/output
contracts forward the input without DSP. Different contracts publish typed silence
with advancing time; source acquisition continues and disabled source outputs are
muted. Routing controls retain their explicit routing semantics. Known disconnected
clocks publish typed silence; unavailable roots propagate unavailability. Compatible history
survives value/route edits. DSP and dataflow continue independently of GUI visibility.

`NumericHistoryRowView` delivers a compact selected row, filled count, capacity and
accepted row ordinal. Ordinals are maintained even with no History callback.
Local History appends rows into an owning ring; disabled History appends a zero
row and preserves older rows. Routes use prepared per-row silence masks. History
Converter owns a destination ring and gathers/converts only newly accepted rows;
initialization, explicit rebase and gaps import the newly supplied rows once.
It preserves previous converted rows when disabled and zeros only changed rows.

`port_contract` queries a typed Input/Output port. `history` is the serialized full
snapshot boundary: it borrows the current owning ring, coordinates and applicable
route mask. An adapter explicitly retains a full snapshot when required. Local
snapshots expose all capacity rows, with zero padding before the real newest
suffix. `history(..., &filled_count, &ordinal)` supplies occupancy and accepted
ordinal from runtime state; neither is inferred from matrix shape. The desktop
snapshot callback carries `(frame, ordinal, filled_count)`, including remote
wire `history.filled`. Capacity edits preserve the newest real rows, truncate
older rows on shrink and add zero padding on expansion without inventing times.
History supports explicit feature coordinates and Seconds/Frames time coordinates
derived from the actual clock stamp or accepted ordinal. Mirrored prepared time
storage publishes logical order without shifting the complete coordinate history.

Local History accepts fixed-width vectors, consistently at domain admission and
runtime preparation. Variable-width row contracts are rejected before execution;
full matrices cannot represent different feature widths per retained row. Local
Regular Seconds is also rejected because source arrivals can be irregular; use
Explicit Seconds for physical time. Default desktop graphs and newly created
History nodes use explicit source-clock seconds. Existing saved contracts with
None time coordinates remain unchanged and present frame ages consistently in
both compact and snapshot delivery. Neither PCM sample rate nor GUI cadence is
used to guess their missing row timestamps. External History may declare Regular
Seconds: compact gathering uses logical row coordinates, and the presentation
uses producer row ordinals for rolling relative ages across capacity wraps and
snapshot resynchronization. Explicit timestamps retain actual time gaps.

## External History ownership

External History uses the executor overload:

```cpp
process_numeric_owned_ports(node, owned_frame, delivery, callbacks,
                            history_ordinal, retention_owner, history_filled_count);
```

For History, both the real accepted row ordinal and the shared owner pinning its
BufferPools/slots/storage are mandatory. DATA4 supplies `Message.history.ordinal`;
full-capacity padded ingress also supplies `Message.history.filled` explicitly.
frame shape and delivery sequence do not substitute for it. Ordinal deltas declare
append/batch/full-lap updates, and ring/count must match that delta. A producer
change or stamp discontinuity explicitly rebases a snapshot. The runtime retains
handles without copying the incoming full payload and releases handles before
their arena owner. Borrowed History ingress is rejected before state mutation.
`PreparedGraph` exposes the same operation with an explicit RuntimeContext.

## Kernels and actual restrictions

Converter and reduction dispatch preserve exact Int8, UInt8, Int16, UInt16, Int32,
UInt32, Int64, UInt64, Float32 and Float64 values according to the selected policy.
Int64/UInt64 payloads are not staged through double. Conversion policies declare
rounding, overflow, precision loss, nonfinite handling and scaling; active modes
are checked against actual kernel support during preparation. Whole History
conversion supports independent physical ring origins and typed masked silence.

Native Window/WindowFunction/FFT/Bark/Merge/Gain/Mel DSP requires native planar
Float32; other representations require an explicit converter. Window/FFT/Bark
handle declared planes, Merge requires stereo, Gain/Mel mono. Stable Merge modes
are Mean=0, Maximum=1, RMS=2. Regular Hertz spectra must match their real sample
rate/FFT geometry; explicit Hertz spectra provide actual finite increasing
frequencies to Bark/Mel. No frequency or time meaning is guessed from array shape.
WLEDEncoder uses the bounded portable packet encoder; transport owns counters.

Float64 arithmetic follows device kernel capabilities. `history_enabled=false`
rejects History in both capability query and preparation. Supported combinations
remain subject to declared frame, metadata, payload, scratch, peak and stack
limits. Host or fake-IDF checks do not certify physical device heap/stack behavior.
