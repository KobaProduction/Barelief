# madmom SuperFlux and classical beat search

The live preset `madmom-superflux-reference` is built by
`numeric_madmom_combined_reference_example(source_rate)`.
It follows [madmom 27f032e8](https://github.com/CPJKU/madmom/tree/27f032e8947204902c675e5e341a3faf5dc86dae)
for classical SuperFlux evidence and Fixed, Local and CRF beat estimation.

## Shared frontend

One PCM source feeds the explicit shared graph.

```text
AudioSource → PcmResampler → FrameBuffer → ArrayMultiply → RealFourier
                                ↓              ↑               ↓
                           ArrayLength → WindowFunction    AmplitudePhase
                                                                ↓
                     FilterMatrixProjection groups → ArrayConcat
                                                                ↓
                        LogCompression → OnsetStrength → FeatureAggregate
                                                                ↓
                                      NumberAccumulator → FrameStride
```

The source rate is explicit and may be 8000–96000 Hz. The existing FIR resampler
produces 48000 Hz. Complete frames have 2048 samples and hop 240 samples, giving
200 ODF frames per second. The symmetric Hann coefficients and windowed PCM are
Float32. A frame timestamp identifies its first sample. The frontend publishes
after collecting the complete frame.

RealFourier publishes separate Float32 Re and Im planes for bins `0…1024`.
AmplitudePhase consumes both complete planes and publishes the raw magnitude and
phase for bins `1…1024`. Its third output is signed DC. The recipe selects
`normalize=false` and `real_endpoints=true`.

The original logarithmic bank covers bins `0…1023` and has zero DC weights.
Cold construction shifts the coefficients onto bins `1…1024` and sets the
Nyquist weight to zero. The bank has 24 bands per octave, 30–17000 Hz,
reference frequency 440 Hz, unique corners and normalized triangles. Five
projections respect the 32768-value parameter-array limit. ArrayConcat preserves
all 137 bands in source order. Each projection exposes weights and Hz centers.

LogCompression computes `log10(1+x)`. OnsetStrength applies positive differences
with lag two ODF frames and a three-band frequency maximum. The first two frames
have zero difference. FeatureAggregate uses the existing compensated Float32 sum.

NumberAccumulator 27 keeps at most 2000 ODF frames with `filled_only=true`.
Warmup snapshots contain the actual occupied count, growing from one frame;
they have no artificial zero prefix. FrameStride 31 publishes every 40 frames
and materializes the selected snapshot chronologically. Samples retain the
200-Hz analysis cadence.

## Beat outputs

All temporal branches derive their explicit inputs from the same `31:0` snapshot.

| Method | Node | Beat seconds | Exact frame indices | View |
| --- | --- | --- | --- | --- |
| Fixed | 80 | `80:0` | `80:1` | 82 |
| Local | 81 | `81:0` | `81:1` | 83 |
| CRF | 4 | `4:0` | `4:1` | 11 |

Fixed and Local share Float32 Hamming smoothing (`90 → 91 → 92`, length 28).
Hamming generators explicitly select centered-cosine evaluation (`evaluation=2`);
the default window arithmetic stays unchanged. The runtime test publishes the
four size constants through `process_clock` before each recording.
Fixed uses `CombFilterBank 93 → AxisMaximum 121 → AxisSum 122`.
The bank explicitly declares delays 48–300 and Float32 alpha .79. Maximum output
2 retains every tied per-frame maximum; Sum explicitly selects pairwise
accumulation (`accumulation=1`) along time. Sequential remains the generic default.
Histogram convolution 96 applies nine Hamming coefficients; AxisMaximum 97 and
ArrayGather 98 select the interval from the bank's explicit delay table. Its
decoder receives that interval on input 1 and smoothed activations on input 0.

Local exposes the comb histogram calculation. IndexExtract 99 in axis mode
(`mode=1`) appends 1999
zeros, CombFilterBank 100 evaluates delays 48–300 with alpha .79 and resets for
each snapshot. AxisMaximum 101 retains every tied row maximum on output 2.
AxisSum 102 performs a sequential Float32 cumulative sum along time. IndexExtract
103 in axis mode skips the first 1999 rows and selects up to 2000 rows, preserving the
actual activation count during warmup. Convolution 104 smooths each row with the
same nine coefficients. AxisMaximum 105 and ArrayGather 106 publish one interval
for each possible prediction index. The local decoder uses the interval at its
current prediction, finds the nearest admissible beat in a centered 4000-frame
context, and predicts the next context from that beat.

This prefix construction relies on the preset's 2000-frame history and
2000-frame half-context with an activation count bounded by 2000: every
centered context contains all input activations,
with leading zeros and a varying zero suffix. Leading zeros preserve the comb
state and cumulative sums preserve the source's sequential addition order.
Changing the history or context geometry requires changing this explicit graph.

CRF uses Double activation convolution `108 → 111` and the explicit chain
`CombFilterBank 112 → AxisMaximum 123 → AxisSum 124` over delays 50–600.
AxisSum 124 explicitly selects Double pairwise accumulation. This preserves the
original NumPy reduction order for its contiguous time axis, including strict
peaks that disappear under sequential rounding. No storage layout implicitly
selects the policy; cumulative sums remain sequential.
The same common CombFilterBank preserves Double state with Float32 alpha
promoted in its recurrence. Seven-coefficient histogram convolution 115 feeds
strict wrapped OrderedPeaks 116. ArrayGather 117 selects their coordinates.
NumericMap 118/119 explicitly compute interval → BPM → truncated interval,
preserving the source's Double arithmetic and top-five ordering. NumericMap 120
narrows the separately exposed smoothed activations for the Float32 CRF decoder.
CRF publishes trial intervals on output 2 and normalized path scores on output 3.
Its sigma is .18. No decoder computes a smoothing window or tempo histogram.

The common primitives use stable semantic PortIds declared in
`domain/numeric/atomic_operation_ports.h`; captions do not determine routing.

Beat seconds and exact indices use the beginning of the current snapshot as
their origin. Index `k` corresponds to its left edge at `k/200` seconds. After
the context rolls, zero refers to the new snapshot beginning. Local and CRF
estimates may change when a later snapshot supplies different context.

## Execution and checks

The graph executes through PreparedGraph and GraphExecutor. Coefficients,
numerical plans and bounded scratch are prepared before processing. Device I/O
belongs to the desktop source adapter. Each temporal snapshot is an independent
offline estimate.

`rhythm_reference_gui_tests` checks the installed preset, shared ODF routing and
publication from the three beat outputs. Mathematical kernels retain their own
regression and pinned-source checks. CRF diagnostics check guarded backtraces
and valid empty results independently of GUI.

The main application connects the neutral BSD DSP adapter composition.
Reference checks use synthetic input without speaker playback. Their evidence
covers the selected settings and supplied input.

## Native-source pipeline qualification

`madmom_reference_pipeline_tests` executes the actual combined preset with a
bounded NetworkInput replacing the physical source. It retains packet-size
invariance, explicit recording reset and repeat, a mid-recording RuntimeContext
clone, history rollover, silence, arbitrary-origin coordinate validation,
named Fourier-provider selection/cloning/rejection, and no hot C++ allocation.
No old standalone-mode builder or whole-wrapper kernel is required.

An optional new output directory exports the shared frontend, every occupied rolling snapshot (up to 2000 frames)
and its ODF endpoint, Fixed/Local supplied intervals, Double
CRF preprocessing, supplied trial intervals, and all decoder outputs. Beat
outputs remain `80:0/1`, `81:0/1`, `4:0/1`; CRF diagnostics are `4:2/3`.
Warmup arrays use explicit UInt64 offsets. Fixed snapshots shorter than their
chosen interval retain explicit rejection records; the oracle requires the
corresponding pinned-source IndexError and treats every successful source return
against a graph rejection as a strict mismatch. CRF NoPath is an available empty
result. `graph.json` records actual prepared node contracts and edges. The final
`regression.json` is written only after the C++ regression checks complete.

After source fan-in, the integration owner runs the target and external oracle
with new artifact directories, preserving all prior evidence:

```text
madmom_reference_pipeline_tests.exe NEW_EVIDENCE
python tests/rhythm_originals/madmom_crf_safe_oracle.py build SOURCE_CACHE NEW_DEFINED_BUILD
python tests/rhythm_originals/madmom_pipeline_oracle.py NEW_EVIDENCE --source-cache SOURCE_CACHE --defined-crf-root NEW_DEFINED_BUILD
```

The Python oracle verifies immutable source hashes and the defined-memory CRF
build. Each exported snapshot is checked against original comb, smoothing and
interval operations; supplied-input source decoders isolate the graph's explicit
contracts. Separate gates run the original tracker on exported ODF and on ODF
computed independently from the original PCM frontend. The pinned Fixed/Local
interval-estimator interface bridge delegates the original mathematical steps;
its raw interface result is recorded separately. Raw CRF is an optional isolated
diagnostic (`--raw-crf`), never a substitute for the mandatory safe-source gate.
A strict mismatch remains a failure. Source presence and these commands do not
assert a numerical PASS before the resulting checks have actually run.

The fresh 2026-11-08 combined, rollover, silence and provider captures qualify all
79 exported rolling snapshots with 1335 passing numerical comparisons and no
strict numerical failures. Original rejection outcomes remain explicit. The
explicit pairwise reductions use the original tolerances; they do not relax CRF
score comparisons. This is bounded synthetic pipeline evidence, separate from
GUI acceptance and musical tracking quality. The complete capture/source ledger is
`cmake-build-release/Testing/atomic-rhythm-20261108-013954/madmom-pairwise-fresh-20261108/madmom-parity.json`.
