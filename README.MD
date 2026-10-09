# Qt Visualization

Windows application and portable SDK for typed signal processing, numeric graph editing, charts, 3D history and WLED audio sync. The GUI, headless CLI and ESP example assemble the same `PreparedGraph` / `GraphExecutor` and application control service.

Desktop builds use CMake 3.30+, C++23 and Qt 6 with Widgets, Charts, DataVisualization, Network and SerialPort. The portable SDK uses C++20, has no Qt/miniaudio/OS socket dependency, and supports builds without exceptions or RTTI. Use the compiler matching your Qt installation. FetchContent dependencies use pinned commits; no separate dependency installation is performed by the project.

## Build

Use the shared CMake presets. `release`, `debug` and `sdk` configure only
`cmake-build-release`, `cmake-build-debug` and `cmake-build-sdk`, respectively.
Run CMake and Ninja from a developer shell with the compiler matching Qt in
`PATH`. Set `QT_ROOT` to your Qt installation (the directory containing `bin`
and `lib/cmake`). For example, after setting your local installation paths:

```powershell
$env:QT_ROOT = '<your Qt installation>'
$env:PATH = "$env:QT_ROOT/bin;<matching compiler bin>;<Ninja bin>;<CMake bin>;$env:PATH"
cmake --preset release
cmake --build --preset release --parallel 4
ctest --preset release -R '^(signal_contracts_tests|numeric_control_session_tests)$'
./cmake-build-release/bin/core_app.exe --config ./cmake-build-release/bin/config.json
```

For Debug, replace `release` with `debug` in configure/build/test commands and run
from `cmake-build-debug/bin`. CMake deploys Qt DLLs, compiler runtime and plugins
directly beside built applications; do not copy executables to another build
directory. CLion can select the same presets. Machine-specific settings can live
in an ignored `CMakeUserPresets.json` inheriting a shared preset; do not put local
installation paths in the shared file. An existing cache must use the matching
compiler/generator; inspect it before attempting a toolchain change.

When opening a Windows PowerShell console from an automation environment, set
the child process's `PSModulePath` to the installed Windows PowerShell module
directories before starting it. `-NoProfile` does not prevent module autoloading:
an inherited Codex runtime module path can load its bundled PSReadLine and cause
an untrusted-publisher prompt. Use the installed system PSReadLine; do not change
Windows execution policy or publisher trust to resolve this prompt. Set the
working directory to the repository root and clear test-only `QT_QPA_PLATFORM`
and `QT_PLUGIN_PATH` overrides before a normal GUI launch.

Run focused checks while changing code; `ctest --preset release` runs the whole
registered suite only when that scope is requested. Offscreen Qt tests use the
platform plugin from the configured Qt installation. Native graphics, physical
capture, hardware timing and long load tests require separate acceptance;
offscreen results do not establish those properties. TestSignal supplies 5–10
changing tones without speaker playback. Historical build trees and review
artifacts are retained until their contents have been reviewed for cleanup.

For a Qt-free C++20 package without exceptions or RTTI, using the compiler and
Ninja in `PATH`:

```powershell
cmake --preset sdk
cmake --build --preset sdk --parallel 4
ctest --preset sdk -R '^(signal_contracts_tests|numeric_control_session_tests)$'
cmake --install cmake-build-sdk
```

The SDK installs into `cmake-build-sdk/install`. External consumers use
`find_package(PortableDSP CONFIG REQUIRED)` and exported `PortableDSP::` targets.
ESP-IDF components compile the same installed SDK source tree, rather than linking
host binaries. Firmware toolchain configuration remains a separate platform setup.

## Applications

`core_app --config <path>` opens a canonical project. The default path is `config.json` beside the executable. For a missing local configuration, first startup creates an arranged TestSignal demonstration preset, opens its graph views and activates it through the normal Apply/Run path. Its eight changing tones stay inside the processing pipeline without speaker playback. Existing projects retain their saved layout and settings and load without automatically starting processing. Apply prepares the draft atomically; Run also requests processing. A rejected document remains protected from overwrite. Existing old/future schemas are rejected; migration and compatibility execution are outside this prototype.

`pipeline_cli` runs a supplied canonical project or its shared typed default graph:

```powershell
./cmake-build-release/bin/pipeline_cli.exe --synthetic --duration 1000
./cmake-build-release/bin/pipeline_cli.exe --config project.json --preset preset-id --duration 1000
./cmake-build-release/bin/pipeline_cli.exe --config project.json --server --listen 127.0.0.1 --port 8213 --allow-mutations
```

`--synthetic` replaces physical capture with silent changing tones. `--duration 0` runs continuously. Presets resolve by stable ID or a unique name. The CLI never saves over the supplied project.

Use `core_app --remote-host <host> --remote-port 8213` to open a numeric TCP endpoint. The CLI uses `--remote <host> --port 8213`. Both support an explicit serial endpoint with `--serial-port COM3 --serial-baud 2000000`; select one transport. TCP and UART carry the same shared CONTROL and DATA protocol. Remote mutation must be explicitly enabled on the server.

`aggregator_server_app` provides discovery metadata. A CLI publisher registers with `--registry-host <IP> --registry-port 8212` and an optional stable `--uuid`. Discovery locates a CONTROL endpoint; actual negotiation and domain/device admission authorize graph operations and DATA bindings.

## Graphs and presentation

Contracts independently describe semantic meaning, numeric dtype, rank, extents, channels, storage representation and coordinates/time. Ten dtypes support scalar, vector and matrix data. PCM, Spectrum and Mel remain distinct. Connections require compatible declared contracts; conversions use an explicit Converter with rounding, overflow, normalization and precision policies.

Sources and Processors expose declared output ports. Sinks consume inputs. A source/processor preview selects an actual output port, including separate History and filled-count outputs. Independent dock instances can select different endpoints and channels. Numeric previews are read-only diagnostics; presentation conversion never changes DSP parameters. Exact integer text is retained when chart floating-point projection loses precision.

History stores `[frame][band]`. Runtime row events update bounded rings incrementally; full history is an explicit cold query. Real coordinates label Hz and seconds. Otherwise labels identify band/frame indexes. Newest-frame age is zero. Hidden views stop drawing while processing continues. Accepted Stop clears presentation; failed commands preserve the working graph.

WLEDEncode is a pure structured-packet processor. WLEDOutput owns the configured `destination`, `interface`, `port` and `cadence_ms`; it sends the 44-byte audio-sync wire format with a private counter and silence after 300 ms without a valid packet. Output I/O is outside prepared processing. Physical WLED behavior requires separate hardware acceptance.

## Camera and settings

Focus the 3D view for W/S, A/D, Space/Ctrl and Shift movement; Z resets the camera. Mouse rotation and wheel zoom remain available. Settings > Controls edits camera bindings. Theme and language apply through the central application services; controls inherit the common palette. Camera and presentation settings save alongside the editable draft, separately from processing parameters.

## Guides and acceptance

- [Architecture and responsibility map](docs/architecture-map.md)
- [Numeric runtime API](docs/numeric-runtime-api.md)
- [Numeric editor](docs/numeric-editor.md)
- [Canonical project format](docs/numeric-project-format.md)
- [CONTROL/DATA and remote adapters](docs/numeric-remote-protocol.md)
- [Embedded profile and NVS](docs/embedded-numeric-profile.md)
- [Integration and independent acceptance checklist](docs/refactoring/progress-checklist.md)

Build, author verification, independent acceptance and native/hardware evidence are recorded separately. Historical bounded checks do not establish acceptance of the current whole system.
