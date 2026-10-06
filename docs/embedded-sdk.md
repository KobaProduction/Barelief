# Embedded SDK

The portable C++20 SDK uses `signal_contracts`, `dsp_kernels`,
`dataflow_runtime`, `bdsp_protocol`, and `embedded_support`. It has no Qt,
miniaudio, OS socket or ESP-IDF dependency. The ESP-IDF adapter is a separate
boundary; its component compiles those **same source files** with the firmware
toolchain. Desktop `.a` files are not firmware libraries.

## ESP32 library-only cross target

The SDK is internal source code. ESP-IDF and ESP-DSP are external dependencies.
`adapters/esp_idf/library` uses ESP-IDF 6.1's native CMake v2 library API
(`idf_project_init` / `idf_build_library`). It registers the same SDK/BDSP/
transport components, with no `main`, firmware ELF, bootloader or flash
invocation. There is no dummy executable or replacement component implementation.
ESP-IDF itself is supplied through `ESP_IDF_SOURCE_DIR` or
`IDF_PATH`, never installed by the build. ESP-DSP 1.8.2
(`a53a0756833c045311ea1d79a2badf495cdfde4c`) and pinned BDSP sources may be obtained
through FetchContent. Source overrides can supply BDSP and ESP-DSP locally; the
ESP-DSP component directory must be named `esp-dsp` for IDF dependency resolution.
Ordinary desktop builds neither fetch nor build these ESP dependencies.

Activate an existing compatible ESP-IDF tool/Python environment first, or set
`ESP32_TOOLCHAIN_DIR` to an existing compiler root containing `bin`. Paths are
configuration inputs, never embedded in source code. PlatformIO is not required:
its installed compiler can be used by supplying that directory. FetchContent
does not install compilers or Python dependencies. Then:

```powershell
# Explicit target from the already configured desktop build:
cmake --build build-gui --target esp32_sdk

# Or a standalone library build, without configuring any desktop target:
cmake -S adapters/esp_idf/library -B build-sdk-esp32 -G Ninja
cmake --build build-sdk-esp32 --target esp32_sdk
cmake --install build-sdk-esp32 --prefix build-sdk-esp32/prefix

# Native helper using IDF_PATH and ESP32_TOOLCHAIN_DIR from the environment:
python scripts/build-esp32-sdk.py
# Optional isolated Python dependency preparation (no compiler/platform install):
python scripts/build-esp32-sdk.py --prepare-python --tools-cache build-sdk-python
```

Standalone options: `PORTABLE_SDK_MAX_FFT_SIZE=512/1024/2048/4096` (default 4096,
compiled capacity, not the active processing window), `PORTABLE_SDK_ESP_DSP=ON`
(default, OFF selects ANSI), `PORTABLE_SDK_TRANSPORTS=ON` (default, OFF excludes
the TCP/UART adapter). Desktop dispatcher equivalents are
`ESP32_SDK_FFT_CAPACITY`, `ESP32_SDK_ESP_DSP`, `ESP32_SDK_IDF_PATH`,
`ESP32_TOOLCHAIN_DIR` and
`ESP32_SDK_BUILD_DIR`. Root target output defaults to `build-gui/esp32-sdk`.
All sdkconfig/generated defaults remain inside that separate build directory.

Installation places SDK, BDSP, optional ESP-DSP and transport archives in
`lib/esp32`, public SDK/BDSP headers in `include`, and the actual sdkconfig/header
plus compiler/backend/capacity metadata in `share/PortableDSP/esp32`.
These archives require matching ESP32 ABI, IDF configuration and IDF framework
libraries at the firmware's final link; they are not a self-contained generic
host CMake package. The existing source components remain the preferred way to
integrate into arbitrary host firmware, letting its IDF build resolve dependency
libraries, linker scripts and chip configuration. No firmware is supplied by the
library-only target.

CI (`.github/workflows/esp32-sdk.yml`) uses pinned IDF 6.1 sources, the official
`idf_tools.py install xtensa-esp-elf` command, an isolated Python environment, and
the same native helper. It installs only a compiler tool package, without a
PlatformIO platform; repeated runs use the tools cache. The matrix checks ANSI
512 and ESP-DSP 2048/4096, installs archives/headers/profile, checks absence of
firmware ELF files, and publishes SDK artifacts. CI execution is only confirmed
after GitHub has run it; writing the workflow is not a successful CI run.

Initial preflight tests used TDD, checking invalid FFT capacity and missing
compiler. The user's clean local IDF checkout is updated in place to stable 6.1
(`fff9895c82d744c7237be8847347bdd1b07c6643`). Its supported compiler is GCC
15.2.0_20251204; SDK/tool version checks are retained. The compiler is installed
separately by the official IDF tools manager, preserving the user's pre-existing
GCC 14.2.0 toolchain. Two mistakenly installed PlatformIO packages were removed;
the final build has no PlatformIO Python/API dependency. Cleanup of the temporary
older compiler and two Python caches was rejected by automatic command review;
the user subsequently removed the reported directories and compiler archive
with the supplied PowerShell command without errors.
Hardware execution remains outside this validation.

The library defaults to IDF's standard performance optimization profile.
GCC 15.2 reproduced an internal compiler error in ESP-DSP's image-convolution
source under `-Og`, with both ESP-DSP 1.6.4 and 1.8.2. This function is unrelated
to the SDK's audio FFT but belongs to the upstream component's archive. The
performance profile is tested separately; do not claim Debug cross builds pass.
This is not a compiler-version-check bypass, per-file flag patch or source patch.

Native acceptance (2026-10-06): Windows cross build and installation passed for
ESP32 / IDF 6.1 / GCC 15.2.0_20251204 / ESP-DSP 1.8.2, compiled FFT capacity 4096,
performance profile, with both TCP and UART adapter sources. Installed archives:
`libportable_sdk.a`, `libbdsp.a`, `libesp-dsp.a`, `libstream_adapter.a`; SDK headers,
sdkconfig and build-profile metadata are present. No firmware `.elf` was produced.
The desktop `esp32_sdk` dispatcher also passed with paths provided through local
CMake cache variables. Desktop rebuild and five targeted SDK/runtime/DSP tests
plus two runner isolation tests passed. This is author validation, not independent
acceptance, physical-board execution, numerical target execution, or a completed
GitHub CI run. The CI matrix covers additional configurations when it is run.

Reference: [official ESP-IDF 6.1 library composition example](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/examples/build_system/cmakev2/features/idf_as_lib/CMakeLists.txt).

## Example and source installation

`examples/esp32_streamer` targets the ordinary ESP32 with an 8 kHz stereo source,
256 samples/block, eight changing sine tones, and no playback. Default is raw
PCM over UART1. `menuconfig` can select TCP and/or Window → Hann → stereo FFT
→ RMS → Mel8. History is OFF (`max_history_frames=0`); the route contains no
history node. The common executor retains its 60 Hz window cadence, including
133/134-sample hops at 8 kHz and initial zero-prefill. The raw route uses the same
executor with AudioSource → NetworkOutput. The active spectral window defaults
to **2048 samples per channel** (4096 planar PCM values across stereo). Choose
512/1024/2048/4096 under "Portable stream example"; SDK capacity is a separate
choice under "Portable DSP SDK", default 4096. A smaller SDK build excludes larger
window choices; admission rejects unsupported spectral sizes before allocation.
Raw PCM block size remains 256 per channel; Mel8/raw wire buffers do not grow with
the internal FFT window.

Profile's memory limit defaults 256 KiB; the example offers an explicit 64–512 KiB
budget, default 384 KiB for 4096 and 256 KiB otherwise. Host ANSI preparation of the
existing stereo Window/Hann/FFT/RMS/Mel8 route reserves 181655 bytes for 2048 and
284055 bytes for 4096 with a 64 KiB adapter reserve and two 2304-byte queue slots,
plus 2048 source PCM bytes. These are host-accounted bounds, not measured ESP32
heap headroom; allocator/firmware/network stacks and shared backend tables must
fit the host-provided reserve and actual chip budget. An insufficient budget is
rejected, never silently reduced to 512.

Register these three directories in the firmware's `EXTRA_COMPONENT_DIRS`:

```
adapters/esp_idf/bdsp
adapters/esp_idf/portable_sdk
adapters/esp_idf/stream_adapter
```

Supply the existing BDSP source tree explicitly with `-DBDSP_SOURCE_DIR=...`,
pinned to `2c68056dfaeffba2f34fc64eee9bc9e331e83c7c`. The wrapper compiles upstream
sources and does not implement a replacement codec. No fetching or SDK installation
is performed. The example's top-level CMake registers these directories itself.
The intended native toolchain is ESP-IDF 5.4+ with C++20 and the `esp_driver_uart`
component; older IDF component names/toolchains are outside this profile.

With an **already installed** ESP-IDF environment, the firmware build commands are:

```powershell
idf.py -C examples/esp32_streamer -DBDSP_SOURCE_DIR=C:/path/to/pinned/BDSP set-target esp32
idf.py -C examples/esp32_streamer -DBDSP_SOURCE_DIR=C:/path/to/pinned/BDSP menuconfig
idf.py -C examples/esp32_streamer -DBDSP_SOURCE_DIR=C:/path/to/pinned/BDSP build
```

The coordinator can include `adapters/esp_idf/install_sources.cmake` in the SDK
install composition. It installs public source/header topology, component manifests
and the example under `share/PortableDSP/embedded`; the component source paths
work in either repository or installed source package. BDSP remains an explicit
external source dependency. `embedded_support` also installs/exports its normal
host target and public headers alongside the other SDK libraries.

The ESP32 example defaults to `PORTABLE_SDK_ESP_DSP=ON`; supply an existing
`esp-dsp` component. Nothing is downloaded/installed. Explicit
`-DPORTABLE_SDK_ESP_DSP=OFF` selects the repository's existing ESP-DSP ANSI
implementation for a standalone fallback/host check. `ON` selects the same
`backend_esp_dsp.cpp`, with an externally supplied `esp-dsp` IDF component;
it excludes bundled ANSI objects so symbols are not duplicated. Firmware owns
that external dependency/version. The maximum FFT capacity is configurable via
`CONFIG_PORTABLE_SDK_MAX_FFT_SIZE` (or explicit CMake override
`PORTABLE_SDK_MAX_FFT_SIZE`); ESP-DSP's own `CONFIG_DSP_MAX_FFT_SIZE` must cover it.
Backend selection stays a CMake option because ESP-IDF5.x expands component
dependencies before Kconfig (see [build-system rules](https://docs.espressif.com/projects/esp-idf/en/v5.1.2/esp32s3/api-guides/build-system.html)).
Do not initialize/deinitialize shared ESP-DSP tables concurrently
with prepared plans. External backend native linking remains unverified here.

## Parallel analysis windows

The existing graph supports AudioSource fan-out to independent Window/Hann/FFT
branches. For example, a 2048/4096 window feeds a merged spectrum/Mel/history,
while a 512/1024 stereo FFT feeds separate left/right FFTView consumers. Short
channel views reuse the same stereo FFT result instead of repeating transforms
for each display. Source sequence/time metadata allows channel synchronization;
different window sizes have different temporal/frequency resolution. The current
window scheduler remains 60 Hz; per-branch rate control/beat detection/effects
are separate features. No new beat detector is claimed here.

The host SDK regression executes long 2048 plus short 512 on one ingress, checks
distinct tone peaks for left/right, shared source clocks and zero processing
allocations. It requires no change to the executor. The streaming example still
publishes a single raw or Mel8 route; multi-stream effect firmware can compose
the same public graph API with an explicitly admitted aggregate memory budget.

## Host firmware responsibilities

UART1 defaults to TX17/RX16, 2,000,000 baud, 8 data bits, no parity, one stop bit.
Pins and baud are menuconfig settings. Use a separate console port; logging must
never share the BDSP UART. The host receiver sends the same BDSP Subscribe
packet as a TCP subscriber. The example installs the UART driver with a bounded
4096-byte RX ring, eight events, and **zero TX ring**. `uart_tx_chars` writes only
available FIFO bytes and `uart_read_bytes(..., 0)` reads without waiting.
[ESP-IDF UART API](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/api-reference/peripherals/uart.html)
defines these calls and their driver constraints.

For TCP, override the weak C hooks in `main/host_hooks.h` in firmware:
`streamer_host_setup()` configures WiFi; `streamer_host_acquire_socket()` promptly
returns an already connected/accepted socket or -1; `streamer_host_release_socket()`
closes it. The adapter sets O_NONBLOCK and borrows the descriptor. Socket creation,
listen/accept, authentication, WiFi credentials and reconnect policy belong to
the host. No endpoint discovery, automatic WiFi configuration, graph deployment
or engine-v1 mutation endpoint is provided. TCP hook defaults intentionally return
-1 until the host supplies a descriptor.

For I2S/Bluetooth, enable `STREAMER_EXTERNAL_PCM` and override
`streamer_host_read_pcm(FrameView*)`. Return `ok` with planar stereo PCM and
accurate metadata, `would_block` when no block is ready, or `unavailable` with a
valid marker (sequence0, no payload). Borrowed buffers must remain alive until
the DSP call returns. The adapter never silently substitutes synthetic audio for
an explicitly selected device source. Physical capture/device driver setup is
an extension point, not implemented hardware support. ISR/capture callbacks may
only copy to a bounded buffer and notify a task; they cannot run DSP or transport.

## Budgets, ownership and recovery

`Profile`/`admit` reject invalid frame geometry, frame/payload capacity, queue
count, arithmetic overflow, memory and bandwidth before starting tasks.
`runtime_limits` bounds graph nodes/edges, frame samples, prepared memory and
turns history off. After `PreparedGraph::prepare`, admission includes its published
`MemoryRequirements`; source vectors are checked separately. These are declared
budgets, not ESP heap/CPU/WiFi measurements. The example reserves 64 KiB for adapter
buffers, driver queues, source/task/control overhead; a two-slot encoded-frame pool
is additional. The configurable total is 256 KiB by default (384 KiB for the
4096-window example). Firmware WiFi/Bluetooth heap
and its other components require their own budget outside this SDK allocation.

Wire admission uses a deliberately conservative upstream COBS-ZPE expansion bound:
two wire bytes per source byte, fragment/header/checksum allowance included. Raw
rate is sample_rate/block_samples; spectral rate is 60 Hz. UART uses baud/10 for
8N1 and 80% utilization; TCP requires an explicit byte/s budget. Raw defaults fit
2 Mbaud or 200,000 TCP bytes/s; 115,200 UART is rejected. Conservative admission
may reject a link that fits typical packets; it never relies on typical compression.

`Session` borrows distinct caller-owned packet/read/frame/fragment/assembly/sample
buffers and the configured UTF-8 stream ID. All calls require serialization.
Callbacks borrow decoded samples/ID only until return. `publish` snapshots output
into the supplied frame buffer; the runtime callback encodes into one of the two
queue slots before its borrowed view expires. No per-tick allocation is introduced
by the source/session. `prepare` allocates the common executor's bounded plan.

DSP and transport use separate FreeRTOS tasks. Capture/processing never waits
for TCP/UART. The transport task performs one bounded partial write/read per poll,
and may idle. Full queues drop complete data frames and mark discontinuity. A
saturated unavailable marker fails the session so stale data cannot outlive it.
If that marker cannot enter the example's producer queue, the callback atomically
stops the current run and ignores subsequent outputs from that tick. The transport
task cancels its Session, purges the queue and releases its connection; no DSP-side
socket access or cross-task Session reset occurs. Restart is explicit. Already
started transport calls finish before the owner performs cleanup.
Malformed framing, geometry/type/ID, driver RX overflow, EOF and partial-write
failure reset pending buffers. Handshake deadline is five seconds. Reconnect is
explicit through `begin`; a new nonzero publisher epoch and cleared transport/queue
are required. UART peers must reset failed sessions too; the example emits the
existing zero delimiter to resynchronize an abandoned partial packet. It does not
define another framing or reconnect control packet.
Subscribers mark sequence gaps as discontinuities as well as epoch/generation
changes, preserve explicit discontinuity flags and reject non-advancing sequences.
Generation and sequence fences apply within the current publisher/producer
identity. A newer-generation unavailable marker admits the new generation's
fresh sequence; an unknown-generation marker keeps the known fence. Discontinuity
does not authorize duplicate or backwards data. Subscriber Session keeps sixteen
retired `(publisher epoch, effective producer epoch)` tuples in a fixed FIFO ring
(256 bytes plus cursor/index state). Effective producer epoch uses producer_epoch
when nonzero, otherwise frame epoch; a source-less clearing marker in the current
publisher belongs to its last known producer. Recently retired data and markers
are ignored before changing cursor or gap state. The oldest tuple is evicted on
the seventeenth retirement, so this is a bounded replay window, not an unlimited
identity history. Explicit stop/begin resets it; all consumers must rebuild after
changes to Session's private object layout. No wire fields or public methods change.

## Wire scope and verification

The typed-v2 stream endpoint uses unchanged BDSP packet framing: ID11 Subscribe,
ID12 Ack (bytes `00 02`, FrameType, UTF-8 ID), ID13 common stream fragments, and
ID14 existing explicit error. Both TCP and UART use the same `Session`,
`PacketStream`, `FrameAssembler` and caller-buffer frame codecs. Engine-v1
QDataStream control, registry registration, and graph mutation are outside this
streaming SDK and their TCP wire is unchanged.

Private Windows host checks (MinGW 13.1, Release, C++20, exceptions/RTTI disabled):

```powershell
cmake -S tests/embedded_sdk_host -B "$env:TEMP/qt-embedded-sdk-01a10ee7" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBDSP_SOURCE_DIR="$PWD/cmake-build-release/_deps/bdsp-src"
cmake --build "$env:TEMP/qt-embedded-sdk-01a10ee7" --target embedded_sdk_tests -j 2
ctest --test-dir "$env:TEMP/qt-embedded-sdk-01a10ee7" -R '^embedded_sdk_tests$' --output-on-failure
cmake -S tests/embedded_sdk_component -B "$env:TEMP/qt-embedded-component-01a10ee7" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBDSP_SOURCE_DIR="$PWD/cmake-build-release/_deps/bdsp-src"
cmake --build "$env:TEMP/qt-embedded-component-01a10ee7" -j 2
ctest --test-dir "$env:TEMP/qt-embedded-component-01a10ee7" --output-on-failure
```

RED: missing synthetic-pipeline public API failed compilation. The first executable
check then exposed unavailable-marker rejection. GREEN: portable test passed,
including 1/127-byte partial IO, handshake/error/timeout, marker, overflow/reset,
reconnect epoch, multi-fragment assembly, UTF-8 rejection, memory/geometry/UART/TCP
admission and raw/shared FFT/Mel execution. Fake-IDF smoke compiles actual component
manifests, ANSI SDK sources, UART/raw and TCP/spectral/device-hook example variants;
adapter and linked component-consumer tests passed 2/2. Stubs validate manifest,
source registration and declared API use only. They are **not native cross proof**.

Source installation was also exercised: `cmake --install` of the private host
build to `%TEMP%/qt-embedded-install-01a10ee7`, then configure the same component
smoke with `-DSDK_ROOT=.../share/PortableDSP/embedded` into a distinct
`%TEMP%/qt-embedded-installed-component-01a10ee7`. All component/example sources
rebuilt from the installed source tree; adapter and linked consumer passed 2/2
(0.13 s). This verifies source-package relative paths, not ESP toolchain behavior.

On this machine IDF_PATH, C:/Espressif and idf.py are absent. Native ESP-IDF build,
external ESP-DSP linking, flashing, physical UART/TCP, WiFi/I2S/Bluetooth capture,
ESP heap/CPU/ISR timing and hardware throughput are **UNVERIFIED**. No SDK was
installed, no speaker signal, full suite, soak or Linux test was run. The author
report is ready for independent review, not acceptance.

Primary-review regressions: sequence1→3 initially failed with no second-frame
discontinuity (RED); the actual example callback with two queued data frames
dropped an unavailable marker and left running=true (RED). The repaired portable
test passes 1/1 (0.08 s). Component checks now include
`embedded_sdk_queue_tests`, which compiles the actual main.cpp callback and
transport-task exit against a bounded fake queue, verifies ordinary data loss,
fatal marker fencing, no post-fence enqueue, old-queue purge, and pending Session
packet cancellation. Updated component checks pass 3/3 (0.19 s), with both example
variants rebuilt. Independent re-review remains the acceptance gate.

## Configurable FFT follow-up (2026-10-06)

New default regression first failed on the old Profile512 value, then passed
with Profile2048. Checks now cover all supported 512/1024/2048/4096 sizes,
unsupported compiled capacity, insufficient memory, finite Mel outputs and
allocation-free ticks. Existing session/marker/partial-I/O tests are retained.
The long/short stereo branch check is characterization of the existing executor,
not a second FFT implementation.

Host verification: portable SDK 13/13, installed-source component 3/3, separate
component configurations for capacities 512/2048/4096 each 3/3, desktop targeted
DSP/runtime/SDK checks 4/4, compact512 SDK targeted check 1/1. Invalid513 capacity
and an ESP-DSP table limit below the requested capacity both fail configuration.
Desktop applications rebuilt. Logs: build-sdk/fft-config-verification.log,
build-sdk/fft-component-*-tests.log, build-gui/fft-config-tests.log.

These are host/fake-IDF checks. Actual ESP-IDF menuconfig/cross-link, optimized
ESP-DSP and board heap/timing need the existing SDK/board; no SDK was installed,
no physical device/native GUI tests were run, and no independent follow-up review
is claimed for this later configuration change.
