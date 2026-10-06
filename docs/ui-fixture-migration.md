# GUI fixture migration (author evidence)

Date: 2026-10-06. Scope: `tests/ui_tests.cpp`,
`tests/workspace_tests.cpp`, `tests/gui_views_tests.cpp`, plus this report.
The coordinator subsequently authorized the WLED block and required include in
`tests/runtime_tests.cpp`; that narrow extension is recorded below.
This is an author slice, not primary or independent acceptance. No production,
CMake, unrelated fixtures, user config/output, physical capture, native graphics,
new chat/subagent, branch or commit was changed or launched by this slice.

Repository rules and the full pinned architecture reference were read in the
preceding fixture audit. Tests retain synthetic source injection.

## Concrete migration

| Fixture | Old assumption | Supported behavior now checked |
| --- | --- | --- |
| `ui_tests.cpp` | Removed pipelineAnalyzer/pipelineCapture checkboxes; null dereference in QAbstractButton::toggle | Presenter Start/Stop commands and actual Analyzer/Capture readiness, five lifecycle rounds, absence of removed controls; graph and saved legacy preferences preserved. |
| `ui_tests.cpp` | Three top menus headed Graphs | Four menus File, Display, WLED, Settings, live RU/EN translation, unchanged renderer instance and processing revision. |
| `ui_tests.cpp` | No title-label child; floating dock uses no custom title strip | Shared draggable strip and Placement persist through detach/reattach; title label hidden when docked, visible and localized when floating. |
| `ui_tests.cpp` | Preset actions owned by editor dock | MainWindow lookup after presets moved into status bar; Load Active and Default still validate active/draft persistence and source changes. |
| `ui_tests.cpp` | Apply always visible; successful Apply displays status text | Explicit manual mode exposes Apply. QSignalSpy requires a positive graph_apply_finished ACK with nonzero request ID, no pending command and actual working graph semantics matching the requested document. Successful Apply leaves no warning/success text, as explicitly requested by the user. Invalid draft still requires the localized accessibility error and unchanged working graph. |
| `workspace_tests.cpp` | FFT toggle belongs to removed graph list in Display | Display excludes graph list; graph Placement/Return to tabs opens the existing renderer, Close affects only selected dock. Other workspace/layout, undo/preset, save, child restart and blocked-GUI DSP/history/network assertions remain. |
| `workspace_tests.cpp` | Generator example preset action owned by editor | Window lookup and explicit non-null check after status-bar reparenting; original position/routes/eight-tone/undo assertions remain. |
| `gui_views_tests.cpp` | Missing/unavailable node frame clears vector size | Geometry dimensions remain, values become zero only in affected exposed view. Independent counts and peaks, dimensions, explicit disconnect and Stop clears remain checked. |
| `gui_views_tests.cpp` | Live local auto ticks can coexist with exact manually emitted maps | Local manual-feed factory isolates exact per-node values; remote factory delivers real synthetic frames over engine transport. |
| `gui_views_tests.cpp` | Generic remote timeout without stage | Named waits report server/client states, epoch, processed frames, and explicit Stop chart counts/peaks. |

The WLED settings fixture also replaces only the final start/restart composition
connections from MainWindow to the Presenter with a localhost EngineCommand
boundary. SettingsWidget mutations, MainWindow signal forwarding, persistence,
the real EngineClient dispatcher, real WLED worker and original Stop connection
remain. The test requires those production connections to exist before replacing
them. It retains the initial valid saved port=80 display check, then uses two
bound ephemeral localhost receivers and verifies actual packets at both initial
and restarted destinations. Changed-port persistence and disabled-port behavior
remain checked. This fixture boundary is necessary because production's network
selector intentionally excludes loopback and its WLED default is LAN broadcast;
there is no persisted destination override. No production networking was changed.
The constructor fixture explicitly stores `wled_ar_server.enabled=false`, so
Presenter initialization cannot bypass the test's later signal boundary to
start a LAN worker. All subsequent `manager.load(saved)` calls are read-only
ConfigManager parses into a separate DTO; this fixture never invokes
CoreApplication.open_configuration, Presenter.initialize or configure_network
again. WLED is stopped/disabled before those persistence checks and later graph
Apply/default recovery. This audit covers the actual configuration-read paths,
not just the checkbox signal route.

The GUI views regression still directly calls `extra->setFloating(true)` and
requires both renderers to become exposed. It does not route this case exclusively
through Placement to evade a valid public QDock/user detach bug. Existing
navigation-only mode also checks inactive-tab suppression and supported placement.

External server `stop_capture(); stop_analyzer();` must clear both remote chart
counts and report GUI Analyzer Stopped. Missing/unavailable zero-geometry behavior
is **not** substituted for this explicit Stop assertion. The remote branch keeps
its source identity, resume and cancellation expectations.

## Production defects reported to coordinator

1. Detaching an inactive dynamic dock directly can leave renderExposed false even
   though the floating dock is visible. Coordinator owns the deferred
   topLevelChanged exposure repair; author does not edit it. Initial per-node
   check subsequently passed against the coordinator's exposure-repaired archive.
2. External remote Analyzer Stop updates server/client status but previously did
   not emit `views_cleared`; per-node unavailable frames zeroed existing geometry
   instead. A deterministic author run reached only the last named wait
   `Remote explicit Stop clear timeout`, with server Capture/Analyzer Stopped,
   processed=17 and nonzero chart counts. Coordinator owns presenter Stop-state
   clear and any required stale-delivery fence. This regression remains intact.

## Verification / snapshot discipline

Private Ninja manifest: `build-fixture-audit/ui-migration.ninja`. It compiles only
the three owned fixture sources. Compile flags/includes/link dependencies were
read from actual `build-gui/build.ninja`; no Ninja was launched in the shared
build directory. Link inputs were copied into a private canonical archive/RCC
snapshot after coordinator explicitly announced completed/frozen libraries.

The initial short workspace run passed its full existing fixture, including
child restoration. GDB of its prior failure proved a null `useTestSource` QAction
at the old editor lookup; this was corrected without deleting the preset checks.

Private tests used these environment settings and commands:

```powershell
$env:PATH = 'C:/Qt/6.10.2/mingw_64/bin;C:/Qt/Tools/mingw1310_64/bin;' + $env:PATH
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_PLUGIN_PATH = 'C:/Qt/6.10.2/mingw_64/plugins'
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:QT_LOGGING_RULES = '*.debug=false'
& C:/Qt/Tools/Ninja/ninja.exe -C build-fixture-audit -f ui-migration.ninja -j2
& ./build-fixture-audit/ui_tests.exe
& ./build-fixture-audit/workspace_tests.exe
& ./build-fixture-audit/gui_views_tests.exe
```

Production facade ABI then changed while header sources remained shared. A GUI
fixture link exposed `feed_typed(…, TypedFrame const&, …)` versus the old archive
symbol. Following coordinator direction, private builds/runs were stopped until
a new synchronized archive **and public-header** snapshot is available. Any
post-signature runs mixing current headers with old archives are rejected as
acceptance evidence. A temporary probe that invoked GUI Stop before the final
external-Stop assertion was removed; its green result is not final evidence.

Final sources are frozen for coordinator's integration. `git diff --check` for
the three owned fixtures passes. Full suite, native/device tests and independent
acceptance are not claimed.

## Final results

Coordinator announced the completed consistent rebuild (session 43140, exit0),
including floating exposure, external Stop clear and finalized AudioPipeline ABI.
The author copied current archives/RCC objects and every existing project include
root in one announced stable window to
`build-fixture-audit/canonical-final-snapshot`, then released the producer.
The private Ninja manifest uses only snapshot canonical headers and archives;
Qt SDK headers/libraries remain from the same installed SDK. AudioPipeline and
GuiPresenter public-header hashes matched the producer at copy time. The 17
project include entries include a normalized duplicate alias and one already
nonexistent optional BDSP-generated include path; no actual header was omitted.

* `workspace_persistence`: PASS against coordinator's consistent rebuilt binary;
  confirmed in `build-gui/Testing/Temporary/LastTest.log`. The earlier author run
  also passed the whole existing fixture, including child restart.
* `gui_independent_views`: PASS against coordinator's consistent rebuilt binary;
  confirmed in the same log. Direct floating and external Stop count-clear
  regressions remain in the final source. No duplicate author rerun was needed.
* `settings_interactions`: final private build and run PASS, exit0, after the
  positive-ACK/manual-mode and localhost WLED fixture changes. Command:
  `ninja -C build-fixture-audit -f ui-migration.ninja ui_tests.exe`, then
  `build-fixture-audit/ui_tests.exe` with the offscreen environment above.
  Fixture compile flags are `-O3 -DNDEBUG -std=gnu++23 -UNDEBUG`.
  Evidence: `build-fixture-audit/ui-tests-final.log`, which ends with
  `Settings and pipeline editor interaction tests passed` and records only
  `127.0.0.1` WLED binds/destinations (62552, then62553 in this run).
  Existing responsiveness checks reported 54ms worst event-loop gap and
  62 heartbeats. Subsequent edits only clarified an assertion failure message
  and explicitly seeded the already-false WLED startup setting; lifecycle/check
  logic did not change after the passing run.

Coordinator must rebuild `ui_tests` from the final source before the final short
suite; the earlier shared settings binary failed its obsolete success-text
assertion. New primary review is separate and must inspect actual changes,
especially the test-only WLED boundary and retained external Stop regression.
No physical, native, full-suite or independent acceptance is inferred from these
synthetic/offscreen author checks. Other fixture files remain outside this slice.

## Authorized runtime WLED extension

Only the WLED lifecycle block and the QUdpSocket include were changed in
`tests/runtime_tests.cpp`. A loopback EngineServer controls the existing
application pipeline through a second EngineClient, allowing positive request
ACK assertions without replacing the Presenter-owned reply callback. Both WLED
interface and destination are explicitly localhost; two UDP receivers reserve
ephemeral ports. Five existing lifecycle rounds now require accepted Start,
Running state and packets, then explicit Stop with a Stopped-state wait followed
by Start at the replacement receiver. Each of the five actual Stop/Start restart
cycles awaits an increasing worker revision and replacement packets, cessation
at the previous receiver, and
silence after the existing double Stop calls. Disabled convenience Restart must
remain stopped and silent. Config, DSP, clear-marker and registration scenarios
were retained.

The coordinator corrected the original second-Start assumption: production
Start is intentionally idempotent while WLED is already running; the existing
convenience Restart performs Stop then Start. The final fixture preserves that
behavior without production changes. ACK confirms command acceptance before
asynchronous worker transitions finish, so revision growth uses a bounded wait.
The coordinator also enabled explicit Presenter view subscription in the
clear-marker scenario before direct callbacks; hidden GUI correctly has no view
delivery.

The coordinator reported the rebuilt current `runtime_regressions` binary PASS
on 2026-10-06: 3.80 s executable time, 3.82 s CTest time. This run includes the
original DSP, stall and Stop assertions and all five localhost Stop/Start restart
cycles with ACK, packet, revision and cancellation checks. The final runtime
source is frozen after the explicit view subscription, bounded revision wait
and Stop-before-Start corrections. This is the coordinator's integration result,
distinct from the author's earlier snapshot run below. Independent Qt source and
probe review determines acceptance; no physical or native-rendering claim follows.

The author's pre-coordinator runtime fixture compiled successfully against the same matched
header/archive snapshot using the private Ninja manifest and `-UNDEBUG`.
The author run with offscreen and forced stderr logging exited 1 before reaching
the WLED block: `Stopped analyzer did not clear GUI spectra`
(`tests/runtime_tests.cpp:268`). Evidence is
`build-fixture-audit/runtime-tests-localhost-final.log`. No WLED execution pass
is claimed from that run, and the unrelated clear-marker assertion was not
weakened. The coordinator was notified to rebuild and run the final source with
the latest production changes. The earlier private recipe accidentally selected
the similarly named dataflow_runtime compile block; that recipe was corrected
before the successful compile, and no stale executable result was accepted.
