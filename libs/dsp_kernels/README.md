# DSP kernels

`dsp_kernels` is the C++20 numerical library. Public headers and algorithms have
no Qt, JSON, device, network, ESP-IDF or domain dependencies. Existing desktop
code may continue using the `portable_dsp` target and `dsp_core/dsp.h` forwarding
header from `src/dsp_core`; new portable consumers use `dsp_kernels/dsp.h` and
the `dsp_kernels` namespace.

Preparation allocates instance scratch/boundaries and initializes shared backend
tables. Processing never allocates or takes the table initialization mutex.
One instance belongs to one processing thread; different instances are independent.
Direct vendor init/deinit must not run concurrently with library operations and
must not deinitialize tables while any prepared plan is alive. Existing external
tables are never resized: preparation rejects an undersized table.

FFT accepts powers of two from 4 through `PORTABLE_DSP_MAX_FFT_SIZE`, and radix-4
requires a power of four. The build limit is 512/1024/2048/4096. Mel preparation
accepts at most half that limit in bins and at most 50 bands. A failed preparation
invalidates the affected plan before processing. Zero-rate/zero-bin Mel keeps
the previous silent behavior. Memory estimates describe payload, not allocator,
object, stack or runtime overhead; retained vector capacities remain bounded by
the build profile. Instance `workspace_bytes()` reports retained capacity.

`DSP_KERNELS_BACKEND=ANSI` (default) builds the vendored ESP-DSP ANSI implementation
with its simulation headers; it needs no ESP SDK. `DSP_KERNELS_BACKEND=ESP_DSP`
requires an already-defined external target, by default `idf::esp-dsp`, overridable
with `DSP_KERNELS_ESP_DSP_TARGET`. It does not fetch/install an SDK. That route uses
SDK dispatch for radix-2 and the characterized ANSI radix-4 route. SDK configuration
can itself select ANSI; `compiled_backend()` identifies the integration, not a
claim that an optimized instruction path or hardware was verified. ESP SDK builds
and numerical parity on hardware remain unverified here.

The numerical implementation compiles with `-fno-exceptions -fno-rtti`; status
returns do not depend on exceptions. Standard allocation at preparation still
uses the platform allocator and its out-of-memory policy. This library does not
promise recovery from allocation failure with those compiler flags.

Local install rules place public headers under `include/dsp_kernels` and static
libraries under `lib`, joining the `portable_sdk_targets` export set. The SDK
composition root supplies the package/export configuration. ANSI static linking
also exports its private `dsp` archive dependency; vendor headers stay private to
the build and are unnecessary for a consumer of the kernel API.

FFT publishes bins `[0,N/2)`, including DC and excluding Nyquist, with magnitude
`abs(DFT)/N`; it does not apply one-sided amplitude doubling. Finite large blocks
are scaled by a power of two before Float32 butterflies, then normalized and
scaled back using Double intermediates. Plain and typed APIs both reject
nonfinite inputs/results rather than publishing successful NaN values. Each
plane stages all magnitudes before publishing. A graph-level failure is not a
transactional rollback of already accepted upstream state or earlier planes.

Mel uses continuous triangular weights in Hertz for both regular FFT grids and
explicit frequency coordinates. Boundaries are equally spaced on the conventional
`2595*log10(1+Hz/700)` Mel scale, with DC and Nyquist endpoints. It sums input
magnitudes times triangle weights; it does not square magnitudes or normalize
by triangle area. Accumulation uses Double and rejects nonrepresentable Float32
results. Zero-rate/zero-bin preparation yields zeros for finite input. Boundaries
and workspace accounting use Double on both 32-bit and 64-bit targets.

### Meaning of Mel coefficients and the historical difference

The output is a bank of Mel bands, not an FFT array in Hertz. Band `m` has
three physical frequency boundaries `f[m]`, `f[m+1]`, `f[m+2]`, equally spaced
on the Mel scale. Hertz is used internally to evaluate a triangular filter at
each FFT bin frequency. A presenter may show band indices or explicitly supplied
centre frequencies; it must not label a band index as Hertz. The stable protocol
and graph identifiers `MelEnergy` remain unchanged, but their historical numerical
meaning is **weighted linear magnitude**. Doubling input magnitude doubles every
coefficient. There is no squaring, logarithm, dB reference, area normalization,
FFT amplitude doubling or compensation for the window in this operation.

For `N=2*bins`, rate `fs`, and `M` bands, the definition is:

```text
f[i] = 700 * ((1 + fs/1400)^(i/(M+1)) - 1), i=0..M+1
w[m,k] = max(0, min((k*fs/N-f[m])/(f[m+1]-f[m]),
                   (f[m+2]-k*fs/N)/(f[m+2]-f[m+1])))
out[m] = sum_k input_magnitude[k] * w[m,k]
```

DC lies at the zero-weight left endpoint, and the FFT does not publish Nyquist.
The first and final bands therefore do not preserve the full weight of a tone
near the spectral endpoints. Interior neighbouring triangles sum to one at a
shared frequency. Because coefficients sum bins rather than integrate a density
or average a filter area, a flat per-bin spectrum produces larger coefficients
in wider high-frequency bands and generally grows with FFT resolution. This is
expected for the existing definition; comparing different FFT sizes or band
counts is not a calibrated power/PSD measurement.

In both `3de4de8` and the post-architecture commit `b18cfbf`, boundaries were
rounded down to integer indices using `floor((N+1)*f/fs)`, and triangles were
evaluated between those integer indices. Continuous boundaries were introduced
by the subsequent uncommitted Mel changes and are retained deliberately: they make a
physical tone's filter weights independent of FFT resolution and avoid collapsed
low-frequency filters. They are not necessary merely to separate architecture
layers, and this is a real numerical change, not a cosmetic rename.

Formula-only comparison with 50 bands (ideal arithmetic, historical floor rule):

| FFT N | fs | Largest weight change | Flat-spectrum per-band change | Empty bands, old / current |
|---|---|---|---|---|
| 512 | 44100 | 1.000000 | -69.29% .. +21.54% | 2 / 0 |
| 512 | 48000 | 1.000000 | -79.32% .. +28.45% | 2 / 0 |
| 4096 | 44100 | 0.147222 | -4.53% .. +5.11% | 0 / 0 |
| 4096 | 48000 | 0.206821 | -6.17% .. +10.87% | 0 / 0 |

Flat-spectrum percentages exclude historically empty bands. Historical Float32
boundary arithmetic can add small rounding differences. For example, a unit
93.75 Hz bin at 48 kHz formerly landed entirely in band 2 at N512 but band 1 at
N4096. It now contributes 0.206821 to band 0 and 0.793179 to band 1 at both sizes.
Consequently old and current low-frequency shapes can look different, especially
at N512, but this change does not establish a common amplitude attenuation.
In a separate eight-tone spectral calculation, frequencies
`93.75,187.5,375,750,1500,3000,6000,12000` Hz were moved together by factors
`1+t/40`, `t=0..7`, then snapped to the nearest actual FFT bin with magnitudes
`(tone+1)/16`. The sum across bands stayed unchanged in all four configurations;
the L1 redistribution divided by the old sum ranged from 48.76% to 54.14%
(N512/44100), 42.76% to 54.90% (N512/48000), 4.95% to 7.63%
(N4096/44100), and 4.46% to 6.33% (N4096/48000). These are spectral formula
comparisons, not a claim about microphone capture or renderer appearance.
Restoring integer rounding or silently introducing power/normalization to match
a picture would alter the numerical meaning again. Independent regression
oracles cover 44.1/48 kHz, N512/N4096 where the build profile permits, flat
spectra, eight moving spectral tones, physical-frequency consistency and linear
gain. Renderer colour/range settings are separate from this computation.

Native PCM/Window/Windowed Float32 DSP accepts FloatUnit normalization. FFT output
uses unrestricted normalization because its spectrum contract is not bounded
PCM. Spectrum Mean uses a stable midpoint; RMS scales by the largest magnitude
before taking a hypotenuse, avoiding false intermediate overflow.

### Explicit PCM sample-rate conversion

`PcmResampler` prepares planar Float32 streaming conversion, with no allocation
in `process`. Supported rates are 8–192 kHz and bounded ratios from 1/8 to 8;
unsupported conversions are rejected during graph preparation. Output capacity
covers `ceil(input_capacity * output_rate / input_rate) + 1`. Ingress partitioning
does not change the rational sample grid or numerical result.

Different rates use a causal 128-tap Hann-windowed sinc and 128 prepared fractional
phases. The low-pass cutoff is `0.45 * min(input_rate, output_rate)` Hz, leaving a
transition region before the smaller Nyquist frequency. Each phase is normalized
to unit DC gain. The filter delays content by 63 input samples; fractional phases
are quantized. Timestamps retain the causal sample/delivery grid. Equal rates use
an exact identity with zero filter delay. This bounded converter has a finite
transition band and phase quantization, rather than a selectable quality profile.

The numerical basis is the windowed-sinc interpolation construction described by
[Julius O. Smith](https://www.dsprelated.com/freebooks/pasp/Windowed_Sinc_Interpolation.html).
Output rates are graph parameters: rate changes reprepare affected contracts.
