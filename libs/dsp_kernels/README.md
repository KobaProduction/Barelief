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
