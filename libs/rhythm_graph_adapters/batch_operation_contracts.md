# Pulse and novelty source-stage bindings

These optional AGPL bindings call existing `rhythm_reference_ports` kernels.
`make_batch_operation_node` derives bounded contracts from caller-owned inputs
and explicit settings. It never prepares a kernel or runs DSP. Factory routing,
neutral kind/schema registration and build manifests are connected for this
increment. The optional `rhythm_operations_composition` executable links the
full factory chain and constructs the existing GraphExecutor. A successful
build/startup of that consumer establishes link/composition sanity, not source
parity, full graph execution or rhythm-recognition accuracy.

All inputs are mono Native/Planar, with explicit shape contracts. Payloads are
Float32 except the Int32 lag observations and UInt64 per-frame peak counts.
Numerical axes/parameters never come from transport metadata. Optional input
coordinate payloads are validated metadata; output axes are indices, and tempo,
time, peak-position and frequency meanings are stated below. Packed pairs are
explicit `[position,value]`; they are not implicitly complex audio.

## Exact ports

Outputs are numbered from zero in the order shown. Variable rows permit logical
zero with physical capacity at least one. `N` is the declared input-vector
capacity, `F` matrix rows, `B` matrix columns, `K=maximum_peaks`,
`M=ceil(maximum_bpm)+1`, `S=F*hop`. Counters use UInt64; status flags use UInt8.

| Kind | Required input ports | Ordered output ports |
| --- | --- | --- |
| PulseTempo | 0 PCM vector | 0 BPM scalar; 1 uncorrected BPM; 2 octave multiplier; 3 classifier features[3]; 4 lag histogram[414]; 5 last magnitude[frame_size/2+1]; 6 last compressed spectrum; 7 last OSS ACF[onset_window]; 8 last enhanced ACF; 9 lag-observation count; 10 audio-frame count; 11 selected lag UInt64; 12 last raw complex `[bin][Re,Im]` |
| PulseOnset | 0 magnitude vector, exactly `frame_size/2+1` | 0 raw novelty; 1 source FIR-filtered novelty; 2 compressed spectrum |
| PulseCorrelation | 0 framed OSS vector | 0 selected lag (−1 when no peaks); 1 generalized ACF[N]; 2 enhanced ACF[N]; 3 interpolated peaks[≤10,2]; 4 pulse scores[≤10]; 5 pulse magnitudes; 6 pulse variances |
| PulseTrainScoring | 0 onset vector; 1 candidate-lag vector | 0 selected rounded lag; 1 magnitudes; 2 variances; 3 scores, each candidate-sized |
| LagTempoConsensus | 0 **Int32** lag observations | 0 BPM; 1 uncorrected BPM; 2 octave multiplier; 3 features[3]; 4 lag histogram[414]; 5 observations UInt64; 6 selected lag UInt64 |
| BandNovelty | 0 linear `[frame][band]` matrix | 0 novelty[F−1]; 1 processed band novelty `[F−1,B]`; 2 actual weights `[1 or 4,B]` |
| FixedTempoCandidates | 0 fixed-length novelty vector | 0 candidates[≤104,2]; 1 ACF[N]; 2 smoothed ACF[N]; 3 status; 4 threshold; 5 refinement count; 6 initial peaks[≤N,2]; 7 distribution histogram[104]; 8 distribution centers[104] |
| FourierTempoCandidates | 0 magnitude vector incl. DC/Nyquist | 0 original spectral peaks[≤K,2]; 1 rounded-BPM observations[≤K+1,2]; 2 tempo row[M] |
| WeightedTempoHistogram | 0 rounded-BPM/magnitude observations `[O,2]`; 1 tempogram `[F,M]` | 0 histogram[M]; 1 candidates[≤M,2]; 2 silent flag |
| FrameTempoPath | 0 **explicit packed peak rows** `[F,2*P]`; 1 **UInt64** active peak counts[F]; 2 main-BPM scalar | 0 frame BPMs[F]; 1 learned mean BPM; 2 status |
| PhaseTempoPulse | 0 FFT phase matrix[F,B]; 1 frame BPMs[F] | 0 pulse[≤S]; 1 ticks[≤S,2], first component seconds; 2 last unwrapped phase[B]; 3 last synthesized cosine frame[window length], empty if no synthesis |
| FourierTempoHistogram | 0 full magnitude matrix[F,B]; 1 full phase matrix with identical geometry/axes | 0 BPM; 1 global candidates[≤M,2]; 2 frame BPMs[F]; 3 source tempogram[F,M]; 4 pulse[≤S]; 5 ticks[≤S,2]; 6 status; 7 concatenated raw peaks[≤F*K,2]; 8 rounded BPM observations[≤F*K,2]; 9 raw-peak counts[F] UInt64; 10 global histogram[M] |
| NoveltyTempoHistogram | 0 novelty vector | Outputs 0–10 as FourierTempoHistogram using the source-derived frame/FFT/hop geometry; 11 actual analysis window; 12 actual synthesis window |

PulseTempo's spectral/OSS diagnostics are the **last successfully processed
frame**, with explicit audio-frame/lag counts, not a fabricated full history.
Core `spectral_frame()` additionally exposes borrowed windowed/FFT-input/raw
complex views. FourierTempoHistogram stores only derived peaks/observations/
tempogram already required by the source algorithm; the adapter reads those
views without cloning magnitude/phase histories. FrameTempoPath adapts its
explicit packed rows through one prepared P-peak row buffer, not a history copy.
Novelty derivative i belongs to source input row i+1. Tick pairs contain seconds
as values; the generic matrix row index is not labeled seconds or Hz.

## Settings and defaults

Keys are generic, stable configuration fields. Integers must be actual integral
NumericLiteral values. Floats must be finite. Array values are copied into the
owning descriptor on the cold path.

- PulseTempo: integer `frame_size=1024`, `audio_hop=128`, `onset_window=2048`,
  `onset_hop=128`, `sample_rate=44100`, `minimum_bpm=50`, `maximum_bpm=210`.
  Complete uncentered PCM frames use the original symmetric unnormalized Hamming
  window and source FIR coefficients; no implicit filter redesign occurs.
- PulseOnset: integer `frame_size=1024`. The input contains the exact corresponding
  N/2+1 raw magnitudes. Source scaling/compression/FIR remain separate observable
  stages, with no hidden FFT.
- PulseCorrelation: Float32 `onset_frame_rate=44100/128`, `minimum_bpm=50`,
  `maximum_bpm=210`. PulseTrainScoring uses the explicit onset/candidate geometry.
- LagTempoConsensus: `onset_frame_rate=44100/128`. PulseTempo/consensus optionally
  accept `classifier_minimum[3]`, `classifier_maximum[3]`,
  `classifier_weights[12]` in row-major 3×4 order. Omission preserves the source
  classical linear SVM constants (including the Float32 underflow to zero).
  This is classical scalar classification, not neural inference.
- BandNovelty: `frame_rate=44100/128`, UInt64 `weighting=9`,
  Bool `normalize_bands=false`, optional `weights[B]` for weighting8 Supplied.
  Weighting0…9 = Flat, Triangle, InverseTriangle, Parabola, InverseParabola,
  Linear, Quadratic, InverseQuadratic, Supplied, Hybrid. Hybrid preserves the
  source product of four weighted curves; it is not replaced with one vector.
- FixedTempoCandidates: Float32 `sample_rate=44100`, `minimum_bpm=30`,
  `maximum_bpm=560`, `tolerance_percent=3`; UInt64 `hop_samples=512`.
- FourierTempoCandidates: Float32 `frequency_step=1`, `minimum_bpm=30`,
  `maximum_bpm=560`; UInt64 `maximum_peaks=50`.
- WeightedTempoHistogram: the same min/max plus `tolerance_percent=3` and
  Bool `weight_by_magnitude=true`.
- FrameTempoPath: Float32 `frequency_step=1`, `frame_rate=44100/512`,
  `minimum_bpm=30`, `maximum_bpm=560`, `tolerance_percent=3`,
  `minimum_change_seconds=5`, `induced_bpm=0`; UInt64 `hop=32`;
  Bool `constant_tempo=false`.
- PhaseTempoPulse: `frame_rate=44100/512`, `frequency_step=1`, UInt64 `hop=32`,
  **required explicit** nonempty `synthesis_window`, already max-normalized as
  in the source. There is no hidden window inference from a neighboring FFT.
- FourierTempoHistogram: FrameTempoPath settings plus `maximum_peaks=50`,
  `weight_by_magnitude=true` and the same explicit `synthesis_window`.
- NoveltyTempoHistogram: Float32 `frame_rate=44100/512`, `frame_seconds=4`,
  `overlap=16`, `minimum_bpm=30`, `maximum_bpm=560`,
  `minimum_change_seconds=5`, `induced_bpm=0`; UInt64
  `zero_padding_factor=0`, `maximum_peaks=50`, `window=0`;
  Bool `weight_by_magnitude=true`, `constant_tempo=false`; optional `raw_window`.
  Window0…8 = Hann, Hamming, CenteredHann, Triangular, Rectangular,
  BlackmanHarris62/70/74/92. Source power-of-two rounding, half-valid centered
  frame rule, analysis sum(abs)=2 normalization, synthesis max-normalization,
  zero-phase placement, padding and FFT stay in the existing source composition.
  `dimensions()` exposes that same geometry without running preparation/DSP.

`frequency_step` belongs to the **novelty** FFT, not PCM Fs. It is always an
explicit setting. Supplied Hertz axes must match it. Paired magnitude/phase
contracts/coordinate payloads must agree, and PhaseTempoPulse's row/BPM axes
must agree. These two bindings declare SharedProvenance SessionSequence and
allow different stream IDs within the same explicit source session. The clock
check is structural delivery validation, never a hidden Fs/hop control.

## State, failures and accounting

PulseOnset retains its flux/FIR state between accepted frames. Its outputs have
the explicitly declared input clock so normal History delivery can preserve
frame timing. FrameTempoPath/FourierTempoHistogram/NoveltyTempoHistogram preserve
the source learned mean between batches. Explicit lifecycle/discontinuity reset
uses `reset(true)` and restores induction. PhaseTempoPulse resets synthesis/FIR
per source process call. Other bindings are independently reset batch stages.
Cold clone deep-copies mutable kernel buffers; warm state transfer explicitly
rejects compatibility and the existing runtime resets rather than pretending
continuity was retained.

All full input views, payload domains, geometry, coordinate payloads and buffer
disjointness are validated before source processing. There is no hot allocation,
JSON, model I/O or retained borrowed input. Outputs become available together
only after successful processing/validation. Failure invalidates every output
and applies the existing ResetState policy; arithmetic prefix consumption is not
misrepresented as rollback. Disabled operations emit typed zeros without DSP or
state advance. Model providers, new observers/executors and arbitrary services
are not introduced.

Only PulseCorrelation output4 and PulseTrainScoring output3 declare AnyIeee:
zero-sum normalization deliberately yields the original NaN diagnostic scores.
No other field silently substitutes confidence or a confirmed BPM. NoPeaks,
NoTatum, ThresholdExhausted, NoReference and NoMatchingTempo remain explicit
source enum states. No observations return the source BPM0; an undefined lag0
with observations can still fail. Fixed-candidate distribution diagnostics are
empty when the distribution stage was not run during the current call.

Every owned core has a no-allocation `storage_bytes` query and a readonly
`workspace_bytes` capacity query. Queries exclude instance headers/shared backend
tables/allocator overhead; the factory adds its processor/descriptor once and
the runtime separately counts latches/output/coordinate/stream ownership.
Known simultaneous cold temporaries (FIR coefficients, explicit windows) are
included in conservative cold-peak bounds. Cold prepare rejects actual capacities
exceeding admission before replacing the old kernel. Fixed arrays, including
consensus Gaussian/SVM tables, are embedded and counted by the owning object.
The declared stack budget includes the reference reservation plus the cold
processor-object temporary; it is not a native ABI measurement.

Reusable chains are PulseOnset → explicit OSS framing → PulseCorrelation →
LagTempoConsensus, and BandNovelty → explicit temporal FFT/polar stages →
FourierTempoCandidates → WeightedTempoHistogram → FrameTempoPath → PhaseTempoPulse.
PulseTempo, FourierTempoHistogram and NoveltyTempoHistogram remain faithful
source compositions beside those independently inspectable stages. Source
revisions/notices remain pinned in the existing upstream catalogue and license
headers. These bindings alone do not establish numerical parity or five-library
whole-goal completion.
