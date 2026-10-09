# Configuration migrations — slice 1C author report

2026-10-06. Status: source implemented and direct-compiler author checks passed;
integration application build and independent acceptance are pending. No next
slice is authorized by this report. Shared dirty baseline was preserved; no
branch, commit, installation, user config.json/output change or native GUI run.

The worker read AGENTS.md, architecture-map.md, implementation-plan.md,
baseline-and-ownership.md, storage-control-contracts.md, node-runtime-contracts.md,
typed-node-routing.md and the complete pinned
[Clean Architecture / DDD reference](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).
The latest explicit coordinator assignment permits this slice despite stale
gate-0 status prose in the planning documents.

## Adapter contracts

`adapters/desktop/include/config/config_migration.h` is a Qt JSON/file adapter.
No application/domain/runtime code depends on it. APIs:

- `classify_document(raw)` accepts absent legacy schema or integer schema 1;
  present null/string/fraction/zero/negative/future is an error.
- `migrate(raw, target_schema=1, target_graph=2)` maps the existing keys without
  creating competing processing/presentation root sections. `project_sections`
  exposes logical processing/draft/presentation/presets/application views.
- `migrate_graph(raw, target=2)` implements explicit graph 1→2 mapping:
  SurfaceView alias, source→FFT Window/WindowFunction, FFT→BarkWeighting.
  Existing IDs and original edge extensions survive; new IDs are checked for
  collisions/exhaustion. An absent graph version explicitly means the historical
  v2 shape, independently of absent project schema and processing version.
- Up migration inserts processing_version 1 only when absent. It maps root graphs,
  root/ui presets and per-device graphs; malformed non-object graph values and
  unrelated arrays remain opaque. Explicit pipeline_graph never falls back to
  scene_graph. A missing applied graph can be seeded from a legacy scene.
- `_configuration_graph_migration` and `_configuration_project_migration` store
  JSON source and mapped provenance. Their names are reserved only when adding
  a migration record; an existing extension collision rejects migration.
  2→1 / schema 1→legacy down steps are supported only for a matching unchanged
  recorded mapping. Edited or native documents without a proved inverse return
  unsupported/lossy downgrade, retaining the input. This conservative policy
  rejects even edits which could theoretically have a reversible mapping; it
  never guesses or silently deletes extensions/new kinds/ports. Records increase
  file size and are storage-only, excluded from processing projection.
- `save_atomic(path, document)` migrates a full candidate before opening
  QSaveFile. It also checks the existing destination independently: malformed JSON
  or unsupported document/graph/processing version prevents even prefs-only
  overwrite. Direct-write fallback is disabled. One write/commit replaces the
  full document; write failure cancels the candidate. Load never rewrites bytes.
- `active_path(explicit_path)` defaults beside the executable and gives explicit
  paths priority. Failed ConfigManager SaveAs keeps the original active path.

`configuration::read` returns original raw JSON after migration eligibility
checks; resolve retains its historical selection API and checks selected graph
versions. ConfigManager uses the explicit migrated candidate for settings/graphs
and retains original raw JSON separately. Save overlays changes on a migrated
base (including standalone projects), so edited applied graphs are not shadowed
by legacy root nodes. `changed_fields` prevents unchanged normalized reader
defaults from replacing explicit raw settings. Unknown arrays/presets/drafts
survive unchanged preferences saves.

Graph adapter APIs: `project_processing_graph` returns document/error; the legacy
object-only wrapper returns empty on rejection, never normalizes invalid version
with toInt(1). Projection consults R's common parameter classification and omits
FFTView channel/merge. `read_processing_graph_document` explicitly maps view
parameters via `domain::project_processing_node`, preserving original raw JSON in
GraphDocument.document. Legacy `read_graph_document` retains FFT selectors.
Layout restore retains local registered display parameters for matching ID/kind;
full request/revision/device correlation remains slice 3 ownership.

## Checks and integration handoff

Characterization was first compiled and run for resolver ID precedence, explicit
malformed applied refusal, standalone selection, recursive overlay and unknown
arrays/drafts/presets. New migration and projection tests were added before their
APIs (initial compilation failed), then compiled and run after implementation.
Direct compiler artifacts are solely `build-refactor-config`.

Toolchain, Qt root, defines and ConfigManager includes are derived from the
existing build-gui cache/build.ninja, without editing or building that directory.
Representative compiler commands (PowerShell variables are cache-derived):

```powershell
& $configCompiler -std=c++23 -UNDEBUG tests/config_migration_tests.cpp -Iadapters/desktop/include "-I$configQtRoot/include" "-I$configQtRoot/include/QtCore" "-L$configQtRoot/lib" -lQt6Core -o build-refactor-config/config_migration_tests.exe
& ./build-refactor-config/config_migration_tests.exe
& $configCompiler -std=c++23 -UNDEBUG tests/config_document_tests.cpp -Iadapters/desktop/include "-I$configQtRoot/include" "-I$configQtRoot/include/QtCore" "-L$configQtRoot/lib" -lQt6Core -o build-refactor-config/config_document_tests.exe
& ./build-refactor-config/config_document_tests.exe
& $configCompiler -std=c++23 -UNDEBUG tests/graph_document_adapter_tests.cpp adapters/desktop/src/graph_document.cpp domain/src/graph_definition.cpp domain/src/graph_validation.cpp libs/signal_contracts/src/frame.cpp -Iadapters/desktop/include -Idomain/include -Ilibs/signal_contracts/include "-I$configQtRoot/include" "-I$configQtRoot/include/QtCore" "-L$configQtRoot/lib" -lQt6Core -o build-refactor-config/graph_document_adapter_tests.exe
& ./build-refactor-config/graph_document_adapter_tests.exe
& $configCompiler -std=c++23 -c adapters/desktop/gui_config/src/config_manager.cpp @configDefines @configIncludes -o build-refactor-config/config_manager.cpp.obj
& $configCompiler -std=c++23 -UNDEBUG -DCONFIGURATION_MANAGER_TESTS tests/config_migration_tests.cpp build-refactor-config/config_manager.cpp.obj @configDefines @configIncludes "-L$configQtRoot/lib" -lQt6Core -lQt6Gui -lQt6Widgets -o build-refactor-config/config_manager_migration_tests.exe
& ./build-refactor-config/config_manager_migration_tests.exe
```

All final compilation/runs exit 0. Covered: legacy/v1/v2/provenance up/down,
malformed/future versions and unchanged failure input, ID exhaustion, idempotence,
opaque arrays and per-device/draft preservation, source overwrite protection,
failed SaveAs active path, raw explicit normalized prefs preservation, standalone
edit save, strict projection/view mapper and local FFT selector restoration.
Earlier direct graph link omitted graph_validation.cpp and failed with undefined
parameters_valid; final command above includes it and passes.

I owns all CMake/manifests. Requested targets: `config_migration_tests` linked to
config_document/QtCore; `config_manager_migration_tests` from the same source with
CONFIGURATION_MANAGER_TESTS linked to desktop_gui_configuration. Existing
config_document_resolver and graph_document_adapter_tests remain checks.
Whole application build and short integrated checks remain mandatory with I
before gate acceptance. No simultaneous shared Ninja process was used.

Default CLI override callsite: apps/desktop_gui/src/core_application.cpp:271 uses
literal config.json; I must wire the executable-relative helper where no explicit
CLI argument is supplied. ConfigManager's implicit constructor already uses it.
GraphProgram::canonical_graph_document in runtime/src/graph.cpp remains owned by
another worker. New migrated graphs carry v2, preventing a second v1 stage
insertion. Its replacement/delegation and full processing control integration
are later handoffs, not changes made by this worker.

Atomic write/commit failure injection, full UI/engine correlation, native rendering
and hardware were not tested in slice 1C. Author checks do not substitute for new
independent review. Further implementation waits for the coordinator's gate.

## Slice 1 integration follow-up: processing equality

The coordinator separately assigned only GraphProgram::same_processing_as in
`adapters/desktop/runtime/src/graph.cpp` and existing `tests/graph_tests.cpp`.
I's smoke log showed 12 equal connections serialized in different order, with
same_processing_as false while spectrum processing was active. The raw vector
comparison treated edge storage order as processing state.

TDD red: new `graph_tests --processing-equality-only`, compiled against I's
existing archives, exited 1 with
`Graph test: Reordered identical edge multiset changed processing identity`.
The only implementation change replaces positional edge comparison with
four-iterator std::is_permutation. It compares the complete Edge equality
(from/to/output_port/input_port) and multiplicity, with no sorting, allocation,
normalization or mutation. This control-plane comparison is O(edges²) in the
worst case, bounded by admitted graph size; it is not DSP hot-path work.

Green: recompiling graph.cpp as an isolated object and linking it before I's
archives, then running the same bounded flag, exits 0. Cases cover reorder in
both comparison directions; reordered nodes also pass without implementation
changes, so positional node logic was not broadened. Changed source endpoint,
output port, removed connection, meaningful signal amplitude, graph ID and node
ID remain unequal. Duplicate incoming edges, duplicate IDs and invalid input
ports remain rejected by compilation. No smoke assertion was weakened.

Commands use the cache-derived compiler/Qt root, graph_tests defines/includes
from build-refactor-integration/build.ninja and its read-only
`ninja -t commands graph_tests` linker libraries. Outputs stay in
build-refactor-config:

```powershell
& $configCompiler -std=c++23 -UNDEBUG tests/graph_tests.cpp @configDefines @configIncludes @configLibs -o build-refactor-config/graph_equality_red.exe
& ./build-refactor-config/graph_equality_red.exe --processing-equality-only # exit 1 before fix
& $configCompiler -std=c++23 -UNDEBUG -c adapters/desktop/runtime/src/graph.cpp @configDefines @configIncludes -o build-refactor-config/graph_equality.cpp.obj
& $configCompiler -std=c++23 -UNDEBUG tests/graph_tests.cpp build-refactor-config/graph_equality.cpp.obj @configDefines @configIncludes @configLibs -o build-refactor-config/graph_equality_green.exe
& ./build-refactor-config/graph_equality_green.exe --processing-equality-only # exit 0 after fix
```

Set QT_FORCE_STDERR_LOGGING=1 for the red diagnostic. I must rebuild existing
graph_tests/application targets and repeat the unchanged failing
silent_gui_pipeline smoke. This worker did not run shared Ninja, GUI, CLI,
capture playback or the broad pipeline_graph scenario for this narrow check.

## Independent-review repairs F1–F3 (source freeze)

Independent reviewer `01a112b8-b2cc-7e03-a6ae-d712e18f91d8` rejected the slice.
Its report/reproduction is build-gate1-storage-review/review.md and repro.cpp.
This author fixed its findings; this is not re-acceptance.

F1: recognized `devices/*/presets[*]/graph` now uses the same preset traversal as
root presets and ui.pipeline_presets. Non-object entries and opaque preset fields
are retained, valid legacy preset graphs migrate, invalid/future graph and
processing versions return the full device/preset error path. The existing
destination version gate in save_atomic therefore protects these fields too.
New tests exercise null/string/99 in both version keys and byte-for-byte source
preservation on rejected overwrite.

F2: graph 1→2 inserts an absent processing_version before recording mapped
provenance. map_graph does not add it after a down step. Integrated unchanged
up/down preserves exact source JSON, including absence versus explicit version,
for applied/draft/root presets/ui presets/device graphs/device presets. Real
graph edits still reject a lossy inverse. Author red new integrated-chain test
exited 3 at down_chain equality. The independent old reproduction also printed
up=1/down=0, future-device-preset migrate=1 and overwrite=1. New tests exit 0.

F3 was separately assigned by the coordinator to these narrow paths:
apps/desktop_gui/src/core_application.cpp startup constructor/initialize call;
presentation/qt/include/core_app/gui_presenter.h initialize overload;
presentation/qt/src/presenter/gui_presenter.cpp initialize guard;
tests/core_smoke.cpp bounded startup and adjusted schema-regression assertions.
Composition distinguishes absent first-run from a failed existing file. Failure
captures the absolute path/error and retains any readable raw JSON as opaque
storage only. It skips default editor seeding and passes startup_error to
GuiPresenter::initialize, which emits the visible warning/apply status and
returns before network configuration, subscription, local/remote bootstrap or
default Apply. GUI remains open; no process exit or modal dialog was added.
Existing missing-file first-run still bootstraps the default with the synthetic
capture adapter. Unsupported existing files remain protected on prefs writes.

`core_smoke --configuration-startup-only` uses QCoreApplication parent and
offscreen synthetic children. It covers future/null/string schema, malformed
JSON and future device preset, visible path diagnostic, no pending Apply,
zero initial/remaining revision, no running Analyzer/Capture and unchanged bytes;
missing-file control requires compiled default source, running Analyzer,
synthetic backend and no pending graph request. A default graph is already
prepared by AudioPipeline, so accepting the identical first-run graph may leave
revision zero; the positive case does not incorrectly require a revision bump.

Author red compiled new core_smoke against prior I archives and exited 1:
`Rejected configuration bootstrapped processing or lost the visible diagnostic`.
New composition/presenter/manager objects compile individually (exit 0), and the
six parent/child cases pass when linked ahead of those archives. **That mixed
binary is not final integration evidence.** The schema_regression recovery run
ended with an access violation in that binary. Its cause is unverified; current
R/S headers plus old archives may disagree. No source fix or acceptance is claimed
for that crash. I must freshly compile dependent targets before rechecking it.

The old schema_regression expected explicit default recovery to overwrite graph
version 99. That expectation contradicts the approved strict future overwrite
protection. With the coordinator's explicit authorization, the test now requires
no automatic startup Apply/activation, unchanged protected source after prefs and
explicit recovery, a valid recovered **working** graph and visible failed-save
status. It still requires the recovery action and cleared working-graph warning.
Saving unsupported raw documents, including SaveAs with that raw source, remains
blocked by the adapter; opening/creating a supported document is required before
normal persistence. No general recovery/SaveAs redesign was made in this repair.

Required I commands after fresh current-source whole desktop compilation:

```powershell
& $cmakeTool --build build-refactor-integration --target core_app core_smoke config_migration_tests config_manager_migration_tests graph_document_adapter_tests config_document_tests graph_tests -j 4
& ./build-refactor-integration/bin/core_smoke.exe --configuration-startup-only
$env:QT_QPA_PLATFORM='offscreen'; $env:QT_FORCE_STDERR_LOGGING='1'
$env:QT_VISUALIZATION_SCHEMA_REGRESSION='1'
& ./build-refactor-integration/bin/core_smoke.exe
Remove-Item Env:QT_VISUALIZATION_SCHEMA_REGRESSION
& $ctestTool --test-dir build-refactor-integration --output-on-failure -R '^(core_startup_shutdown|silent_gui_pipeline|config_migration_tests|config_manager_migration_tests|config_document_resolver|graph_document_adapter_tests)$'
```

Names/registration for new migration tests belong to I. Existing schema/first-run
smoke assertions were not reduced to status-only evidence. No shared Ninja,
user config/output, install, commit or later slice work was performed. Source is
frozen for I build and independent re-review. A failed fresh integration scenario
must return to the owner rather than being waived by these author observations.

## Second independent-review repairs — freeze for fresh integration

The fresh I and independent-review builds reproduced the recovery crash; the
earlier mixed-ABI hypothesis was insufficient. Read-only GDB on I's fresh binary
identified SIGSEGV in qt_qFindChild_helper through _save_configuration during
SchemeEditorWidget destructor flush and window destruction. Breaking on the
first __cxa_throw showed `Protected save failure is not visible`; destruction
then masked that assertion. Evidence is in build-refactor-config:
schema-fresh-gdb-stack.log and schema-fresh-first-throw-message.log.

Coordinator released the freeze for these narrow repairs:

- CoreApplication destructor now invokes the existing normal window close path
  while the full tree is valid. That path stops workspace/camera timers, flushes
  editor presentation, saves workspace and performs enabled autosave. Only then
  presenter save callbacks are disconnected and the _main_window pointer is
  cleared before deletion. _save_configuration returns without widget access
  when that pointer is absent. No editor source or general Qt teardown was
  rewritten; enabled final autosave/workspace persistence is preserved.
- GuiPresenter stores a separate file_protection_warning. Accepted explicit
  working Apply can clear saved_graph_warning, but the existing warning signal
  carries the retained file diagnostic (combined with any working warning) to
  the existing label. It outlives temporary statusbar messages and is not cleared
  by working recovery. Open creates a new application with newly checked file
  authority; unsupported-source overwrite/SaveAs protection remains unchanged.
- Optional migration inspection uses value() instead of mutating [] for root
  presets, ui and ui.pipeline_presets. Absent fields remain absent; explicit null
  remains present. UI-only preset selection survives preferences saves. A
  present malformed/null root preset list retains its historical authority and
  does not silently fall back to the UI list.

Tests: config_migration_tests adds minimal schema1/v1 exact integrated up/down,
absence/null checks, UI-only preset save/resolve; the manager variant additionally
loads it, changes language, saves and resolves the same ID. Initial minimal
roundtrip assertion exits 3 before the lookup correction; final migration and
manager source-compiled tests exit 0. F1/F2/device/draft/opaque cases remain.

core_smoke schema_regression now explicitly clears the transient status message
after recovery and requires file_protection_warning plus the visible existing
label containing the source path, while working warning clears and original
bytes remain intact. A source-compiled candidate passes that scenario (exit 0).
`--configuration-shutdown-only` adds parent/child intentional exception unwind:
enabled valid config must retain pending language, editor view and saved workspace;
disabled autosave and protected future-file config must retain exact bytes. Every
child must reach the catch and exit normally. Linking the test against pre-fix I
archives reproduces abnormal protected-child exit (parent exit 1); new direct
composition/presenter/manager objects pass all three children (exit 0). The initial
valid-enabled/disabled cases already passed before the fix and characterize
legitimate final-save behavior; they are not claimed as new failing tests.

Final save-order correction uses normal closeEvent, preserving MainWindow's
workspace save in addition to editor flush. The final shutdown test verifies a
nonempty persisted ads_state and workspace version, and passes. All changed
source objects/test source compile; scoped diff --check passes. Direct object
checks do not replace a fresh dependent build: the header changes require I to
rebuild all affected desktop dependencies. No more mixed-archive investigations
are planned. Source is frozen for that build and new independent acceptance.

I handoff: existing core_smoke target, add/register the bounded
`core_smoke --configuration-shutdown-only` case if desired (all manifests remain
I-owned). Run fresh whole affected desktop build, core_configuration_startup,
core_configuration_schema_recovery, the bounded shutdown case,
core_startup_shutdown, silent_gui_pipeline and existing migration/manager tests.
No test is waived; recovery crash/persistent warning need fresh integration PASS.
