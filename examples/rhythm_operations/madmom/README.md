# madmom SuperFlux and classical beat search

This composition uses pinned
[madmom 27f032e8](https://github.com/CPJKU/madmom/tree/27f032e8947204902c675e5e341a3faf5dc86dae).
It is an explicit non-neural recipe, combining the documented
`SpectralOnsetProcessor(onset_method='superflux', fps=200,
filterbank=LogarithmicFilterbank, num_bands=24, log=np.log10)` frontend with
classical tempo and beat postprocessing. SuperFlux is onset evidence, not a
replacement for `RNNBeatProcessor` probabilities. No neural weights are needed.

`numeric_madmom_reference_example(native_source_rate, mode)` constructs three
separate graphs:

| Preset ID | Beat stage | Explicit source settings |
| --- | --- | --- |
| `madmom-superflux-fixed` | `BeatDetectionProcessor` / fixed interval search | Comb, 40–250 BPM, alpha .79, activation Hamming .14 s, histogram Hamming 9 bins, look-aside .2 |
| `madmom-superflux-local` | `BeatTrackingProcessor` / local interval search | Same tempo settings; look-ahead 10 s in each direction of the supplied snapshot |
| `madmom-superflux-crf` | `CRFBeatDetectionProcessor` | Comb, 20–240 BPM, activation Hamming .09 s, histogram Hamming 7 bins, sigma .18, top 5 intervals, no factors |

These are rolling snapshot estimates. Local and CRF results may change when the
snapshot changes. They do not announce irrevocable real-time beat confirmations.
The separate peak branch is diagnostic `peak_picking` with threshold .5,
pre/post maximum 1 frame, pre/post average 0, no smoothing or event combination.
It is not silently substituted for any beat decoder.

The separate finite research builder
`numeric_madmom_whole_reference_example(pcm_samples, mode, source_rate)` uses
`madmom-whole-superflux-{fixed,local,crf}` identities. It initially accepts only
native 48000 Hz and rejects other rates explicitly. Its PCM frontend is shared
with the recipe above. Whole CRF explicitly selects `WindowFunction precision=1`:
Double symmetric Hann coefficients and the mixed Float32 PCM/Double window
product remain Double through the existing reference real FFT. Re/Im are
published as Float32 before magnitude, matching the original complex64 boundary.
The same whole-CRF branch explicitly selects `magnitude_norm=scaled_ratio_fma`,
reusing the existing Float32 NumPy complex-absolute operation order.
Its raw `normalize=false` magnitude does not use the window calibration edge.
Whole CRF also selects `LogFrequencyFilterConfig::precision=DesktopReference`
before composing the explicit filter matrix. The source construction computes
triangle ramps in Double, publishes Float32 coefficients, and normalizes with
the source Float32 pairwise sum and Float32 division. Its matrix projections
explicitly use `accumulation=sequential_fma` with `compensated=false`.
The logarithm and final compensated SuperFlux sum retain their existing policy;
the recipe does not reproduce OpenBLAS panel blocking or add an OS math backend.
Whole Fixed/Local and the rolling presets retain their Float32 frontend and
separate saved project identities. For N real input samples, complete framing produces
`F = 1 + (N - 2048) / 240` with integer division (N must be at least 2048).
N=514761 yields F=2137 and 73 samples beyond the last complete window. That tail
is consumed as real PCM but does not create a padded frame or a resampler flush.

This finite graph has two explicit ingresses on the same PreparedGraph. Feed
PCM to source 1 and collect every Float32 scalar at output 3, including the
initial zero-valued SuperFlux warm-up frames. The collector owns storage for
exactly F values before processing starts. Output 3 keeps the PCM clock
`local`, 48000 Hz, tick 1/48000, and timestamp `k*240` for frame k; scalar
publication clears its PCM sample interval. Its inherited `source_sequence`
may repeat within one input packet and is not an ODF frame counter.

After all N samples have been processed and all F scalars collected, submit
exactly one complete vector to source 31 outside callbacks. Query the prepared
port contract instead of inventing an ingress layout. It is a fixed-F Float32
Numeric/Values vector with a Time axis, explicit `madmom-whole-odf` clock
(200 Hz, tick 1/200) and regular seconds coordinates `k/200`. These coordinates
refer to the left edge of each complete PCM window. The fresh batch stamp starts
at timestamp 0 and has no PCM sample interval; do not copy the last PCM frame's
begin/count or confuse batch completion with a value's coordinate. Tempo/CRF
still receive their explicit 200-Hz configuration, independently of metadata.

Only this EOF batch drives Tempo 35 and beat decoder 4. The finite graph omits
rolling accumulator 27, stride gating, and rolling peak branches 32/33/36.
No partial last snapshot is labelled whole-input output. Beat seconds and exact
indices refer to the whole ODF origin, and event bounds scale with F. A caller
must reject incomplete collection, extra samples or a second EOF submission;
it must serialize calls and retain no borrowed callback views. The regression
entry point is `madmom_reference_pipeline_tests --whole <new-evidence-dir>`.
Execution and source-comparison results are tracked in the workspace issue;
the existence of this builder does not establish a successful native comparison.

The pinned native CRF leaves its terminal state and back-pointers uninitialized
when no path is reachable. Zero activations, or positive evidence starting after
the first-interval prior support, can reach that branch even with values in
[0, 1]. The checked reference copy stops before the first invalid access; the
separate defined source copy initializes sentinels and returns an empty result
for this case while preserving the arithmetic and selected paths of valid
trials. Raw crash/invalid-path evidence remains separate from this comparison.
Neither copy clamps SuperFlux to probability values or changes the CRF formula.

The portable CRF exposes `Error`, `NoPath` and `Path`. A finite nonnegative
input with no tempo or reachable path succeeds with available empty beat
seconds and indices, plus current diagnostics. Attempted unreachable intervals
have score `-Inf`; no-tempo has the source candidate sentinel `[NaN, 0]` and
no attempted intervals. Malformed inputs, numeric overflow or an inconsistent
selected finite backtrace return an error with a reason and expose no stale
result. Lower-level decoder/search boolean results still mean `Path` only.
Short-input model scratch is reserved before processing without padding the
actual activation input. See the adjacent CRF contract in
[runtime API](../../../docs/reference/runtime-api.md).

The processing graph is:

```text
AudioSource → PcmResampler(48000) → FrameBuffer(2048, hop=240)
                                          ↓
                     ArrayLength → WindowFunction(symmetric Hann)
                                          ↓
                   ArrayMultiply → RealFourier → AmplitudePhase(raw magnitude)
                                          ↓
         FilterMatrixProjection(groups) → ArrayConcat → LogCompression(log10(1 + x))
                                          ↓
             OnsetStrength(lag=2, max_size=3) → FeatureAggregate(sum)
                    ↓                                  ↓
     NumberAccumulator(2000, filled_only)    Spectrogram(2000, filled_only)
                    ↓                                  ↓
             FrameStride(40)                     FrameStride(40)
               ↙          ↘                            ↓
       TempoAnalysis    fixed/local/CRF         ActivationPeakMatrix
```

Only the existing `PreparedGraph`/`GraphExecutor` executes these stages. FFT,
window generation, resampling, logarithmic triangles, log compression, positive
max-filtered differences, smoothing, comb histogram and beat search reuse the
existing kernels. The composition stores the explicit band-major filter weights
and Hz centers in the graph. Each projection has at most 32 band rows, so its
1024-bin coefficient parameter fits the unchanged 32768-value array limit.
ArrayConcat rejoins group outputs in source band order; no numerical reduction
crosses a group boundary. Individual projections expose the Hz-center values;
the merged band's axis is an index, and no Hertz axis is invented after concat.
Both FrameStride nodes explicitly set `materialize=true`: every selected snapshot
is copied in chronological order into the prepared output buffer before entering
the temporal operations. Skipped frames do not copy the history. Borrowed ring
views are never retained as asynchronous operation inputs.
It does not infer DSP settings from upstream kinds
or from incidental metadata.

The filterbank has 24 bands **per octave**, 30–17000 Hz, A4=440, unique quantized
corners and normalized triangles. It is constructed on the source physical
`k * 48000 / 2048` grid. RealFourier exposes k=1…1024, whereas source madmom uses
k=0…1023. Cold construction verifies that every DC weight is zero, shifts the
remaining weights, and assigns exactly zero to the additional Nyquist bin.
This preserves the projected bank, without inventing a second FFT or projection.

PCM, raw magnitude, filtered magnitude, logarithmic magnitude, SuperFlux,
onset peak indices, tempo candidates/strengths, raw/smoothed interval histogram
and beat seconds/indices are exposed as real node outputs. IndexExtract selects
the first candidate's BPM for the scalar tempo display. CRF also exposes its
trial intervals and normalized scores. Histograms are indexed by frame interval
(48…300 for fixed/local, 50…600 for CRF), not Hertz. The separate interval-values
view shows those explicit coordinates; the histogram line's x axis remains a
bin index into that interval vector. Candidate rows contain BPM
and relative strength. Strength is not a calibrated confidence probability.

Beat payload times are seconds **from the beginning of the current ODF
snapshot**. Onset peak positions and exact UInt64 beat positions are frame
indices within that snapshot. A frame index corresponds to a left edge at
`index * 240 / 48000` seconds; its even-frame reference center is 1024 samples
later (the symmetric Hann center is 1023.5 samples later), and its
complete-frame availability is 2048 samples later. The transport stamp separately
carries the source frame timing and continuity; FrameBuffer timestamps the first
sample of each complete frame, independently of input packet boundaries. After the 2000-frame context rolls,
zero is the new snapshot origin. Do not treat these relative event values as
absolute recording timestamps.

The following differences from unconstrained source defaults are explicit:

- Analysis Fs is fixed at 48000, yielding exactly 200 frames/s with integer hop
  240. Other native rates use the visible existing FIR resampler. This is the
  project's resampling policy, not madmom's file decoder resampler. The supplied
  numerical exporter compares native 48000, where this stage is identity.
- Frames are complete and left aligned (`origin='future'` with an explicit frame
  count in the oracle). There is no initial zero prefix or EOF partial frame.
  Source's default centered offline padding is not claimed. Lag 2 follows the
  existing Hann/.5-overlap lag helper, and the first two novelty values are zero.
- Scalar ODF context is bounded to 2000 frames (10 seconds), filled only, and
  tempo/beat snapshots are evaluated every 40 frames. Live ODF state continues
  across snapshots; each temporal snapshot is an independent offline call.
  An owner starting a separate recording explicitly resets both history nodes
  (27 and 32); a source epoch change alone preserves compatible history.
- Published values are Float32. The CRF recipe explicitly sets `precision=1`
  (`DesktopReference`) on TempoAnalysis and CrfBeatSequence: activation and
  histogram Hamming/convolution, comb history/accumulation, and strict peak
  decisions retain Double until candidate extraction. Comb alpha remains
  Float32, promoted in the Double recurrence, matching the Cython argument.
  CRF selects intervals from Double candidate tempi; its independently
  truncated smoothing narrows to Float32 before the existing CRF decoder.
  Reference mode supports offline mono Comb without interpolation or factor
  expansion; unsupported combinations reject preparation atomically. The
  default `precision=0` and fixed/local recipes retain Float32 behavior.
- Source NumPy Hann/window products are Double before Complex64 FFT output.
  Whole CRF preserves that boundary; rolling and whole Fixed/Local retain the
  shared Float32 frontend. Sum order follows existing Float32 reducers. Numerical tolerance
  and any changed discrete decisions require actual comparison, not a parity
  claim inferred from the same formula.
- Direct pinned fixed/local calls and the explicit estimator bridge are reported
  separately. The pinned `interval_histogram` and `dominant_interval` methods
  exist. The equivalent bridge delegates to the unchanged source histogram
  processor, then source histogram smoothing and first argmax; it does not
  patch source files. Short-input failures must match the bridge too.
- No-evidence tempo retains the source `[NaN, 0]` sentinel. Beat search may reject
  short or no-candidate snapshots; a kernel error is a rejection, not PASS or
  a fabricated beat. Every rejected snapshot end is exported and checked
  against raw original CRF or the explicit estimator bridge for fixed/local;
  the latter's raw legacy-wrapper failure is recorded separately. A successful
  checked source return, even an empty path, cannot
  be accepted as a matching rejection. Fixed/local behavior on silence is not a meaningful beat
  confirmation even if the original interval search produces a grid.

The standalone C++ regression/exporter is
`madmom_reference_pipeline_tests`. With a fresh evidence-directory argument it
writes per-mode stage exports outside processing; existing mode directories
are rejected to preserve earlier evidence. The companion
`tests/rhythm_originals/madmom_pipeline_oracle.py` reads those exports using the
existing pinned Python/Cython source environment and verifies the pinned file
hashes before comparison and exclusively creates a new report. Numerical
comparison failures accumulate across all modes and keep a nonzero final exit;
structural and oracle setup/output failures still stop immediately. Beat tracker
calls run in fresh subprocesses with a 30-second deadline. Their input hashes,
stdout/stderr and results remain in a fresh worker directory alongside the
report. Native process exit and timeout while the actual source call is in
progress (started marker present, returned marker absent) are explicit original
rejections. Failures after return are oracle-output faults and remain hard failures. A returned graph path cannot pass comparison with
a rejected original path; it is marked uncomparable and keeps the strict failure.
Worker import/setup failures cannot count as matching source rejections. `--report`
selects a fresh report path for unchanged captures without replacing earlier
evidence. Raw-original rejection, explicit-bridge comparison,
graph integration and independent source acceptance are separate results.
A successful original return against a rejected graph snapshot, including an
empty or invalid path, is recorded as a strict numerical mismatch with actual
events and worker provenance. It continues case aggregation without becoming
an accepted rejection; source identity/setup/output faults still stop execution.
Their current acceptance state belongs to the workspace issue, not this guide.

The fresh v31 three-second synthetic C++ graph regression passes all three
modes (592 frames, 137 bands), including chunk/reset/rollover/silence checks.
Fixed and local pass their original stage and exact-event comparisons. CRF
now matches the original 10x2 tempo candidate table (maximum absolute error
4.487879124e-6); the former missing 84.507042 BPM candidate is recovered.
The earlier v12/v16 Float32 9-versus-10 failure remains in immutable evidence.

Raw CRF beat equivalence is still unaccepted. Its final original worker exits
with `0xC0000005`, while the graph returns five beats: uncomparable, never PASS.
At rejected snapshot end 161, v31 original worker 0013 instead returned
`[0.055, 0.020, 0.010]` seconds (frames `[11, 4, 2]`), a strict mismatch with the
rejected graph snapshot. This descending path violates the pinned transition
invariant: [beats_crf.pyx lines 205-217](https://github.com/CPJKU/madmom/blob/27f032e8947204902c675e5e341a3faf5dc86dae/madmom/features/beats_crf.pyx#L205)
store predecessor `i-j <= i`, then lines 238-239 backtrack these links;
[beats.py line 791](https://github.com/CPJKU/madmom/blob/27f032e8947204902c675e5e341a3faf5dc86dae/madmom/features/beats.py#L791)
only divides by positive fps. The source allocates uninitialized backlinks
(line 185) and declares `next_state` without initialization (line 190), assigning
it only when a terminal score exceeds negative infinity (lines 226-234).
The exact offending trial/state was not captured, so it is not asserted here.
The earlier v16 call with the same envelope SHA256
`c608f75348652b2bc748f9c5fea3f66379fd3c5f6574c8ff4c221083029f55f8`
exited natively; no unchanged-binary claim is inferred from a source revision.
An invalid successful original return is not a beat path to emulate or relabel
as a matching rejection. No tolerances or raw-original gates are weakened.

The original v31 report is incomplete because the previous rejection assertion
stopped at this mismatch. The corrected oracle now aggregates this strict
failure and retains all case fields; the sole integration owner must verify
that reporting change before its new report is accepted. All v31 reports,
worker outputs and earlier evidence remain unchanged.
Measured evidence and follow-up decisions are in the
[madmom lane record](https://github.com/ArthurKoba/ai-science/issues/28#issuecomment-6582264082).

The persisted research projects are `examples/rhythm_operations/projects/`
`madmom-superflux-fixed.json`, `madmom-superflux-local.json` and
`madmom-superflux-crf.json`. They use the explicit research execution profile.
From the checkout root, the optional release runner selects the same recipe:

```powershell
./cmake-build-release/bin/rhythm_reference_runner.exe --preset madmom-superflux-fixed --synthetic --duration 4000
```

Substitute the local or CRF preset ID for the other modes. `--synthetic` feeds
changing tones internally without speaker playback. The bounded run reports
telemetry; exit zero alone does not establish successful temporal publications,
numerical parity or wall-clock real-time throughput. The normal application and
SDK retain their existing execution budgets.

The madmom recipe uses the neutral BSD DSP adapter composition. It does not add
GPL/AGPL reference ports to the main application or SDK. Native UI/audio-device
acceptance and performance on arbitrary music are outside synthetic parity.
