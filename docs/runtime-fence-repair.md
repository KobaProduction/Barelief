# Runtime stream fence repair

Author verification, 2026-10-06. This is repair evidence, not independent acceptance.
The independent findings are R1–R3 in `review-final-runtime.md`.

## Changes

- Unavailable ingress runs through the same GraphExecutor as data. Only accepted
  control output updates adapter serials and clears affected presentation state.
  Runtime-produced markers retain generation and producer identity when forwarded;
  old-generation or retired-producer markers neither clear views nor emit controls.
  Data and markers preserve an explicit producer epoch, with frame epoch as fallback.
- NetworkSubscriber checks the decoded type and exact negotiated UTF-8 stream ID.
  A mismatch aborts the binding and enters recovery without delivering data. TCP
  and UART share this receive path. NetworkPublisher encodes the explicit route ID
  passed to `publish`, without changing the caller's frame.
- Publication scopes the inherited producer epoch by the publisher incarnation
  using a deterministic nonzero opaque 64-bit identity. This makes a restarted
  source distinct even when its local epoch repeats, and propagates that change
  through a stable relay. Generation, timestamp and source sequence are untouched.
  Unknown unavailable markers use the last producer for the advertised route;
  the route cache is created during advertise, not grown during frame delivery.
- The subscriber cursor includes transport epoch, effective producer epoch and
  generation. Gap or discontinuity cannot authorize a non-increasing sequence in
  the same identity. Unavailable retains its same-identity sequence fence; a newer
  generation or genuine producer restart permits a fresh sequence. Explicit Stop
  resets the binding cursor; reconnect loss alone retains it.

Only desktop adapter bodies and their targeted tests changed. No portable DSP or
runtime algorithm, wire layout, public API, header or GUI/reviewer code changed.
The concurrent neutral DTO header extraction was rebuilt consistently before tests.

## Reproduction and validation

Before fixes, active-assertion Release tests reproduced all findings:

- `typed_graph_runtime`: older generation revived history after an accepted marker.
- `typed_stream_transport`: wrong-stream payload reached the subscriber.
- `stream_transport_tests --sequence-fence-only`: sequence 10 was delivered after
  sequence 11 and unavailable in the same identity.

After the repairs, the two original CTest cases passed in 20.62 seconds. New
assertions cover forwarded marker provenance, stale/retired marker rejection,
independent branch preservation, newer-generation recovery, producer restart,
geometry change, forward-gap discontinuity, exact stream selection, publication
ID normalization and unchanged caller data. Original codec goldens, roundtrip,
reconnect and backpressure assertions remain.

The first extended guard run passed 12/14 cases and exposed two linked issues:
the UART fixture omitted its required marker stream ID, and a repeated local
producer epoch after process restart was not scoped by its publisher incarnation.
The fixture now names `uart`; publisher scoping resolves the real restart failure.
No restart, numeric, availability or codec assertion was removed.

Final CLI and affected test targets rebuilt successfully. Final scoped CTest:
**5/5 passed in 32.58 seconds**: typed_stream_transport (17.04), typed_graph_runtime
(5.39), distributed_three_cores (9.97), desktop_history_demand (0.07),
desktop_serial_stream (0.06). The three-core check runs real CLI processes and
retains the post-restart numerical comparisons. The history allocation test still
checks zero allocations on unchanged unsubscribed geometry and bounded subscribed
copies independent of retained ring length.

```powershell
ctest --test-dir build-desktop-integration --output-on-failure -R '^(typed_graph_runtime|typed_stream_transport|desktop_history_demand|desktop_serial_stream|distributed_three_cores)$'
```

The private build
is `build-desktop-integration`; Qt 6.10.2/MinGW 13.1, assertions enabled. Tests use
synthetic frames, loopback TCP and fake UART, with no speakers or physical capture.

Physical UART/ESP, native graphics, long stress/soak and Linux were not tested.
No branch, commit, push, SDK installation or user config/output modification was
performed by this repair worker. Independent unchanged probes must be rerun before
the coordinator can claim acceptance.

## R4–R7 follow-up

Subsequent independent probes exposed SDK generation/retired-producer admission,
portable newer-generation sequence reset, desktop retired replay, and portable
same-identity discontinuous replay. Each was reproduced with active assertions
before its repair; earlier R1–R3 acceptance does not cover these newer changes.

Session and desktop subscriber now retain sixteen retired publisher/effective
producer tuples in fixed FIFO storage. Membership is checked before any cursor
or gap mutation. Unknown-generation unavailable keeps the current known fence;
new generation or producer can restart sequence. Rejected old data or markers
produce no callback. Explicit stop resets the bounded history; reconnect loss
alone does not erase the desktop subscriber fence. Older evicted identities are
outside the retained window. Session's tuple array uses 256 bytes, plus a producer
cursor, two indices and an identity-known flag; no dynamic container was added.

Portable executor changes are limited to receiver admission: an accepted marker
advancing generation resets the sequence baseline, while a same-generation marker
preserves it; discontinuity cannot bypass a duplicate/backwards sequence check.
Numerical DSP and prepared plan algorithms are unchanged.

New Session tests use caller-owned arrays and a fixed byte-ring transport with
one-byte partial reads/writes, real packet/fragments and codec parsing. A guarded
allocation counter checks zero C++ allocations across generation changes, rejected
controls/data, fresh producers and sixteen-entry eviction. Existing multi-fragment
tests remain. Timestamp, source sequence and stream ID are asserted unchanged.
Portable new admission cases also run under the existing allocation guard.

Initial targeted R4–R7 checks: dataflow_runtime_tests, embedded_sdk_tests and
typed_stream_transport **3/3 passed in 17.31 seconds**. The standalone SDK host
build uses C++20, active assertions, `-fno-exceptions -fno-rtti`; its targeted
embedded_sdk_tests **passed in 0.04 seconds** (55 build steps).
Source/header freeze is declared to the coordinator; independent runtime and
architecture re-review remains required.

Final application and affected test targets rebuilt successfully. Final targeted
CTest passed **9/9 in 32.38 seconds**: dataflow_runtime_tests (0.02),
embedded_sdk_tests (0.03), typed_stream_transport (16.89), typed_graph_runtime
(5.43), distributed_three_cores (9.71), graph_disconnect_continuity (0.06),
graph_preparation_transaction (0.07), desktop_history_demand (0.06), and
desktop_serial_stream (0.06).

```powershell
ctest --test-dir build-desktop-integration -R '^(dataflow_runtime_tests|embedded_sdk_tests|typed_stream_transport|typed_graph_runtime|distributed_three_cores|graph_disconnect_continuity|graph_preparation_transaction|desktop_history_demand|desktop_serial_stream)$' --output-on-failure
```

This is author repair evidence, not independent acceptance of R4–R7.
