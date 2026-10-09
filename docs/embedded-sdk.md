# Embedded SDK

The canonical numeric C++20 SDK shares the desktop processing implementation:
`signal_contracts`, `dsp_kernels`, `dataflow_runtime`, domain/application,
`bdsp_protocol`, and `embedded_support`. Portable targets have no Qt, miniaudio,
OS socket or ESP-IDF dependency and support builds without exceptions/RTTI.
ESP adapters and composition roots own platform resources. Firmware components
compile the same SDK sources; desktop `.a` files are not firmware libraries.

The current API and stock resource ledger are documented in
[Embedded numeric profile](embedded-numeric-profile.md). That guide is the current
reference for DATA4, the sole executor/control service, graph link admission,
NVS restore, memory ceilings and verification scope.

## Host SDK build and installation

Use a separate build directory and an existing compiler/CMake environment:

```powershell
cmake --preset sdk
cmake --build --preset sdk --parallel 2
cmake --install cmake-build-sdk
```

Root FetchContent pins BDSP to
`2c68056dfaeffba2f34fc64eee9bc9e331e83c7c`. A local source override can be
supplied with `FETCHCONTENT_SOURCE_DIR_BDSP`. `PORTABLE_DSP_MAX_FFT_SIZE`
selects compiled host capacity 512/1024/2048/4096, independently of active windows.

A host consumer uses the installed package and its transitive dependencies:

```cmake
find_package(PortableDSP CONFIG REQUIRED)
target_link_libraries(consumer PRIVATE
    PortableDSP::embedded_support
    PortableDSP::portable_numeric_control_session)
target_compile_features(consumer PRIVATE cxx_std_20)
```

`adapters/esp_idf/install_sources.cmake`, included by the root SDK install,
installs the public source/header topology, component manifests and example under
`share/PortableDSP/embedded`. Component source paths work from the repository
and the installed source tree. BDSP remains an external source dependency.

## ESP32 library-only cross target

`adapters/esp_idf/library` uses ESP-IDF 6.1's native CMake v2 library API
(`idf_project_init` / `idf_build_library`). The default builds SDK/BDSP and
optional ESP-DSP, transport and persistence archives without a firmware ELF,
bootloader or flash invocation. ESP-IDF is supplied through `ESP_IDF_SOURCE_DIR`
or `IDF_PATH`; the build does not install a platform or compiler.

Activate an existing compatible ESP-IDF tool/Python environment, or set
`ESP32_TOOLCHAIN_DIR` to an existing compiler root containing `bin`. The library
composition pins ESP-DSP 1.8.2 sources to
`a53a0756833c045311ea1d79a2badf495cdfde4c` and BDSP to the revision above.
FetchContent source overrides can supply them locally. The ESP-DSP component
directory must be named `esp-dsp` for IDF dependency resolution. Ordinary desktop
builds do not build these IDF components.

```powershell
# Dispatcher from an already configured desktop build:
cmake --build --preset release --target esp32_sdk

# Standalone native library composition:
cmake -S adapters/esp_idf/library -B build-sdk-esp32 -G Ninja
cmake --build build-sdk-esp32 --target esp32_sdk
cmake --install build-sdk-esp32 --prefix build-sdk-esp32/prefix

# Existing IDF_PATH / ESP32_TOOLCHAIN_DIR environment:
python scripts/build-esp32-sdk.py
```

The helper also supports `--prepare-python --tools-cache build-sdk-python` for
explicit isolated Python dependency preparation. It does not install a compiler
or platform. Configuration paths are supplied by the host, not embedded in code.

| Standalone option | Scope |
| --- | --- |
| `PORTABLE_SDK_MAX_FFT_SIZE` | 512/1024/2048/4096 compiled capacity; default 4096 |
| `PORTABLE_SDK_ESP_DSP` | ON uses the external optimized component; OFF uses ANSI |
| `PORTABLE_SDK_TRANSPORTS` | ON includes TCP/UART adapter |
| `PORTABLE_SDK_PERSISTENCE` | ON includes NVS adapter |
| `PORTABLE_SDK_EXAMPLE_ENTRYPOINT` | OFF by default; ON also compiles stock main and UART entrypoint objects into native build artifacts |

The optional example archive still does not create a firmware ELF or execute
`app_main`; it requires both transports and persistence. Desktop dispatcher
options include `ESP32_SDK_FFT_CAPACITY`, `ESP32_SDK_ESP_DSP`,
`ESP32_SDK_IDF_PATH`, `ESP32_TOOLCHAIN_DIR` and `ESP32_SDK_BUILD_DIR`.
Generated sdkconfig/defaults stay inside the dedicated build directory.

Installation places native archives in `lib/esp32`, public SDK/BDSP headers in
`include`, and sdkconfig/compiler/backend/capacity metadata in
`share/PortableDSP/esp32`. Native archives require matching ESP32 ABI, IDF
configuration and framework libraries at final firmware link. Source components
let the host IDF build resolve its dependency libraries, linker scripts and chip
configuration.

The native composition selects IDF's performance optimization profile. A previous
GCC 15.2 Debug `-Og` build encountered an upstream ESP-DSP image-convolution
compiler failure; performance archive proof does not establish Debug support.
CI matrix configuration is in `.github/workflows/esp32-sdk.yml`; a configured
workflow does not establish that GitHub has executed it.

## Firmware example and source integration

`examples/esp32_streamer` defaults to UART1, raw stereo PCM at 8 kHz,
256 samples/channel per block, eight changing synthetic tones and no playback.
Menuconfig can select TCP, external capture, spectral Mel8 and active FFT window.
RAW and spectral outputs include an explicit Native→FixedLE Converter before
NetworkOutput. History is OFF. Default spectral window is 2048/channel; 4096
requires a matching compiled SDK/backend capacity. Source block size remains 256.

Register these component directories in the firmware's `EXTRA_COMPONENT_DIRS`:

```text
adapters/esp_idf/bdsp
adapters/esp_idf/portable_sdk
adapters/esp_idf/stream_adapter
adapters/esp_idf/persistence
```

The stock example registers them itself. Supply existing pinned BDSP sources
with `BDSP_SOURCE_DIR`; the component wrapper compiles upstream sources without
providing a replacement codec. The example defaults to
`PORTABLE_SDK_ESP_DSP=ON` and requires an existing `esp-dsp` component. Explicit
`PORTABLE_SDK_ESP_DSP=OFF` selects the existing bundled ANSI implementation.
The source example does not install dependencies.

With an already configured ESP-IDF environment:

```powershell
idf.py -C examples/esp32_streamer -DBDSP_SOURCE_DIR=C:/path/to/pinned/BDSP set-target esp32
idf.py -C examples/esp32_streamer -DBDSP_SOURCE_DIR=C:/path/to/pinned/BDSP menuconfig
idf.py -C examples/esp32_streamer -DBDSP_SOURCE_DIR=C:/path/to/pinned/BDSP build
```

SDK capacity is `CONFIG_PORTABLE_SDK_MAX_FFT_SIZE` or explicit CMake
`PORTABLE_SDK_MAX_FFT_SIZE`; ESP-DSP's `CONFIG_DSP_MAX_FFT_SIZE` must cover it.
Do not resize/deinitialize shared ESP-DSP tables concurrently with prepared plans.

## Canonical source, control and DATA APIs

The firmware owns one `GraphExecutor`, `GraphLifecycle` and
`ProcessingControlService`. CONTROL Prepare/Apply/parameter operations use the
same working graph as processing and ExplicitPersist. Processing DTO contains
`NumericProcessingGraph`; runtime limits are `NumericRuntimeProfile` and
`NumericRuntimeLimits`. Device queries preserve runtime global resource overhead.

For the local source, `default_processing_graph(profile, stream_id)` constructs
an owning canonical graph; a Status/out overload reports errors.
`SyntheticPipeline::prepare_source(profile, stream_id)` prepares only source
storage. `tick_source(NumericFrameHandler, user)` supplies borrowed Native PCM
which the host passes to `GraphExecutor::process_numeric_ports`. The standalone
`prepare`/`tick` helper includes its own prepared graph and is not used alongside
the firmware executor.

CONTROL and DATA share one PacketStream. DATA uses positive Subscribe26/Ack27
negotiation and Fragment31, DATA payload4. `NumericSession` external mode receives
`PacketView` through `accept_packet` and copies bounded fragments through an
injected PacketSender. `publish`/`drain_packets` execute after CONTROL poll returns.
Reference bindings fence nonce/session/revision/publisher epoch and delivery
sequence. No in-band fallback to a legacy graph, config or stream protocol exists.

`graph_link_admission(profile, graph, packet_payload_capacity)` checks aggregate
actual sink geometry, cadence and negotiated fragment overhead before Prepare
and before DATA binding. Unchanged source cadence does not imply unchanged
Window/FFT output bandwidth. Selected endpoint/contract must match the applied
graph. Apply retires stale DATA while preserving unaffected CONTROL and
TCP/UART resources through selective queue discard.

## Host firmware responsibilities

UART1 defaults to TX17/RX16, 2 Mbaud, 8N1. Pins/baud are menuconfig settings;
logging uses a separate console. For TCP, host hooks supply WiFi setup and an
accepted/connected descriptor. The adapter makes it nonblocking; the host owns
closing it through `streamer_host_release_socket`. Default socket acquisition
returns -1 until the firmware provides a connection.

Enable `STREAMER_EXTERNAL_PCM` and override
`streamer_host_read_pcm(signal_contracts::DataFrameView*)` for I2S/Bluetooth.
Return `ok` with matching Native planar PCM, `would_block` when no block is ready,
or `unavailable` with a valid typed marker. Borrowed storage lives until the
synchronous processing call returns. Capture does not substitute synthetic audio.
ISR/capture callbacks copy only into a bounded owning queue and notify a task.
Physical drivers are host extension points.

One serialized stock task owns source, DSP, control and transport. Processing
callbacks encode/copy to preallocated DATA buffers; outer polling performs I/O.
No graph processing or transport occurs in an ISR. Backpressure retains bounded
pending delivery or drops a complete frame and reports discontinuity. A blocked
unavailable marker fences the session so old payload cannot outlive it.

`streamer_host_memory_budget()` supplies actual memory available after other
firmware owners and is sampled before constructing Device. Configured default
320 KiB is a ceiling; stock uses the minimum of that ceiling and actual host
budget. CONTROL defaults to 4 KiB wire and 8 KiB decode arena with 1024-byte
packets. The ledger measures session owners, DTO snapshots, objects and one
exclusive cold pool shared by CONTROL staging and persistence. The private primary
decode arena remains separate. Task stack and node-plan ceiling are 32 KiB each.
See [Embedded numeric profile](embedded-numeric-profile.md) for the accounting
and separate cold replacement peak. Compact TCP/UART stereo FFT2048 passed bounded
host composition checks at 320 KiB. Atomic replacement at that budget was rejected
with the working graph preserved; successful replacement and FFT4096 used an
explicit 1024 KiB budget. These results do not measure physical ESP32 heap or stack.

Stock NVS opens namespace `numeric` without erase, restores admitted last-good
processing from two CRC-protected slots, and prepares/applies through the same
service. Only ExplicitPersist writes; replay does not write again. Optional
host-owned persistence/restore hooks are declared in `main/host_hooks.h`.

## Current verification

Targeted canonical host oracles are `embedded_numeric_persistence_tests`,
`embedded_numeric_session_tests` and `embedded_nvs_adapter_tests`. They cover
both codecs, exact64 metadata, HistoryOFF/device/wire admission, interrupted and
future records, shared DATA partial delivery/backpressure and source-only execution.

`tests/embedded_sdk_component` compiles real component manifests/main sources
against fake IDF. Its `numeric_bootstrap_raw`, `numeric_bootstrap_spectral2048`
and `numeric_bootstrap_spectral4096` tests execute stock bootstrap, ExplicitPersist,
reboot restore and rejection of insufficient actual host memory. These are host
composition oracles. The current six-test harness includes ordinary
320 KiB stereo FFT2048 TCP/UART cases: State via both codecs, continuous DSP,
Persist/reboot and bounded rejection preserving the working graph.
Extended RAW/2048/4096 cases use 1024 KiB and also exercise replacement. All six
passed in FINAL; see [the core fixes report](core-fixes-final-2026-10-09.md).
These host checks do not
execute a native task or measure chip resources.
The component harness accepts `SDK_ROOT` to verify installed source topology.

Actual Xtensa ESP-IDF cross archive build and the installed canonical host package
consumer have separate evidence. See [Embedded numeric profile](embedded-numeric-profile.md)
for current logs and proof boundaries. Host/fake-IDF PASS, native compilation,
independent acceptance and physical-board execution remain separate claims.
No hardware heap/stack/timing, power-cut durability, throughput or physical
WiFi/I2S/Bluetooth capture proof follows from these builds.
