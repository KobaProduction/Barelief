# Aubio default onset and tempo graph

The optional `rhythm_reference_graphs` target builds `aubio-reference` from
aubio `ad5cf975aed08cc4562dd008cf9f83b12b82ffb8`. It selects two independently
stateful entrypoints: `aubio_onset("default")` (HFC) and
`aubio_tempo("default")` (positive spectral flux). It does not combine all
aubio methods into one estimator.

The common frontend is explicit:

`AudioSource → PcmResampler(44100) → FrameBuffer(512, hop512) → Window1024 →
ArrayMultiply → ArrayReindex(left512) → RealFourier1024 → AmplitudePhase →
ArrayReindex(bins0…512)`.

`ArrayLength → ArrayFill(values)` supplies the exact source `hanningz`
coefficients, generated during cold graph construction through the existing
source window kernel. The coefficient array is saved in the graph. This keeps
the original Double angle rounded to a Float32 `cosf` argument and the final
Float32 coefficients. Ordinary WindowFunction's Double trigonometry has a
different rounding order. RealFourier explicitly selects
`backend_id=aubio_ooura_fft8g_float32` and `include_endpoints=true`: one real
input, 513 raw Re/Im values for bins0…512. This is the existing pinned aubio
`src/spectral/ooura_fft8g.c` adapter; no FFT formulas are copied into the graph.
Polar settings are raw magnitude, squared norm, atan2, real endpoints, no FFT
or window compensation. DC and Nyquist are included.

The onset branch is `OnsetMagnitude(whiten=false, compression1) →
OnsetDescriptor(HFC) → AdaptivePeakPicker(threshold.058) → OnsetDecision`.
Decision also receives `PcmSilence(-70dB)` from the current512 PCM samples.
Minimum interval is2205 samples, delay2201 samples (truncation of4.3×512).
Whitening is an exposed preprocessing stage, disabled in this source default.
Changing the descriptor alone does not imply applying another method's defaults.

The independent tempo branch is `OnsetDescriptor(positive-flux) →
AdaptivePeakPicker(.3) → ContextBeatSequence`. Silence is a separate
`PcmSilence(-90dB)` connection. There is no whitening or compression here.
Frame rate is44100/512; the source-rounded context has512 frames and a128-frame
tracking step. The tracker consumes the preceding history before adding the
current thresholded descriptor. Delay is0; tatum signature4 and exactly one
explicit poll per hop are selected by `poll_tatum=true`.

Views expose raw PCM, magnitude, phase, compressed magnitude, HFC, thresholded
flux, source history and autocorrelation, BPM, source tactus, detected onsets,
exact UInt64 onset samples, Int64 beat/tatum samples and UInt32 tatum polling.
Scheduled beats can have an exact zero tactus; the scheduled flag and numeric
tactus are distinct source outputs. Tempo confidence preserves source IEEE
edge cases, including undefined values.

Sample outputs use the local analysis origin, including startup left padding;
they are separate from transport timestamps. Scalar features carry no PCM
interval. A discontinuity starts fresh analysis in graph execution. Source
`aubio_onset_reset` itself resets only decision clocks; that API remains in the
native port and is not presented as the graph's whole-stream restart.

The optional runner uses the existing NumericEngine/GraphExecutor and exports
fresh canonical project files. Normal application/SDK targets do not link
GPL/AGPL source ports. Exported graphs can be opened in the editor; executing
source operations requires the research runner endpoint. No user config is
overwritten. Export refuses changed existing destinations. Its cold composition
selects the installed Ooura or KISS factory from explicit Fourier `backend_id`
values. Empty IDs retain builtin execution without a provider; unknown IDs and
conflicting nonempty IDs reject before engine construction. There is no fallback
or selection from preset names, signal shape or upstream topology. The selected
factory is retained by the runtime profile; each prepared plan owns its resources.

Live PCM conversion uses the existing explicit FIR resampler. This is a
source-rate adaptation, not an assertion of equivalence to aubio file loaders.
The source Ooura RDFT is selected to preserve its rounding, rather than the
previous graph's common Float32 full FFT.
Numerical/event equivalence must be established by the corresponding tests and
external oracle evidence; source settings alone do not establish it. Synthetic
changing-tone checks do not establish music-dataset quality.

Historical builtin-full-FFT baseline (16 s / 1378 complete hops): that graph matches
local composition across 512/777/4096-sample packets, silence, producer-epoch
restart, and allocation checks. Against the separately compiled pinned original,
phase, HFC/picker/decision, tempo and exact onset/beat/tatum events pass the
existing policies. Magnitude has 4/706914 values outside the unchanged spectral
envelope (maximum absolute error 1.441687e-6); overall external verdict is
STRICT_FAIL, not numerical equivalence. Evidence is recorded in workspace
issue #28 under RHYTHM-FOUR-PIPELINES-20261102. This bounded signal result does
not establish arbitrary audio behavior.

The explicit Ooura graph's fresh v31 capture passes the complete local scenarios
and a fresh execution of the separately compiled pinned original under the
unchanged comparison policies. All 706914 magnitude values and 103504
phase-qualified values match exactly. HFC/picker/decision maximum absolute
error is0.00048828125 and tempo error7.62939453125e-6, within their existing
policies; onset/beat sample positions, onset flags and tatum polls match exactly.
The saved Ooura project also executes through the canonical project loader:
an eight-second synthetic probe of HFC port21:0 published669 nonempty values,
with no unavailable output. These are bounded synthetic/source checks, not
arbitrary-audio quality or native rendering acceptance. Final shared-suite
acceptance is tracked separately in issue28.
