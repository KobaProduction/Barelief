# Qt Visualization

Windows/Linux application for real-time system audio capture, FFT visualization and WLED audio sync.
Requires CMake 3.30+, a C++23 compiler and Qt 6.7+ with Widgets, Charts, DataVisualization, SerialPort and Network.
Use the compiler matching your Qt installation (for example the Qt MinGW toolchain).
BDSP, QtNodes and miniaudio 0.11.25 are fetched through CMake FetchContent at fixed revisions.

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
both channels. Capture follows the default device and retries after 1, 2 and then 5 seconds if unavailable.
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
responsiveness and aggregator failure/reconnect scenarios. The manual capture soak runs for at least 30 minutes. For native graphics validation, run `bin/ui_tests.exe` directly with
`QT_QPA_PLATFORM` unset; CTest uses offscreen for deterministic GUI tests.
CI builds Windows Debug/Release and Linux GUI/headless configurations, tests
PulseAudio virtual capture, and verifies Windows Release deployment.


Performance tracing is opt-in and bounded. Native load tests use the silent,
configurable `TestSignal` node by default; no playback helper is required:

```powershell
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:QT_VIS_TRACE_FILE = "$PWD/cmake-build-release/signal.trace.json"
Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
./cmake-build-release/bin/render_tests.exe
Remove-Item Env:QT_VIS_TRACE_FILE
```

This measurement uses the real Qt event loop, changing audio,
visible charts and the pipeline editor, plus a local UDP WLED receiver. It checks
analysis cadence, GUI delivery and heartbeat delays, and exports per-stage
counts, mean, p50/p95/p99 histogram bounds and maximum duration. Percentiles
that reach the overflow bin use the observed maximum as their upper bound;
the nominal histogram edge never hides longer waits. The trace opens in
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
60/s; camera movement and pending spectrogram geometry are applied together before
rendering. The dynamic spectrogram uses shared vertices with smooth shading and no
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
lifecycle callbacks. The default factory creates the miniaudio capture adapter;
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

Omit `--synthetic` for miniaudio capture; use `--config <file>` for saved settings.
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
native `ui_tests` require a graphical session; `offscreen` checks do not validate 3D.
GitHub CI includes a Qt-free Linux DSP build with ASan/UBSan; physical WLED and
actual audio-device removal still require hardware checks.

## Capture and workspace on Windows/Linux

Miniaudio 0.11.25 is fetched by CMake FetchContent; its upstream CMake targets are
not added. The local capture target uses its device API only; engine, graph, resource manager and codecs are
excluded. Windows uses WASAPI capture/loopback. Linux requires `libpulse-dev` at
build time and a running PulseAudio server (or PipeWire with pipewire-pulse).
Loopback selects the default sink's monitor via PulseAudio introspection; missing
monitors fail explicitly instead of switching to the microphone. The desktop DSP
library does not depend on miniaudio, PulseAudio or Qt.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.2/gcc_64
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/bin/pipeline_cli --capture-diagnostics --duration 10000
./build/bin/native_capture_checks 1800000 tone
```

`native_capture_checks` uses a very quiet antiphase test tone when `tone` is
specified; omit it for silence/real music, or use `microphone`; `0 lifecycle` checks cancellation while opening/retrying. Diagnostics report
backend, device, rate, queued/lost frames, discontinuities and dispatch latency.
The callback copies stereo float PCM into a preallocated 250 ms SPSC queue.
Overflow drops a whole incoming block and marks the next delivered frame. An
independent consumer delivers owned frames; a silent loopback continues with
zero PCM on the monotonic audio clock. Stop joins capture/consumer threads.
On Windows the worker pins the current default endpoint. An event-only COM
adapter flags default changes/removal; reopening happens on the capture owner
thread, with miniaudio automatic rerouting disabled. Notifications never close
devices or perform DSP. Physical endpoint unplug remains unverified.

Spectrogram, Data Flow and all graphs are dock panels. Drag titles to split, tabify or
float panels; reopen them through View or the toolbar. View > Reset layout restores
the initial workspace. Closing a panel only disables its display subscription.
Window/dock layout lives in `ui.workspace` version 1; editor zoom/center lives in
`ui.editor_view` version 1. Camera settings remain separate and survive docking.
Arrange uses measured node sizes and one undo command. Dragging and viewport
changes save independently of processing; only connections/parameters apply DSP.
Autosave debounces by 350 ms and flushes on close; Ctrl+S flushes manual saves.
Saving a modified editor draft explicitly clears `layout_pending`; otherwise
recursive preservation of the original JSON could restore the initial-layout
marker and rearrange saved positions on restart. The workspace regression starts
with that marker, drags/undoes nodes, saves with autosave disabled, closes before
the debounce fires, and verifies positions in a new process. It also checks that
unknown nested workspace/editor settings survive the save. This regression passed
on Windows and Linux.

For a 30-minute silent native GUI soak,
set `QT_VIS_CAPTURE_SOAK_MS=1800000`, then run `bin/render_tests` (`.exe` on
Windows). This mode uses the configured `TestSignal` source factory,
rotates Spectrogram continuously and reports resident memory, queue occupancy,
losses, discontinuities and dispatch latency. Clear the variable afterwards.
Set `QT_VIS_REQUIRE_SIGNAL=1` to require nonzero FFT data in at least 95% of
one-second report intervals. This verifies signal delivery through the configured
source, FFT, history and GUI. `QT_VIS_DEVICE_CAPTURE=1` selects real system-output
capture instead; that separate check needs an external signal and a virtual
render endpoint if the test must remain inaudible. The in-memory source does not
validate WASAPI or PulseAudio device I/O. Do not automatically run a playback
helper on physical speakers. The manual `native_music_player` helper is legacy
device-QA tooling, not part of normal silent tests.
The QA fixture is [Vibe Ace by Kevin MacLeod](https://freemusicarchive.org/music/Kevin_MacLeod/Jazz_Sampler/Vibe_Ace),
licensed [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), retrieved from
[librosa's audio examples](https://librosa.org/data/audio/Kevin_MacLeod_-_Vibe_Ace.hq.ogg)
and converted to 48 kHz stereo PCM16. The recording is not included in this repository.
`QT_VIS_RENDER_SILENCE=1` instead feeds zero PCM without opening an audio device,
which isolates native rendering of silence from capture. Clear it after the check.
On Windows, `QT_VIS_SWAP_PROBE=1` enables a test-only timing probe at the Qt
Windows plugin's `SwapBuffers` import. It records only the GUI thread, restores
the import before Qt teardown and reports timings after the measured interval.
It is disabled by default and does not alter production executables.
On Windows the native test also records the input desktop, connection state and
[session notifications](https://learn.microsoft.com/en-us/windows/win32/termserv/wm-wtssession-change).
If the desktop is locked/disconnected, or its state cannot be verified, the test
keeps measured telemetry but exits with code 2 for an invalid interactive graphics
measurement. It never locks/unlocks the workstation. This distinguishes desktop
availability from application latency without changing the performance limits.
`QT_VIS_QA_TOPMOST=1` keeps only the test window above other windows for the
duration of the native run. Session telemetry also reports foreground samples,
AC/battery status and explicit process power-throttling flags. Windows can
[ignore higher timer resolution for fully occluded windows](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod),
so visibility is relevant measurement context. These probes do not change power
policy or production-window flags. Optional `QT_VIS_SCREENSHOT_FILE` captures the
test widget layout plus a separate `.spectrogram.png` from the same Spectrogram renderer,
after all timing measurements; parent-window capture may omit native children.

Native QA on Windows covered both physical screens (DPI 100%/200%), floating,
closing/reopening and tabifying all panels without replacing Spectrogram/camera.
The added silence check exposed intermittent native render waits of 150–541 ms
on the first screen, also with injected zeros. Updating the QWidget host improved
steady rendering, but the strict silence/resize latency gate is still open.
A repeat without concurrent builds still exceeded the 100 ms maximum (541 ms);
DSP stayed at 59.9 frames/s. The native five-second GUI stall check also verified
that history and local UDP output continue while GUI events are not processed.
Expose events use the same 60 Hz scheduler as camera/data changes; native resize
stress checks all these frames rather than excluding Qt's synchronous expose path.
An injected-silence resize trace also found two approximately 200 ms DSP waits
between history and publication despite sub-millisecond FFT. Dedicated
`pipeline.callback_snapshot` and `wled.publish` trace stages localize that interval;
average throughput alone does not prove the absence of these stalls.
The traced WLED publication stall reached 195 ms. WLED now uses a one-frame
mailbox polled on its network thread, and does not publish while disabled.
With WLED active during native resize, publication max fell to 0.002 ms and
network queue max was 21 ms. The same trace still showed a 121 ms stall in
GUI spectrum publication. Audio/view delivery now polls a bounded latest-frame
mailbox on its consumer thread; the history owner polls a snapshot-request flag.
Neither path posts Qt events from DSP. Polling timers disappear when a delivery
is replaced or cancelled, and the history timer stops with the analyzer.
With these changes, a native silence/resize run with UDP active measured spectrum
publication max 0.285 ms, DSP work max 2.598 ms and WLED queue max 16.310 ms.
Analysis averaged 59.8–60.1 frames/s; observed processing gaps reached 36.721 ms.
Native Spectrogram render still waited up to 172 ms, exceeding the strict UI gate;
these figures do not claim a completed 30-minute music soak.
An optional native swap probe attributed a 2224 ms render wait to `SwapBuffers`
(2224.450 ms), while DSP work max remained 3.851 ms. Disabling swap interval
did not eliminate the waits; releasing the context every frame shifted the
wait and increased ordinary frame cost, so both experiments were reverted.
Windows display configuration identifies TL160MDMP03-0 on Intel and MKQ34C170L
on NVIDIA; the OpenGL renderer is NVIDIA. Cross-adapter presentation is a
candidate cause, not established by these timings alone. System ETW profiling
was unavailable: WPR rejected profile activation with policy error 0xc5585011
and confirmed no recording active afterwards.
The 30-minute capture soak used a quiet test tone, not a 30-minute music session;
a single 70 ms queue overflow under concurrent builds was counted and marked.
A subsequent 30-minute native Windows soak used the real Vibe Ace recording,
with gain 0.01 and no concurrent builds. It passed the multi-screen/workspace
checks: DSP averaged 60.00 frames/s, no PCM frames were lost, history stayed
bounded at 400 frames, and all 1798 one-second report intervals had nonzero FFT
data. Peak resident-memory growth was 9.77 MiB. Spectrogram render p95 was at most
3.00 ms (max 36.999 ms); GUI heartbeat p95 was at most 18.25 ms (max 85.433 ms).
DSP work max was 6.833 ms, processing gaps max 48.247 ms, and WLED queue max
19.704 ms. These measurements validate this music run; they do not establish
that the intermittent driver waits seen in silence were eliminated.
The final injected-silence repeat still failed the native UI latency gate:
Spectrogram render max was 154.719 ms and GUI heartbeat max 319.795 ms. DSP work
max remained 1.974 ms, spectrum publication max 0.487 ms and WLED queue max
16.498 ms. The capture/editor/workspace implementation is validated, but the
intermittent native presentation delay remains an open performance issue.
Additional diagnostics separate native rendering, Qt event dispatch and direct
window geometry changes. In a silence repeat, same-screen resize max was
9.929 ms, while moving between screens took up to 334 ms in `setGeometry`.
Long GUI timer events matched Spectrogram rendering (up to 147 ms); no nested
Spectrogram render was found in the longest traced frame. Software OpenGL removed
the long Spectrogram wait, but increased ordinary render cost and did not pass the
resize test. Rendering through `renderToImage` reduced large-window throughput
to 27–39 frames/s; removing the alpha buffer did not fix the native waits.
Neither rendering alternative nor the alpha-buffer experiment is enabled in
production. `gui.event` telemetry keeps at most 16 slow-event identities and
only profiles the GUI thread when measurement is enabled.
Temporary NVIDIA profiles applied only to the native test executable did not
remove the waits: disabling driver threading reached 177 ms Spectrogram time;
requesting maximum performance reached 1639 ms. Both test profiles were removed
and their absence verified; no application or global driver setting was retained.
An external sampler subsequently captured GUI-thread waits in the NVIDIA driver
under `SwapBuffers`, and during Qt texture creation. Some stacks could not be
unwound reliably; exported symbol names without private symbols are approximate.
The sampled repeat passed, but sampling perturbs timing and does not establish
a fix for the intermittent waits. The native silence latency issue stays open.
A following unsampled repeat also reported heap corruption during application
teardown. Spectrogram now detaches its callback capturing the owner and destroys the
native-window container before the owner's member state is destroyed. The manual
`bin/native_spectrogram_lifecycle` check covers 24 create/show/update/resize/destroy
cycles, alternating docked and floating views across available screens. It passed
on Windows and native WSLg; this removes an ownership hazard, but successful repeats do not
prove the original intermittent corruption is fully resolved. Qt also reported
stale cached DPI values during some transitions between the two Windows screens.
Two debugger launches subsequently reproduced heap corruption in the MinGW
thread-local destructor callback for `MelFilterBank`. Its argument contained freed
memory markers before the destructor freed the filter's vector. DSP scratch and
the cached Mel filter now belong to the pipeline's `GraphEvaluationContext`, not
owning thread-local objects. The immutable graph can also use a caller-owned
context; its convenience evaluation overload owns temporary scratch locally.
Graph tests reuse the context across 16 joined workers and verify filter refresh
after FFT-size/rate changes. The first native debugger repeat after this change
exited normally with the latency-test failure code, without a heap exception.
A fresh native 30-minute Windows music soak after the context-ownership fix
finished with normal exit code 0. DSP averaged 60.00 frames/s, no PCM frames were
lost, history stayed at 400 frames, and all 1798 signal intervals contained FFT
data. Peak resident-memory growth was 11.63 MiB. Spectrogram render p95/p99 were at
most 3.00/3.25 ms (max 32.250 ms); DSP work p95/p99 were at most 0.50/0.75 ms
(max 6.843 ms). WLED queue p95/p99 were at most 15/16 ms (max 20.730 ms).
GUI heartbeat p95/p99 were at most 19.25/21 ms, with one 106.775 ms maximum;
GUI event dispatch max was 32.573 ms. The initial multi-screen, resize, dock and
five-second GUI-block checks passed. This confirms this run and clean teardown;
it does not establish elimination of the previously observed silence-only native
presentation waits or every isolated GUI scheduling delay.
After the user reported that the laptop had locked during unattended checks, a
repeat in the current active session passed on both screens. Spectrogram render max
was 12.172 ms, GUI heartbeat max 67.864 ms, same-screen resize max 10.989 ms, and
DSP work max 1.526 ms. Earlier logs did not record session state, so this does not
prove that every previous outlier was caused by locking.
A subsequent session-verified repeat had an active `Default` desktop, 34 session
samples and no session changes. Its native latency limits passed (Spectrogram max
35.486 ms, GUI heartbeat max 68.623 ms), but the built-in-screen resize throughput
gate failed: 92 requests in two seconds, below the required >100. Deferring native
container resize delivery to one update per frame did not improve this (91
requests, higher mean resize cost), so that experiment was reverted. Thus desktop
lock is useful context, not a demonstrated explanation for all remaining failures.
Marking the Windows host widget opaque also failed to improve resize throughput
(76 requests), and was reverted. A later unmodified production-renderer repeat
passed with 109/127 resize requests and active desktop telemetry; its foreground
sample count was zero. A further topmost-window run verified 34/34 foreground
samples, AC power, no explicit throttling and no session changes. Spectrogram max
was 11.886 ms and GUI heartbeat max 51.256 ms, but the built-in screen still
completed only 94 resize requests (external screen 126). Therefore resize
throughput remains an open issue even when the test is visible and focused.
Linux discovery now reads the chosen source's sample specification and requests
that rate explicitly. It also reopens when the rate changes without a device-ID
change. Periodic discovery runs on a separate cancellable owner; a slow PulseAudio
introspection response cannot block PCM delivery or silence generation. Its query
checks both source cancellation and its own stop token. Shutdown cancels/joins
discovery before releasing the device. A device-poller regression covers a blocked
query, cancellation during an interval/query, default changes and server errors.
After this change, Windows/Linux GUI suites passed 19 tests each and headless
suites passed 13 each. The Linux monitor-switch/removal, mono and microphone
script passed again with no PCM losses; stop took 1 ms for monitors and 17 ms
for the microphone. The original `RDPSink` was restored and QA sinks removed.
The PulseAudio script verifies a 44.1 kHz one-channel monitor with in-phase
playback at the same rate: nonzero captured PCM is duplicated identically into
left/right channels. It also verifies switching to a 48 kHz monitor, removing it,
returning to the original monitor, and microphone capture. The virtual sinks and
default output are restored after success or failure.
Linux capture/endpoint removal was verified with WSLg PulseAudio at 44.1/48 kHz.
Native WSLg checks used a desktop OpenGL 4.5 context. Rendering leaves a short
event-loop gap after expensive frames; resetting the workspace restores Qt's
saved default dock state. One multi-screen check passed with 49–56 native
frames/s and p95 GUI response 20–27 ms; a subsequent stress run reached 34 ms
on the large screen and failed the 30 ms limit. Physical Linux GPU performance
remains unverified; these figures describe this WSLg session.

`QT_VIS_GUI_EVENT_PROFILE=1` enables bounded, GUI-thread-only event grouping in
the native render check. The report includes inclusive and exclusive durations
by receiver class and event type, with per-screen resize sections. Exclusive
time excludes nested `notify` calls; neither value is an OS CPU-time counter.
The grouping allocates no per-event storage and never runs on DSP threads.
The resize-only baseline measured 106/129 chart geometry events on the two
screens. Chart viewport geometry now updates immediately while expensive axis
layout and resolution-dependent series rebuilding share the existing 17 ms
display timer. Hidden charts retain the last resize until shown. Plot-area
changes also invalidate decimation so its final resolution converges without
new audio. UI regressions check resize convergence and hide/resize/reopen.
An initial native trial reduced chart geometry events to 46/55 and passed with
105/146 resize requests; DSP stayed at 59.78–60.10 frames/s. Its session was
active throughout, but foreground samples were zero, so this trial alone does
not establish foreground high-DPI resize performance.

The default workspace now contains Spectrogram above Data Flow; FFT/Mel start hidden.
Graph toolbar clicks replace the attached top graph, while Ctrl-click adds a tab.
Detached panels remain independently usable. The Data Flow toolbar action toggles
the bottom panel. Reset layout restores Spectrogram/Data Flow and hides other graphs.
`workspace.layout_version=2` identifies this default; older layouts migrate while
window geometry and unrelated settings remain preserved. Subsequent custom dock
layouts restore normally, with stable view instances and camera state.

Native load checks now default to eight stereo sinusoids with independently
varying amplitudes and continuous-phase frequency sweeps (73–13901 Hz carriers,
±18% frequency deviation). The owned playback helper supports
`native_music_player --tones 100` for real loopback tests without music: it
precomputes a phase-continuous six-second loop before starting the device.
The enlarged resize test checks actual width/height excursions instead of
alternating by eight pixels. A real WASAPI swept-tone trial traversed
800×600–1512×859 and 800×600–3416×1337, but completed only 64/62 resize requests
in two seconds and failed the existing >100 gate. Its 45-second capture interval
ran at 60.01 DSP frames/s without PCM loss; this is a short diagnostic, not a
replacement for the required long run. Strong-resize optimization remains open.

Normal test signals now originate inside `IAudioSource`, not a playback device.
Data Flow's **Test source** button replaces the input with a configurable
`TestSignal` block, preserves node position and connections, and supports undo.
Configure 5–10 tones, sample rate, carrier range, amplitude, sweep period and
frequency deviation in that block. Changing source parameters restarts the
producer with a new generation. Stop joins it before returning. No miniaudio
device/context is opened and no PCM is sent to speakers.
`tests/fixtures/test-signal.json` is a reproducible silent GUI/console graph;
WLED sending and autosave are disabled in that fixture. Run either application
with `--config tests/fixtures/test-signal.json`. If no editor draft is saved, the
GUI initializes its editor from the configured processing graph rather than an
unrelated default source. `pipeline_cli --synthetic` uses the same source and
preserves a configured generator's parameters. The saved-graph console and GUI
smoke tests check actual signal delivery, rather than only compiling a graph.

Use test profiles instead of running every suite for every edit:

| Profile | Scope |
| --- | --- |
| `quick` | Fast portable DSP, bounded telemetry, buffers and discovery logic |
| `dsp` | Portable computation tests only |
| `pipeline` | Source lifecycle, graph processing, cadence and transport |
| `ui` | UI interactions, workspace, presenter and startup |
| `capture` | Capture buffers/discovery and real device lifecycle |
| `network` | Aggregator and engine transport |
| `integration` | Cross-component correctness checks |
| `performance` | Opt-in native FPS/latency benchmark, separate from correctness |
| `full` | All correctness groups for CI/pre-commit, excluding native performance |

On Windows, `tests/run-tests.ps1 -Profile ui -BuildDirectory cmake-build-release`
selects the UI group; add `-List` to inspect selection without executing it.
Pass `-CTest <absolute-ctest-path>` when CTest is not on PATH. The equivalent
cross-platform command is `ctest --test-dir <build> -L '^ui$' --output-on-failure`.
Register native benchmarks explicitly with `-DENABLE_NATIVE_QA=ON`; run the
`performance` group in a real graphical session, never `offscreen`. CI runs full
correctness groups; hardware/GPU timing stays separately scheduled/manual.

Silent in-memory integration does **not** validate WASAPI/PulseAudio capture.
WASAPI loopback reads a render endpoint: see Microsoft's
[loopback recording documentation](https://learn.microsoft.com/windows/win32/coreaudio/loopback-recording)
and miniaudio's [loopback example](https://miniaud.io/docs/examples/simple_loopback.html).
For inaudible real-device QA, route playback into a dedicated virtual audio
endpoint and capture that endpoint, with no connection to physical speakers.
No virtual Windows audio endpoint is currently installed on this machine;
that full capture route is unverified. A miniaudio custom backend can test the
adapter/callback boundary without OS sound, but cannot certify the WASAPI driver.

### Presentation and localization

The main menu contains File, Settings and Graphs. Graph actions toggle the same
persistent dock instances; visible graphs remain switchable in the dock tab bar.
Data Flow stays below the graph by default and can be detached or hidden. Its
status-bar toggle and Ctrl+D reopen it. Capture/analyzer controls belong to
Data Flow rather than a global graph toolbar. Settings menu entries open General,
Controls, WLED and Network tabs directly (Ctrl+, and Ctrl+Alt+2/3/4).

Russian is the default UI language; General offers live Russian/English switching.
Qt Linguist compiles `src/visualization/i18n/ui_ru.ts` into an embedded QM resource;
English is the source language. The presentation-only Language service owns
QTranslator and explicit text bindings. Switching language does not reconstruct
views, change camera state, translate model identifiers or compile a DSP graph.
Only the GUI build requires LinguistTools; portable DSP and headless capture do
not depend on the localization module. Keep new UI strings in the Ui TS context.

GraphPanel owns reusable display controls and visible-view metadata. Auto/manual
Y ranges and splitter preferences are stored under `ui.graph_views`; they do not
change processing parameters. FFT channel and spectral merge controls follow
the existing presenter command/acknowledgement path. Quiet spectra use a small
automatic range floor rather than a fixed magnitude of one. FPS appears in the
status bar: chart paints and native Spectrogram renders are counted against monotonic
elapsed time, independently of audio analysis. Static/hidden views do not report
an assumed 60 FPS.

Data Flow wheel zoom uses proportional deltas, preserves the cursor's scene
point and clamps to 20–300%. The percentage control and Fit graph use the same
scale API. Centering no longer calls QtNodes' implicit fitInView, which bypassed
the scale bounds and left the displayed percentage stale. View changes remain
separate from graph processing revisions.

Design references: [Qt internationalization](https://doc.qt.io/qt-6/i18n-source-translation.html),
[Microsoft command placement](https://learn.microsoft.com/en-us/windows/apps/design/basics/commanding-basics)
and [settings guidance](https://learn.microsoft.com/en-us/windows/apps/design/app-settings/guidelines-for-app-settings).

### Naming and saved-document compatibility

The history visualization is named **Spectrogram** in the GUI, node registry,
C++ views, frame flags, profiling stages and native lifecycle tests.
`SpectrogramGraph` uses Qt's `Q3DSurface` renderer; Qt API names are unchanged.
`Window signal` / `window_signal` names the FFT window before weighting;
`Windowed signal` / `windowed_signal` names the signal after the window function.
The Russian labels are «Оконный сигнал» and «Сигнал после оконной функции».

The loader accepts the former visualization node name and presentation keys.
Version-2 dock layouts restore with their original serialized names before panels
receive the new runtime identifiers; subsequent saves use layout version 3.
Unknown configuration fields and incomplete drafts remain preserved.
Language/theme/FPS and pipeline auto-apply are settings; source/channel/merge
parameters are edited in the pipeline. Named presets save graph parameters,
connections, node coordinates and editor viewport alongside the user settings.
