# Essentia RhythmExtractor2013 reference graphs

These optional AGPL research presets implement one pinned recipe with explicit
`Degara` and `MultiFeature` choices, using Essentia revision
`b9fa6cb674ca43dfb94d28d293aeda441c6745db`. They require the research composition's
reference operation factory and batch memory profile. Main/SDK linkage does not
implicitly acquire these operations.

Preset IDs are `essentia-reference-degara` and
`essentia-reference-multifeature`. The latter is the original method default;
both retain minTempo40/maxTempo208. The public builder is
`numeric_essentia_reference_example(EssentiaReferenceMethod)`.

## Causal diagnostics for the pinned original

The source defects below are separate from the numerical coverage of these
eight-second presets. The checked-source probe is an owner-run diagnostic, not
an adapter, another implementation of MultiFeature, or a new parity result:

```powershell
python tests/rhythm_originals/essentia_defined_behavior_probe.py `
  --source C:/Users/Koba/.codex/tmp/rhythm-originals-20261027/essentia-b9fa6cb6 `
  --native C:/Users/Koba/.codex/tmp/rhythm-originals-20261027/native `
  --evidence C:/Users/Koba/.codex/tmp/essentia-causal-FRESH
```

Choose a new evidence directory. The script binds the pin, native archive,
compiler, source overlays, binaries, commands, return codes and raw outputs.
It creates copies only in that fresh directory. This initial diagnostic packet
changes only `reserve` to `resize` in the explicitly named `storage-sized`
copy. It does not clear an output or repair the streaming buffer. Those repairs
require the corresponding measured reproduction and a separate defined version.
No result from an earlier raw execution is rewritten.

* `OnsetDetectionGlobal::computeBeatEmphasis`, pinned lines410-415, reserves
  forty slots in an empty vector, copies to its invalid logical destination,
  sorts the empty range and indexes element24 outside its size. The source's
  top40% comment requires sorting forty actual values. For descending weights
  40..1 the defined cutoff is25 and sixteen values remain; ties at the cutoff
  remain included. A standalone exact-block probe uses checked STL modes. Its
  `_GLIBCXX_DEBUG` executable never links the release Essentia ABI. The native
  entry probe instead uses ABI-compatible `_GLIBCXX_ASSERTIONS` in its own and
  the onset translation units; other cached native objects remain release.
* Standard InfoGain appends to the caller's vector without clearing it. Fresh
  instances produced6 values for empty output and12 for prefilled output, both
  in raw and storage-sized copies. This is independent of the emphasis sort
  defect. The streaming wrapper's fresh local vector masks output reuse.
  BeatEmphasis already clears its output at pinned line224: its6-value outputs
  were identical with empty and prefilled vectors. The earlier hypothesis of
  an emphasis output-lifetime defect is refuted; no extra clear is required.
* The separate streaming writer computes all ODF values before pushing them
  into a fixed327680-token buffer. The boundary probe uses actual infogain,
  frame16/hop1, first verifies its actual standard ODF count and then runs the
  real streaming network with `VectorOutput`. It tests327679/327680/327681
  tokens using about327696 PCM samples, without an hour-long recording or the
  unrelated PoolStorage output collector corner case. An unsupported shortened
  geometry is a setup failure, not evidence of output-buffer overflow.

The owner executed all14 causal cases in `essentia-causal-20261104-v2`;
`report.json` SHA256 is
`41c3815170b04cce8c27a52e08177d3d29532f150fd531d5553e6b5836cfb2e7`.
All observations matched the diagnostic protocol. Checked raw STL failed on
the invalid destination/index; sized descending/tie controls retained the
expected25/16 and1/40 threshold/selection results. Actual streaming counts
327679 and327680 completed exactly;327681 threw the native output-buffer-full
C++ exception. No access violation or timeout was observed in these cases.
The preceding v1 attempt stopped at a hidden `configure` overload during
harness compilation, before executing any case; its evidence is retained.

The separate follow-up uses a fresh defined-source copy with just the sort
storage fix and an InfoGain entry clear. It also tests the existing public
`SourceBase::setBufferInfo` before network construction, provisioned for the
known complete burst. This is cold owning capacity, without a streaming source
patch or processing-time growth:

```powershell
python tests/rhythm_originals/essentia_defined_behavior_repair.py `
  --source C:/Users/Koba/.codex/tmp/rhythm-originals-20261027/essentia-b9fa6cb6 `
  --native C:/Users/Koba/.codex/tmp/rhythm-originals-20261027/native `
  --causal-evidence C:/Users/Koba/.codex/tmp/essentia-causal-20261104-v2 `
  --evidence C:/Users/Koba/.codex/tmp/essentia-defined-FRESH
```

Its three new regressions require exact fresh/prefilled InfoGain equality to
the sealed fresh baseline, unchanged fresh BeatEmphasis and327681 streaming
values equal to the actual standard curve. These are required checks, not a
claimed execution result or a replacement for full MultiFeature parity.

The existing release observation (86/687 emphasis samples outside tolerance)
is a numerical mismatch, not a recorded native crash. A deliberately checked
STL assertion is also distinct from an uninstrumented native access violation.
The diagnostic ledger preserves these classifications and any incomplete setup.
Output reuse observations are recorded whether they support or refute the
hypothesis; no clearing repair is selected automatically.

On the API-observed master commit
[`33108c70929684ed14091dfb671a30264cf8bb84`](https://github.com/MTG/essentia/commit/33108c70929684ed14091dfb671a30264cf8bb84),
the onset source SHA256 is still
`666915af9e462571f3a29a4c2cb0271a085191f5b6772453597014edf69a124a`, identical
to the pin. [Issue1528](https://github.com/MTG/essentia/issues/1528) and the
unmerged [PR1539](https://github.com/MTG/essentia/pull/1539) concern the long-input
streaming capacity, not the sort buffer. The PR head observed through the API
was `1332bbaa3a37d95a5b7097096722e7d5da431259`; its reported measurements are
upstream claims until reproduced here. Neither this observation nor a leaf ODF
match establishes fresh full MultiFeature parity: all five curves, candidates,
agreement and summary still need positive and degenerate whole-input cases.

## Explicit complete finite input

The separate `numeric_essentia_finite_reference_example(EssentiaFiniteOptions,
EssentiaReferenceMethod)` builder exposes IDs `essentia-finite-multifeature`
and `essentia-finite-degara`. It retains the whole five-ODF, five-tracker,
candidate barrier, agreement and summary recipe; it does not split a recording
into independent eight-second estimates. The original live preset IDs and
saved JSONs retain their existing meaning.

`EssentiaFiniteOptions{input_samples, maximum_native_pcm_samples,
input_sample_rate}` declares known input N and cold capacity M, with N>0,
M>=N and native Fs44100 only. Another Fs is rejected explicitly. Source1
accepts real PCM packets of1..4096 samples and feeds resampler2. The host copies
available output2 into preallocated owning storage, validates contiguous sample
intervals and fixed generation/epoch, then waits for every source1 call to
return. Identity44100 must actually produce A=N; capacity is never used as
actual length. Only the first discontinuity marker is allowed. No borrowed view
is retained and no executor call occurs from a delivery callback.

After real EOF, without a flush, future tick or synthetic PCM, the host injects
exactly one `NetworkInput`3 vector of actual A within bounds[1,M] into the same
prepared graph. There is no edge from2 to3. Its clock is local44100 with
tick1/44100, seconds coordinates k/44100 and sample interval[0,A); observed
latency remains separate. Empty input is handled explicitly without batch
injection. `CoverEnd` pads only the final2048-sample analysis window, retaining
the true PCM length A. Both hop1024 and hop512 branches derive their actual
frame counts from A. Fixed preview nodes6/7/100/101 are omitted.

M determines declared frame geometry and prepared workspace. Overflow or an
unsupported geometry is rejected cold; the caller's explicit runtime resource
profile must admit the graph before processing. This builder does not increase
the shared main/SDK execution budget or grow buffers on the hot path.

Required final publications are all five ODF vectors, five candidate tick
vectors, agreement60 ports0..4 and summary61 ports0..3, once per complete
MultiFeature input. Degara requires only its complex branch/candidate and
summary, with explicit zero confidence. An available successful empty vector
is distinct from a missing or failed publication. Native comparison consumes
`recipe.json` with actual `session_samples=A`, raw PCM, every curve/candidate,
selected ticks, intervals, estimates, BPM and confidence. Additional
`scores.f32`, `selected.u64` and `pairwise.f32` declare Float32[5], UInt64[1]
and Float32[5,5]. `--require-full-agreement` checks these graph diagnostics for
consistency; the native wrappers independently compare candidates, ticks and
confidence rather than fabricating internal native observations.

The direct batch regression uses9*44100+17 real samples with M=A+4096,
changing eight-tone and silence cases, and a separate independent complete
prepared recipe. Packet97/4096 equivalence and source1→output2 collection are
integration-runner obligations. The source batch alone is not proof of those
executions or of native whole-input parity.

## Existing live sessions

The source PCM rate is44100. An explicit PCM resampler precedes a nonoverlapping
352800-sample session `FrameBuffer`. A complete session is necessary before any
batch estimate is produced. A final shorter capture is not implicitly EOF or a
complete eight-second session. Each result starts a new independent source
compute; beat seconds refer to the beginning of that session, not wall-clock
time. At native44100 the frontend needs no rate conversion. The graph's existing
FIR conversion at other input rates is an explicit input variant; it does not
claim equivalence to Essentia MonoLoader's libsamplerate recipe.

Each spectral branch is separate: unwindowed `TemporalFrameSequence`, explicit
normalized symmetric Hann coefficients, `ArrayMultiply`, half-frame
`ArrayReindex`, `RealFourier`, positive-bin selection, `AmplitudePhase`.
Framing starts at sample0 and retains the source `CoverEnd` right-zero-padded
last frame. There is no fake initial left prefix. Source window coefficients
are generated cold by the existing `spectral_window(DesktopReference)` API:
Double trigonometry followed by Float32 stores and Float32 sum(abs)=2
normalization. They enter an ordinary coefficient-array node and a visible
product edge. FFT/resampling/window formulas are not copied into the graph.
The RealFourier nodes explicitly select `backend_id=kiss_fft130_float32` and
`include_endpoints=true`: separate Re/Im planes both contain bins0..1024.
Their separate DC output remains available; the recipe uses the full planes. The
research profile must supply the matching pinned KISS130 provider; a missing or
mismatched provider rejects preparation. The independent local composition uses
the same provider. Earlier full-complex KISS arithmetic differed from source
RealForward on bit-identical inputs and failed strict ODF gates; v16 diagnostics
located that first divergence without changing thresholds. The recipe now
explicitly selects the source real transform. AmplitudePhase uses
`real_endpoints=false`, ordinary Float32 sqrt(Re*Re+Im*Im) and atan2 at every bin,
including DC/Nyquist. This matches the source composition's `spectral_frame`
and `spectrum` calls in `pcm_beat_recipe.cpp` and `global_onset_recipe.cpp`, which
omit the real-axis shortcut. Signed zeros and subnormal products remain measured.
The primary frame sequence publishes its explicit column length2048 through
`ArrayLength(axis=1)` to the coefficient-array node. Each RealFourier has one
explicit real input; the source half-frame rotation remains a visible operation.
Known physical sample rates other than44100 or regular frequency grids other
than k*44100/2048 are rejected at admission. Explicit coordinate payloads are
checked before row-state reset; index/unknown axes retain the explicit fixed
source recipe and cannot select a different bank. Time payloads declare Seconds.

The primary2048/1024 branch feeds three separate source ODFs: complex,
RMS and MelFlux. Every ODF has its own `PeriodBeatTracker` with x2
endpoint-linear ODF expansion. Degara uses only the complex branch. The
additional2048/512 branch feeds PLUS-phase squared novelty ->40 ERB bands
-> `BandBeatEmphasis`, and separately the11-frame `InformationGain` stage
(source bins2 through232 inclusive, no tail flush). These two trackers do
not expand their ODFs. Windowing, raw magnitude/phase, ODFs, period estimates
and each candidate tick sequence remain inspectable independent graph values.
`EssentiaCandidateInputs` waits for the five current-session ODF vectors, then
publishes their unchanged values in complex/RMS/Mel/emphasis/information order.
This explicit barrier preserves the source order of draws from the shared C RNG
when the executor recursively delivers output ports. Each vector keeps its own
capacity; no earlier session is accumulated. The five trackers remain separate.
Onset-row and information-row stages additionally publish Float64 ODF-grid
seconds on port1 from an explicit frame_rate setting. Complex/Mel intermediate
rows are on port2. Information gain retains its original retrospective five-frame
delay; its time grid is the emitted ODF grid and is not silently latency-corrected.

`EssentiaAgreement` receives all five complete candidate vectors. It delegates
to `PreparedBeatAgreement`, preserving first5s comparison trimming, first-maximum
selection, the selected complete original sequence, pairwise scores and the
source confidence scale. The pinned integer-template log2(40) is5; confidence
is neither a probability nor clipped to[0,1]. `BeatIntervalSummary` then computes
the source two-BPM-bin vote/mean with a strict five-BPM selection tolerance.
Degara supplies confidence0 explicitly. BPM, confidence, interval seconds,
BPM observations and tick seconds are separate outputs. Plot axes of event
vectors are indices; time values are in their payloads.

Matrix frontends make the bounded research cost explicit: the eight-second
global branch has687x2048 samples before FFT. It uses the existing executor;
there is no background analysis service or second processing path. This is
not a bounded-cost live causal tracker or an MCU performance claim.
The UI subscribes to a4096-sample PCM tail and the last1025-bin magnitude row;
the full session and large spectral matrices exceed ordinary delivery slots.
The small687x40 ERB matrix and5x5 agreement matrix remain direct diagnostics.

`essentia_reference_pipeline_tests` exports intermediate/final evidence when
given an evidence directory. It compares the decomposed graph with the existing
PCM source composition using the same Windows C RNG seed17, checks independent
session reset and counts ordinary C++ allocations during processing.
Numerical mismatches are collected across the three cases and retain a nonzero
final exit; counts, first mismatch and maximum errors are reported without
changing tolerances. Admission, execution, publication and allocation failures
still stop immediately. Existing evidence case directories are rejected.
The direct executor fixture explicitly publishes the Degara zero-confidence
constant, as the application's clock orchestration does. Unavailable boundary
markers before a port's first successful output are counted separately; later
unavailable/error events fail, and every summary port must really publish.
An independent native-original comparison is still required: equality with another local
composition does not establish original-library parity. Original runtime
rejections and the storage-corrected global safe oracle must remain separate
results. Finite tone fixtures do not establish accuracy on arbitrary music.

The test executable's `--fourier-diagnostic` mode compares both primary and
global branches with synchronous source stage outputs. It checks bit-identical
transform inputs and independently executes the pinned KISS real and complex
transforms on those inputs, preserving DC/Nyquist and reporting signed-zero
differences. The graph's declared Fourier/RealFourier kind explicitly selects
its expected geometry and independent transform. Graph bins, magnitude and phase are compared separately;
the existing strict downstream ODF gates retain a failing exit on mismatch.
All capture storage is allocated before processing. This diagnostic does not
change numerical acceptance thresholds. The same mode also captures source ERB
rows and graph ERB/weight outputs, comparing builtin and KISS versions of the
existing PreparedBandBeatEmphasis on identical rows. The source reference keeps
its factory propagation into ACF; this diagnostic does not silently switch it
to builtin or change the graph's existing BandBeatEmphasis implementation.

This guide describes the source contract. Build, integration, numerical and
independent acceptance are tracked separately in workspace issue28.

Measured v19 evidence in issue28 passes all three complete local sessions and
the unchanged strict ODF/candidate/summary gates. On the tone fixture, graph FFT,
magnitude, phase and ERB rows bit-match the local source composition. The
remaining builtin/KISS emphasis ACF difference stays within the existing policy;
the reference continues to use KISS throughout its source composition.

Native comparison passes all six storage-corrected-global families and eleven
of twelve raw/safe case-family combinations. The raw MultiFeature tonal emphasis
curve remains a recorded strict failure (86/687 values). The pinned upstream
`onsetdetectionglobal.cpp` lines411-415 reserves an empty `sorted` vector, copies
through its invalid logical range, sorts the empty range and indexes beyond its
size. The separately labelled safe source changes only reserve to resize plus
observation calls; the same graph passes that variant. The implementation uses
the sized sort buffer and does not emulate upstream undefined storage behavior.
The raw-original comparison therefore retains its failing exit.

The saved projects are `examples/rhythm_operations/projects/essentia-reference-degara.json`
and `examples/rhythm_operations/projects/essentia-reference-multifeature.json`.
They use the optional research execution profile. From the checkout root:

```powershell
./cmake-build-release/bin/rhythm_reference_runner.exe --preset essentia-reference-degara --synthetic --duration 20000
```

Use `essentia-reference-multifeature` for the five-candidate recipe. The synthetic
source feeds changing tones internally without speaker playback. A full session
requires eight seconds of PCM plus processing time. The bounded runner reports
telemetry; exit zero alone does not establish successful final publications,
strict numerical parity or real-time throughput. The normal application and SDK
retain their existing execution budgets.
