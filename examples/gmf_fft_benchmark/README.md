# FFT / GMF comparison stand

The stand compares `FFT2R`, `FFT4R`, `RFFT2R*`, `RFFT4R*`, stock GMF Q15/HP,
and experimental radix-2/radix-4 candidates in both Q15 and HP modes.
The star is a display annotation
for an implementation in this project, not part of a stable API identifier.
It does not register a graph operation or replace the Float32 SDK contract.

GMF sources are explicitly supplied, Apache-2.0, pinned by revision and SHA256
in `vendor.lock.json`. CMake verifies them and downloads nothing. Q15 means
signed 16-bit fixed point with 15 fractional bits. HP is block floating point:
16-bit mantissas plus a shared exponent; it does not return Float32.

## Extended sweep

`GMF_EXTENDED_SWEEP=ON` selects ten forward routes for N=512,1024,2048,4096,
8192,16384. FFT4R supports only powers of four. Stock GMF supports at most8192;
16384 is an explicitly separate extension in the adapted candidates.

Windows uses an isolated build with the same production DSP sources:

```powershell
./examples/gmf_fft_benchmark/prepare-vendor.ps1
cmake -G Ninja -S examples/gmf_fft_benchmark/host -B cmake-build-release/gmf-sweep-host -DCMAKE_BUILD_TYPE=Release -DGMF_FFT_SOURCE_DIR="$PWD/.agent-workspace/gmf-fft-vendor"
cmake --build cmake-build-release/gmf-sweep-host --parallel 4
./cmake-build-release/gmf-sweep-host/gmf_candidate_check.exe
./cmake-build-release/gmf-sweep-host/gmf_hp_check.exe
./cmake-build-release/gmf-sweep-host/gmf_sweep.exe
```

Use the existing compiler environment; do not embed machine paths in shared
presets. `GMF_TRAP_OVERFLOW=ON` enables a separate GCC signed-overflow check for
the candidate numerical test. `GMF_UBSAN=ON` is optional if the compiler provides
the sanitizer runtime.

For ESP32/S3 use the existing ESP-IDF/toolchain described in
[embedded SDK](../../docs/reference/embedded-sdk.md), an independent directory
under `cmake-build-release`, explicit `IDF_TARGET`, `ESP_DSP_SOURCE_DIR`,
`BDSP_SOURCE_DIR`, `GMF_FFT_SOURCE_DIR`, and `GMF_EXTENDED_SWEEP=ON`.
Set CPU160MHz, optimized ESP-DSP, CONFIG_DSP_MAX_FFT_SIZE_16384 and main stack12288.
Keep PORTABLE_SDK_PLATFORM_RFFT, PORTABLE_SDK_RFFT_EXTRA_OPTIMIZATIONS and
PORTABLE_SDK_FUSED_RFFT enabled for the optimized Float32 reference.
PSRAM is not used. Flash offsets/frequency follow the chosen board target.

Each route owns the process during its test. The harness initializes minimum
sufficient shared tables, destroys its plan, then deinitializes those tables.
Production owners must never deinitialize tables while any prepared plan lives.
Minimum-table policy differs from the SDK default of initializing compiled
maximum-capacity tables. RAM admission includes caller buffers and a4096-byte
reserve; insufficient total or contiguous heap produces explicit NO_RAM.

## Timing boundary

- **Core:** native in-place prepared data through the complete mathematical
  transform, including permutation, internal scaling and real split. Source
  restoration and input packing happen before each timer. RFFT core includes
  both quarter transforms and mixed/real reconstruction, not just CFFT(N/2).
- **Full:** already-windowed native real input to native complex Re/Im, including
  validation, input copy/packing, permutation, butterflies, real split, required
  scaling and output publication. GMF preserves its Q15 input with an in-place
  working-buffer copy. Float32 plans use their production `process()`.
- Window generation/application, accumulation, preparation/allocation, dtype
  adapters, magnitude, phase extraction, squaring, power and PSD are excluded.
  Scale-aware Float32 decoding for numerical validation is outside the timer.

All routes receive exactly the same Q15-quantized seven-tone Hann signal;
Float32 input equals pcm/32768. Each timed call starts from the original input,
never from a previous FFT output. There are8 warmups and24 batches (2–8 calls on MCU,64–512 on Windows),
with per-call clocks. Median/p95 are quantiles of batch means, not individual
latency tails. Empty-timer cost is reported without subtraction. On Windows the measurement thread is pinned to logicalCPU0 only; final runs
use HIGH priority for that benchmark process alone. Scheduling
and turbo remain uncontrolled. Repeat runs are required.

Inverse tables use prepared native spectra. Core excludes restoration; full
includes copying. Ordinary GMF has round-trip gain4/N. The external N/4
round-trip correction is only in the accuracy calculation. HP inverse returns
Q15 and uses the exponent from the same handle's forward call. This is an
experimental inverse contract, not a new production SDK API.

## Adapted candidates and accuracy

`GMF2R Q15*` replaces O(NlogN) software bit reversal with an incremental O(N)
permutation. On S3 it retains upstream PIE butterflies and the existing hardware
permutation for complex lengths<=1024. Portable C narrows complex product
intermediates only under the proven generated-Q14-twiddle bound, preserving
stock C integer results exactly. It does not change saturation or FFT scale.

`GMF4R Q15*` is a mixed radix-4/2 DIF candidate with the same plan tables and
forward scale2/N. Four-input products use an exact quotient decomposition instead of unsafe
int32 narrowing: all product bits and the int64-reference result are preserved.
Quantization happens once
per radix-4 stage, so it is mathematically equivalent but not bit-identical.
It uses portable C; it is not claimed to be a vectorized S3 radix-4 kernel.
Benchmark it before selecting it: fewer stages do not guarantee faster code.

The Q15 radix-4 product has an exact int32 fast path when both butterfly
components are bounded by65536; larger values retain exact quotient
decomposition. This changes neither scale nor rounding. In particular a
portable C radix-4 kernel need not beat the S3 PIE radix-2 kernel.
Portable complex products share one narrow-range predicate for Re/Im and,
on the wide path, the same quotient/remainder components. This preserves the
exact product, rounding and saturation while removing duplicated checks.
HP radix4 stage-shift selection stops at peak23169: shift2 then exceeds the
original bound, and shift3 already covers every int16 peak through32768.
Smaller peaks retain the complete scan. HP real-pair helpers are inlined to
allow wide calculation and publication without an out-of-line tile call.
These C changes apply to supported portable targets, including ordinary ESP32;
speed remains compiler/target dependent. The SIMD/ASM options below remain S3-only.

`GMF HP R2*` preserves upstream forward/inverse/exponent bit-for-bit through8192.
Portable code uses int32 products under the generated Q14-twiddle L1 bound23172,
and stops BFP peak scanning once the original shift threshold is reached.
S3 retains the upstream PIE butterflies and pack; permutation remains incremental
above the hardware-supported complex length1024.

`GMF HP R4*` uses true mixed radix-4/2 DIF and an adaptive uniform block shift
on each stage. The exponent counts shifted bits, not stages. A two-pass wide
Hermitian pack/unpack chooses the minimum global shift before publishing int16
results; it recomputes pairs without extra heap scratch. Rounding differs from
radix-2. This experimental portable C backend is not an S3 SIMD implementation.
The HP inverse uses the preceding forward exponent from the same plan.

`gmf_hp_check` checks48 sets at each of the six sizes, HP2 bit-exact
forward/inverse/exponent, and HP4 all-bin double-FFT and round-trip comparisons
for8 fixtures per size (the remaining sets check random inputs and guards), plus
an arbitrary Hermitian spectrum against inverse DFT. The16384 HP2 extension is
checked separately against the oracle. Normal and `GMF_TRAP_OVERFLOW=ON` builds
must pass; mathematical equivalence does not imply identical quantization.

The candidate test checks all bins against an independent double FFT for zero,
impulse, DC, Nyquist, multitone and noise, plus random full-range bit-exact
forward/inverse comparisons of adapted radix-2 to stock through8192. An arbitrary
low-level Hermitian spectrum is checked against direct inverse DFT samples,
separately from round-trip. All six sizes have allocation canaries and cleanup.

The sweep checks16 independent direct-DFT bins for seven fixtures per route,
core/full equality for ordinary finite Float32 input, and fixed-point round-trip.
The4096-point noise case additionally checks every bin against a double FFT,
cross-checked with direct DFT. Hermitian weights are1 at DC/Nyquist and2 elsewhere;
Parseval-equivalent RMS error includes bias and false lines, not just white noise.
These are software arithmetic errors against the quantized input, not ADC noise.

RAM reports workspace, caller buffers, shared tables, payload total and measured
MCU heap delta separately. Payload excludes object headers for Float32 plans,
stack, allocator overhead, code/IRAM, and test-only oracle/snapshot fixtures.
GMF plan header is included, plus128 guard bytes outside payload. IDF backend
synchronization is initialized before per-route accounting and its persistent
heap delta is reported once. SDK shared
tables are charged once per process, never per frame. On S3 small radix-2 plans
use the ROM-provided table without an additional heap allocation. The published
MCU RAM tables use actual heap delta including alignment/allocator/guard costs;
Windows tables use the explicitly separate payload model. Runtime
`PreparedRfft::Options::platform_optimizations` and firmware build options remain
available; disabling runtime optimizations does not unload IRAM code. Historical
extra372 bytes referred to executable/IRAM, not an extra372-byte heap buffer.

PASS certifies the tested numerical tolerance, guards/heap and harness completion;
it does not certify GMF Q15 precision, no saturation, or production suitability.
Large-input GMF behavior is characterized separately. Issue#11 remains deferred.
Execution on ESP32/S3 is not a physical benchmark on a processor without FPU.

## Experimental ESP32-S3 PIE radix-4

`GMF_R4_S3_SIMD=ON` selects the experimental Q15/HP radix-4 PIE backend in an
isolated ESP32-S3 extended-sweep build. It is OFF by default pending hardware
acceptance. R2 continues to use pinned upstream PIE. No production SDK dispatch
is changed. The sweep prints the selected backend before measurements.

The backend processes four true radix-4 butterflies at once with signed32
vector sums. For a wide butterfly component `x=4h+l`, `h` fits signed16 and
`0<=l<=3`. Three CMUL evaluations retain the coarse quotient, low product bits,
and residual. With `P=h*W`, `L=l*W`, product shift `S=14..17` and rounding `b`:

```
floor((4P+L+b)/2^S)
  = (P>>15)*2^(17-S) + (((P&32767)+(L>>2)+(b>>2))>>(S-2))
```

Here `b` is zero or a multiple of4. Q14 twiddle L1<=23172 proves that both
`P>>15` and `L>>2` fit signed16. Reconstruction and final saturation use signed32.
Thus the SIMD candidate preserves the C R4 rounding, saturation and HP exponent;
it does not narrow an18-bit sum prematurely. Wide Hermitian reconstruction uses
the same identity before the existing Q15 or two-pass HP publication policy.
When all four real/imaginary components fit signed16, a checked exact path
reconstructs the product from high/low CMUL results. For zero rounding and S>=15,
the Q14 L1 bound additionally permits direct CMUL: its output already fits
signed16. Other inputs retain the full wide decomposition, including -32768
and saturation cases. Neither path changes C R4 quantization or HP exponents.

Each batch loads its four inputs once and caches signed32 sums. Twiddle setup
is shared across boxes. A single ASM invocation traverses all disjoint boxes
for one twiddle group using a private context copy; the small8 stage similarly
traverses its pairs of boxes internally. This reduces C/ASM calls and address
setup without changing butterfly order, per-stage shifts or the caller context.
Single-box groups retain the direct batch path to avoid unnecessary loop setup.
Small stages use scalar complex copies without out-of-line
memcpy; length8 boxes use aligned64-bit half-vector transfers. The final unity
radix-2 stage uses SIMD with the original shift/rounding/saturation. No padded
bins or separately rounded radix-2 factorization are introduced.
Real reconstruction reads native pairs directly, including the4-byte-aligned
forward block, and publishes packed results with vector quantization in the
same kernel. Wide pair outputs stay in registers through publication, avoiding
a64-byte output tile and a second ASM call. HP's
first pass reduces wide extrema without publishing unused temporary results;
its second pass applies the same globally selected shift as C R4.

`GMF_S3_VECTOR_PERMUTATION=ON` enables an additional experimental exact
permutation for aligned complex lengths>1024, shared by adapted Q15/HP R2/R4.
It requires the S3 SIMD hardware-preflight build and defaults OFF. Stock GMF,
host/ESP32 and smaller S3 lengths keep their previous routes. Eight indices are
reversed in parallel by masked16-bit SWAR using PIE32-bit operations; the final
mask removes cross-half transfers and sign extension. Indexed32-bit transfers
move complete complex16 words with `max(i,reverse(i))`, visiting each pair once.
This covers complex counts through8192 without extending the hardware
`EE.BITREV`10-bit mode, heap tables or arithmetic on signal samples. The internal
kernel requires16-byte alignment; the adapted dispatch retains C fallback.
Firmware checks44 permutations at complex counts8..8192 against an independent
index oracle and C, then checks involution and guards before measurements.

There are no new heap allocations or twiddle formats. The implementation uses
bounded stack scratch for four butterflies/symmetric pairs; stack high-water
usage and code footprint must be evaluated on hardware alongside heap and speed.
The enabled S3 R4 route requires16-byte-aligned native working buffers; an
unaligned buffer is rejected before mutation. Wide intermediates use aligned
stack scratch. QR/SAR/SAR_BYTE are caller-clobbered as in the existing
PIE backend. Assembly frames reserve windowed-ABI register save areas separately
from scratch, including during interrupt/window spilling.
Constant vectors use explicit `.balign 16`: Xtensa `.align 4` specifies four
bytes, not16. Verify the final ELF addresses as well as buffer alignment.

`GMF_R4_HOST_MODEL=ON` selects the arithmetic semantic model for host oracle
tests. It is not a PC SIMD implementation and its timing is not a performance
result. `gmf_r4_simd_check` compares200000 batch cases against an int64 oracle,
then48 sets at nine complex lengths16..8192 against the C R4 kernel and real
pack/unpack. Use the separate overflow-trap build as well.

The S3 firmware executes `gmf_r4_preflight()` before any timing: actual PIE
batch and wide-pair outputs are compared to the model, peak reduction includes
`-32768` and multi-iteration loops, and surrounding guards are checked. A failure
stops the benchmark. Cross-compilation and host PASS do not replace this gate
or demonstrate hardware speed. Compare the enabled image against a separate
OFF image and fast R2 on the same board/CPU frequency.
The firmware also compares640 complete CFFT/real-split cases against C R4 at
ten complex lengths16..8192, both directions/precisions, including HP shifts
and guard regions. These cold checks allocate temporary aligned buffers before
timing. Stack telemetry reports the whole benchmark's cumulative high-water
usage in bytes, including IDF's main-task stack overhead (`ESP_TASK_MAIN_STACK`),
not the isolated FFT kernel's stack usage.
Another128 model comparisons exercise full/small8 ASM box loops with counts
1,2,3,5, both directions, shifts14..17 and both rounding policies; guards and
unchanged caller contexts are checked. Looped batch frames use256 bytes,
including private context/loop state, with the upper32 bytes ABI-reserved.

`GMF_R4_STAGE_PROFILE=ON` builds separate instrumentation for N4096 Q15/HP R2/R4
forward/inverse: common permutation, and for R4 peak scan, batch butterflies, unity tail, layout/loop
overhead, wide pair reconstruction, and remaining real publication work.
R2 profile rows use `detail=permutation_only`; their other stage fields are
uncollected, not zero-cost. Fused R4 pair intervals include quantization/publication,
so their internal boundary differs from a separate-publisher image; compare the
whole real interval when assessing speed.
Box-loop fusion also reduces the number of instrumented batch calls, so the
instrumented CFFT before/after difference includes changed collector overhead.
Only the separate OFF images establish the actual performance gain.
The profile image sweeps only4096. `GMF_BENCHMARK_4096_ONLY=ON` also restricts an
uninstrumented extended performance sweep to4096; the default sweeps all six sizes.
The platform composition supplies the modulo32 CPU cycle counter; subtraction
handles wrap within an interval shorter than one wrap. The fixed CPU frequency
converts cycles to microseconds. Profile lines explicitly use `unit=us`,
`clock=cycles`, eight repeats, and `instrumentation=1`; nested counts are inclusive.
They diagnose stages rather than establish speed. The normal performance image
has this option OFF, compiling all collector calls out of the kernels.
Regular timing rows from the instrumented image are prefixed `diagnostic_timing`
instead of `timing`, preventing their ingestion as normal performance records.

## Earlier bounded modes

Default `GMF_EXTENDED_SWEEP=OFF` preserves the earlier512…4096 stand with its
96x8 batch scheme. Historical `float_contract_us` fields remain only in raw
legacy evidence, not in the published native transform comparison.
`GMF_SPECTRUM_ONLY=ON` exports exact4096-point Q15 inputs and Float32-decoded
spectra as hexadecimal integer words, without a timing sweep. These diagnostics
and their firmware hashes are separate from performance images.
