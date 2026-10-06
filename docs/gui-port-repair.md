# GUI engine input port repair (A1)

Author report for the independent finding in review-final-architecture.md. The reviewer report is unchanged; closure remains subject to independent acceptance.

## Change

GuiPresenter receives an abstract EngineProvider and owns active/inactive endpoints as EnginePort. Its accessor and callback registration use that interface. Initial local/remote selection and later device selection call the injected provider; returning to an existing device reuses the owned endpoint. No concrete EngineClient/audio_pipeline header, constructor, cast or local_engine accessor remains in the presenter.

DesktopEngineProvider in apps/desktop_gui assembles local and TCP EngineClient instances and retains the composition-only borrowed local diagnostics reference used by CoreApplication::pipeline(). Capture factory injection remains in desktop composition. EngineClient implements EnginePort with overrides; its implementation and receive fences are unchanged by this repair.

The Qt-facing delivery facade is outside portable application/domain. engine_port.h uses QtCore/QtNetwork and owned DTOs; engine_models.h uses QtCore and standard containers. SpectrumFrame, SpectrumMerge, AudioFrame, NodePreview, component/history/view models retain their original fields, order and defaults. RuntimeState retains its enum values and metatype registration in core/runtime_state_model.h. TypedFrame remains Qt-free in pipeline/owned_frame.h (STL + signal_contracts); the canonical adapter typed_frame.h includes that header. Root owns CMake integration, including the pure include path on portable_stream_codec without Qt linking. No portable lifecycle/executor/DSP behavior is changed.

Existing presenter tests retain their actual adapter/loopback characterization. Presenter fixtures now obtain endpoints from application composition with synthetic capture. The generic synthetic_capture.h stays independent of the GUI provider; synthetic_engine_provider.h is used only by the three GUI fixture targets.

## TDD and verification

New --replaceable-port-only case uses FakeEndpoint/FakeProvider, with no concrete endpoint, capture or socket construction. It exercises local bootstrap/configuration/Apply replies, initial remote connection, dynamic provider host/port selection, inactive unsubscription, selected commands, status/window/view delivery, inactive snapshot/view isolation, endpoint reuse, draft/editor restoration and unknown config preservation.

RED: test authored before the new interface; compiler rejected missing pipeline/engine_port.h. Command: `g++ -std=c++23 -fsyntax-only -include pipeline/engine_port.h tests/gui_presenter_tests.cpp`; captured in review-gui-port-repair/tdd-red.log. This is an API compilation RED, not an executed behavioral failure. An initial normal object-target build was interrupted before completion to avoid compiling through header extraction.

The new test was also replayed against a private copy of the original HEAD GuiPresenter header, using current DTO interface headers and the real target include flags. Compilation rejected `GuiPresenter(state, shared_ptr<FakeProvider>)`, initial remote provider injection and comparison of the concrete accessor with fake endpoints. Evidence: review-gui-port-repair/tdd-baseline-api-red.log. The shared source tree was never rolled back for this replay.

GREEN: `cmake --build review-gui-port-repair --target core_app gui_presenter_tests editor_tests qt_workspace_resize_tests -j 8` passed (full consistent interface rebuild; build-green.log). CoreApplication, EngineClient, AudioPipeline and presenter were rebuilt with the same headers. Existing Qt deprecated setActiveWindow warnings and optional deployment dxcompiler/dxil warning did not fail the build.

`ctest --test-dir review-gui-port-repair -R '^(gui_replaceable_engine_port|gui_presenter|gui_serial_runtime_options|editor_clipboard_navigation)$' --output-on-failure` passed 4/4 in 3.03 s: fake endpoint/provider 0.10 s, presenter characterization 2.49 s, runtime-only Serial options 0.13 s and granular editor integration 0.27 s. Evidence: review-gui-port-repair/test-green.log. No further UI/native/stress profiles were run.

Private configuration uses Qt 6.10.2 / MinGW 13.1, Release, Ninja, BUILD_GUI=ON, BUILD_LEGACY_APP=OFF, BUILD_TESTING=ON and pinned existing BDSP/miniaudio/QtNodes source directories under cmake-build-release/_deps. No shared Ninja invocation, branch, commit, push, hardware capture, native UI, speaker output or stress run.

`g++ -std=c++20 -fno-exceptions -fno-rtti -fsyntax-only -Iadapters/desktop/include -Ipresentation/ports/include -Ilibs/signal_contracts/include review-gui-port-repair/owned-frame-consumer.cpp` passed without Qt include paths. `git diff --check` passed.

## Limits

EngineClient gains a second interface base and requires consistent recompilation; old objects must not be mixed with the new headers. The composition diagnostics pointer is borrowed and only valid while its presenter-owned local endpoint exists. This report covers A1, not parallel runtime repairs or native rendering/physical capture acceptance.
