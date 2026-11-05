# BTrack reference graph

[Open the exported Barelief project](../projects/reference-btrack.json).

The optional research preset `reference-btrack` implements the default `BTrack()`
recipe pinned at `9d6127618a5679e9caa74c594b88f1d74f0e035f`. It uses the existing
`PreparedGraph` executor. The main application and SDK do not implicitly acquire
the GPL/AGPL reference implementations.

Build the optional runner explicitly from the repository root:

```powershell
cmake --build --preset release --target rhythm_reference_runner
./cmake-build-release/bin/rhythm_reference_runner.exe --export "./.agent-workspace/btrack-reference-export"
./cmake-build-release/bin/rhythm_reference_runner.exe --preset reference-btrack --synthetic --duration 15000
```

Export writes the reference presets as versioned Barelief project files; this
preset is `.agent-workspace/btrack-reference-export/reference-btrack.json`.
Choose a fresh export directory. Open that file in the editor
to inspect its nodes, connections and ordinary diagnostic views. Execution of
these optional source stages uses the research runner. The synthetic command
uses internal changing tones without speaker playback and exits after 15 seconds.
Add `--port <port>` to expose its existing CONTROL/DATA service on loopback for a
remote editor; the default port 0 disables the listener. These commands describe
the entrypoints; verification results must come from the corresponding run.

`numeric_btrack_reference_example(input_rate=44100)` builds the graph. Its explicit
PCM resampler converts the source to 44100 Hz; this ingress conversion is outside
the original BTrack recipe and is identity at 44100 Hz. `FrameBuffer(size512,
hop512)` fixes cadence independently of capture packet size. `Window(1024)` adds
the initial zero-left prefix. `FrameStride(every_n=1,materialize=true)` owns that
bounded chronological window at the extension boundary. `BTrackWindowProduct`
uses explicit N1024/Hann settings and the original Double coefficients/product,
rounding only its result to Float32. It contains no framing, rotation or FFT.
`ArrayReindex` swaps the two 512-sample halves. `Fourier(1024)` explicitly selects
`backend_id=kiss_fft130_float32`; the research profile pins the matching existing
KISS1.30 provider. A missing or mismatched provider rejects preparation. Empty
backend selection retains the ordinary builtin FFT in other graphs.

The source ODF receives full real/imaginary FFT outputs directly. The separate
`AmplitudePhase` branch is a Float32 diagnostic view and is not the numerical
input to source ODF. Its settings are raw full1024 SquaredNorm/Atan2, without
normalization.

The remaining graph edges are:

```text
FFT Re/Im -> BTrackOnset (Float64 ODF) -> BTrackHistory
BTrackHistory[1] -> BTrackHistoryResample -> BTrackCombObservation
BTrackHistory[0] -----------------------> BTrackBeatDecision[0]
BTrackCombObservation[0] ---------------> BTrackBeatDecision[1]
```

`BTrackOnset(cartesian_input=true)` reuses the original Double polar/strict
positive complex-distance/history/reduction code through an explicit complex
frame API. It publishes Float64 ODF. `BTrackHistory` computes abs+Double0.0001,
then rounds once into its Float32 seeded512 ring. This preserves the source
boundary rather than rounding ODF before epsilon. The default polar-input onset
mode remains Float32 for explicitly selected portable use. The ring starts with
unit impulses every 43 frames and advances without shifting the full history.
The independent history
SRC is a finite 512→512 window, **not PCM rate conversion or a bypass**.
It uses the existing prepared sinc backend, full libsamplerate 0.2.2 best-quality
coefficients and its explicit DesktopReference arithmetic profile. Its I/O and
coefficient table are Float32; original coefficient interpolation and serial
accumulation inside this backend use Double. Comb/prediction analytics are Float32. A missing provider rejects preparation, including at ratio 1.

`BTrackCombObservation` reuses the source adaptive thresholds, balanced complex
ACF 1024, four-harmonic comb/Rayleigh weights and 41 observations for 80:2:160 BPM.
It does not advance a tempo prior. `BTrackBeatDecision` scores/predicts first with
tightness 5 and alpha 0.9; only on `beat_due` does it advance the tempo transition
and change the next hop's period. This preserves source order without a hidden
graph feedback edge. Analysis/SRC are additionally evaluated between beats;
their diagnostics describe the current snapshot, while tempo state remains held.
That extra work is a scheduling cost, not evidence of a realtime budget.

| Stage | Output ports |
| --- | --- |
| BTrackOnset | 0 Float64 raw scalar ODF in source Cartesian mode |
| BTrackHistory | 0 adjusted ODF; 1 seeded history512 |
| BTrackHistoryResample | 0 sinc history512 |
| BTrackCombObservation | 0 observations41; 1 thresholded comb128; 2 balanced ACF512 |
| BTrackBeatDecision | 0 Float32 beat flag; 1 BPM; 2 period in hops; 3 newest score; 4 score history512; 5/6 Int64 next-beat/prediction timers; 7 tempo-state41 |

History axes use seconds relative to the newest point (zero). ACF lags use
seconds; comb positions use frames; tempo-state coordinates are BPM values.
Source stamps retain exact counters and clocks. Feature frames do not claim to
be PCM sample intervals. Diagnostic callbacks borrow views only synchronously.

The source window/product and polar/ODF retain their original Double arithmetic;
FFT inputs/outputs use the source Float32 KISS provider. ACF/prediction analytics
remain Float32 and do not claim bitwise equivalence to every upstream Double
intermediate. The source sinc arithmetic is selected independently. Invalid/nonfinite input is rejected
before state mutation; arithmetic failure invalidates outputs and resets the
affected stage. Disabled stages publish zeros without advancing state. Cold
clones own independent histories/backends; Apply explicitly declares reset for
these reference stages. Warm setTempo/fixTempo, alternate ODFs/windows/hops and
all BTrack entrypoints are outside this single canonical preset.

`btrack_reference_pipeline_tests` compares the modular graph against the existing
monolithic portable composition, checks scheduling/no C++ allocation/missing
provider, and can export intermediate traces using an output prefix argument.
`tests/rhythm_originals/btrack_pipeline_compare.py` compares such a trace to a
separately compiled pinned source. Raw failures/rejections and optional repaired
oracle results are recorded separately. The raw SRC observation bridge must be
declared explicitly; already-thresholded source buffers are not raw SRC evidence.
These are validation tools, not claims that unexecuted checks passed or that
synthetic agreement establishes quality on arbitrary music.

The earlier 2026-11-02 generic-FFT snapshot integration run passed the modular runtime/lifecycle and
no-C++-allocation checks, including 97/8192-sample packet partitions and producer
epoch reset. Comparison against the pinned raw native original on the same 1024
hop, eight-tone trace **failed the unchanged numerical tolerance**: one ODF
sample at hop 345 differed by 0.4138854 (allowed 0.219811), followed by 529 raw SRC
history mismatches. Beat flags and rounded periods matched exactly; BPM, score,
tempo-state and comb passed their declared tolerance. This is a measured limit
of this preset's current numerical comparison, not whole-pipeline parity PASS.
The cause must be isolated before attributing it to a particular FFT/gate stage;
no backend or tolerance was changed to turn this result into a pass.

The optional runner also completed a bounded 4-second internal-synthetic smoke
run: 309 complete ODF/history/SRC frames and all comb/decision outputs, with zero
processing errors, deadline misses or dropped PCM samples. This confirms actual
execution beyond Apply; it does not establish native rendering, arbitrary-music
accuracy or a hardware realtime budget.

The source-precision correction above supersedes that earlier graph. The fresh
v12 graph trace passes the unchanged native comparator on the same 1024-hop,
eight-tone PCM: ODF and pre-threshold SRC history both have maximum absolute
error 0; beat flags and periods match exactly. BPM, score, tempo state and comb
pass the original tolerances (these Float32 tracker values are not all bitwise
identical to upstream Double). The raw-original execution is explicitly reused
from v3 after verifying identical PCM SHA and original binary/output hashes;
the corrected graph capture is fresh. No original rerun or tolerance change is
claimed. Runtime also passes exact Double ODF against the existing source
frontend at every hop, 97/8192 packet partitions, producer epoch reset,
no-C++-allocation and missing/mismatched/default-empty Fourier-provider checks.
The separate source-boundary test checks 1024 complex frames, NaN and reset.
These finite cases do not establish arbitrary-input or general music accuracy.
