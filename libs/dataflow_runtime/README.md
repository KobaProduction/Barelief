# Portable dataflow executor

`dataflow_runtime/runtime.h` is C++20 and depends on `graph_application`,
`graph_domain`, `signal_contracts` and `dsp_kernels`. It has no Qt, JSON, socket,
device or GUI dependency. `PreparedGraph` is the actual application `PreparedPlan`;
`RuntimeContext` owns every mutable kernel, cadence counter, producer marker and
buffer. Different contexts can execute the same immutable plan independently.
Owner access to an individual context is serialized.

`PreparedGraph::prepare` validates the definition, calculates a deterministic
topological plan and detached clock dependencies once, checks ingress contracts
and `RuntimeLimits`, and allocates buffers and FFT/Mel workspaces. Ingress specs
describe actual rate, semantic type, dimensions and maximum PCM block size.
Unknown ingress has no prepared contract and `process` returns `unavailable`.
Contract/rate/shape changes require preparation outside processing. No sample rate
is guessed. History ingress needs its declared capacity so storage can be bounded.

All 19 canonical kinds use the same executor. AudioSource/TestSignal and
NetworkInput accept externally produced frames; capture/generation is an adapter
responsibility. Window preserves the existing fractional 60 Hz hop cadence and
can emit multiple windows in one tick. Legacy stereo-spectrum ingress through
the local source/window route passes the already computed spectrum onward.
Local source ingress accepts PCM or geometrically valid legacy stereo spectrum.
Owned desktop Window ingress requires the explicit `allow_legacy_window` flag;
the adapter normalizes omitted legacy FFT metadata before preparation.
Windowing, FFT and Mel call `dsp_kernels`; Bark and RMS merging preserve existing
operation order. Sinks emit their semantic frame without performing I/O.

`process` validates a borrowed `FrameView` before mutation and returns an explicit
status. Its prepared adjacency has bounded graph depth; processing does not
allocate, grow containers, parse documents, wait on sockets or copy complete
history. Each Spectrogram has one contiguous `[frame][band]` ring. Detached/gated
known contracts produce typed zeros on the retained clock. A newest-row overlay
provides gated history without modifying the producer ring. Compatible histories
and cadence survive re-preparation; incompatible descendants reset. Epoch
retirement is bounded to the latest 16 producer incarnations. Explicit ingress
unavailability clears dependent state and emits typed unavailable outputs; Stop
is a separate lifecycle operation. Detached history resumes with zero rows after
unavailability or an explicit history reset.

An `OutputCallback` receives each output as it is produced, including individual
windows. All spans and row overlays are valid **only during that callback**.
Callbacks must not retain these views or recursively operate on the same context.
A consumer can encode into a bounded caller buffer or copy into its own bounded
retention pool. Owned desktop snapshots are a separate compatibility adapter and
are excluded from the runtime allocation guarantee. Consumers choose when to
materialize history for display; the runtime does not create GUI snapshots.

MemoryRequirements reports owned object and payload/workspace bytes. Allocator
bookkeeping, call stack, caller ingress/output storage and process-wide DSP backend
tables are separate budgets. No ingress/event queue exists in this executor;
callback output expansion is bounded by graph size and maximum PCM block size.
The desktop adapter may negotiate a larger block limit explicitly during
preparation. Preparation may allocate; allocation failure follows the host
allocator policy. Reported validation/limit/process failures require no exceptions
or RTTI.
The desktop adapter retains the maximum negotiated block capacity across its
ingresses and graph rebinds. Failed preparation leaves the working plan and
context unchanged.

`GraphExecutor` implements the real application preparation/runtime ports for
`GraphLifecycle`. It validates prepared bundle identity without RTTI, consumes it
atomically on Apply, preserves compatible state, and gates processing on Start
and Stop. Apply refreshes compatible state from the latest live context into the
prepared buffers at the serialized owner boundary. Ticks or Stop between Prepare
and Apply cannot rewind history or restore cleared state. It owns no transport
retry or delivery queue: adapters must cancel and
fence their own queued deliveries on the lifecycle boundary.

`tests/dataflow_runtime_tests.cpp` exercises all canonical kinds, multiple windows
at 44100 Hz, allocation guards, a wrapped history overlay, limits, marker fences,
detached zeros, context deep copies and a real GraphLifecycle transaction.
