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

`numeric_btrack_reference_example(input_rate=44100)` builds the current graph.
The PCM resampler converts ingress to 44100 Hz; this conversion is outside the
original recipe and is identity at 44100 Hz. `FrameBuffer(size512,hop512)` fixes
cadence independently of capture packet size. `Window(1024)` includes the initial
zero-left prefix. The general `WindowFunction` has `precision=1`,
`evaluation=Direct`, `pi=3.14159265358979`, symmetric Hann and explicit N1024.
`ArrayMultiply` applies its Double coefficients; an explicit Float32 `Converter`
is the source rounding boundary. `IndexExtract(mode=Axis)` swaps the two
512-sample halves through explicit indices; the same operation later selects the
first 512 autocorrelation lags.
`Fourier(1024)` selects `backend_id=kiss_fft130_float32`; missing or mismatched
providers reject preparation. No BTrack stage frames or transforms PCM internally.

`BTrackOnset` consumes full real/imaginary FFT planes and publishes the raw
Double positive complex spectral difference. Its Double polar histories are
part of that source descriptor. A separate `ComplexPolar` exposes raw Float32
magnitude/phase for comparison, using all 1024 bins and SquaredNorm/Atan2.
The ODF's absolute value, Double epsilon0.0001 and Float32 conversion are explicit
`AbsoluteValue`, `NumericMap(scale=1, offset=0.0001, output_dtype=Float64)` and
`Converter` nodes. The finite Double offset is applied on the first input hop;
scaling and addition have separate Double rounding.
`BTrackHistory` only appends that Float32 value to a seeded512 ring; the initial
ring has unit impulses every43 frames. It performs no preprocessing or resampling.

The whole-window sinc operation is a separate `BTrackHistoryResample` node. Its
finite512→512 call uses libsamplerate0.2.2 best-quality coefficients and explicit
DesktopReference arithmetic, including at ratio1. Float32 I/O/table values and
Double coefficient interpolation/serial accumulation are retained. This operation
is independent of the ingress PCM resampler and requires the declared provider.

The observation chain is fully explicit:

```text
history -> whole-window sinc -> BTrackAdaptiveThreshold(size512)
 -> concatenate512 zeros -> Fourier1024
 -> Re*Re + Im*Im -> InverseFourier1024 -> hypot(Re,Im)
 -> first512 lags -> divide by [512,511,...,1]
 -> BTrackCombResponse -> BTrackAdaptiveThreshold(size128)
 -> BTrackCombObservation -> BTrackBeatDecision.tempo_observations
history.newest_onset ------> BTrackBeatDecision.onset
```

The powers and their sum explicitly choose the last-updated operand as the
publication driver; no stale opposite FFT plane is reused. `BTrackCombResponse`
contains only the source four-harmonic/Rayleigh reduction. Thresholding and
41 observations for80:2:160 BPM are separate pure stages. The tempo transition
owns no FFT or resampling plan. `BTrackBeatDecision` scores/predicts first with
tightness5 and alpha0.9; it advances tempo state only on a predicted beat, then
sets the period for the next hop. This preserves the source scheduling order.

| Stage | Semantic outputs |
| --- | --- |
| BTrackOnset | raw Float64 onset strength |
| BTrackHistory | newest Float32 onset; seeded onset window512 |
| BTrackHistoryResample | resampled onset window512 |
| BTrackAdaptiveThreshold | baseline-subtracted positive values, same geometry |
| BTrackCombResponse | comb response128, period axis in frames |
| BTrackCombObservation | tempo observations41,80:2:160 BPM |
| BTrackBeatDecision | beat flag; BPM; period in hops; newest score; score window512; Int64 next-beat/prediction timers; tempo probabilities41 |

The GUI shows BPM, the last512 beat flags and current raw onset strength.
A beat flag of1 marks the corresponding analysis hop. Intermediate outputs stay
available at their explicit producer ports. Stable numeric port IDs are expressed
by typed names in `rhythm_graph_adapters/btrack_ports.h`; display names do not
select routes or tests. Feature frames preserve source provenance without claiming
PCM sample intervals. Synchronous diagnostics borrow their views only for the call.

`btrack_reference_pipeline_tests` checks this graph against shared ACF/SRC
primitives and separate tempo/prediction state, including every-hop history
rounding, no C++ allocation, packet partitioning, producer epoch reset and missing
providers. Its optional output-prefix argument exports stage traces.
`tests/rhythm_originals/btrack_pipeline_compare.py` compares those traces with the
separately compiled pinned original. The fresh 2026-11-08 source-fan-in capture
passes all eight unchanged native comparison policies, including complete
intermediate state. Events and periods match exactly; floating values remain
within their recorded budgets. The bounded tones/silence fixture does not establish
music-dataset quality. Its identities, policies and results are recorded in
`cmake-build-release/Testing/atomic-rhythm-20261108-013954/btrack-source-fanin-v4/qualification.json`.

Unused PCM predictive wrappers, their graph adapters, directed benchmarks and
monolithic reference exporters have been removed. They are not retained as a
second processing path or as hidden preset implementations.
