# Automatic fixture audit (author slice)

Date: 2026-10-06. Baseline HEAD: `0387743`. This is source-audit and author
verification evidence, **not independent acceptance** or a full-suite pass.
Existing changes belonging to other owners were preserved. No production source,
common CMake, physical audio device, native window, shared Ninja build, branch,
commit, or user `config.json` / `output/` was changed or run by this slice.

Read authority: `AGENTS.md`, `docs/refactor-execution.md`,
`docs/architecture-map.md`, and the complete local
`docs/clean-architecture-ddd-reference.md` pinned to
`9bc1b1ff27f7204fa946253a4d5612fd80102e8f`.

## Findings and repairs

`AudioPipeline::apply_graph` can auto-start an enabled AudioSource, even when a
fixture only appears to validate/apply a default graph. Constructor preparation
alone does not open capture. Tests must examine later Apply and Start commands,
including commands sent through EngineClient/GuiPresenter, rather than assume
the absence of an explicit `start_capture` makes a fixture safe.

Added `tests/synthetic_capture.h`, explicitly authorized by the coordinator.
Its factory always constructs the existing in-memory eight-changing-tone worker;
it never calls `default_audio_source_factory`. The wrapper reports the requested
SystemOutput/Microphone/TestSignal identity through `source()`, delegates
configure/start/stop/readiness/diagnostics, and preserves the diagnostic backend
`In-memory test generator`. Graph documents and their source values remain
unchanged. This allows source-switch, persistence and Running-state checks to
retain their original purpose without opening a physical driver.

The optional `deliver_audio=false` mode delegates worker lifecycle but discards
generated audio callbacks. It is used only where existing tests supply their own
frames and make exact delivery/numerical assertions; background tones must not
contaminate those assertions. No existing assertion was removed or relaxed.

| Changed fixture | Evidence / preserved goal |
| --- | --- |
| `graph_tests.cpp` | Apply(defaults) now receives synthetic capture; graph validation, layout revision, disconnected Apply, and synthetic CLI subprocess checks remain. |
| `pipeline_tests.cpp` | Injected manual-feed factory prevents setting/channel Apply from opening hardware or contaminating exact stereo/WLED/numerical and latency inputs. |
| `runtime_tests.cpp` | CoreApplication receives manual-feed factory; three loopback start/stop cycles use the in-memory worker while exact GUI delivery/generation tests keep their supplied frames. |
| `ui_tests.cpp` | CoreApplication factory covers Default recovery, Apply(source=1), explicit Start, and live switch to source=0. Source values and Running assertions remain. |
| `workspace_tests.cpp` | CoreApplication receives manual-feed factory on initial and child restoration paths; original AudioSource/undo/persistence identity and explicitly supplied WLED data remain. |
| `core_smoke.cpp` | Both ordinary/first-run and incompatible-schema recovery CoreApplication constructions are injected. Existing TestSignal-backend assertion remains; first-run still validates the real source selection in the graph, executed by synthetic capture. |
| `graph_settings_tests.cpp` | Both CoreApplication and external registry EngineServer pipeline receive the synthetic factory. Existing synthetic document/theme/config assertions remain. |
| `gui_views_tests.cpp` | CoreApplication and remote EngineServer pipeline are injected. Independent local/remote view, transport, unavailable-marker and generator goals remain. |
| `engine_typed_views_tests.cpp` | Local EngineClient and server AudioPipeline are injected; remote NetworkInput view/clear/reconnect behavior remains. |
| `registry_discovery_tests.cpp` | Local EngineClient and server AudioPipeline are injected; discovery/reconnect/offline semantics remain. |

Factory forwarding through CoreApplication -> GuiPresenter -> EngineClient ->
AudioPipeline is implemented by the coordinator and integration owner. This slice
only consumes their explicit overloads; it did not implement a production seam.

## Other audited sources / scope boundaries

* `test_signal_tests.cpp`: only TestSignal graph Applies/Starts; intentionally
  retains the production TestSignal factory route and backend checks.
* `typed_graph_tests.cpp`: applied graphs are NetworkInput-only until the final
  two-TestSignal graph; the counted remote-only factory proves zero local opens.
  Default AudioSource graphs are compiled/evaluated directly, without capture.
* `default_runtime_tests.cpp`: existing injected factory checks requested
  SystemOutput but constructs only TestSignal, including failure/restart cases.
* `graph_continuity_tests.cpp` and `desktop_history_demand_tests.cpp`: existing
  throwing factories forbid local capture; applied graphs are NetworkInput-only.
* `distributed_transport_tests.cpp`: CLI source documents use TestSignal;
  other sources are NetworkInput, including the independent branch. No physical
  AudioSource was found in the executed documents.
* `editor_tests.cpp`: scripted remote peer records commands; no real executor
  handles those default/start commands.
* `legacy_smoke.cpp`: another owner's existing factory injection already uses
  `make_test_signal_source`; it was preserved.
* Pure DSP/contracts/protocol/config/geometry/byte-transport fixtures have no
  capture construction or Start path. Device poller exercises injected polling
  callbacks, not capture.
* `native_capture_checks`, `native_music_player`, `native_gui_soak`,
  `native_spectrogram_lifecycle`, and `render_tests` remain manual device/native
  programs. They were not run or edited. Physical/native CTest registration must
  stay opt-in with `ENABLE_DEVICE_QA=OFF`, `ENABLE_NATIVE_QA=OFF` by default.
* Explicitly excluded files were not edited: `engine_transport_tests.cpp`,
  `graph_preparation_tests.cpp`, `application_lifecycle_tests.cpp`,
  `dataflow_runtime_tests.cpp`, `desktop_serial_stream_tests.cpp`,
  `console_config_tests.cpp`, `gui_presenter_tests.cpp`, `network_runtime_tests.cpp`.
  Coordinator was notified to check injection in local GuiPresenter/EngineClient
  and server fixtures, and synthetic selection in CLI subprocess arguments.

## Release assertions / coordinator CMake list

Source search found assert-based tests in `signal_contracts_tests.cpp`,
`codec_buffer_tests.cpp`, `bdsp_protocol_tests.cpp`, `stream_fragment_tests.cpp`,
`stream_transport_tests.cpp`, `graph_document_adapter_tests.cpp`, and the embedded
SDK tests, plus excluded runtime/preparation files. Assertions include stateful
calls such as pool acquire/retain, assembler accept, publish/listen/decode and
delivery begin. `NDEBUG` would remove both their checks and those calls.

Coordinator has added directory-level `/UNDEBUG` (MSVC) or `-UNDEBUG` (GNU/Clang)
at the top of `tests/CMakeLists.txt`, before portable/embedded includes and the
SDK early return. Do **not** add redundant per-file undefines. No source in this
slice redefines NDEBUG; the only source undef found was another owner's excluded
`graph_preparation_tests.cpp`.

Required integration checks / CMake responsibilities:

1. Preserve that assertion flag for every automatic test target, including the
   portable early-return path; verify actual Release compiler flag ordering.
2. Preserve device/native QA options OFF and omitted hardware/native registrations
   in the automatic CTest inventory.
3. Rebuild changed GUI/test consumers after the factory-overload integration;
   no new target or dependency is required for the header.
4. Existing per-target UNDEBUG in stream transport/portable tests is redundant
   but harmless; cleanup is optional and belongs to the coordinator.

## Commands and results

Source searches used `rg` for AudioPipeline/CoreApplication/EngineClient,
Apply/Start/source kinds, assertions and NDEBUG. `git diff --check -- tests`
passed (only Git LF/CRLF notices).

Private scratch output: `build-fixture-audit`, separate from common Ninja trees.
All commands use the existing Qt/MinGW installation, without downloads.

```powershell
$env:PATH = 'C:/Qt/6.10.2/mingw_64/bin;C:/Qt/Tools/mingw1310_64/bin;' + $env:PATH
& C:/Qt/Tools/mingw1310_64/bin/g++.exe -std=c++20 -O2 -DNDEBUG -UNDEBUG -Itests -Iadapters/desktop/include -IC:/Qt/6.10.2/mingw_64/include -IC:/Qt/6.10.2/mingw_64/include/QtCore -IC:/Qt/6.10.2/mingw_64/mkspecs/win32-g++ build-fixture-audit/synthetic_capture_probe.cpp adapters/desktop/platform/pipeline/src/test_signal.cpp -LC:/Qt/6.10.2/mingw_64/lib -lQt6Core -o build-fixture-audit/synthetic_capture_probe.exe
& ./build-fixture-audit/synthetic_capture_probe.exe
& C:/Qt/Tools/mingw1310_64/bin/g++.exe -std=c++20 -O2 -DNDEBUG -UNDEBUG -Ilibs/signal_contracts/include tests/signal_contracts_tests.cpp libs/signal_contracts/src/frame.cpp libs/signal_contracts/src/buffer_pool.cpp -o build-fixture-audit/signal_contracts_tests.exe
& ./build-fixture-audit/signal_contracts_tests.exe
& C:/Qt/Tools/mingw1310_64/bin/g++.exe -std=c++20 -O2 -DNDEBUG -UNDEBUG -Ilibs/signal_contracts/include -Ilibs/bdsp_protocol/include tests/stream_fragment_tests.cpp libs/bdsp_protocol/src/stream_fragments.cpp -o build-fixture-audit/stream_fragment_tests.exe
& ./build-fixture-audit/stream_fragment_tests.exe
```

All three checks passed. The private fixture probe validates selected source kind,
frame delivery, synthetic diagnostics, joined Stop/no later callbacks, and
manual-feed callback isolation. The existing contracts/fragment tests passed in
Release with assertions executing. An initial compiler invocation before adding
MinGW runtime to PATH failed; the corrected commands above compiled and ran.

## Remaining gates and risks

The changed application fixtures have **not** been rebuilt/run by this slice;
coordinator owns synchronized application rebuild and short targeted integration
checks after source freeze. No full-suite, native graphics, physical capture,
ESP-IDF or hardware acceptance is claimed. CMake option defaults alone do not
override an existing cache that explicitly enables device/native QA; the final
automatic configure/inventory must set both options OFF. Independent review must
inspect actual changes and integration evidence before accepting this author slice.
