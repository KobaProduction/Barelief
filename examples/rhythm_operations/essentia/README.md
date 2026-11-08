# Essentia RhythmExtractor2013 reference graph

The optional AGPL preset `essentia-reference` implements the pinned Essentia
`b9fa6cb674ca43dfb94d28d293aeda441c6745db` Degara and MultiFeature methods in
one graph. The existing `PreparedGraph`/`GraphExecutor` executes every stage.
The research operation factory and memory profile are explicit composition inputs.

The live source publishes independent eight-second sessions at 44100 Hz. The
finite builder has a separate PCM ingress: its explicit `input_samples` selects
the complete native input prefix within `maximum_native_pcm_samples`. No EOF
samples or eight-second segmentation are inserted into the finite route.

## Explicit processing

The two frontends use `TemporalFrameSequence`, normalized symmetric Hann
coefficients, `ArrayMultiply`, half-frame `IndexExtract(mode=1)`, and the named
`kiss_fft130_float32` `RealFourier`. Their lengths are2048 and their hops1024/512.
Raw polar values preserve source square/sum/square-root and unconditional atan2.
The complex and RMS onset operations receive these spectra directly.

MelFlux receives an explicit40-band log-power matrix. The graph projects
source HTK/Log10Mel unit-sum triangular weights over0..4000Hz, squares input
magnitudes in `FilterMatrixProjection`, clamps power at1e-10, applies log10,
and multiplies by20. Its descriptor computes positive differences and suppresses
the first frame. It contains no filter-bank or logarithm preprocessing.
Two independent twenty-band projections concatenate along the band axis, retaining
the original band order while keeping each coefficient array within32768 values.

The emphasis branch exposes ERB projection, population-standard-deviation
normalization along time, last-value extension by16, a17-tap causal FIR,
aligned mean subtraction, and clamping. Only the first real frame prefix is
thresholded; the appended16 normalized samples remain unchanged. An explicit
transpose and zero padding feed the native Float32 real FFT. Real/imaginary
squares, their sum, Hermitian expansion, inverse FFT, and division by actual
lag overlap counts implement unbiased correlation. The graph then applies
harmonic-comb coefficients, row maxima, maximum normalization, a0.6 quantile
mask, weighted bands and a row sum. No direct-correlation replacement is used
for this source FFT algorithm.

Each of five candidates exposes maximum normalization, endpoint-preserving
linear expansion, edge padding, causal FIR subtraction, temporal framing,
unbiased direct autocorrelation, harmonic comb, bounds mask and sum
normalization. `PeriodPath` receives the resulting normalized observation
matrix and performs only noise injection and the period HMM. The full-ODF
filter's final state connects explicitly to the comb filter's initial state;
comb rows continue that same FIR state in source order. `RenewalBeatDecoder`
receives the unthresholded expanded ODF, decoded periods and block ends.

The candidate barrier preserves the shared C RNG order. `EssentiaAgreement`
selects one complete candidate. Its raw source confidence uses integer
`log2(40)==5` minus entropy; it is not a probability. `BeatIntervalSummary`
receives selected ticks and supplied confidence explicitly.

## Ports and results

Typed selectors are declared in `rhythm_graph_adapters/essentia_ports.h` and
`domain/numeric/array_processing_ports.h`; numeric wire IDs remain internal.
Candidate nodes50..54 expose Mask0, StatePath1 and Times2. Degara and
MultiFeature summaries remain nodes62/61. The combined preset keeps final
views103/114 (ticks) and108/107 (BPM/confidence).

The interval, time and frequency values retain their actual source meanings.
Intermediate index axes never acquire invented Hz/seconds labels. All working
storage is bounded and prepared before processing. Batch execution resets
source histories and does not allocate on the hot path.

The pipeline test exports complete finite/live observations. Independent
numerical acceptance uses `tests/rhythm_originals/essentia_pipeline_oracle.py`
against the pinned native implementation. The retired complete PCM, legacy,
Percival, novelty-FFT, loudness and activity recipes have no graph dispatch or
alternative execution path.

The fresh 2026-11-08 canonical whole-input run completes all six cases and produces
92 output files bitwise identical to the previously qualified v6 capture. This
preserves that native comparison lineage: the defined-memory global source passes
12/12 families and 84/84 fields. The raw original remains 10/12, with two upstream
global-buffer defects recorded as strict failures. The new run is whole-input
evidence, not a new live-engine or native GUI capture. Binary/source identities,
the byte comparison and both native classifications are recorded in
`cmake-build-release/Testing/atomic-rhythm-20261108-013954/essentia/acceptance.json`.
