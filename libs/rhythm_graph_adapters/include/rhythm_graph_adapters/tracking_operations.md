# Rhythm mathematical stages

`make_tracking_operation_node` owns bounded contracts; the existing executor
runs the optional factory. Inputs are native mono Float32 except the declared
count/control cases. Axes are indices; seconds/BPM are explicit payloads.

| ID | Inputs | Outputs in order |
|---|---|---|
| InformationGain | magnitude | information gain scalar |
| ErbFilterBank | linear spectrum | projected values, centers Hz, weight matrix [band][bin] |
| PeriodPath | matrix [temporal frame][normalized period observation] | period seconds0, block-end seconds1, noisy observations2, UInt32 decoded path3 |
| RenewalBeatDecoder | ODF0, period seconds1, block-end seconds2 | UInt8 beat mask0, UInt32 state path1, tick seconds2 |
| BeatAgreement | matrix [method][beat], UInt64 valid counts per method | UInt64 selected original method, confidence, method scores, pairwise information-gain matrix |
| BandTempoFeatures | band power | scaled band differences, cumulative feature |
| PeriodPhaseTicks | period vector, phase vector | ticks, cumulative matching periods, Int32 next phase, UInt64 consumed frames, UInt8 source status |
| HarmonicComb | ACF vector or matrix rows0, explicit weights1 | comb vector or matrix rows0 |
| PeriodicPhaseResponse | feature window | pulse phase response |
| FrameSilencePolicy | PCM frame | conditioned frame, UInt64 start/stop/observed frame counts |
| LagObservationPool | cumulative matching-period prefix | unique stored values, flattened source Pool sequence, UInt64 row/flattened counts |
| TickSequenceRefinement | tick candidates | refined ticks, intervals, UInt8 source status |
| LagTempoVote | matching-period lags | voted BPM, selected BPM observations |

`PeriodPath` accepts already normalized observations of width `period_hop`.
It has no temporal framing, correlation, comb or mean subtraction. Explicit
settings include `frame_rate`, `frame_duration_s`, `frame_length`, `period_hop`,
BPM bounds and `noise_amount`. The reference graph supplies their source values.
`RenewalBeatDecoder` independently consumes the unthresholded expanded ODF,
periods and end times; its settings include frame rate and expansion factor.
Typed input/output selectors are in `essentia_ports.h`.

The reference graph exposes all preprocessing through common mathematical
blocks. Its two FIR stages connect FinalState to InitialState explicitly,
preserving the source coupling and sequential comb-row state. Complete PCM,
legacy and five-candidate convenience processors are removed from the factory.

BeatAgreement retains the pinned source integer `log2(40)==5` confidence
expression and complete candidate selection; it does not return a probability.
