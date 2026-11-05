# Rhythm stage and composition API

`make_tracking_operation_node` in `tracking_composition.h` builds owned contracts for the following operations. Install the optional factory with `with_reference_operations`; the existing `GraphExecutor` processes these nodes. The core kernels retain the pinned Essentia implementation, source order, Float32 formulas and source defaults. There is no new executor or implicit frontend.

All inputs use native mono Float32 vectors, except the matrix/count cases below. Output order is the order listed. Vectors and matrices have bounded variable first extents; empty results are valid where the source produces none. All coordinates are explicitly **indices**. Seconds, BPM, periods and phases are payload values, not misleading index-axis labels. Complete PCM source recipes expose actual last-frame sample origin and source Fs as separate scalar ports. They expose existing synchronous per-frame views in their core API; graph output snapshots do not retain callbacks or allocate a second spectrogram.

| ID | Inputs | Outputs in order |
|---|---|---|
| InformationGain | magnitude | information gain scalar |
| GlobalOnset | PCM | ODF curve, band weights, last distances, ERB values, ERB centers Hz, spectral snapshot (8 ports below) |
| BandBeatEmphasis | matrix [frame][ERB band] | weighted ODF, band weights |
| ErbFilterBank | linear spectrum | projected values, centers Hz, weight matrix [band][bin] |
| MovingThreshold | nonnegative ODF | thresholded curve |
| PeriodPath | matrix [ODF frame][sample] | period seconds, block-end seconds, observation matrix, UInt32 decoded path, last raw ACF, last comb, last thresholded comb |
| RenewalBeatDecoder | ODF, period seconds, block-end seconds | UInt8 beat mask, UInt32 state path |
| PeriodBeatTracker | ODF | beat mask, tick seconds, period seconds, block-end seconds, normalized expanded ODF, UInt32 state path |
| BeatAgreement | matrix [method][beat], UInt64 valid counts per method | UInt64 selected original method, confidence, method scores, pairwise information-gain matrix |
| MultiFeatureBeatTracker | 5 ODF vectors: complex/RMS/Mel flux/emphasis/information gain | ticks, confidence, selected method, method scores, 5 complete candidate tick vectors |
| PcmBeat | PCM | ticks, confidence, method scores, 5 ODFs, 5 candidate vectors, spectral snapshot (8) |
| BandTempoFeatures | band power | scaled band differences, cumulative feature |
| CombPeriodPhase | feature window | period, phase, ACF, comb weights, comb response, phase response |
| FeaturePeriodTracker | feature vector, one frame per call | per-feature periods, phases, UInt8 cadence-ready flag |
| PeriodPhaseTicks | period vector, phase vector | ticks, cumulative matching periods, Int32 next phase, UInt64 consumed frames, UInt8 source status |
| HarmonicComb | ACF, per-period weights | harmonic comb response |
| PeriodicPhaseResponse | feature window | pulse phase response |
| FrameSilencePolicy | PCM frame | conditioned frame, UInt64 start/stop/observed frame counts |
| LagObservationPool | cumulative matching-period prefix | unique stored values, flattened source Pool sequence, UInt64 row/flattened counts |
| TickSequenceRefinement | tick candidates | refined ticks, intervals, UInt8 source status |
| LagTempoVote | matching-period lags | voted BPM, selected BPM observations |
| LegacyRhythm | PCM | ticks, intervals, BPM, BPM observations, UInt8 source status, last features, band power, periods, phases, frame ticks, matching periods, UInt64 silence [start,stop,frames], spectral snapshot (8) |

The eight spectral snapshot ports are magnitude, phase, raw complex [bin][Re,Im], windowed input, FFT input, actual window coefficients, Float64 first sample, Float32 source sample rate. Complex and polar values preserve source `sqrt(Re*Re+Im*Im)` and unconditional `atan2`; no calibrated magnitude/power/PSD is substituted. FFT input shows the original zero-phase rotation. Invalid/inactive source branches produce empty vector/matrix views and zero snapshot scalars. This is not a confirmed-beat classifier: confidence retains the source information-gain scale and selected-method/status outputs retain their original meaning.

`BeatAgreement`, `MultiFeatureBeatTracker` and `PcmBeat` preserve the actual pinned
`TempoTapMaxAgreement` confidence expression: its integer-template `log2(40)`
returns **5**, from which phase entropy is subtracted. This raw score is not
clamped or scaled to a probability. The source's prose mentioning 5.32 describes
the mathematical logarithm rather than the expression's actual return type.

## Explicit settings

Settings are read only from the node, never inferred from another frame's clock or metadata. Unspecified fields use these defaults; unsupported combinations are rejected atomically by preparation.

- Global/ERB: `sample_rate=44100`, `band_sample_rate=44100`, `frame_size=2048`, `hop=512`, `method=0` (information gain; 1 beat emphasis), `first_bin=0`, `stop_bin=input size`; `bands=40`, `low_hz=80`, `high_hz=22050`, `width=1`. ERB design Fs and physical PCM Fs remain distinct as the original omits sample-rate forwarding. Global uses fixed 40 bands; standalone ERB and band emphasis accept explicit bands.
- Period/renewal: `frame_rate=44100/512`, `minimum_bpm=40`, `maximum_bpm=208`, `frame_duration_s=5.944308390022676`, `frame_length=512`, `period_hop=128`, `noise_amount=0.0001`; `upsampling=1`, `maximum_period_s=1.5`, `sigma_ibi=0.025`, `transition_mix=0.5`. `PeriodPath` row width must equal its explicit `frame_length`. `PeriodBeatTracker` resolves its native source frame length from the explicit rate, uses the existing endpoint-linear expansion, and preserves the shared FIR state/order between its full-ODF and ACF thresholds.
- Multi: `frame_rates=[44100/1024,44100/1024,44100/1024,44100/512,44100/512]`, `upsampling_factors=[2,2,2,1,1]`, `minimum_time=5` for agreement trimming. Different ODF lengths/rates remain separate; there is no forced equal-length join or implicit resampling.
- PCM beat: `method=1` multi-feature, 0 single complex; bounds above. Source Fs=44100 and frontend frame/hop 2048/1024 remain fixed and explicit in the source API. The global feature branches use 2048/512. These fixed recipes are not silently relabeled as arbitrary-Fs variants.
- Legacy/features: `sample_rate=44100`, `audio_frame=1024`, `audio_hop=256`, `period_window=1024`, `evaluation_hop=1024`, `use_onset=true`, `use_bands=true`, `tempo_hints_seconds=[]`. The standalone comb window must match `period_window`. `gains` defaults to the eight `default_tempo_band_gains`; `frame_time_samples=512`. Whole Legacy retains the original band design Fs=44100 and band frameTime=512 independently of the outer PCM settings.
- Bounded results: `maximum_rows=4096`, `maximum_matches=65536`, `maximum_ticks=65536`, `maximum_vote_bpm=1000`. Whole Legacy uses its prepared PCM maximum, tick and Pool limits. Capacity exhaustion fails, never truncates source observations. Pool flattens repeated cumulative prefixes using bounded source row metadata; only explicit output delivery writes the flattened result.
- Standalone helpers: `half_window=8`; harmonic `first=1`, `stop=weights count/2`, `harmonics=4`; phase `period=1`, `response_length=input size`; silence `leading_zeros=0`, `acquired=frame size`, `threshold_power=1e-6`, `noise_trigger_power=1e-10`, `noise_gain=1e-10`, `add_noise=true`, `silence_boundaries=1` streaming (0 standard), `noise_seed=0`; refinement `total_frames=0`, `start_silence_frame=0`, `stop_silence_frame=total_frames`, `minimum_interval_seconds=0.24`, `last_beat_interval_seconds=0.1`.

Noise callback providers remain explicit core API parameters; the graph bindings use the original default generators. No model, random source, filesystem or operating-system service is loaded in processing.

## State, delivery and resources

All frame helpers continue state across accepted calls. `FrameSilencePolicy` finalizes only on explicit `EndOfStream`; a short packet does not imply EOF. `PeriodPhaseTicks` opts into shared-provenance/session-sequence matching, equal declared clocks/coordinates and equal actual feature counts, while allowing distinct input stream IDs. Whole PCM/global tracking is an independent complete-buffer source computation; Legacy deliberately retains its original matching-state semantics between computes and declares a continuing mode. Explicit reset clears graph-owned induction/matching state; silence reset preserves generator state as the source does.

The explicit-port tracking builder additionally accepts optional scalar UInt8 control65500 for `FrameSilencePolicy`: 0 continue, 1 batch complete, 2 EOF, 3 reset. EOF finalizes exactly once even without a fresh PCM frame; a final frame can instead arrive with the EOF event. An EOF-only call publishes updated silence bounds and leaves the conditioned-frame output unavailable, so no PCM is replayed or fabricated. Repeated EOF is idempotent; processing another frame requires reset. Disabled calls emit typed zeros without finalizing. Whole Legacy/PCM recipes perform their batch finalization internally.

Deep cold clones own mutable state; agreement row spans are rebound to the clone's prepared storage before use. Warm state transfer explicitly returns incompatible and the existing runtime resets, rather than falsely claiming retained continuity. Processing validates whole input/output views, domains, bounded storage and aliasing before source calls. Disabled processing emits typed zeros without advancing source state. On failure all graph outputs are unavailable; no partially produced snapshot is confirmed. All payloads and axes in this packet are finite-only.

Assessment uses pure structural queries without allocating or preparing FFT/source plans. Conservative bounds include nested source storage and packet staging; source prepare and factory prepare enforce actual capacity <= admitted bytes. Object headers are counted once. The BeatAgreement batch necessarily gathers only the current bounded candidate packet because the original API requires simultaneous contiguous candidate spans; it retains no borrowed input/history. All other PCM and vector inputs borrow the runtime's proven Float32 span, and matrix feature rows use the existing scalar reader.

## Source chains

Global onset is spectral framing -> raw polar -> information gain, or complex-distance -> ERB -> BandBeatEmphasis. Degara is MovingThreshold -> explicit framing -> PeriodPath plus unthresholded normalized ODF -> RenewalBeatDecoder; PeriodBeatTracker is the exact existing composition. MultiFeatureBeatTracker runs five independent period candidates and BeatAgreement; PcmBeat supplies its original five feature frontends. Legacy is FrameSilencePolicy -> raw Fourier -> spectral onsets plus BandTempoFeatures -> FeaturePeriodTracker/CombPeriodPhase -> PeriodPhaseTicks -> LagObservationPool -> TickSequenceRefinement/LagTempoVote. Generic FFT, window, normalization, correlation and filter graph operations already exist and are reused rather than reimplemented.

Pinned originals and detailed provenance remain in the core headers and implementation files: Essentia `b9fa6cb674ca43dfb94d28d293aeda441c6745db`, rhythm algorithms `onsetdetectionglobal.cpp`, `tempotapdegara.cpp`, `tempotapmaxagreement.cpp`, `beattrackermultifeature.cpp`, and outdated `rhythmextractor.cpp`. Compilation and numerical/source comparison are separate stages; this API document does not claim rhythmic accuracy.
