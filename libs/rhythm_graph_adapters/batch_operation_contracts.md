# Independent mathematical batch operations

`make_batch_operation_node` builds bounded native mono Float32 contracts for
the following stages. Counter/status outputs retain their declared integer
types. Indices describe array positions; meaningful times/frequencies are
explicit payloads or declared coordinates. Variable first extents can be empty.

| Kind | Required input ports | Ordered output ports |
| --- | --- | --- |
| PulseTrainScoring | 0 onset vector; 1 candidate-lag vector | 0 selected rounded lag; 1 magnitudes; 2 variances; 3 scores, each candidate-sized |
| LagTempoConsensus | 0 **Int32** lag observations | 0 BPM; 1 uncorrected BPM; 2 octave multiplier; 3 features[3]; 4 lag histogram[414]; 5 observations UInt64; 6 selected lag UInt64 |
| FourierTempoCandidates | 0 magnitude vector incl. DC/Nyquist | 0 original spectral peaks[≤K,2]; 1 rounded-BPM observations[≤K+1,2]; 2 tempo row[M] |
| WeightedTempoHistogram | 0 rounded-BPM/magnitude observations `[O,2]`; 1 tempogram `[F,M]` | 0 histogram[M]; 1 candidates[≤M,2]; 2 silent flag |
| FrameTempoPath | 0 **explicit packed peak rows** `[F,2*P]`; 1 **UInt64** active peak counts[F]; 2 main-BPM scalar | 0 frame BPMs[F]; 1 learned mean BPM; 2 status |

The factory receives limited structural buffer contracts and explicit parameter
settings. It never reads transport metadata to choose FFT sizes, rates, grids
or numerical parameters. Mathematical outputs are published only after shape,
finite-value and owned-storage validation. Preparation accounts for all bounded
scratch; processing allocates no storage.

The complete PCM tempo and novelty-framing/Fourier convenience recipes have
been removed. The actual GUI reference recipe is composed from explicit common
blocks in `apps/default_graphs/src/essentia_graph_parts.cpp`.
