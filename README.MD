# Qt Visualization

Windows application for real-time system audio capture, FFT visualization and WLED audio sync.
Requires CMake 3.30+, a C++23 compiler and Qt 6.7+ with Widgets, Charts, DataVisualization, SerialPort and Network.
Use the compiler matching your Qt installation (for example the Qt MinGW toolchain).
BDSP and QtNodes are fetched at pinned revisions.

## Build and test

Run from a terminal with the compiler and Qt `bin` directories in PATH:

```powershell
cmake -S . -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/mingw_64
cmake --build cmake-build-release
ctest --test-dir cmake-build-release --output-on-failure
```

`core_app` is the application with runtime settings; `qt-application` is the compatibility visualizer using the same audio runtime.
`core_app --config <path>` selects a JSON configuration file (default: `config.json` in the working directory).
The aggregator server and client are separate executables.
Tests require the Qt Test component. They cover startup/shutdown, FFT accuracy,
component restarts, TCP registration, GUI stalls and settings mouse interactions.
Audio capture feeds the analyzer independently of the GUI; charts consume the latest
snapshot, so a slow window does not accumulate old audio frames.

## Deployment

```powershell
cmake --install cmake-build-release --prefix dist
cpack --config cmake-build-release/CPackConfig.cmake
```

Installation and ZIP packaging include Qt DLLs, plugins and the MinGW runtime.
Launch `dist/bin/core_app.exe`. Keep the complete installed directory together.
Configuration is saved atomically; the destination directory must be writable.
3D graphs require a working graphics context; other tabs remain available if it cannot be created.

## 3D camera

Click the 3D graph to focus keyboard controls. Hold W/S to move towards/away from
its target, A/D to strafe, Space/Ctrl to move up/down, and Shift to move faster.
Press Z or click Reset camera to restore the original view.
Controls use physical key positions and work across keyboard layouts.
In Settings > Controls, click a primary or alternative key to rebind it;
Escape cancels, Clear removes an alternative, and Restore default keys resets all bindings.
Changes apply immediately and use the existing autosave / Ctrl+S behavior.
Mouse rotation and wheel zoom remain available. Camera rotation, zoom and target
relative to the graph center are stored in `ui.camera`. Autosave waits until
movement pauses; with autosave disabled, use Ctrl+S to save the current view.

Drag the divider between the 3D view and its controls to resize either pane.
The pane proportion is saved with UI settings. Controls scroll on smaller windows,
and the 3D view grows with the application window. Close zoom extends to 5000%.

The top toolbar controls Capture, Analyzer and graph visibility. WLED is configured
separately in Settings > WLED / Network. FFT, Mel and
3D are one-click toggles; More graphs contains the remaining views. Settings open
in a separate window (Ctrl+,) with General, Controls and WLED / Network sections.
Camera translation accounts for graph axis scaling, so strafing has the same
world-space speed at different view angles.

## Audio pipeline and editor

Capture, stereo analysis, history and WLED run independently of the GUI. Analysis
uses an audio-driven sample hop at a base rate of 60 Hz (including fractional hops); views consume a bounded latest-frame mailbox and render at
most 60 Hz. The editor previews at most 10 Hz and debounces draft changes. History
snapshots are prepared on the pipeline event loop; the GUI never locks live history.

Settings > General > Audio source selects System output or System microphone.
The AudioSource node offers the same selection. A valid source change restarts
capture and analysis, discarding stale frames; a mono microphone is duplicated into
both channels. Capture follows the Windows default device and retries if unavailable.
Microphone access must be enabled in Windows privacy settings.

Each channel has its own FFT. The FFT view defaults to Left. SpectrumMerge joins
magnitudes after FFT using Mean, Maximum or Energy (RMS, default), preserving
opposite-phase channel content. One spectrogram receives the merged Mel result.

Open Pipeline in the toolbar to edit typed connections and node parameters. Valid
changes can apply automatically; Apply also validates manually. Missing inputs,
cycles and incompatible ports are rejected. Version 1 supports one audio source,
one stereo FFT, one history and one of each output; merge/gain branches may repeat.
Numeric demonstration nodes do not form a working audio pipeline.

The existing configuration stores `scene_graph` as the editable draft and
`pipeline_graph` as the last valid graph, including parameters and node positions.
Invalid drafts never replace the active graph. Settings and node controls are
synchronized, with existing autosave/manual-save behavior preserved.

The same engine and saved graph run without Qt Widgets:

```powershell
./cmake-build-release/bin/pipeline_cli.exe --config config.json
./cmake-build-release/bin/pipeline_cli.exe --microphone --duration 1000
./cmake-build-release/bin/pipeline_cli.exe --synthetic --duration 1000
```

WLED packets retain the 44-byte audio-sync format. MajorPeak is in Hz. After
300 ms without audio, the sender emits silence. Network failures appear in the
component status. Local UDP tests verify packets; physical WLED needs a separate
hardware check.

Automated checks include stereo/antiphase signals, 2 ms 8 kHz bursts, analysis
cadence and input-to-result latency, blocked GUI, graph persistence, editor
responsiveness and aggregator failure/reconnect scenarios. No long-duration test
is required. For native graphics validation, run `bin/ui_tests.exe` directly with
`QT_QPA_PLATFORM` unset; CTest uses offscreen for deterministic GUI tests.
The Windows workflow builds Debug and Release with pinned Qt/toolchain versions
and dependency revisions, runs tests and verifies Release deployment.


Performance tracing is opt-in and bounded. To reproduce the native live-music
check, play audio through the default Windows output device and run:

```powershell
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:QT_VIS_LIVE_AUDIO = '1'
$env:QT_VIS_TRACE_FILE = "$PWD/cmake-build-release/music.trace.json"
Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
./cmake-build-release/bin/ui_tests.exe
Remove-Item Env:QT_VIS_LIVE_AUDIO, Env:QT_VIS_TRACE_FILE
```

This ten-second measurement uses the real Qt event loop, changing audio,
visible charts and the pipeline editor, plus a local UDP WLED receiver. It checks
analysis cadence, GUI delivery and heartbeat delays, and exports per-stage
counts, mean, p95 histogram bounds and maximum duration. The trace opens in
Perfetto or a Chrome trace viewer. Capture, FFT, graph processing, history,
network queues, GUI delivery and painting have separate timings.

Instrumentation is disabled by default. Enabled hooks use fixed atomic counters
and at most 32768 events; overflow is reported, without growing the buffer.
Audio payloads are never included. File export runs outside the measured interval
and DSP workers. This desktop diagnostic storage is approximately 1.1 MiB;
embedded deployment needs its own memory budget and instrumentation backend.
Windows capture uses WASAPI events instead of polling sleeps. Event-driven
loopback requires Windows 10 version 1703 or later, as documented by
[Microsoft](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording).
Desktop measurements do not establish timing or RAM limits on a microcontroller.


Native rendering QA is separate from offscreen tests:

```powershell
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:QT_VIS_TRACE_FILE = "$PWD/cmake-build-release/render.trace.json"
Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
./cmake-build-release/bin/render_tests.exe
Remove-Item Env:QT_VIS_TRACE_FILE
```

The benchmark moves the window to every connected screen, tests small and large
3D viewports with continuous rotation/translation and a changing synthetic audio
source on its own thread, then tests the large FFT view. It reports screen refresh
rate, device pixel ratio, DSP cadence, native render submission rate/duration and
GUI heartbeat delays. Native submissions retain VSync and are coalesced to at most
60/s; camera movement and pending surface geometry are applied together before
rendering. The dynamic surface uses shared vertices with smooth shading and no
soft shadows. FFT uses one batched path of vertical stems, with maxima per logical
pixel column preserving narrow peaks and frequency positions in Hz. Resizing
invalidates the display geometry without changing DSP resolution or history.
These timings include driver waits; they do not directly measure physical screen
scanout or establish that a particular monitor/driver cannot tear.

## Architecture and independent engine

`portable_dsp` contains FFT, stereo window processing and prepared Mel filters. It
uses C++20 and the ANSI ESP-DSP backend, with no Qt, Windows, sockets or worker
threads. Preparation allocates scratch; processing reuses it. `estimate_memory`
reports payload budgets and `workspace_bytes()` reports retained processor capacity;
allocator overhead and application frame copies are documented separately.

`AudioPipeline` owns capture, analysis, bounded history and WLED. Its audio worker
consumes rational sample hops (`Fs / 60`) independently of rendering. The capture
queue holds 16 preallocated stereo windows; overflow drops the oldest pending
windows, records the loss and marks a discontinuity. FFT is computed for each
channel before spectrum merging. Graph compilation resolves operations, parameters
and buffer slots; evaluation reuses worker scratch and cached Mel filters.

`EngineClient` provides asynchronous commands, acknowledgements and immutable
snapshots to `GuiPresenter`. Widgets and QtNodes use GUI state and the presenter;
they do not obtain capture/analyzer/WLED pointers. The local client embeds the same
engine as `pipeline_cli`; the TCP client implements the same command interface.
Editor drafts and view/camera preferences are separate from the active engine graph.
Invalid drafts do not replace it. Unknown configuration fields survive saving.
At startup, an incompatible saved schema is retained while a temporary default
graph runs. Bootstrap does not autosave that fallback; preference changes preserve
the original schema fields. Recovery requires an explicit graph apply.
Settings changes update editor nodes only after acceptance, and only if those
nodes have not been edited while the command was pending.
The editor preserves the saved local draft on remote connection. `Load active
pipeline` explicitly loads the host graph into the editor without applying or
saving it; an outstanding apply command must finish first.

`IAudioSource` is the runtime capture port: it delivers owned stereo frames and
lifecycle callbacks. The default factory creates the Windows WASAPI adapter;
tests inject a fake source. Stopping a source joins its producers before returning.
DSP classes do not include Windows capture headers.

Build only the portable kernel, without Qt or downloaded desktop dependencies:

```sh
cmake -S . -B build-dsp -DBUILD_DSP_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-dsp
ctest --test-dir build-dsp --output-on-failure
```

For a smaller kernel table budget, set `-DPORTABLE_DSP_MAX_FFT_SIZE=1024`.
Supported limits are 512, 1024, 2048 and 4096 (default). Radix-4 tables use the
largest power of four within that limit. The desktop Analyzer retains its
4096-sample windows and 16 queue slots; this option is intended for kernel-only
builds, not as a complete microcontroller memory profile.

FFT scratch belongs to each plan, but vendor ESP-DSP tables are process-wide.
`prepare` checks their actual capacity and rejects an undersized externally
initialized table without replacing it. External vendor initialization and
deinitialization must be synchronized; never deinitialize tables used by a live
plan. `estimate_memory` reflects the chosen profile's heap payload, excluding
allocator overhead and constant tables in the executable.

The kernel returns complex FFT magnitude divided by N. Legacy-route tests include
the original complex-to-real steps: old FFT2R doubled non-DC magnitudes and old
FFT4R inflated DC by sqrt(2). These normalization corrections are intentional;
the frequency-weighted FFT4R output remains equivalent because its DC weight is
zero. Independent DFT and known-tone checks guard the mathematical result.

Build the Windows console runtime without Qt GUI modules using `-DBUILD_GUI=OFF`.
Disable the compatibility executable using `-DBUILD_LEGACY_APP=OFF`.

Run an independent engine and attach the GUI (two terminals):

```powershell
./cmake-build-release/bin/pipeline_cli.exe --server --port 8213 --synthetic
./cmake-build-release/bin/core_app.exe --remote-host 127.0.0.1 --remote-port 8213
```

Omit `--synthetic` for WASAPI capture; use `--config <file>` for saved settings.
The server listens on localhost by default and permits local control. An explicitly
selected external address is read-only unless `--allow-mutations` is supplied.
Closing a remote GUI leaves the engine running. Closing the local GUI stops its
owned engine. A remote connection never reapplies the client's local startup graph.

TCP uses existing BDSP COBS/ZPE framing on a separate endpoint from the aggregator.
The engine codec uses QDataStream Qt_6_0, little-endian fields and float32 arrays.
Messages start with version 1. IDs 5..10 carry commands, replies, snapshots, views,
audio and fragments. Command IDs match the published EngineCommand enum; replies
include request ID, acceptance/error, revision, epoch and component generations.
Graph edits can require an expected revision. Disconnects fail pending commands;
mutations are never automatically replayed. Subscriptions select views, raw audio,
history, node previews and diagnostic samples/window data.

Assemblies are capped at 2 MiB, BDSP chunks at 60000 bytes, clients at eight and
pending client commands at 128. Decoders validate lengths before allocation.
Slow consumers retain the latest queued view; socket queues are bounded and a
client that cannot keep up is disconnected. Reconnect delays grow from 200 ms to
5 seconds. Command timeout is 5 seconds. History snapshots are prepared at most
60 Hz and requested only when needed; unchanged history is omitted from repeated
TCP snapshots. Rendering remains independent, capped at 60 Hz.
UART/WebSocket can implement another client adapter without moving DSP
into graphical node models.

The aggregator remains a component registry, not an audio router. Its existing
registration/state/snapshot byte layout is preserved by explicit codecs and golden
packet tests. QObject timers and sockets stay on their owning event loop. Delivery
mailboxes are cancelled before their receiver is destroyed.

Portable numerical/allocation/cadence tests and TCP malformed-packet, fragmentation,
revision, subscription and lifecycle tests run through CTest. `render_tests` and
native `ui_tests` require Windows graphics; `offscreen` checks do not validate 3D.
GitHub CI includes a Qt-free Linux DSP build with ASan/UBSan; physical WLED and
actual audio-device removal still require hardware checks.
