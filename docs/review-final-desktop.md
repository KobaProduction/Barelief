# Independent final desktop/Qt review

**Latest verdict: READY for the assigned desktop/Qt scope after the narrow
rereview below.** D1 is closed by the owner correction and an independent run of
the actual shared test. The initial frozen-HEAD review was NOT READY; its original
RED and diagnostic evidence are preserved below. This verdict does not infer
architecture, final runtime-body, protocol, native graphics, or hardware
acceptance.

Reviewed on 2026-10-06, Windows, HEAD
`22369719cd00c6e57fafdc166f42a842aa5f161c`, baseline `3de4de8`.
Production and shared tests were read-only. Only this report and private material
under `review-final-desktop/` were written. No author/reviewer reports or chat
histories were read as evidence. Concurrent changes to `.gitignore` and
`docs/refactor-execution.md` were preserved; a subsequent concurrent edit to
`tests/gui_presenter_tests.cpp` was not part of any compiled target or acceptance
claim here. No pending engine-port repair is accepted by this report.

## Finding D1 — P2: settings/theme fixture rejects required editor clipboard shortcuts

Location: `tests/graph_settings_tests.cpp:396-402` (recursive QAction enumeration
and whitelist). Related intended behavior:
`presentation/qt/src/editor/editor_widget.cpp:266-273` restores Copy/Cut/Paste on
the editor with widget scope. `docs/architecture.md` explicitly requires standard
Ctrl+C / Ctrl+X / Ctrl+V in the editor and preservation of text-field input.

Reproducer, from the repository with the private build below:

```powershell
$env:PATH='C:/Qt/Tools/mingw1310_64/bin;C:/Qt/6.10.2/mingw_64/bin;'+$env:PATH
$env:QT_QPA_PLATFORM='offscreen'
$env:QT_PLUGIN_PATH='C:/Qt/6.10.2/mingw_64/plugins'
$env:QT_FORCE_STDERR_LOGGING='1'
./review-final-desktop/build/bin/graph_settings_tests.exe
```

Expected: the test permits required editor-local clipboard shortcuts and rejects
unintended application shortcuts; the settings/theme regression gate runs to
completion. Actual: exit 1 after about one second, with
`Unexpected application Ctrl shortcut remains`. The recursive search includes
three actions owned by `QtNodes::GraphicsView`, with Ctrl+X/C/V and
`Qt::WidgetShortcut` (0). Only File Save/Save As/Open are permitted by the check.
This failure prevents the later theme/contrast checks from executing.

Evidence: `review-final-desktop/settings-failure.log`,
`review-final-desktop/targeted-2.log`, and independent action enumeration in
`review-final-desktop/probe-run.log`. The source diff shows this assertion already
exists at baseline; migration changes to this file are synthetic-source
injections, not an introduced production shortcut regression. It nevertheless
blocks the requested final current-source regression acceptance.

Isolation: a private copy, `review-final-desktop/settings-diagnostic.cpp`, changes
only this condition to permit widget-scoped Copy/Cut/Paste. It preserves all
remaining checks, compiles with `-UNDEBUG`, and exits 0, including its subsequent
Light/Dark/System-mode, menu, QtNodes contrast, settings and unknown-field checks.
This is diagnostic evidence, **not a passing result for the unchanged shared
test**. Closure requires an owner correction preserving the application-shortcut
restriction and editor/text-input checks, followed by an independent rerun of the
real `graph_settings_theme` target.

## Source findings and scope

- Actual GUI composition creates GuiPresenter/EngineClient, which creates
  AudioPipeline. CLI creates the same AudioPipeline. Its preparation uses
  GraphLifecycle prepare/apply and processing calls GraphExecutor::process for
  subscribed and unsubscribed paths (`audio_pipeline.cpp:11-23,757,812`). The Qt
  node models hold parameters, previews and UI state, not an alternative DSP
  executor. This establishes shared execution behavior; it is not a blanket
  clean-architecture boundary approval.
- ConfigManager is byte-identical by Git blob to baseline. Save uses QSaveFile
  and recursive configuration overlay; schema values are preserved unless
  changed. Device, graph and node view identity keys and dock IDs remain stable.
  Legacy dock restoration is still explicit in main_window.cpp:791-820. Runtime
  serial overrides are applied to a copy, not raw_document. Own GUI save probe
  preserved opaque root/UI/workspace JSON.
- Default workspace still uses Spectrogram above Data Flow, with available
  graph views grouped as tabs. Public dock floating uses the same renderer and
  exposure refresh. Single click switches an existing reserved area; explicit
  context commands create one. Hidden panels gate subscriptions/render delivery
  rather than runtime execution. Targeted graph navigation and independent views
  exercise focus retention, last interacted slot, empty-area behavior, side-by-
  side placement, public QDockWidget::setFloating, remote unavailable versus
  remote explicit Stop, and dynamic floating restoration.
- Inspector selection no longer forces expansion. Divider click restores the
  previous actual width, including a narrow 165 px case. Wrapping parameter
  controls and title sizes are bounded by the inspector viewport. Offscreen
  selection/narrow/footer checks passed, including Light and Dark narrow forms,
  camera stability, single Data Flow without empty dock space, and footer bounds.
- ui::Theme, localization service, settings UI and both spectrogram renderer
  implementation files have identical baseline/current Git blobs. Own probe
  switches RU/EN and Light/Dark on a selected real inspector; parameters,
  splitter widths, graph revision and renderer camera state remain unchanged.
  Newly created disabled combo boxes inherit the application palette. Free
  camera and normal-view production renderer code are preserved, not natively
  validated here.
- Fixture diffs reviewed directly include CMake, UI, presenter, runtime,
  workspace, graph/settings/views, typed graph and engine transport cases.
  SyntheticCapture wraps the eight-tone worker while retaining the selected
  source identity; it never delegates to a physical device. Runtime/UI WLED
  fixtures use loopback receivers and assert delivery/replacement/Stop.
  Apply checks use request IDs, accepted ACK and actual working graph; removed
  toolbar controls are not the success oracle. CMake puts `-UNDEBUG` after
  `-DNDEBUG` (verified in private build.ninja); no assertion-elided Release
  acceptance was used. D1 is the remaining inconsistent fixture condition found.

## Own builds and checks

Private configure used Qt Tools CMake 3.30, Ninja, MinGW 13.1, Qt 6.10.2:

```powershell
cmake -S . -B review-final-desktop/build -G Ninja `
 -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe `
 -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe `
 -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe `
 -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/mingw_64 -DCMAKE_BUILD_TYPE=Release `
 -DFETCHCONTENT_SOURCE_DIR_BDSP=C:/Users/Koba/CLionProjects/qt-visualization/cmake-build-release/_deps/bdsp-src `
 -DFETCHCONTENT_SOURCE_DIR_MINIAUDIO=C:/Users/Koba/CLionProjects/qt-visualization/cmake-build-release/_deps/miniaudio-src `
 -DFETCHCONTENT_SOURCE_DIR_QTNODES=C:/Users/Koba/CLionProjects/qt-visualization/cmake-build-release/_deps/qtnodes-src `
 -DDEPLOY_BUILD_RUNTIME=OFF -DBUILD_LEGACY_APP=OFF -DBUILD_TESTING=ON
cmake --build review-final-desktop/build --target core_app qt_workspace_resize_tests gui_views_tests graph_settings_tests engine_transport_tests desktop_history_demand_tests graph_document_adapter_tests -j 3
```

Configure and all requested build targets succeeded. Initial compiler detection
failed before MinGW was added to PATH; corrected private configure succeeded.
Private probe injection uses
`-DCMAKE_PROJECT_qt-visualization_INCLUDE=.../review-final-desktop/probe.cmake`.
An initial injection lacked root-scope Qt6::Test; adding find_package in the
private script fixed it. Neither setup issue is attributed to production code.

| Own invocation | Result |
| --- | --- |
| CTest exact regex for engine_transport, desktop_history_demand and three qt_workspace_resize cases | 5/5 pass, 12.52 s total |
| gui_independent_views | Pass, 1.65 s |
| gui_graph_navigation | Pass, 0.77 s |
| graph_settings_theme, unmodified | Fail, 1.09 s; D1 |
| Direct graph_document_adapter_tests.exe | Exit 0 |
| Own final_desktop_probe.exe | Exit 0; actual GUI composition, opaque JSON, RU/EN, palette inheritance, parameter/camera/width stability, hidden DSP continuation, explicit slots, public detach, Stop |
| Private corrected-condition final_settings_diagnostic.exe | Exit 0, about 7 s; investigative isolation only |
| Build pipeline_cli, then --synthetic --duration 250 | Build success, exit 0; 14 processed spectrum frames with stderr logging |

Logs: `configure.log`, `build.log`, `targeted-1.log`, `targeted-2.log`,
`settings-failure.log`, `probe-build.log`, `probe-run.log`,
`diagnostic-build.log`, `settings-diagnostic-run.log`, `document-run.log`,
`cli-build.log`, `cli-run.log`, all under `review-final-desktop/`.
The first short CLI run had stderr logging disabled; it exited 0. The second
250 ms run enabled logging to collect the actual processed-frame evidence.
The first CTest regex did not match the actual graph_document_adapter_tests name;
that binary was subsequently run directly and passed.

## Source provenance

`source-hashes.txt` and `actual-diff.txt` under the private directory retain the
own source manifest and actual baseline diff. Principal Git blob hashes:

```text
apps/desktop_gui/src/core_application.cpp 524685f55732c348719b01b2b21e4abcb090cc2e
apps/desktop_core/main.cpp 9c5ecf60b7888d68322dc14fea6c8a62c8a7029e
presentation/qt/src/presenter/gui_presenter.cpp f822a6ddef8a0f6ea10fac75a7a8d46a3684dde4
presentation/qt/src/editor/editor_widget.cpp 372ff6e2e3c44653940562d763db33ee2f3d9b28
presentation/qt/src/editor/audio_node_model.cpp cd71205a7278752471c4da49b3a1b2eba1f0cb20
presentation/qt/src/workspace/dock_workspace.cpp 4a27a69d9d1df8da09903271412e24889849937b
presentation/qt/src/renderer/node_views.cpp abc0dd63c2e574d043efb82152d899af13b920e1
adapters/desktop/runtime/src/audio_pipeline.cpp 8fdc2cd1c29e6184fa63a6b063b53aa0d0bb633d
adapters/desktop/runtime/src/engine_client.cpp 78952846267fc1e0a3e99b26c0c5f93f5b81a862
tests/graph_settings_tests.cpp 8da47a49f8ae9233a2f52da06ee9db1887224339
tests/CMakeLists.txt 1df9845c11e94adb324b54394643655c03ec2540
```

Baseline/current equal blobs: Theme `7f5dfed301698130257068a97b7f29c869df6f00`,
SpectrogramGraph `08402c5bda3c5ce7e92f916bad87cbe3a5d05cd1`, modifier
`42d1d4bae9c8fa0595e2a54808cda5c9341be0f4`, ConfigManager
`f5f0f0de3a6621efd1e5db26fb02b7b07abf609f`, SettingsWidget
`735a2b376006f8608da0a604443a6a6a3396c766`.

Read AGENTS.md, the full pinned reference, architecture.md, architecture-map.md,
network-stream-v2.md and the desktop final-review brief. Independently fetched
the [pinned reference](https://github.com/ArthurKoba/ai-agent-workflow/blob/9bc1b1ff27f7204fa946253a4d5612fd80102e8f/skills/software-engineering/clean-architecture-ddd.md);
it matches the local snapshot after removing the snapshot comment and normalizing
newlines. No author test result is used as acceptance evidence.

## Limits and remaining gates

Offscreen validates model/widget behavior, geometry, subscription and data
delivery. It does not validate native OpenGL pixels, free-flight rendering,
hardware capture/UART/WLED, native window-manager drag quality, OS System-theme
response or multiple monitors. Setting System in an offscreen test proves only
the application-mode path. No physical capture, LAN output, cursor movement,
screenshots, native session, SDK installation, shared Ninja, commits, branches,
new chats/subagents or long/full suites were used. User config.json and output/
were never used as fixtures or written.

After D1 is corrected, rerun that exact fixture independently. After the pending
engine-port owner freezes changes, the changed composition/header/fixture scope
needs a separate narrow rereview; these frozen-HEAD binaries do not accept it.
The coordinator's full short-suite and deployment/DLL gate remains separate.

### Subsequent owner correction (source-only check)

After the frozen builds/runs above finished, the coordinator corrected only the
shortcut guard at tests/graph_settings_tests.cpp:401-405. Own git diff inspection
confirms the new exception accepts exact standard Copy/Cut/Paste sequences only
with Qt::WidgetShortcut, preserving the File exceptions and rejection of other
Ctrl shortcuts. This is the same condition independently isolated in the private
diagnostic. The correction is appropriate by source inspection. D1 remains
pending integration verification: the shared corrected target has not been
rebuilt/rerun by this reviewer. Production/header/fixture engine-port edits were
then observed concurrently; no further compilation was attempted against that
moving source. The NOT READY verdict and source hashes above refer to the frozen
HEAD, not acceptance of those edits.

## Narrow rereview closure — READY for desktop/Qt

The coordinator subsequently froze the GUI EnginePort/provider extraction and
D1 guard. This reviewer inspected the actual new canonical source and diff,
rebuilt the application and affected test targets privately, and independently
ran the changed scenarios. No author reports or reported results were used.
HEAD remains `22369719cd00c6e57fafdc166f42a842aa5f161c`; this rereview covers the
uncommitted GUI/port/fixture source identified below, not just HEAD.

**D1 closed.** The actual shared graph_settings_tests.cpp with blob
`a7a005fa244afe5f9c8c3b89f78d5bba7a98d80e` compiles and passes
`graph_settings_theme`. The exact-standard-sequence WidgetShortcut exception
permits required editor Copy/Cut/Paste while retaining the restrictions on
application Ctrl actions. The actual editor clipboard/navigation case also
passes. The original `targeted-2.log` and `settings-failure.log` remain unchanged
as RED evidence; acceptance no longer relies on the private diagnostic copy.

The provider extraction changes GuiPresenter construction and dynamic selection
to injected `EngineProvider::create`. Current endpoint and cached inactive
endpoints are owned through `unique_ptr<EnginePort>`; the provider is retained
through shared ownership. Concrete EngineClient/AudioPipeline assembly lives in
DesktopEngineProvider under apps/desktop_gui. CoreApplication supplies it and
keeps local diagnostic access there. Source inspection confirms DTO field types,
defaults, subscription bit values, states and stable namespaces are preserved
by the extraction. New port headers contain owned DTOs/contracts rather than
capture/executor/transport implementations. This is a desktop behavior and
composition check, not an independent verdict on the architecture review.

Own `port-probe.cpp` includes GuiPresenter and ordinary Qt headers, builds its
own minimal JSON graph, and implements EnginePort/EngineProvider without
EngineClient or AudioPipeline in the fixture. It verifies local/remote provider
routes; runtime-only config delivery; positive Apply ACK with nonzero request ID;
current, stale-generation and inactive-endpoint frame handling; late-ACK Start
intent cancellation by Stop; external explicit Stop clearing; endpoint reuse;
per-device draft/view restoration; unknown JSON retention; and pending callback
cancellation with endpoint destruction. It passes. The existing own
CoreApplication composition probe required no public-constructor edit (that
constructor deliberately still accepts a synthetic source factory); its fresh
build now exercises the new concrete provider assembly and passes its original
checks without weakening them.

The newly adapted shared constructors were inspected directly: editor and
resize fixtures add synthetic_engine_provider and pass it explicitly; presenter
fixtures use the same provider and retrieve concrete diagnostic state from that
provider instead of the abstract client. The exact replaceable-port case uses
fake endpoints and checks command/snapshot/view behavior and selected-device
restoration. Assertions outside necessary construction changes are retained.

### Runtime isolation and source identity

Runtime owner repairs were still active in audio_pipeline.cpp and
stream_transport.cpp. Before configuring the narrow build, this reviewer copied
those two actual source bodies into review-final-desktop/frozen-runtime. A
private CMake deferred function replaces only those two source entries of the
audio_pipeline target with the immutable copies. build.ninja and build output
confirm those source paths. GUI/provider/DTO/fixture sources are the frozen
current canonical sources. The copies are unmodified; their SHA256 hashes are:

```text
audio_pipeline.cpp 9C4C96C323FFADA9523F6B0361C60FD77C8566086D22A52A76C99FB2FE7643AF
stream_transport.cpp DC711C5939F4048683F21330D0A2F41358DE35964DBA5E8DD3EF4A4FFE3D755D
```

This deliberately does not accept the runtime owner's final bodies. Final
integrated runtime and deployment verification remains with its independent
reviewer/coordinator. No mixed moving-body run is presented as final runtime
acceptance. GUI/provider/DTO/shared guard hashes before and after the narrow
build/runs compare equal in narrow-source-hashes.txt and
narrow-source-hashes-after.txt.

```text
apps/desktop_gui/include/core_app/desktop_engine_provider.h 028f9c6cc6ddf5d421d11c1c209c47a6e930815d
apps/desktop_gui/src/core_application.cpp 6f2541dcef020c233622a2ba6845c19042f4bb5d
apps/desktop_gui/include/core_app/core_application.h c4aea12dbd278d894ff13f32d94f4616cb3b17c9
presentation/qt/include/core_app/gui_presenter.h cb7f9ae03d17a54fd405b7f2a0a045bff0dc5e93
presentation/qt/src/presenter/gui_presenter.cpp d8f6d77e9eda04f1a961853d629e47f6829b8a92
presentation/ports/include/pipeline/engine_port.h 3d135dc6575dd821d3a236e7fb660ae8dcebf8d3
presentation/ports/include/pipeline/engine_models.h a929b82865bd2417a9fb35243aaa0d5c654f1100
presentation/ports/include/pipeline/owned_frame.h 79da82879ba71a46d95d64447876c59c293e3b1a
tests/gui_presenter_tests.cpp 67373b023975b0910325cd61cdb185cf6412f3f1
tests/graph_settings_tests.cpp a7a005fa244afe5f9c8c3b89f78d5bba7a98d80e
tests/editor_tests.cpp 68eef46cb23e078e877fa0d518c1b6fa89b11cb6
tests/qt_workspace_resize_tests.cpp 44079682a8b75d09e0f977be2c631ee9ca745159
```

### Narrow commands and results

Using the same private Release environment and offscreen/Qt plugin variables:

```powershell
cmake -S . -B review-final-desktop/build
cmake --build review-final-desktop/build --target core_app final_desktop_probe final_port_probe graph_settings_tests gui_presenter_tests -j 3
./review-final-desktop/build/bin/final_port_probe.exe
./review-final-desktop/build/bin/final_desktop_probe.exe
ctest --test-dir review-final-desktop/build -R '^(graph_settings_theme|gui_replaceable_engine_port|gui_serial_runtime_options)$' --output-on-failure
cmake --build review-final-desktop/build --target editor_tests qt_workspace_resize_tests -j 3
ctest --test-dir review-final-desktop/build -R '^(editor_clipboard_navigation|qt_workspace_resize_selection)$' --output-on-failure
```

Both own probes exit 0. Exact shared cases: gui_serial_runtime_options 0.11 s,
gui_replaceable_engine_port 0.10 s, graph_settings_theme 7.08 s (3/3 pass,
7.31 s total); editor_clipboard_navigation 0.27 s and
qt_workspace_resize_selection 0.50 s (2/2 pass, 0.80 s total). All requested
narrow build targets succeed, including core_app. New tests and private probes
retain -UNDEBUG.

An initial private port-probe compile depended on the former transitive
GraphProgram declaration. The private fixture was changed to construct its
minimal JSON directly, avoiding that concrete test dependency; none of its
behavior assertions were removed. The retry succeeded. This setup error is not
a production failure.

Logs: narrow-configure.log, narrow-build.log (initial private-fixture failure),
narrow-build-retry.log (successful rebuild), narrow-port-run.log,
narrow-composition-run.log, narrow-ctest.log, narrow-callers-build.log and
narrow-callers-ctest.log. Original RED/probe logs are retained. User config.json
still has SHA256
`9EE158D2264D7D1D64F54A238D347AE492548857D7495FD6B2AC7FC315FD98BE`.
Production/shared tests and user config/output remain untouched by this reviewer.
No full-suite, native/hardware or OS-theme acceptance is added.
