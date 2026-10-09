# Portable dataflow executor

`dataflow_runtime/runtime.h` is C++20 and depends on application/domain,
`signal_contracts` and `dsp_kernels`, without Qt, JSON, transport or device I/O.
`PreparedGraph` is the single executor plan; `RuntimeContext` owns mutable
operation state. A node stores only its applicable FFT or Mel kernel. Contexts
are independent, and an owner serializes each context's processing and Apply.

`prepare_numeric` validates bounded DTO ownership before lowering dtype edges,
checks full contracts/capabilities/resources, and prepares topology, storage,
operation workspaces and physical-sample scheduling. Processing never expands
containers, parses documents or waits on I/O. Backend FFT tables are shared
process resources initialized during preparation, not on the processing path.

The graph's `execution_interval` is a positive reduced rational duration in
seconds, default `{1,60}`, within 1 ms through 60 s. Window derives a fractional
sample hop from this interval and the declared physical sample rate. Targets
requiring less than one sample per hop are refused. Maximum windows and callback
expansion per ingress are bounded by the runtime profile. Changing the interval
restarts Window phase, retains compatible History and preserves source clocks;
it neither resamples PCM nor changes its timestamp unit. A Window timestamp is
its input block timestamp plus the exact integer floor of the consumed sample
duration in the declared clock ticks. Coarse clocks therefore quantize time.
Startup windows use zero padding until enough real samples arrive. Unavailable
intervals, declared discontinuities and recovery discard Window accumulation.

Disabled computational nodes with identical full input/output contracts forward
an immutable identity view. Different contracts produce typed zeros with the
input time. Disabled Window advances its schedule without reading PCM, and its
buffer is cleared on enable transitions. Disabled History appends typed-zero
rows and time coordinates. Source acquisition is an adapter concern and continues;
a disabled Source produces typed zeros. Gate/Switch/Splitter routing mute remains
a separate policy. Sink disable cannot stop upstream processing.

History storage is `[row][feature]`, with bounded mirrored row masks. Local History
allocates and exposes its entire fixed capacity from preparation onward; older
unused rows contain semantic typed zero (including unsigned PCM midpoint). The
separate filled count reports real retained iterations. A capacity-only Apply
preserves the newest rows, exact row coordinates and accepted ordinal, truncating
older rows or extending the older tail with typed zero. Snapshot consumers must
use the explicit filled count, rather than infer occupancy from array shape. Routing
updates every changed row in an external batch and never modifies the producer
payload. Converter replacement, rewire, ordinal regression and reset rebase its
retained rows. Full History snapshots are a separate serialized borrowed boundary.

Local History admits fixed-width vectors only: every retained row shares one
feature geometry. Variable-width vectors are rejected during domain admission,
before execution, consistently with the runtime's existing support policy.
For physical time use explicit Seconds coordinates: each appended row stores its
source-clock timestamp in bounded mirrored coordinate storage. The desktop
default uses this contract, so compact rows and wrapped snapshots retain identical
times without deriving analysis cadence from the PCM sample rate. Local History
rejects Regular Seconds because processing does not guarantee regular arrivals.
External History sources may declare Regular Seconds; their regular row spacing
is part of the producer contract.
Asynchronous adapters retain bounded owning rows before returning from callbacks;
borrowed history ingress is rejected. All callback views expire when the callback
returns. Reentrant processing of the same context is unsupported.

Apply activates continuous processing immediately. There is no user Start/Stop
or writable run intent. `shutdown` is an owner teardown operation. Apply checks
preparation identity and refreshes compatible state from the latest live context
before consuming the prepared bundle. Failed preparation or Apply preserves the
working plan. Source incarnation retirement retains the latest 16 epochs.

`GraphExecutor::snapshot_preparation_base` takes an owning state snapshot under
the processing lock. `prepare_numeric_detached` prepares from it outside that
lock; `register_prepared` verifies the working plan and private preparation token
under the lock. The adapter separately serializes control transactions. Snapshot
copying still allocates and holds the processing lock for bounded state-copy time;
detached admission conservatively charges a full extra working allocation. The
ordinary serialized preparation API avoids that extra snapshot. In no-exceptions
builds allocation failure follows the platform allocator policy.

Memory admission includes DTO, buffers, operation workspaces and conservative
stack reservation. Allocator overhead, actual platform stack high-water, callback
cost and optimized-backend WCET require measurement on the target. Portable
source support does not establish ESP32 hardware timing or native GUI acceptance.

`PcmResampler` is an explicit PCM-to-PCM rate boundary (`output_rate`). Both
contracts retain the clock domain but use their own sample rate and canonical
`1 / sample_rate` tick. Frames have variable length; ingress may produce no output.
Timestamps start at the input stream origin converted with mathematical floor to
output ticks, then advance by the exact output sample count. Unavailable/error
markers use the same output clock and producer sequence. Discontinuities,
epoch/generation changes, timestamp gaps and unavailable ingress reset fractional
phase and FIR history; the next actual output is discontinuous. Unchanged Apply
preserves phase/history through allocation-free prepared state transfer. Disabled
conversion between incompatible clocks keeps the output grid and emits typed
zeros. See the kernel README for filter delay and quality bounds.

Numeric view endpoints accept every primitive dtype within their structural rank:
line/frequency views take Vector, SpectrogramView takes Matrix, and ScalarView
takes one to eight independent single-channel Scalar inputs with stable port IDs. Their labels
select presentation initial settings, never PCM/FFT/Mel admission. Semantic labels do not execute DSP
or allocate an accumulator. Only the explicit History operation accumulates rows;
remote ring snapshots carry explicit ring geometry/ordinal/occupancy. Dense Matrix
input remains a current value and is routed, converted or reduced as such.

`IndexExtract` selects a primitive Vector/Matrix element without numeric conversion.
Its zero-based `channel_index` selects a channel position, `index` selects a vector
element or logical matrix row, and `column` selects a matrix column (zero for a
Vector). Ring rows use logical oldest-first order. The Scalar output preserves
dtype, representation, quantity, unit and processing clock; coordinates are absent
because the selected result has no array axes. Its timestamp is the evaluation
time of the incoming value. Admission checks maximum extents; a shorter actual
frame produces an unavailable error marker, never stale data. Disabled extraction
emits typed silence on the declared output. All buffers and index records are
prepared before processing. Disconnected ScalarView inputs independently emit
typed silence on a matching known source clock or unavailable markers when no
such clock is known; other connected inputs continue delivering independently.
