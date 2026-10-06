# Portable signal contracts

`signal_contracts` is C++20 and depends only on the standard library. FrameType
values preserve the existing pipeline and BDSP identifiers. FrameView borrows
sample spans and its metadata stream ID: runtime outputs are valid until the
next tick, unless the producer explicitly offers a shorter lifetime. A shape
does not establish PCM, Spectrum or Mel compatibility; PortContract includes
the semantic type and dimensions. `compatible` requires exact known contracts.

HistoryView describes contiguous physical `[frame][band]` ring storage and
logical oldest-to-newest rows.
`newest_override` is an optional borrowed span of exactly `bands` finite values;
`row(frames-1)` uses it instead of the physical newest row, preserving older
history and ring storage. It changes no semantic contract or wire format. Runtime
may use a prepared zero row to gate the newest sample without a history copy.
Retain and codecs consume logical rows; retained snapshots materialize the
replacement and clear the override so its source may safely expire.

Its physical capacity is distinct from the
advertised metadata history capacity; a compact retained snapshot may contain
only the active rows. `row` requires a validated descriptor and an in-range
logical index. No physical time/frequency coordinates are invented.

BufferPool uses caller-provided byte storage and initially unused BufferSlots;
the pool and both arrays must outlive every BufferHandle. Handles share a block
with atomic reference counts and prevent reuse until the last reference is
released. Pool acquisition is bounded by the slot count. Acquire/release may
run concurrently; publishing buffers and sharing the same handle object require
caller synchronization. Prepare storage before processing. Float snapshots
require float-aligned block starts. No allocation, exceptions or RTTI are needed.

`retain` copies all active samples and stream ID into one block, preserves
metadata, linearizes logical history rows, and leaves its output unchanged on
failure. After publication treat the buffer as immutable. RetainedFrame copies
retain ownership; its view remains valid only while at least one owner exists.
Pool exhaustion, unavailable markers, invalid contracts and ordinary typed
zero data are explicit states. Pool exhaustion does not allocate fallback memory.
