# Optional rhythm graph adapters

This AGPL-3.0-or-later composition boundary depends on `dataflow_runtime`,
`domain`, `rhythm_reference_ports`, `dsp_kernels` and `signal_contracts`.
The portable cores and domain do not depend on this module. The composition
root explicitly installs `make_rhythm_operation_factory(options)`; unsupported
kinds are delegated to the supplied fallback. An absent factory does not grant
support for these operations. Target/registry integration is separate.

`make_interval_operation_node` creates an owning configured node with explicit
input contract/parameters and derived output capacities. It is a production
composition helper; it runs no DSP and reads no upstream topology or frame.

## Independent interval and fluctuation operations

All four bindings consume one mono native Float32 vector at input0. The
runtime supplies proof-backed contiguous Float32 object storage. Other encodings
require an explicit converter. Every input and output selects `FiniteOnly`.
Outputs use ordinary Float32 scalar/vector/matrix contracts, index axes, and
no inherited PCM interval/clock. Batch stamps retain source provenance counters
only; event times and BPM are numerical values. Output axes are absent or an
explicitly declared regular index grid (Values/Frames, origin0, step1); output
clock domain is empty, sample rate0 and tick period1. Output Hertz/Seconds axes,
PCM clocks and explicit coordinate arrays are rejected. Input coordinates are
fully validated against the prepared physical contract, without using them as
computational parameters. Each trigger is a complete independent batch;
EOF is an explicit event, never inferred from a short packet.

| Stable kind | Explicit parameters (Float32 unless stated) | Outputs |
|---|---|---|
| `BeatIntervalHistogram` | None; input values are interval seconds | 0:250 BPM-bin weights; 1:2×3 `[peak][bpm,weight,spread]` |
| `HarmonicTempoCandidates` | `reference_bpm=60` (≥1), `minimum_common_bpm=20` (≥1), `tolerance_percent=5` (≥0) | 0:0…N BPM candidates |
| `TempoChangeSections` | `tolerance=.08` (0…1), `long_region_seconds=20`, `short_region_seconds=4` (both≥0) | 0:start seconds, 1:stop seconds; each0…N |
| `DetrendedFluctuation` | Required `scales`: sorted Float32 array of integer sample counts≥2, below2³¹; repeats retained | 0:score, 1:scale fluctuations, 2:adjacent exponents, 3:source diagnostic enum |

Processing validates the complete input NumericView/stamp/layout/ring/coordinates,
agreement with the prepared physical contract, finite logical input and disjoint
storage before invoking the original kernel. An error invalidates every output;
no old result is published. These four kernels have no analytical state between
independent batches. Compatible state transfer therefore needs no allocation;
cold clone owns independent kernel vectors. There is no input history clone.
Proof-backed Float32 values are checked against logical view values, including
ring/silence semantics. No byte storage is cast to Float32. A single DFA scale is
valid: exponent capacity is1 while its logical published count is0.
Disabled processing publishes typed zeros without invoking/advancing a kernel:
fixed outputs keep their declared extent; candidate/section event vectors are
empty. The DFA diagnostic is `NotProcessed`. These index-only outputs do not
invent a PCM frequency/time axis.

Admission is a pure settings/geometry query. Source `required_workspace_bytes`
queries include all dynamic vectors; `workspace_bytes` reports actual capacities.
Preparation compares actual allocations with the admitted bound atomically.
Descriptor ownership uses the shared runtime DTO ledger; object bytes and kernel
dynamic storage are charged once, runtime latches/output storage separately.
The common `numeric_reference_stack_bytes` reservation applies, including the
histogram's fixed250-bin temporary arrays; it is not a measured MCU ABI claim.

These bindings reuse pinned
[BpmHistogramDescriptors](https://github.com/MTG/essentia/blob/b9fa6cb674ca43dfb94d28d293aeda441c6745db/src/algorithms/rhythm/bpmhistogramdescriptors.cpp),
[HarmonicBpm](https://github.com/MTG/essentia/blob/b9fa6cb674ca43dfb94d28d293aeda441c6745db/src/algorithms/rhythm/harmonicbpm.cpp),
[BpmRubato](https://github.com/MTG/essentia/blob/b9fa6cb674ca43dfb94d28d293aeda441c6745db/src/algorithms/rhythm/bpmrubato.cpp)
and the DFA stage of
[Danceability](https://github.com/MTG/essentia/blob/b9fa6cb674ca43dfb94d28d293aeda441c6745db/src/algorithms/highlevel/danceability.cpp).
Experimental outputs are estimates/diagnostics, not calibrated probabilities.

## Stateful spectral frame stages

`make_spectral_operation_node` composes the following independently reusable
source stages. Magnitude input0 and phase input1 (when required) are separate
mono native Float32 vectors of exactlyN bins. Phase/magnitude must identify the
same source frame/session and coordinate grid; the descriptor explicitly selects
`SessionSequence` / `SharedProvenance`. Stream IDs may differ. Settings never
derive FFT size, Fs, frame period or coefficients from a produced FrameStamp.

| Stable kind | Settings | Intermediate/output ports |
|---|---|---|
| `PolarNovelty` | UInt64 `method=0`: Complex / ComplexPhase / BeatEmphasis | 0:N source per-bin distances |
| `HistogramOnset` | UInt64 `method=0`: Phase / WeightedPhase / SpectralDifference; `histogram_bins=10`, minimum0, maximumπ, magnitude_threshold.1 | 0:descriptor; 1:H center-weighted histogram counts; 2:N rescaled histogram values |
| `SpectralWhitening` | `frame_period_s=512/44100`, `relax_time_s=250`, `floor=1e-4` | 0:N whitened values; 1:N peak history |
| `OnsetMagnitude` | Whitening settings above, Bool `whiten=false`, `compression=0` | 0:N processed values; 1:N whitening peaks |
| `SpectralRmsFlux` | No extra settings | 0:source sqrt(sum(magnitude²))/N positive RMS difference, including first-frame suppression |

Histogram setting keys are `histogram_minimum`, `histogram_maximum` and
`magnitude_threshold`. All Float32 scalar settings must be finite; source
validation applies. Methods are bounded0…2; histogram bins are positive.
Whitening period/relax/floor are positive; compression is nonnegative.
SpectralDifference requires magnitude0 only: the source consumes magnitude and
previous magnitude, and the adapter calls the existing kernel's magnitude-only
entry. Phase1 is required only for Phase/WeightedPhase and PolarNovelty. The
magnitude-only branch neither invents nor validates a phase row.

Optional HeldValue ports preserve source warm-control history: OnsetMagnitude
port10 is a UInt8 flag0/1, port11 is Float32 compression≥0; both whitening
operations accept Float32 floor>0 at12 and relaxation seconds>0 at13. All controls
are validated before changing any source state. An unavailable optional control
retains current/default settings. Optional UInt8 scalar65500 uses the shared
continue/batch-complete/EOF/reset protocol. Complete frame geometry remains
explicit; these stages do not synthesize partial frames or perform transport
framing. Every input/output explicitly selects `FiniteOnly`.

These outputs keep input0's **declared frame clock**, verified for every output
at admission; their payload axes remain ordinary indices. Successful processing
preserves that frame's timestamp/source interval and clears borrowed stream text.
Thus differently sized distance, histogram and scalar payloads do not acquire
different clocks merely because their extents differ. Disabled processing emits
typed zeros without changing source state or applying controls; it retains the
incoming source tick/provenance and makes no fabricated source sample interval.

Each prepared instance owns its histories, scratch and output row. Pure planning
reuses source `storage_bytes`; actual `workspace_bytes` recursively includes all
owned vector capacities. Cold preparation refuses an exceeded bound atomically.
Cold cloning deep-copies the current source histories. Repreparation explicitly
declares reset for vector-history stages via incompatible state transfer; RMS
retains its scalar state without allocation when configurations agree. Arithmetic
failure invalidates all outputs and invokes the declared reset policy; it does
not pretend to roll back a consumed prefix. No input/full-spectrogram history is
retained or cloned during processing, and no FFT is repeated by these stages.

The stage mathematics stays in the pinned
[Essentia onset kernel](https://github.com/MTG/essentia/blob/b9fa6cb674ca43dfb94d28d293aeda441c6745db/src/algorithms/rhythm/onsetdetection.cpp)
and existing ports of aubio
[specdesc](https://github.com/aubio/aubio/blob/ad5cf975aed08cc4562dd008cf9f83b12b82ffb8/src/spectral/specdesc.c),
[histogram](https://github.com/aubio/aubio/blob/ad5cf975aed08cc4562dd008cf9f83b12b82ffb8/src/utils/hist.c)
and [adaptive whitening](https://github.com/aubio/aubio/blob/ad5cf975aed08cc4562dd008cf9f83b12b82ffb8/src/spectral/awhitening.c).

## Descriptor, decision and buffered stages

The owning production helpers in `descriptor_composition.h`,
`event_composition.h`, `buffered_composition.h` and `contrast_composition.h`
document settings, defaults, ranges and port geometry. They prepare contracts,
not source data. All eleven stages below reuse existing R/D kernels.

| Stable kind | Existing source kernel | Outputs |
|---|---|---|
| `OnsetDescriptor` | `PreparedOnsetDescriptor`; nine source presets plus old HFC alias | scalar0; histogram methods also weighted-counts1 and rescaled-values2 |
| `SpectralOnset` | `PreparedSpectralOnset`; six source presets | scalar0; complex methods distances1; MelFlux source amp2db bands1 |
| `TriangularSpectralBands` | `PreparedTriangularSpectralBands` → existing generic `MelFilterBank` matrix projection | bands0, center-Hz-values1, weights2[band,bin] |
| `AdaptivePeakPicker` | source forward/reverse recursive filtering, median/mean, interpolation | position0, picked1 UInt8, thresholded2, mean3, statistic4 |
| `OnsetDecision` | source startup/minimum-interval/delay acceptance | position0, local-sample1 UInt64, detected2 UInt8 |
| `PcmSilence` | existing `pcm_is_silent`, explicit dB threshold | silence0 UInt8 |
| `BufferedMaximum` | `PreparedBufferedMaximum`, source causal/noncausal initialization/reset | filtered0 |
| `CausalAverage` | `PreparedCausalAverage` → existing recursive FIR | averaged0 |
| `BufferedBandFlux` | existing pair API, persistent source maximum + shared positive difference | novelty0, filtered-reference1, differences2 |
| `ContrastPeakPicker` | existing source contrast picker, average/maximum/combine | event-seconds0, averages1, maxima2, detections3 UInt64 |
| `ContrastPeakAccumulator` | existing source streaming boundary/accumulator | accumulated event-seconds0, count1 UInt64 |

Only phase-dependent methods declare/require phase input1. The descriptor
dispatcher uses the common values-only kernel for energy/HFC/flux/KL/MKL,
and histogram SpectralDifference uses its magnitude-only entry. No synthetic
zero phase or repeated FFT is inserted. Triangular filter edges, warp, power,
normalization and logarithm remain explicit source settings; the operation does
not silently substitute a different Mel construction. Its center frequencies
are data values in a separate port; weights and band vectors have index axes.

The shared small frame-I/O helper checks complete NumericViews, prepared physical
contracts, logical/proof-backed Float32 agreement, coordinates and buffer aliasing
before processing. Phase pairs match frame/session identity. BufferedBandFlux's
delayed1 is an explicit required HeldValue, with the same session/band grid but
its own timestamp; no upstream topology or hidden history is inspected. All
operations select `FiniteOnly` per port. Disabled calls emit bounded typed zeros
without source processing or state advance.

Frame-stage outputs retain the explicitly declared input0 clock. Contrast event0
has an empty clock and provenance-only batch stamp: its seconds are source
numeric values, not input tick counts. Contrast diagnostics retain the source
frame clock. Explicit EOF may flush the accumulator without a PCM chunk; an
available final chunk is consumed once before source finish/reset. Chunk length
never signals EOF. Histories and source quirks persist between ordinary calls;
reprepare declares reset except allocation-free compatible scalar decision state.
Cold clones deep-copy all nested buffers. Buffered/contrast arithmetic failures
retain the source's consumed prefix and invalidate outputs, rather than claiming
rollback; complete validation failures consume no source state.

Pure resource queries account nested kernels, owned rows and known simultaneous
cold temporaries, including triangular coefficient/support/coordinate staging
and the moving-average numerator. Actual capacity queries and cold atomic bounds
use the same shared checked ledger. Descriptor/runtime/output storage is charged
by its existing owner; no cold prepare occurs during capability assessment.

These stages preserve pinned aubio
[peakpicker](https://github.com/aubio/aubio/blob/ad5cf975aed08cc4562dd008cf9f83b12b82ffb8/src/onset/peakpicker.c)
and [onset decision](https://github.com/aubio/aubio/blob/ad5cf975aed08cc4562dd008cf9f83b12b82ffb8/src/onset/onset.c),
plus Essentia
[TriangularBands](https://github.com/MTG/essentia/blob/b9fa6cb674ca43dfb94d28d293aeda441c6745db/src/algorithms/spectral/triangularbands.cpp),
[SuperFluxNovelty](https://github.com/MTG/essentia/blob/b9fa6cb674ca43dfb94d28d293aeda441c6745db/src/algorithms/rhythm/superfluxnovelty.cpp)
and [SuperFluxPeaks](https://github.com/MTG/essentia/blob/b9fa6cb674ca43dfb94d28d293aeda441c6745db/src/algorithms/rhythm/superfluxpeaks.cpp).

## PCM/context and batch source compositions

`pcm_stage_composition.h` provides `make_pcm_stage_node` for `HopSpectrum`,
`PcmOnsetSequence`, `PcmContextTempo`, `ContextBeatTracker` and
`ContextBeatSequence`. Its public contract lists all explicit settings, warm
controls and intermediate ports. Frequency axes use the block's own Fs/N;
local acquisition coordinates include source zero padding and differ from
incoming provenance timestamps. Onset reset preserves analytical histories as
in the source; context restart clears analysis. Tatum polling is explicit.

`make_batch_operation_node` and [the batch API map](batch_operation_contracts.md)
compose the thirteen pulse/novelty/tempo-histogram stages through the same
factory. Their source kernels and per-operation domain/resource policies stay
in the batch binders; no source mathematics is duplicated in the router.

`predictive_composition.h` adds the five predictive/comb/PCM source stages,
including all ten ODF choices. The tracker requires an explicitly supplied
whole-window SincBestQuality provider; absent support remains unsupported.
Raw source FFT order and independent callback-rebound clones are retained.
Optional held inputs expose source warm method, tempo/fix/release, and standalone
prediction period/indication setters. Unchanged held values do not repeatedly
reseed source histories. The public header lists their exact per-kind ports;
hop/frame/window geometry remains cold preparation. Optional protocol65500
uses the existing explicit continue/batch/EOF/reset mechanism.

`sequence_source_composition.h` adds ten feature-fusion, onset-selection,
onset-rate, interval/PCM-summary, activation, spectral-difference and trajectory
stages. Matrices are written synchronously into bounded executor outputs;
input PCM/spectrograms are never retained or cloned. Its public header records
source matrix order, reset/chunk behavior and intermediate ports.

The factory also routes the twenty-two stages described in
[tracking operations](tracking_operations.md) and the loop/loudness/meter stages
from `loop_composition.h` / `loudness_composition.h`. Source window/raw-amplitude
policies remain explicit; calibrated projections are separate operations.
