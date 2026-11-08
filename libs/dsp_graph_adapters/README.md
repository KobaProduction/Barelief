# Portable DSP graph adapters

This target binds the existing `dsp_kernels` implementations to the existing
`NumericOperationFactory`/`PreparedGraph` protocol. It has no Qt, platform I/O,
reference-port/GPL dependency, model loader or executor of its own. Classical
model parameters enter through the owning `ClassicalModelProvider` port.

The extension receives only explicit settings and physical `NumericBufferContract`
geometry, in cold factories as well as processing. Clock, coordinate, meaning and
provenance values remain private to the runtime. `operation_constraints.cpp`
declares admission and per-frame requirements, retained coordinate routes and
output grids. Runtime generates declared index/time grids and copies retained
coordinates; supplied projection centers use a write-only coordinate capability.
Outputs carry opaque origins with Frame/Point/Batch publication semantics.
Full graph contracts enter only through `spectral_composition.cpp`; the runtime
profile enters only through the profile composition entrypoint.

## Common operations

All analytical payloads are mono native Float32, except exact histogram counts
(UInt64). Inputs and output capacities are declared contracts. Parameters are
registered by `domain/numeric/common_operations.h`; no library names select DSP.

| Stable kind | Explicit settings / inputs | Outputs |
| --- | --- | --- |
| FilterMatrixProjection | Vector or row-major matrix; band-major `weights`, optional `centers_hz`, `compensated`, `square_input`, `accumulation` | Projected values; weight matrix; supplied centers, empty when absent |
| LogCompression | Vector; `multiply`, `add`, `base` (0 ten, 1 natural, 2 two), `signed_input` | Shared logarithmic compression; zero argument may produce −Inf |
| VectorNormalize | Vector; `method` (0 none, 1 max, 2 min, 3 L0, 4 Lp), `exponent` | Shared normalized vector |
| RecursiveFilter | Vector chunks; `numerator`, `denominator`, `form` (0 transposed II, 1 direct I), `subnormal`, `input_zero_threshold` | Filtered chunk; current delay state |
| DirectCorrelation | Two equal-length vectors; signed `first_lag`, `lags`, `normalization` (raw/unbiased/biased/overlap-energy) | Shared cross-correlation with signed lag coordinates in frames |
| CurveInterpolation | Values and query vectors; explicit `coordinates`, method/order/fill/extrapolation/bounds settings | Shared interpolation, including explicitly allowed IEEE results |
| CombFilterBank | Vector chunks; positive integer `delays`, scalar/per-filter `alpha`, forward/backward `direction` | `[time][filter]` values; delay settings |
| ValueHistogram | Vector; `bins`, `method` (0 uniform range, 1 rounded counts), positive `step` | Exact UInt64 counts; Float32 bin centers |
| InterpolatedPeaks | Vector; `threshold`, `endpoints`, explicit coordinate `origin`/`step` | Variable peak positions; original bin heights; per-bin quadratic offsets |
| SameConvolution | Vector; explicit `kernel`, `crop` (0 largest, 1 input) | Shared zero-padded same convolution |
| InverseFourier | Full N-bin real and imaginary vectors; `fft_size` | Full real/imaginary inverse using existing `PreparedFourierTransform`, normalized by 1/N |

`FilterMatrixProjection.accumulation=separate` retains the default Float32
products and selected plain/compensated sum. Explicit `sequential_fma` uses one
IEEE Float32 fused rounding per product/add and requires `compensated=false`;
unknown modes and conflicting options are rejected during preparation.
`square_input` still rounds its square separately and rejects overflow.
The kernel implements finite fused arithmetic without a platform FMA provider.

There is no second FFT/frontend. Existing `TemporalFrame`, Fourier/RealFourier,
MatrixFFT, ComplexPolar, AmplitudePhase, SpectrumProjection, Mel, PowerToDb,
MatrixCenter, MatrixAutocorrelation, SpectralAutocorrelation and tempogram blocks
remain the reusable stages for framing/windows/padding/rotation, raw complex
transforms, source polar rules, calibration, filtering and correlation.
InverseFourier has a distinct full complex input shape; it does not guess an
inverse from a magnitude spectrum or fabricate conjugate symmetry.

Shape-preserving transforms retain declared axes and transport clocks. Matrix
projection retains the row axis and publishes supplied band centers as Hertz;
without supplied centers its band index remains an index. Correlation labels
lags, not seconds; there is no hidden Fs from another node. Queries and peak
positions are explicit payloads/settings. No metadata supplies DSP parameters.

The recursive and comb blocks retain state between chunks and reset on explicit
runtime reset/discontinuity. Other common blocks process independent batches.
Input/output shape, finite/domain policy and disjointness are checked before
source mutation. An arithmetic failure may consume a source prefix: the runtime
invalidates every output and applies ResetState. No old result is published as
current. Disabled blocks emit typed zeros without advancing DSP state.
Coordinates are always finite. Only LogCompression and CurveInterpolation
explicitly permit nonfinite numerical results; this is not an implicit clipping
or replacement policy.

Cold clone copies each common operation's mutable buffers independently.
Compatible warm transfer is conservatively rejected, so the existing runtime
performs an explicit reset. Pure assessment calls static checked storage queries,
never prepare/FFT/factorization; cold preparation checks actual capacities against
the admitted bound before replacing a processor. Descriptor payload, primitive
storage and bounded packet/row scratch are counted once. Runtime owns latch,
output, coordinate and stream storage separately. Hot calls allocate nothing and
retain no borrowed input/output views or full history snapshot.

## Explicit production composition

Link `dsp_graph_adapters` explicitly and use the existing executor:

```cpp
#include <dsp_graph_adapters/profile.h>

auto profile = dsp_graph_adapters::with_dsp_operations(
    configured_profile, classical_models);
auto capabilities = dataflow_runtime::PreparedGraph::numeric_capabilities(profile);
dataflow_runtime::GraphExecutor executor{std::move(profile)};
```

The caller supplies admission/resource limits and classical model parameters;
the helper does not enlarge budgets. The previous factory is the final fallback.
To opt into the optional reference-port target, explicitly link
`rhythm_graph_adapters` and instead call
`rhythm_graph_adapters::with_reference_operations(configured_profile,
classical_models, window_resampler)`. Routing is common primitives → classical
DSP sequences → optional reference bindings → prior factory. Main application
and SDK acquire no implicit GPL/AGPL linkage. Missing factories/models/backends
remain unsupported capabilities.

Stage-chain examples, with explicit compatible N/Fs/axes and owned bounded
packet delivery at each edge:

- TemporalFrame → RealFourier → ComplexPolar → FilterMatrixProjection →
  LogCompression. Keep the separate DC output or deliberately select first_bin1.
- Explicit onset-envelope batch → VectorNormalize → SpectralAutocorrelation →
  InterpolatedPeaks; compare with DirectCorrelation on the same explicit data.
- Explicit complex tempogram planes → InverseFourier for a selected full
  complex row. Positive-half data require an explicit source reconstruction,
  never a guessed negative half.
- Explicit values → RecursiveFilter → SameConvolution → ValueHistogram;
  inspect the separate filter-state/count/center outputs.

Sequence-family bindings are implemented in the separately owned sequence files.
Their catalogue and source-chain descriptions live in
`domain/numeric/sequence_operations.h`. Current source registration does not
establish complete five-library coverage, numerical parity or integration-test
acceptance. Ring-history ingress still follows the existing runtime retention
and gather rules; these adapters do not retain asynchronous borrowed histories.

## Absolute values and explicit elementwise arithmetic

`AbsoluteValue` has one values input and one values output. It accepts native
mono Float32/Float64 scalar/vector/matrix values, preserves dtype, shape, clock
and coordinates, and rejects nonfinite input before publication. It needs no
workspace or hot allocation. Disabled processing publishes typed zeros.

The core `ArrayMultiply` node uses typed `domain::ArrayMathOperation` selectors
serialized as `multiply`, `add`, `subtract` and `divide`. Subtract/divide have
exactly two semantic inputs: left/numerator0 and right/denominator1; output0 is
the result. Scalar and matrix-row-vector broadcasting use the existing prepared
factor buffers. The explicit leading frame input controls publication/shape and does not swap
the semantic left/numerator and right/denominator operands. `trigger_input=-1`
retains the existing automatic pending-frame publication, including a cold frame
waiting for its first held factor. An explicitly selected trigger port waits
for its arrival before publishing the pending leading frame, allowing a matrix
to keep its geometry/time while a later derived weight vector triggers processing.
Updates after a published leading frame do not republish it. An unwired trigger
factor is a ready typed zero on the leading clock, preserving disconnected-edge
zero delivery.
`zero_division=reject` is the only supported division policy: a zero denominator
returns invalid_argument and does not publish a partial or stale numeric result.
Unknown operation/policy settings reject preparation. Float32 binary subtraction
and division preserve Float32 arithmetic; a Double operand selects Double.
