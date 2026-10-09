# Baseline и границы владения

## Актуальная организационная поправка — 2026-10-07

Последующее прямое указание пользователя отменяет требования legacy support и
промежуточных TDD/characterization/test прогонов: prototype использует единственный
canonical numeric pipeline. Старые конфиги и протоколы не мигрируются; исходные
пользовательские файлы сохраняются и неподдерживаемый формат диагностируется.
Implementation и сборка продолжаются; новые тесты и независимая приёмка выполняются
на финальной подключённой системе. Все краткие V1 freezes сняты, не начатый
повторный V1 build/oracle отменён. Исторические RED/PASS ниже остаются фактами своих
редакций. I отвечает за desktop engine/capture/provider/presenter composition и
CMake/SDK/ESP wiring; владельцы передают один готовый API block.

По переданному координатором прямому указанию пользователя лимит **трёх активных
рабочих чатов снят**. Старые записи о лимите, микрогейтах и завершённых snapshots
ниже сохраняют историческую evidence и не ограничивают новые явно назначенные
крупные блоки. Сохраняются disjoint writers, запрет отката чужих изменений и один
Ninja на build-каталог. I остаётся единственным владельцем общих CMake/build,
composition, SDK export/install и ESP manifests; ранняя общая сборка не читает live R.

Актуальные назначения координатора: R — runtime и явно расширенная canonical
application facade; T — назначенные desktop control mapper/session; E — назначенные
embedded session/profile/NVS; G — назначенные ports/renderer; P — lossless presets
repair. Эти названия не передают соседние файлы. Точные file/API handoff каждого
владельца определяют compiled closure перед интеграцией. Общие header-изменения
передаются читателям как один краткий API handoff. i18n остаётся read-only для G
до согласования с U. I согласует короткий R checkpoint непосредственно перед
общим build, после готовности P и связанных владельцев.

Текущий обзор и отметки ведёт координатор/workflow updater в
[progress-checklist](progress-checklist.md), правила UI —
[codex-plan-tracking](codex-plan-tracking.md). В I настоящий native `update_plan`
не предоставлен; checklist открыт через `open_in_codex` как **Markdown fallback**
(ответ `queued`, визуальное открытие не утверждается). I не обновляет общий
native plan или coordinator-owned checklist. Implementation, integration и
independent acceptance различаются; прежний V1 GREEN не является whole V1 PASS.

Позднее пользователь явно отверг повторное открытие Markdown fallback ради
статусов. Это указание имеет приоритет над fallback-строкой AGENTS: новые открытия
UI-документов для плана не выполняются; workflow/root продолжают вести файлы.

### Активные назначения координатора

Короткий compiled checkpoint I от 2026-10-07 завершён: приложение и V1 targets
собраны, R/G inputs не изменились между receipts, владельцы освобождены сразу.
Actual editor preset integration остаётся **RED**: первый SavePreset сообщает
успех, но перечитанный файл не содержит новый preset. P исправляет visual version3
boundary; независимая приёмка ещё не выполнена. Evidence:
`build-refactor-integration/v1-presets/integration-report.md`, `ctest.log`,
`combined-before.json`, `combined-after.json`, `artifacts.json`.
Единственный delta receipt — live U `ui_ru.ts`; текущая QM closure не считается
замороженной. Этот checkpoint не принимает V2 runtime или V3 renderer.

| Владелец | Чат | Назначенная зона |
| --- | --- | --- |
| T | `01a116dd-2b72-7490-8472-6d2ce1e5b4bc` | Desktop numeric control mapper/session companions; прежняя schema1 compatibility |
| E | `01a116dd-3ca9-76c2-aed9-c05919486823` | Embedded numeric session/profile и portable persistence/NVS adapter |
| G | `01a116dd-5177-7471-bb3b-bed85a373888` | Назначенные ports/renderer, numeric delivery/projection companions |
| R | `01a11292-e236-7210-932c-a72255389b1b` | Runtime + расширенная application canonical facade |
| I | `01a11292-f153-79f3-99cf-b51f85b55304` | CMake/shared build/composition/SDK export/install/ESP manifests |

### Numeric CONTROL: текущая bounded приёмка

Координатор явно принял **bounded numeric CONTROL codec module, BOTH CBOR/Protobuf**
после [independent mask rereview](../../build-gate-numeric-control-review/mask-rereview/review-report.md).
P2 zero-mask finding закрыт: исходный failing oracle сохранён без изменений,
добавлены independently malformed decode wires; actual build0/run0, 66 checks,
0 failures, eleven-file freeze unchanged. Authority —
`build-refactor-numeric-control/source-freeze.json` (message.cpp
`D1E9836AB2620335B…`). Исторический FAIL и прежние доказательства не изменены.
T/E могут потреблять этот codec contract. Это не V4 end-to-end, application/SDK
integration или hardware acceptance; прежние allocation/task-stack ограничения
сохраняются. I CMake/ESP numeric CONTROL registration подготовлен, но общая
сборка ещё ждёт P freeze и короткого immutable R closure checkpoint.

Дата: 2026-10-06 (Europe/Moscow). HEAD: b18cfbf3c2ac62b4baf022de5348f5a29d46e2cc.
Статус начального baseline: dirty workspace, **не baseline чистого HEAD**.
Актуально: GATE 1 принят координатором, только assigned slice 1; итоговая acceptance запись в конце.
Scope этой работы: только этот файл и implementation-plan.md. Ни один исходник не исправлялся.
[Утверждённый план и gates](implementation-plan.md) задаёт будущую реализацию.

## Прочитанная authority

Полностью прочитаны AGENTS.md, docs/architecture-map.md, docs/typed-node-routing.md,
исходный запрос C:/Users/Koba/.codex/attachments/4887cc54-112e-4836-84d3-82950e988245/Вставленный текст.txt
и весь [справочник закреплённой редакции](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md)
через raw GitHub (части 1–590 и 591–конец). Справочник не превращён в backlog.
Соседние docs/refactoring/node-runtime-contracts.md и storage-control-contracts.md не существовали
при чтении; перед gate 0 требуется сверка с ними. Координатор получает эту зависимость.

## Карта черновика

Начальный git diff --stat: **40 tracked files, 615 insertions, 116 deletions**. Изменения заранее
внесены координатором и не приняты. git status --short дополнительно показал:
config.json, output/ (пользовательские, не читать/заменять для fixtures);
docs/typed-node-routing.md, libs/bdsp_protocol/include/device_protocols/, tests/node_routing_tests.cpp.
Срез 0 сохраняет всё это без отката. Новые docs/refactoring и build-refactor-baseline относятся этой работе.
Параллельная работа может изменить subsequent status; начальная карта не утверждает неизменность всего workspace.

| Группа | Смысл наблюдаемого diff | Будущий владелец |
| --- | --- | --- |
| domain + signal_contracts | NodeRole/new kinds, routing/reduction params, Scalar/packet contracts/ownership | R |
| dataflow_runtime + embedded session | Multi-output/routing/reduction/history/packet execution | Runtime R; embedded exact assignment среза 4 |
| bdsp_protocol + desktop stream codecs | New typed payload/subscribe versions | S; семантику согласовать с R |
| desktop graph/document | Typed graph compatibility + JSON processing projection/version | graph runtime — assignment среза 2; graph_document — C среза 1 |
| desktop audio_pipeline | Callback/delivery, including port identity | Assignment среза 2 |
| desktop engine_client/network/stream transport | Control bridge and network binding lifecycle | S |
| WLED server | Packet delivery/stale compatibility | Assignment среза 2, согласовать с transport owner |
| presentation ports/Qt/i18n | Editor dynamic ports, catalogs and views | P |
| CMake and test manifests | Общие target/dependency/export/build integration | I |
| AGENTS.md | Project authority update | Координатор; никто из владельцев не правит |
| Tests | Черновые assertions не являются acceptance | Соответствующий владелец; manifests — I |

Точный initial tracked path list:
```text
AGENTS.md
adapters/desktop/include/desktop/graph_document.h
adapters/desktop/include/pipeline/audio_pipeline.h
adapters/desktop/include/pipeline/graph.h
adapters/desktop/include/wled/wled_audio_reactive_server.h
adapters/desktop/platform/pipeline/src/network_runtime.cpp
adapters/desktop/platform/pipeline/src/stream_transport.cpp
adapters/desktop/platform/wled/CMakeLists.txt
adapters/desktop/platform/wled/src/wled_audio_reactive_server.cpp
adapters/desktop/runtime/src/audio_pipeline.cpp
adapters/desktop/runtime/src/engine_client.cpp
adapters/desktop/runtime/src/graph.cpp
adapters/desktop/runtime/src/stream_codec.cpp
adapters/desktop/src/graph_document.cpp
domain/include/domain/graph_definition.h
domain/include/domain/graph_validation.h
domain/src/graph_definition.cpp
domain/src/graph_validation.cpp
libs/bdsp_protocol/src/stream_codec.cpp
libs/dataflow_runtime/CMakeLists.txt
libs/dataflow_runtime/include/dataflow_runtime/runtime.h
libs/dataflow_runtime/src/runtime.cpp
libs/embedded_support/src/session.cpp
libs/signal_contracts/include/signal_contracts/frame.h
libs/signal_contracts/src/buffer_pool.cpp
libs/signal_contracts/src/frame.cpp
presentation/ports/include/pipeline/owned_frame.h
presentation/qt/i18n/ui_ru.ts
presentation/qt/include/core_app/main_window.h
presentation/qt/include/core_app/scheme_editor/audio_node_model.h
presentation/qt/src/editor/audio_node_model.cpp
presentation/qt/src/editor/editor_widget.cpp
presentation/qt/src/renderer/graph_panel.cpp
presentation/qt/src/renderer/node_views.cpp
tests/domain_contract_tests.cpp
tests/editor_tests.cpp
tests/embedded_sdk_tests.cpp
tests/graph_document_adapter_tests.cpp
tests/portable_foundation.cmake
tests/stream_transport_tests.cpp
```

## Future ownership: три направления и единая integration ответственность

R = node/runtime рабочий чат; S = control рабочий чат; C = отдельный config worker среза 1;
P = будущая desktop/UI ответственность, конкретный чат назначается координатором.
I = этот integration рабочий чат. Персональные thread IDs и выдачу срезов закрепляет координатор.
Координатор подтвердил: stream_transport_tests.cpp — S, lifecycle — R,
новый application control — S, все CMake/manifests — I. Для среза 1 R получает domain/signal,
runtime public capabilities validation и lifecycle; C получает config/storage, S — control contracts.
Desktop delivery и embedded глобально R не назначены: exact ownership выдаётся в срезах 2/4.
Таблица будущих путей ниже — карта передачи responsibilities для дальнейшего назначения,
не глобальная выдача файлов. Строки desktop/embedded в R не являются утверждённым ownership.
Это распределение responsibility; до выдачи среза все исходники read-only.
Максимум 3 одновременно работающих чата, включая I; coordinator не worker и не входит в лимит.
После gate 0 R/S/C реализуют срез; I начинает integration лишь когда один worker idle.
Independent review запускается отдельной фазой после готовности среза, соблюдая этот лимит.
Dependent code не разрешён до соответствующего accepted gate; author-ready/review-running не gate.
Общий файл нельзя редактировать двумя владельцами: cross-owner изменение направляется владельцу/координатору.

| Владелец | Exact ownership среза 1 / дальнейшая выдача | Срезы |
| --- | --- | --- |
| R | domain/include и domain/src; libs/signal_contracts/include и src; libs/dataflow_runtime/include/dataflow_runtime/runtime.h и src/runtime.cpp только public capabilities validation; application/include/application/graph_lifecycle.h и src/graph_lifecycle.cpp; tests/domain_contract_tests.cpp, signal_contracts_tests.cpp, dataflow_runtime_tests.cpp, application_lifecycle_tests.cpp по выданным cases | 1; остальной runtime/routing/WLED — точная выдача среза 2 |
| S | Новые application/include/application/processing_control.h и application/src/processing_control.cpp, typed control/persistence ports и tests/processing_control_tests.cpp; stream_transport_tests.cpp остаётся единым S при будущих transport случаях | 1 control contracts; codecs/transport/resources — точная выдача среза 3 |
| C | Config/storage отдельным worker после gate 0: adapters/desktop/include/config/config_document.h, configuration project/migration adapters, include/desktop/graph_document.h и src/graph_document.cpp; tests/config_document_tests.cpp, graph_document_adapter_tests.cpp, будущие project migration cases. ConfigManager/legacy graph migration shared files назначает координатор отдельно | 1 migration/storage; точные дальнейшие файлы выдаются координатором |
| I | CMakeLists.txt; все **/CMakeLists.txt; cmake/**; adapters/esp_idf/install_sources.cmake и build manifests; tests/*.cmake; SDK packaging/export/dependency pins/codegen scripts; разрешённые docs/refactoring/implementation-plan.md и baseline-and-ownership.md | Единственный общий CMake/SDK integration owner; изменения только после выдачи среза |
| Пока не назначено | Desktop delivery/audio_pipeline/WLED sockets, libs/embedded_support, IDF/NVS/examples, presentation/ports и Qt, apps composition roots: exact source/test ownership координатор выдаёт при срезах 2/3/4; нельзя выводить ownership из этой карты | 2/3/4/5 |

Любой неуказанный existing shared файл или новый root требует назначения координатором до записи.
CMake исключения выше имеют приоритет над ownership root. Device-specific WLED encoder header,
сейчас libs/bdsp_protocol/include/device_protocols/wled_packet.h, передаётся R только после
согласованного boundary/path решения с S/I; не переименовывать стабильные wire IDs.
Общий tests/stream_transport_tests.cpp имеет единственного владельца S: R передаёт WLED test cases ему.
Новый portable application command header принадлежит S; config/storage среза 1 принадлежит C; изменение lifecycle ports выполняет R
после согласования. Runtime capability inference не заменяет actual DeviceCapabilities application.

API handoffs: R предоставляет configured descriptors/semantic contracts/OutputView identity (node, port),
bounded ownership и admission; S — commands/replies/revision/capabilities/control codecs/projection;
C — config/storage migration;
P — EnginePort/EngineProvider clients, presentation-only state; I — targets, headers, sources, install,
same-source firmware and composition. Подробности соседних контрактов согласуются до gate 0.

## Инструменты и прочитанные caches

CMakePresets.json отсутствует. В PATH найден ninja C:/Python/Python314/Scripts/ninja.exe;
cmake/ctest/g++ через Get-Command не найдены. Исполнение использует инструменты из существующего
CMakeCache, без установки и без новых hardcoded paths в проекте.

| Профиль | Cache evidence |
| --- | --- |
| build-sdk | Ninja, Release, BUILD_PORTABLE_SDK=ON, BUILD_GUI=OFF, PORTABLE_DISABLE_EXCEPTIONS_RTTI=ON, FFT 4096 |
| build-gui | Ninja, Release, BUILD_GUI=ON, SDK OFF, FFT 4096, Qt 6.10.2 mingw_64 |
| build-sdk-current-esp32 | Existing ESP-IDF esp32 gcc/toolchain, ESP-DSP ON, compiled FFT 4096 (будущий default 2048 ещё не реализован) |
| build-refactor-baseline | Новый изолированный portable profile, те же MinGW/compiler/BDSP cache, exceptions/RTTI OFF |

Discovered paths (evidence, не инструкция hardcode):
CMake C:/Qt/Tools/CMake_64/bin/cmake.exe; Ninja C:/Qt/Tools/Ninja/ninja.exe;
GNU 13.1 C:/Qt/Tools/mingw1310_64/bin/{gcc,g++}.exe;
Qt6_DIR C:/Qt/6.10.2/mingw_64/lib/cmake/Qt6.
Existing BDSP source cmake-build-release/_deps/bdsp-src, HEAD
2c68056dfaeffba2f34fc64eee9bc9e331e83c7c (matches top-level pin).
Existing miniaudio and QtNodes sources also point to cmake-build-release/_deps.
Build GUI cache declares IDF C:/Users/Koba/CLionProjects/esp-idf, HEAD
fff9895c82d744c7237be8847347bdd1b07c6643; idf.py exists.
Existing Xtensa root C:/Users/Koba/.platformio/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf,
bin/xtensa-esp32-elf-g++.exe exists. Existing Python build-sdk-current-tools/python/Scripts/python.exe exists;
IDF_TOOLS cache build-sdk-current-tools/idf-tools. No IDF/ESP/Qt/CMake overrides were returned from environment.
No cross-build, install consumer or desktop DLL check performed in slice 0.

Current build boundary: root desktop C++23; portable targets C++20 via configure_portable_targets;
BUILD_PORTABLE_SDK early return avoids Qt/miniaudio. Prepared runtime currently links bdsp_protocol;
WLED device format boundary is a recorded later defect, not fixed.
Source SDK install_sources.cmake copies portable include/src and IDF component topology;
host archives are not firmware archives.

## Executed nonmutating baseline

Only artifacts under build-refactor-baseline were generated. No shared build was run.
Exact PowerShell cache discovery/configure recipe used:
```powershell
function Read-CacheValue($name) {
  ((Get-Content build-sdk/CMakeCache.txt |
    Where-Object { $_ -match ('^' + $name + ':') }) -split '=',2)[1]
}
$cmakeTool = Read-CacheValue 'CMAKE_COMMAND'
$compilerCpp = Read-CacheValue 'CMAKE_CXX_COMPILER'
$compilerC = Read-CacheValue 'CMAKE_C_COMPILER'
$ninjaTool = Read-CacheValue 'CMAKE_MAKE_PROGRAM'
$bdspSource = Read-CacheValue 'FETCHCONTENT_SOURCE_DIR_BDSP'
$env:PATH = (Split-Path $compilerCpp) + ';' + $env:PATH
& $cmakeTool -S . -B build-refactor-baseline -G Ninja '-DBUILD_PORTABLE_SDK=ON' '-DBUILD_GUI=OFF' '-DBUILD_TESTING=ON' '-DPORTABLE_DISABLE_EXCEPTIONS_RTTI=ON' '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_CXX_COMPILER=$compilerCpp" "-DCMAKE_C_COMPILER=$compilerC" "-DCMAKE_MAKE_PROGRAM=$ninjaTool" "-DFETCHCONTENT_SOURCE_DIR_BDSP=$bdspSource" '-DFETCHCONTENT_FULLY_DISCONNECTED=ON'
& $cmakeTool --build build-refactor-baseline --target signal_contracts_tests domain_contract_tests application_lifecycle_tests dataflow_runtime_tests bdsp_protocol_tests stream_fragment_tests embedded_sdk_tests node_routing_tests -j 4
$ctestTool = Join-Path (Split-Path $cmakeTool) 'ctest.exe'
& $ctestTool --test-dir build-refactor-baseline --output-on-failure -R '^(signal_contracts_tests|domain_contract_tests|application_lifecycle_tests|dataflow_runtime_tests|bdsp_protocol_tests|stream_fragment_tests|embedded_sdk_tests|node_routing_tests)$'
```

Configure exit 0 (GNU C/C++ 13.1.0), build exit 0 (69 steps), CTest exit 0:
**8/8 passed, 0.31 s**. Selected signal/domain/lifecycle/runtime/protocol/fragments/embedded/routing tests
are present-current-workspace author baseline. They do not validate all review defects, desktop delivery,
UI, storage, negotiated control, Protobuf/CBOR or hardware. Assertions remain enabled via -UNDEBUG.
No changes were made merely to make baseline pass.

## Later short targeted commands and gates

Run only after the assigned slice and build-owner coordination; derive cmake/ctest from cache as above.
Names below are existing targets/tests; new codec/NVS/revision tests must be registered by I after owners supply them.

| Scenario | Target / test |
| --- | --- |
| Domain/contracts/admission | domain_contract_tests, signal_contracts_tests, application_lifecycle_tests |
| Routing/history/no allocation | node_routing_tests, dataflow_runtime_tests; desktop graph_disconnect_continuity / graph_preparation_transaction |
| JSON/projection/resolver | graph_document_adapter_tests; config_document_resolver |
| BDSP goldens/bounded/fragmented | bdsp_protocol_tests, codec_buffer_tests, stream_fragment_tests, embedded_sdk_tests |
| Network resources/new types | network_graph_apply_start; registry_capabilities; typed_stream_transport; desktop_serial_stream |
| Specific editor routing | editor_tests --contracts-only (existing branch flag, currently not a separate registered CTest) |
| Specific network routing/WLED | stream_transport_tests --typed-routing-only (same caveat) |
| UI scalar/packet/revision | Future focused cases supplied by P; do not run entire UI profile by default |

Whole desktop application build, Windows DLL deployment, Qt-free installed consumer and existing ESP-IDF
cross-build are later mandatory integration gates. Isolated build directories must be assigned before those
commands; old cache presence is not evidence of current successful build. Final full automated tests are
authorized by the approved slice 5 plan, after integration. No native launch, hardware, Linux, soak,
audio playback, branch, commit or archive was performed.

## Risks/status

Initial dirty changes cannot be accepted by inheritance. Future versions/pins, codec byte schema,
request/revision semantics and exact component source manifests need owner contracts and independent review.
Actual embedded caps must be tested independently of desktop defaults. Existing FFT 4096 cache differs
from intended default 2048; do not silently rewrite it in slice 0.
Status: portable author baseline passed; gate 0 PASS по решению координатора;
срез 1 выдан R/S/C, I разрешён после готовности R; gate 1 пока не принят.

## Сверка соседних документов перед gate 0

node-runtime-contracts.md и storage-control-contracts.md прочитаны полностью после появления.
Semantic descriptors, bounded APIs, actual caps, silence/unavailable, history/identity,
codec/storage/projection и explicit NVS согласованы с общим планом. Pure WLED target/path
и sink-input callback детализируются владельцами до среза 2. Canonical schema_version=1
с прежними keys совместим с logical sections; это контрактный proposal, не разрешение rewrite.

Актуальное назначение: R — domain/signal/runtime public caps validation/lifecycle;
S — новый typed control; C — config/storage migration; I — все CMake/manifests.
Desktop delivery/embedded назначаются точно в срезах 2/4; прежние предложения не являются ownership.
Stale замечания о прежних таблицах удалены; gate 0 принят координатором после review.

Уточнение atomic control contract: typed Start/Stop reject сохраняет lifecycle state,
run intent, working graph, revision и ресурсы. Intent/state/revision commit только после
успешного runtime ответа; legacy compatibility отделена и проверяется characterization.
R владеет lifecycle invariants, S — typed command consistency и token orchestration.

Prepared storage: один GLOBAL slot max 1, token owned by session + base revision + graph identity/executor.
Foreign Prepare при занятом slot — Busy и сохраняет old token. Invalid candidate keeps old valid
preparation; успешная замена владельцем атомарна. TTL 30 s по host monotonic clock.
Foreign/expired/stale Apply отклоняется до commit; disconnect освобождает slot,
reconnect не передаёт token ownership. Смена revision делает token stale.
Per-session дополнительные prepared slots запрещены. R/S отражают эту единую модель
в своих документах; данный чат меняет только свои два docs.

Targeted contract tests: failed typed Start/Stop без intent/state/revision change;
GLOBAL max 1, Busy foreign, invalid keeps old, TTL 30 s, stale/foreign Apply,
unchanged working/resources; legacy characterization отдельно. Dependent code until gate запрещён.

## Срез 1 — integration evidence ledger (I)

Gate 0 PASS сообщён координатором; R ready освободил слот I. Максимум S/C/I соблюдён,
C затем сообщил author-ready. I не меняет R/S/C source/API; ошибки передаются через координатора.
Разрешённый extra entrypoint: apps/desktop_gui/src/core_application.cpp только default config path.
Default option больше не задаёт cwd literal, configuration::active_path(parser.value(config))
выбирает executable-relative путь при отсутствии CLI, explicit path сохраняет приоритет.

Integration wiring:
- application/CMakeLists.txt включает S processing_control.cpp в existing graph_application;
  public includes/domain link/C++20 и no-RTTI/no-exceptions profile сохраняются.
- tests/portable_foundation.cmake регистрирует processing_control_tests → graph_application,
  AUTOMOC OFF/C++20/assertions ON; R target registrations не изменены этой интеграцией.
- tests/CMakeLists.txt регистрирует config_migration_tests → config_document/QtCore,
  config_manager_migration_tests из того же source + CONFIGURATION_MANAGER_TESTS → desktop_gui_configuration.
- adapters/esp_idf/portable_sdk/sources.cmake включает тот же application/src/processing_control.cpp;
  install_sources.cmake уже копирует application include/src, нового vendor/codegen нет.

Build ownership: только I пишет build-refactor-integration; build-gui пока не используется.
Configure cache-derived from build-gui: compiler C/C++, Ninja, Qt6_DIR, existing BDSP/miniaudio/QtNodes,
QtADS source; Release, GUI/legacy/testing ON, FETCHCONTENT_FULLY_DISCONNECTED=ON,
DEPLOY_BUILD_RUNTIME=ON. Начальный configure exit 1: QtADS find_package(QT) не нашёл alias
по одному Qt6_DIR. Повтор с QT_DIR=Qt6_DIR exit 0; никаких installation/source patches для этого.
Logs: build-refactor-integration/configure-slice1.log, configure-slice1-final.log.

До финальной готовности S исполнялись только независимые C targets:
cmake --build build-refactor-integration --target config_migration_tests config_document_tests -j 4
exit 0; CTest regex ^(config_migration_tests|config_document_resolver)$ exit 0, 2/2 passed, 0.14 s.
Logs: build-config-standalone.log, tests-config-standalone.log в build-refactor-integration.
Qt/compiler runtime PATH для этих двух test processes выводился из cache, не прописывался в проекте.
Full desktop, DLL deployment, R/S/C integrated tests пока ожидают S ready; gate 1 не принят.
Дополнительно cache-derived configure build-refactor-integration/portable exit 0:
BUILD_PORTABLE_SDK=ON, GUI OFF, Release, exceptions/RTTI OFF, existing BDSP, C++20.
Log configure-portable-slice1.log. Этот профиль ещё не компилировался до S ready.

### После S FINAL READY / compile freeze

Все авторы idle, I единственный worker. Desktop command:
cmake --build build-refactor-integration --target core_app qt-application pipeline_cli
aggregator_server_app aggregator_client_app domain_contract_tests signal_contracts_tests
application_lifecycle_tests dataflow_runtime_tests processing_control_tests config_migration_tests
config_manager_migration_tests graph_document_adapter_tests config_document_tests core_smoke
console_config_tests graph_preparation_tests -j 4
**exit 0, 250 steps**; log build-refactor-integration/build-desktop-slice1.log.
Все пять applications присутствуют в bin. DEPLOY_BUILD_RUNTIME=ON выполнил windeployqt
и MinGW runtime copy. Наличие проверено: Qt6Core/Network/SerialPort/Charts/DataVisualization/Gui/
OpenGL/OpenGLWidgets/Svg/Widgets.dll, libqtadvanceddocking-qt6.dll, libgcc_s_seh-1.dll,
libstdc++-6.dll, libwinpthread-1.dll, platforms/qwindows.dll, D3Dcompiler_47/opengl32sw.dll.
Deployment warning dxcompiler.dll/dxil.dll not found сохранён в log; native D3D/OpenGL acceptance
не выполнялась. Palette deprecated warning из существующей темы не исправлялся этой интеграцией.

Portable command: cmake --build build-refactor-integration/portable -j 2 — exit 0, 86 steps.
CTest regex ^(signal_contracts_tests|domain_contract_tests|application_lifecycle_tests|
dataflow_runtime_tests|processing_control_tests)$ — exit 0, **5/5 passed, 0.14 s**.
cmake --install build-refactor-integration/portable --prefix build-refactor-integration/sdk-install
— exit 0. Logs build-portable-slice1.log, tests-portable-slice1.log, install-portable-slice1.log.
Installed application header processing_control.h и graph_application archive экспортируются existing package.

Installed external consumer CMake artifact: build-refactor-integration/consumer-src/CMakeLists.txt.
find_package(PortableDSP), SDK consumer existing tests/sdk_consumer/main.cpp и control consumer
tests/processing_control_tests.cpp against installed graph_application. Compile include path только
sdk-install/include, link installed archives; **C++20, -UNDEBUG, -fno-exceptions, -fno-rtti**, без Qt.
Configure/build/run **exit 0, 2/2 passed, 0.06 s**. Initial consumer configure exit 1 из-за
ошибочного relative test-source path в I build artifact; исправлен ../.., tracked source не менялся.
Logs configure-consumer-slice1.log (failure), configure-consumer-slice1-final.log,
build-consumer-slice1.log, tests-consumer-slice1.log; consumer-build/build.ninja содержит flags/includes.

Desktop CTest command:
ctest --test-dir build-refactor-integration --output-on-failure --output-junit slice1-targeted.xml
-R "^(signal_contracts_tests|domain_contract_tests|application_lifecycle_tests|dataflow_runtime_tests|processing_control_tests|config_migration_tests|config_manager_migration_tests|graph_document_adapter_tests|config_document_resolver|console_config_selection|graph_preparation_transaction|console_pipeline|silent_gui_pipeline)$"
— **exit 8, 12/13 passed, 2.67 s**. Logs tests-desktop-slice1.log, Testing/Temporary/LastTest.log,
slice1-targeted.xml. Не broad UI profile: один разрешённый silent synthetic GUI smoke и
console synthetic 300 ms; остальные short unit/config/preparation cases.

Failing silent_gui_pipeline: spectra=28, engine/editor compile=1, source=2,
backend=In-memory test generator, same_processing=0. Read-only diagnosis:
adapters/desktop/runtime/src/graph.cpp:241 compares _definition.edges vector directly;
desktop graph mapper preserves JSON connection order. Logged Engine/Editor each has 12 edges,
**SameSet=True, SameOrder=False**, proof smoke-edge-comparison.log. Это вероятная причина
equality regression; никаких чужих source/test edits или изменения fixtures выполнено не было.
Нужен назначенный через координатора владелец graph equality с characterization и focused rerun.
Gate 1 остаётся pending failure исправления и нового independent review.
Первые tool отправки координатору отклонялись ActiveTurnNotSteerable Compact;
после compaction компактный отчёт успешно передан. Назначение source owner ожидается.
Application/DLL inventories: application-inventory.log, runtime-dll-inventory.log.

### Narrow equality repair — integration complete

Координатор назначил C владельцем только graph.cpp same_processing_as/equality и
tests/graph_tests.cpp bounded --processing-equality-only. C author-ready:
full Edge multiset std::is_permutation с multiplicity, без allocation/source normalization;
meaningful source/port/node/graph/parameter differences не маскируются.
I не исправлял source и не ослаблял smoke.

Affected rebuild command:
cmake --build build-refactor-integration --target graph_tests core_smoke core_app
qt-application pipeline_cli aggregator_server_app aggregator_client_app -j 4
— **exit 0**, log build-equality-repair.log. graph.cpp/audio_pipeline пересобраны;
graph_tests/core_smoke и зависимые apps relink/deploy успешны, registry targets unaffected.
bin/graph_tests.exe --processing-equality-only — **exit 0**, log tests-equality-repair.log.
ctest --test-dir build-refactor-integration --output-on-failure
--output-junit slice1-smoke-repair.xml -R "^silent_gui_pipeline$"
— **exit 0, 1/1 PASS, 0.98 s**, log tests-smoke-repair.log.

Ранее 12 PASS cases сохранены, повторялся только затронутый equality case и прежний failing smoke.
Portable/installed SDK не зависят от desktop graph.cpp, поэтому их 5/5 и 2/2 evidence остаётся актуальным.
Суммарно все 13 выбранных desktop cases имеют PASS evidence (12 initial + 1 repaired),
дополнительно bounded equality PASS; это не единый повторный full suite и не independent acceptance.
I integration ready и idle освобождает слот JSON reviewer. Gate 1 не принят до независимых
review результатов/решения координатора. Срез 2 не начинался.

### Independent findings и bounded portable repair integration

Первый independent review среза 1 был FAIL по R/S/C. Прежние 13 desktop PASS —
author integration evidence до этих findings, не overall acceptance.
Координатор подтвердил independent R rereview PASS после strict capability/limits и
DiagnosticLocation repairs. Источники независимых доказательств:
build-gate1-domain-review/review2-tests.log (4/4, 0.06 s),
review2-acceptance.log (strict/legacy, fixed bounds, pending/working/intent,
static diagnostic lifetime/identity/allocation PASS), review2-original-repro.log
(strict FFTView rejection и unknown/known Mel limits rejection), review2-build.log и
review2-acceptance-build*.log. Это результаты reviewer, не присвоенные авторскому прогону I.
Overall gate 1 остаётся pending S/C; C Qt startup/migration ещё active.

По отдельному назначению I после R/S freeze выполнял только portable profile.
Desktop/Qt не компилировались, чужие source не исправлялись.
cmake --build build-refactor-integration/portable --target signal_contracts_tests
domain_contract_tests application_lifecycle_tests dataflow_runtime_tests processing_control_tests -j 4
— exit 0, 16 steps; log build-portable-review-repair.log.
ctest --test-dir build-refactor-integration/portable --output-on-failure
--output-junit review-repair-portable.xml
-R "^(signal_contracts_tests|domain_contract_tests|application_lifecycle_tests|dataflow_runtime_tests|processing_control_tests)$"
— **exit 0, 5/5 PASS, 0.10 s**; log tests-portable-review-repair.log.

Перед install после public header changes актуализированы portable exported libraries:
cmake --build build-refactor-integration/portable --target signal_contracts bdsp_protocol
graph_domain graph_application dataflow_runtime dsp_kernels embedded_support -j 4 — exit 0.
Только affected embedded_support dependent objects потребовали ещё 3 steps; прочие archives up-to-date.
Log build-sdk-review-repair.log. cmake --install build-refactor-integration/portable
--prefix build-refactor-integration/sdk-install — exit 0; install-portable-review-repair.log.
Installed domain/application/runtime headers, source SDK и архивы обновлены совместно.

Existing external consumers rebuild:
cmake --build build-refactor-integration/consumer-build --target sdk_consumer control_consumer -j 2
— exit 0, 4 steps; build-consumer-review-repair.log.
ctest --test-dir build-refactor-integration/consumer-build --output-on-failure
--output-junit review-repair-consumer.xml — **exit 0, 2/2 PASS, 0.04 s**;
tests-consumer-review-repair.log. C++20/no Qt/-fno-exceptions/-fno-rtti unchanged.
Control consumer повторно компилировал существующие behavioral tests через installed headers,
линковал installed graph_application/domain/signal archives; ABI/header refresh проверен.
Broad suites/native/hardware/new dependencies не запускались. I bounded portable ready / idle;
desktop repair integration ждёт C ready и отдельного назначения координатора.

### C F1–F3 freeze — fresh whole-dependent desktop integration

Координатор подтвердил independent R/S rereview PASS и C source freeze.
S independent sources: build-gate1-control-review/control-recheck.log,
expanded-recheck.log (TTL branches, precise diagnostic/replay/ports PASS),
original-recheck.log (expired resource released), lifecycle-recheck.log.
Overall gate 1 всё ещё не принят; I продолжил только выданную интеграцию.

I добавил реальные registrations tests/CMakeLists.txt:
core_configuration_startup → core_smoke --configuration-startup-only;
core_configuration_schema_recovery → core_smoke с QT_VISUALIZATION_SCHEMA_REGRESSION=1.
Оба offscreen/QT_FORCE_STDERR_LOGGING, timeout 15 s; existing Qt plugin-path setup применяется.
Первый case выполняет шесть isolated synthetic детей на temporary configs (five rejected + absent control);
второй — существующие строгие schema/explicit working recovery assertions, source не менялся I.
Existing core_startup_shutdown — first-run synthetic workspace case.

Fresh command:
cmake --build build-refactor-integration --target core_app qt-application pipeline_cli
aggregator_server_app aggregator_client_app core_smoke config_migration_tests
config_manager_migration_tests graph_document_adapter_tests config_document_tests graph_tests
domain_contract_tests signal_contracts_tests application_lifecycle_tests dataflow_runtime_tests
processing_control_tests -j 4
— **exit 0**, log build-desktop-review-repair.log. Normal Ninja header dependency rebuild
обновил domain/application/runtime/graph document/desktop pipeline/network/GUI presenter/config/
composition и dependent test/app binaries; смешанных вручную слинкованных old archives нет.
Whole five apps готовы, windeployqt/MinGW copy повторён для affected app targets.

Fresh CTest command:
ctest --test-dir build-refactor-integration --output-on-failure
--output-junit slice1-review-repair-desktop.xml
-R "^(signal_contracts_tests|domain_contract_tests|application_lifecycle_tests|dataflow_runtime_tests|processing_control_tests|config_migration_tests|config_manager_migration_tests|graph_document_adapter_tests|config_document_resolver|core_configuration_startup|core_configuration_schema_recovery|core_startup_shutdown|silent_gui_pipeline)$"
— **exit 8, 12/13 PASS, 11.59 s**. Logs tests-desktop-review-repair.log,
Testing/Temporary/LastTest.log, slice1-review-repair-desktop.xml.
R/S5 + config4 PASS; core_startup_shutdown first-run PASS 2.14 s,
core_configuration_startup six children PASS 7.32 s, silent_gui_pipeline PASS 1.10 s.
graph_tests --processing-equality-only current-headers binary дополнительно exit 0,
tests-equality-current-headers.log.

**Failure: core_configuration_schema_recovery SegFault 0.67 s.**
Command equivalent: QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1
QT_VISUALIZATION_SCHEMA_REGRESSION=1 bin/core_smoke.exe.
Log начинается с unsupported pipeline_graph/version, configuration not loaded, UI setup completed,
после чего access violation; root cause ещё не локализован. Fresh current-source failure подтверждён,
поэтому прежняя mixed-binary гипотеза не заменяет расследование и не даёт acceptance.
Через координатора передано C: schema_regression в tests/core_smoke.cpp / startup presenter lifetime
для narrow назначения; I не меняет чужой source, не ослабляет assertions и не повторяет crash без repair.
Userdata/config.json/output не затронуты, no playback/native graphics/hardware/commits.
I ready with finding / idle; ждёт owner freeze для affected rerun. Gate 1 pending crash repair + C review.

## ProposedSlice2 — inventory / handoff only, NOT STARTED

Gate 1 теперь принят; для ProposedSlice2 разрешено только согласование контрактов.
Ни реализация среза 2, ни его configure/build не разрешены этим разделом. Это предложение координатору для точной
выдачи трём workers после gate; I сохраняет единоличное владение всеми CMake/manifests/export/install.
Максимум три workers включая I: R2/D2/T2 могут работать после согласования API, I integration
начинается только после освобождения одного слота; reviewers отдельной фазой.
Основание: implementation-plan.md срез 2, node-runtime-contracts.md §§3–7,
storage-control-contracts.md identity/stream compatibility; до реализации каждый worker читает
AGENTS.md и закреплённый Clean Architecture / DDD, сохраняет stable IDs/wire/unknown fields.

| Worker / scope | Actual inventory и proposed source ownership | Tests (один владелец на файл) |
| --- | --- | --- |
| R2 Runtime | libs/dataflow_runtime/include/dataflow_runtime/runtime.h, src/runtime.cpp; libs/signal_contracts/include/signal_contracts/frame.h, buffer_pool.h и src/frame.cpp, buffer_pool.cpp. Per-output contracts/callbacks/unavailable, отдельный sink-input callback, Splitter/Switch/Gate, compatible state/clock/history и bounded packet retention. Domain include/src только descriptor/caps clarification по согласованному API, без переписывания принятых slice 1 rules | tests/node_routing_tests.cpp, dataflow_runtime_tests.cpp, signal_contracts_tests.cpp; domain_contract_tests.cpp только если меняется согласованный descriptor |
| D2 DSP/device codecs | Новый pure WLED package предлагается libs/wled_protocol/include/wled_protocol/wled_packet.h + src/wled_packet.cpp; exact path/API/target подтвердить до создания. Actual extraction source libs/bdsp_protocol/include/device_protocols/wled_packet.h — только D2. ScalarReduce numeric helper предлагается в existing libs/dsp_kernels/include/dsp_kernels/scalar_reduce.h + src/scalar_reduce.cpp (не новая reduction library); existing dsp.h/src/dsp.cpp только при доказанной необходимости API | Новые proposed tests/scalar_reduce_tests.cpp, wled_protocol_tests.cpp; перенос WLED сначала old byte goldens, finite/bounds/silence/counter characterization. D2 передаёт integration cases R2/T2, их test files сам не редактирует |
| T2 Desktop delivery | adapters/desktop/include/pipeline/{audio_pipeline,graph,engine_client,stream_codec,stream_transport,network_runtime}.h; adapters/desktop/runtime/src/{audio_pipeline,graph,engine_client,stream_codec}.cpp; adapters/desktop/platform/pipeline/src/{stream_transport,network_runtime}.cpp; presentation/ports/include/pipeline/{owned_frame,engine_models,engine_port}.h; adapters/desktop/include/wled/** и platform/wled/src/wled_audio_reactive_server.cpp. Node+port snapshots/previews/subscriptions/unavailable, History/count разделение, sinks delivery и packet destination/cadence/stale. Scalar/WLED registry membership: adapters/desktop/include/aggregator/{capabilities,packet_codec,utils}.h и platform/aggregator/src/{utils,aggregator_server,aggregator_client}.cpp; stream scalar/packet boundaries libs/bdsp_protocol/include/bdsp_protocol/stream_codec.h + src/stream_codec.cpp | tests/typed_graph_tests.cpp, graph_continuity_tests.cpp, graph_preparation_tests.cpp, desktop_history_demand_tests.cpp, engine_typed_views_tests.cpp, stream_transport_tests.cpp, capabilities_tests.cpp, registry_discovery_tests.cpp, codec_buffer_tests.cpp, bdsp_protocol_tests.cpp. Network runtime/serial/embedded cases только при затронутом data delivery и отдельной выдаче |

R2 владеет всем runtime.cpp: D2 не внедряет DSP helper вызовы прямо в этот файл.
D2 передаёт pure reduction/encoder APIs, R2 подключает их и выбирает latest History row/both stereo
channels, управляет prepared buffers. T2 единственный владелец desktop graph.cpp и audio_pipeline.cpp;
R2/D2 передают runtime API и legacy behavior, не правят desktop callback/equality.
Signal generic validator принадлежит R2: убрать device-byte interpretation по handoff D2,
сохранить opaque Packet geometry/finite validation; device packet decode/check принадлежит D2.
BDSP source принадлежит T2 только для typed data transport, не WLED wire semantics.
Embedded/IDF/NVS/controller, JSON migrations/config/presets, typed control service/codecs,
GUI editor/catalog/renderers/ScalarView/widgets/i18n, selective resource reconciliation —
read-only в этой выдаче; их реализация остаётся срезам 3/4 или отдельному назначению.
Никаких CBOR/nanopb/TinyCBOR fetch в срезе 2. Existing processing control/lifecycle rejects/caps
не менять ради routing; изменения shared application/domain APIs сначала согласует координатор.

### API seams до параллельного кода

1. R2/T2 фиксируют output envelope node+output_port+graph/revision/producer metadata и
   отдельный sink-input direction/port envelope; borrowed callback lifetime и bounded async owner.
   Snapshots/previews/subscribe/unavailable не используют node-only ключ для двух выходов Spectrogram.
   Explicit legacy wrappers сохраняют старые DTO/signals там, где их consumers ещё не перенесены.
2. D2/R2 фиксируют caller-buffer/no-allocation C++20 numerical API maxabs/signed mean/RMS и
   bounded pure WLED encode/decode/valid silence, errors/used size, packet ownership.
   Helpers не знают NodeKind/JSON/Qt/UDP; runtime выбирает population и semantic contracts.
3. D2/T2 фиксируют старые WLED AudioSync bytes и ownership send counter/cadence/stale 300 ms
   у destination adapter; pure encoder не становится сокетом. Packet preview decode read-only.
4. T2/R2 фиксируют stream key/port identity и supported types registry из общего descriptors/caps.
   Existing type IDs 0–6/payload/framing goldens сохраняются; Scalar/Packet нельзя выдавать за legacy.
   Конкретная negotiated subscription/payload identity extension согласуется до изменения wire,
   без скрытого введения slice 3 control protocol.
5. До R2/D2/T2 implementation координатор утверждает file assignments, signatures/limits,
   fixture handoff и temporary bridge removal conditions; unresolved seams не обходить dummy code.

### Temporary legacy bridges и remove owner

| Bridge | Владелец / условие завершения |
| --- | --- |
| Старый include device_protocols/wled_packet.h после pure extraction | D2 может оставить forwarding compatibility header на новый codec; R2/T2 переводят свои consumers, I wiring/export. D2 удаляет временный implementation dependency из BDSP только после conversion + old goldens + SDK consumer checks; публичный необходимый compatibility include не удалять ради структуры |
| Combined legacy WLED node → pure encoder + existing delivery adapter | R2 сохраняет runtime legacy input semantics; T2 сохраняет identity/destination/cadence/counter/stale bridge. T2 owns cleanup временных adapter internals после parity; сам сохранённый legacy WLED kind/UUID не удаляется и документы не переписываются автоматически |
| Node-only Qt snapshot/signals consumers | T2 owns explicit boundary adapter: port 0 historical stream без перезаписи History портом count, новые DTO keyed by port. T2 owns removal после consumer handoff в срезе 4 и совместимости; sink-input не объявляется фиктивным output 0 |

### I manifests после source-ready / отдельной выдачи

Actual files: root CMakeLists.txt; libs/{dataflow_runtime,dsp_kernels,signal_contracts,bdsp_protocol}/CMakeLists.txt;
новый libs/wled_protocol/CMakeLists.txt; adapters/desktop/platform/wled/CMakeLists.txt;
tests/portable_foundation.cmake и tests/CMakeLists.txt; cmake/portable_sdk.cmake;
adapters/esp_idf/portable_sdk/sources.cmake и adapters/esp_idf/install_sources.cmake.
I добавляет sources/tests/exports и same-source firmware package, снимает runtime→bdsp_protocol
dependency только если extraction действительно оставила runtime без protocol usage.
Общий install/no-Qt consumer и desktop DLL build — integration gate после freeze владельцев;
не author shortcuts, не shared concurrent Ninja. Сейчас только этот documentary inventory записан.
Status ProposedSlice2: CONTRACT AGREEMENT ALLOWED; implementation NOT STARTED, только после handoff координатора.

### C second repair freeze — fresh desktop lifecycle checks PASS

Координатор назначил новое I rebuild после C author freeze. C исправил (не I) normal close/save
перед teardown, persistent file-protection warning и absent-vs-null migration lookups.
I добавил только CMake registration core_configuration_shutdown →
core_smoke --configuration-shutdown-only; offscreen/Qtlogging, timeout 15 s.
Actual parent case проверяет три synthetic children с intentional unwind:
enabled final prefs/editor/workspace persisted; disabled и protected future bytes не записываются.
Startup/schema assertions источника не редактировались I.

Fresh build command:
cmake --build build-refactor-integration --target core_app qt-application pipeline_cli
aggregator_server_app aggregator_client_app core_smoke config_migration_tests
config_manager_migration_tests graph_document_adapter_tests config_document_tests -j 4
— **exit 0**; build-desktop-lifecycle-repair.log. Header dependencies заново пересобрали
graph_document/configmanager/GuiPresenter/GUI dependent objects/composition/core_smoke;
все five apps готовы (unaffected registry up-to-date), affected apps relink + Qt/MinGW deployment
успешны. No mixed direct-link archives, no shared build-gui.
Application/DLL metadata: application-inventory-lifecycle-repair.log,
runtime-dll-inventory-lifecycle-repair.log, включая platforms/qwindows.dll.

Fresh CTest command:
ctest --test-dir build-refactor-integration --output-on-failure
--output-junit slice1-lifecycle-repair.xml
-R "^(core_configuration_startup|core_configuration_schema_recovery|core_configuration_shutdown|core_startup_shutdown|silent_gui_pipeline|config_migration_tests|config_manager_migration_tests|config_document_resolver|graph_document_adapter_tests)$"
— **exit 0, 9/9 PASS, 12.20 s**; tests-desktop-lifecycle-repair.log,
Testing/Temporary/LastTest.log, slice1-lifecycle-repair.xml.
Startup six children PASS 6.38 s, shutdown three children PASS 2.01 s,
schema recovery PASS 0.59 s (no fresh SegFault), first-run PASS 2.00 s,
silent pipeline PASS 1.00 s; all four config cases PASS.
Schema case проверяет сохраняющееся file warning после transient clear/accepted working recovery
и unchanged protected file; no assertion waiver. Desktop tests offscreen/temp configs/synthetic,
без playback/native visual/hardware acceptance.

R/S headers/source неизменны с прошлого portable 5/5 и SDK consumers 2/2 PASS, потому они не
повторялись без причины; их bounded portable evidence остаётся актуальным.
I ready / idle, source fixes не делал. Independent C rereview идёт отдельным ownbuild;
Этот author report предшествовал принятому gate 1; итоговое решение и scope зафиксированы ниже.

## GATE 1 ACCEPTED — решение координатора, 2026-10-06

Координатор принял **GATE 1 PASS** после чтения actual independent R/S/C evidence и current I logs:
все три assigned independent направления PASS, no remaining assigned blockers.
Это принятие **только среза 1**: descriptors/roles/validation/caps, typed control contracts/service,
JSON migration/projection/storage и назначенные narrow equality/startup/recovery/shutdown repairs.
Начальный dirty draft вне assigned scope не объявляется принятым. Full goal, срезы 2–5,
native Windows visual/3D, ESP32/WLED hardware acceptance НЕ завершены.

| Evidence | Reference / результат |
| --- | --- |
| Independent R | ../../build-gate1-domain-review/review2-original-repro.log, review2-acceptance.log, review2-tests.log и review2-build*.log: strict actual caps/limits, unknown/known Mel admission, lifecycle/diagnostic invariants PASS; targeted 4/4 |
| Independent S | ../../build-gate1-control-review/expanded-recheck.log, original-recheck.log, control-recheck.log, lifecycle-recheck.log: TTL branches, session/token/revision, precise reply diagnostic/replay/atomic invariants PASS |
| Independent C | [acceptance.md](../../build-gate1-storage-review/acceptance.md): final assigned JSON/storage/projection/equality/default path/startup/recovery/shutdown PASS; supersedes old findings. Verified files: tests-final-acceptance.log, repro-final-acceptance.log, build-core-app-acceptance.log и source-hashes-final.txt; equality result recorded in acceptance.md |
| I fresh desktop | ../../build-refactor-integration/build-desktop-lifecycle-repair.log exit 0: все пять apps core_app/qt-application/pipeline_cli/aggregator_server_app/aggregator_client_app готовы, affected libs/apps rebuilt normally, DLL deployment successful |
| I DLL inventory | ../../build-refactor-integration/application-inventory-lifecycle-repair.log, runtime-dll-inventory-lifecycle-repair.log: Qt/ADS/MinGW runtime и qwindows plugin присутствуют |
| I current C/startup integration | ../../build-refactor-integration/tests-desktop-lifecycle-repair.log + slice1-lifecycle-repair.xml: **9/9 PASS, exit 0, 12.20 s**; startup/shutdown/schema/first-run/silent pipeline + config4 |
| I portable/current R/S | ../../build-refactor-integration/tests-portable-review-repair.log: **5/5 PASS, exit 0**; build-portable-review-repair.log и build-sdk-review-repair.log exit 0; C++20/no Qt/no RTTI/exceptions |
| I installed consumers | ../../build-refactor-integration/install-portable-review-repair.log exit 0; build-consumer-review-repair.log/tests-consumer-review-repair.log: **2/2 PASS, exit 0** against installed current SDK headers/archives |

Paths являются файлами evidence, не заменой самостоятельного reviewer исследования.
Native/platform warning dxcompiler/dxil из windeploy log остаётся вне offscreen/host acceptance scope.
Historical failing logs и промежуточные 12/13/13 PASS reports сохранены; актуальное acceptance
использует current frozen sources + final independent evidence, не наследует старый dirty draft.

ProposedSlice2: стадия согласования контрактов разрешена; dependent implementation/build
начинается только после конкретного handoff координатора. I остаётся единственным владельцем
всех CMake/manifests/export/install; максимум 3 одновременно работающих чата включая I,
coordinator не worker. Веток/коммитов/push/PR/ZIP не создавалось.
I DOC UPDATE COMPLETE / IDLE; полный goal не объявлен завершённым.


## Slice 2 — I D2 build wiring, 2026-10-06

Coordinator authorized this bounded implementation while R2/T2 remain active;
D2 sources are frozen. This supersedes the preceding contract-only I status,
without accepting Slice 2 or the complete goal.

I wired scalar_reduce.cpp into dsp_kernels and added standalone wled_protocol
(C++20, public headers, portable export/install and same-source ESP manifests).
The historical device_protocols header is forwarded through bdsp_protocol's
PUBLIC wled_protocol dependency; there is one packet implementation. Runtime
include/source rg audit found only wled_protocol, no BDSP/device_protocols usage,
so dataflow_runtime now links wled_protocol instead of bdsp_protocol. Desktop
WLED also links the codec. portable_stream_codec PUBLIC and qt_engine_ports
INTERFACE expose graph_domain for the final shared direction/kind DTO.
Desktop audio_pipeline registers T2 port_delivery_codec.cpp; that assigned
source was still absent at this check and no Qt configure/compile was attempted.

Own build only: build-refactor-integration/portable, Release,
BUILD_PORTABLE_SDK=ON, PORTABLE_DISABLE_EXCEPTIONS_RTTI=ON. Configure exit 0;
cmake --build --target scalar_reduce_tests wled_protocol_tests --parallel 4
exit 0 (8 steps, no R2 runtime or T2 protocol/desktop compilation).
CTest exact regex ^(scalar_reduce_tests|wled_protocol_tests)$: exit 0,
2/2 PASS, 0.04 s. Both helper tests compile with -UNDEBUG, -fno-exceptions,
-fno-rtti, C++20; pure codec test includes the historical shim without pulling
transport objects. MinGW bin PATH was derived from the existing compiler cache
for compilation and tests; initial compiler process failures had no diagnostics
and were resolved by that environment setup without source changes.

Evidence in build-refactor-integration: configure-slice2-d2.log,
build-slice2-d2.log, tests-slice2-d2.log, commands-slice2-d2.log,
source-hashes-slice2-d2.txt; JUnit portable/slice2-d2.xml.
No SDK install/consumer recheck yet: pending R2/T2 source freeze and coordinator
handoff. Author D2 goldens + I integration tests are not independent acceptance.
No source-owner fixes, tool installation, user config/output edits, branches,
commits, ZIP, native/UI/hardware/full/soak/Linux checks. I D2 WIRING READY / IDLE;
whole desktop build/deployment and affected integration remain assigned later.

### Slice 2 — R2 portable integration, bounded interim result

Coordinator subsequently froze R2 and authorized portable R2 integration.
Section 12 of node-runtime-contracts.md read. Exact build targets:
node_routing_tests signal_contracts_tests domain_contract_tests
application_lifecycle_tests dataflow_runtime_tests scalar_reduce_tests
wled_protocol_tests. Own portable configure exit 0, runtime/frame/tests compiled;
build exit 1 only at node_routing_tests link: its existing BDSP encode/decode
characterization had depended on the removed runtime→BDSP transitive link.
I fixed the test manifest with PRIVATE bdsp_protocol, preserving clean runtime
boundary. Final configure exit 0. No source-owner edits.

BDSP codec sources belong active T2, so node_routing rebuild/run and full SDK
install/consumers are held pending portable freeze confirmation; current archive
is not treated as current-source evidence. Six already built cases exact CTest
regex (signal_contracts_tests|domain_contract_tests|application_lifecycle_tests|
dataflow_runtime_tests|scalar_reduce_tests|wled_protocol_tests) all PASS,
exit 0, 6/6, 0.11 s. Libraries retain C++20/no Qt/no RTTI/exceptions; runtime
harness may use exceptions for its allocation guard, as documented by R2.
Evidence: build-refactor-integration/configure-slice2-r2.log,
configure-slice2-r2-final-wiring.log, build-slice2-r2.log (preserved link failure),
tests-slice2-r2-partial.log and portable/slice2-r2-partial.xml.
No acceptance claimed for node_routing/current installed SDK/desktop yet.
I MANIFESTS FROZEN / HOLDING DEPENDENT BUILDS; coordinator informed.

### Slice 2 — portable build/install/consumers complete after T2 partial freeze

Coordinator confirmed T2 partial freeze for bdsp_protocol stream_codec.h/.cpp
with R2 frame dependencies frozen, and authorized dependent portable checks.
T2 desktop remains active; no Qt configure/compile or UI execution.

Own cache-derived CMake/MinGW/Ninja environment, Release portable SDK:
cmake --build build-refactor-integration/portable --target node_routing_tests
bdsp_protocol_tests stream_fragment_tests signal_contracts_tests domain_contract_tests
application_lifecycle_tests dataflow_runtime_tests scalar_reduce_tests
wled_protocol_tests --parallel 4: exit 0 (10 rebuild/link steps).
CTest exact nine-target regex: exit 0, 9/9 PASS, 0.16 s, assertions enabled.
This supersedes the interim node_routing link failure/hold; correct PRIVATE
BDSP test dependency is retained and runtime export has no BDSP dependency.

Built all SDK library targets explicitly (signal_contracts bdsp_protocol
wled_protocol graph_domain graph_application dataflow_runtime embedded_support
dsp_kernels), exit 0; embedded_support rebuilt against current frozen interfaces.
cmake --install portable --prefix build-refactor-integration/sdk-install exit 0.
Installed export includes PortableDSP::wled_protocol; bdsp_protocol transitive
codec dependency makes old forwarding header usable; runtime dependencies are
only graph_application, dsp_kernels, wled_protocol. Installed ESP source-package
hash comparison: all 9 checked helper/runtime/BDSP header/source/manifest files
MATCH working files. This is package consistency, not ESP crosscompile/hardware.

Own external find_package consumer project extended in build-refactor-integration
only, reusing existing tests: sdk_consumer, control_consumer, runtime_consumer
(node_routing), scalar_consumer, wled_consumer (links installed bdsp_protocol
and exercises old forwarding header transitively). Configure/build exit 0;
CTest 5/5 PASS, exit 0, 0.10 s. All consumers C++20 -UNDEBUG -fno-exceptions
-fno-rtti, include and link installed headers/archives, not source include dirs.

Evidence under build-refactor-integration: build-slice2-portable.log,
tests-slice2-portable.log + portable/slice2-portable.xml, build-sdk-slice2.log,
install-sdk-slice2.log, configure-consumer-slice2.log, build-consumer-slice2.log,
tests-consumer-slice2.log + consumer-build/slice2-consumers.xml,
commands-consumer-slice2.log, installed-source-hashes-slice2.txt.
Current integration is author evidence; independent R/D review and Slice 2 gate
remain pending. No foreign source patches/tool installs/user config or output
writes/branches/commits/ZIP/full/native/hardware/Linux checks. I PORTABLE READY / IDLE;
whole desktop/DLL and affected integration await T2 freeze and coordinator handoff.

### Slice 2 — partial independent D2 PASS, 2026-10-06

Coordinator received FINAL PASS from independent D2 reviewer chat
01a112e6-fce8-7fa0-94d2-d2439609b6ca and assigned this ledger update.
I read actual review-report.txt, review-final.log, source-hashes.txt and
run-review.ps1 under build-gate2-device-review. Scope is only seven frozen D2
numerical/codec/helper-test files: scalar_reduce header/source, wled_packet
header/source, historical device_protocols forwarding header, and two helper tests.
Reviewer reports no actionable P0/P1/P2/P3 finding and no product fix request.

Independent evidence: review-final.log records all five compile/run pairs exit 0;
run-review.ps1 contains the five exact direct compiler cases scalar_reduce_tests,
wled_protocol_tests, independent, numeric_extreme, limits_numeric with C++20,
-fno-exceptions -fno-rtti -Wall -Wextra -Werror. source-hashes.txt identifies
all seven frozen files. Reviewer independently reproduced six literal HEAD
packet goldens plus 2000 parity cases, foreign finite/raw peak preservation,
atomic malformed/limit failures, overlap/suffix safety, no-allocation loops,
FLT_MAX RMS populations and scalar double-reference/large indexing limits.
Own reviewer harness typo was fixed only in its artifacts; no product defect.

This is PARTIAL INDEPENDENT D2 PASS, not runtime population/frame acceptance,
T2 destination counter/cadence/stale/Stop/ownership acceptance, desktop/SDK/ESP
or hardware/native graphics acceptance. I portable 9/9 + installed consumers
5/5 remain separate actual integration evidence. GATE 2 NOT ACCEPTED.
Independent R2 review chat 01a112ea-e77b-7043-bdfa-e523b72a76bd is active;
T2 implementation is active. No additional builds/source edits/commits were
performed for this doc-only update. I DOC UPDATE COMPLETE / IDLE.

### Slice 2 — independent R2 FAIL / owner repair required, 2026-10-06

Coordinator assigned a doc-only update after independent reviewer
01a112ea-e77b-7043-bdfa-e523b72a76bd reported FAIL. I read actual
build-gate2-runtime-review/review-report.md. Two actionable runtime findings:

| Finding | Reproduction / implication | Owner / required gate |
| --- | --- | --- |
| F1 P1: consecutive closed History Gate/Switch resurrect rows | Gate disabled or Switch output inactive after rows 1/2: tick 4 delivers [1,2,3,0], expected [1,2,0,0]. Latest-only borrowed override permits a previously silenced upstream row to reappear. Runtime-delivered canonical sink Input is affected, independently of T2 caches. | R2: bounded prepared per-output history state, no full-history copy or hot-path allocation; consecutive closed ticks, ring wrap, reopen, canonical callback/snapshot, read-only source and allocation regressions; repeated independent reproduction required. |
| F2 P2: unused routing output removal resets surviving histories | Splitter outputs3→2 with unused port2 removed and surviving edges/contracts/clocks unchanged resets both downstream histories from 2 rows to 0; reset_nodes={1,2,3}. | R2: endpoint-specific compatibility/reset assessment; shrink/expand, used-output removal, unaffected siblings, detached typed-zero and analogous Switch regressions; repeated independent reproduction required. |

Source locations recorded by reviewer: runtime.cpp:654/688 and 952/974 for F1;
104/345–350/378 for F2. Reviewer own build/test 7/7 PASS did not override the
independent repro: run-repro.ps1 compiled exit 0 with C++20/assertions/no RTTI/
exceptions against fresh own archives; independent_repro.exe exit 1, four failed
expectations for these findings. Evidence: review-report.md, independent_repro.cpp,
run-repro.ps1, repro-build.log, repro.log, source-hashes.txt in
build-gate2-runtime-review. No T2 transient-source acceptance claimed.

R2 author source freeze is released for assigned repairs; T2 implementation remains
active. D2 partial independent PASS is unchanged. I portable 9/9 and installed SDK
consumers 5/5 are historical integration evidence for the pre-fix snapshot, not R2
acceptance or proof against F1/F2. After R2 READY/FROZEN handoff, I will perform only
coordinator-assigned rebuild/reinstall/affected tests, followed by independent R2
re-review. GATE 2 NOT ACCEPTED; no next-slice/future implementation authorized here.
This update performed no builds or product source fixes/commits. I DOC UPDATE COMPLETE / IDLE.

### Slice 2 — aggregator public descriptor dependency

Coordinator assigned minimal CMake wiring while T2 desktop remains unfrozen.
Actual adapters/desktop/include/aggregator/capabilities.h publicly includes
signal_contracts/frame.h and calls signal_contracts::frame_descriptors() in its
inline capabilities construction. Actual aggregator-library target lives in
adapters/desktop/platform/aggregator/CMakeLists.txt. I added PUBLIC
signal_contracts so both header include requirements and descriptor linkage
propagate to consumers. Scoped diff --check passed (line-ending warning only).
No configure/build or source edits; T2 continues tests 18–20/registry/WLED and
R2 continues F1/F2 repairs. Desktop build awaits freeze/handoff; GATE 2 NOT ACCEPTED.
I DEPENDENCY UPDATE COMPLETE / IDLE; no commits.

## ProposedSlice3 — ownership / handoff proposal only, NOT STARTED

2026-10-06. Coordinator authorized only this documentary dispatch preparation.
GATE 1 accepted; GATE 2 NOT ACCEPTED. R2 F1/F2 repairs and T2 delivery/tests are
active; D2 independent PASS is partial. No Slice 3 source assignment, implementation,
fetch/download/install/codegen/build/new chat or commit follows from this proposal.
Before code each assigned chat reads AGENTS.md and the full pinned reference:
https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md
Pass that link/read requirement in implementation and architecture-review dispatch.
Actual ownership must be confirmed by coordinator after Gate 2 acceptance/freeze.

### Existing seams and gate dependencies

* Accepted Slice 1: application/include/application/processing_control.h and
  src/processing_control.cpp provide ProcessingDTO, sessions/epochs, limits,
  commands/replies, capabilities and runtime/persistence ports. GraphLifecycle
  remains the single executor lifecycle; no transport-specific second controller.
* Accepted Slice 1 JSON boundary: adapters/desktop/include/desktop/graph_document.h
  and src/graph_document.cpp provide read_processing_graph_document,
  project_processing_graph, processing_graph_document and restore_graph_layout.
  config_document/config_migration and ConfigManager preserve one project document.
* Slice 2 pending Gate 2: canonical node+direction+port identities, shared kind
  descriptors, DeliveryStamp/producer identities, bounded owned delivery and
  BDSP data packet-byte decode. T2 negotiated DATA18/19/20 must be frozen/accepted
  before control registry IDs and transport bindings are specified. No reuse of
  those IDs or historical 5..17 without complete registry review/approval.
* Desktop seams after Gate 2: EnginePort/engine models, engine_client,
  audio_pipeline server handling, stream_transport and NetworkRuntime. Existing
  legacy Qt wire remains a compatibility adapter to the same application service.
  Resource selective reconciliation was explicitly deferred from Slice 2.
* Storage projection consumes configured descriptor field classification;
  display settings stay local in both command and state/reply directions.
  Pending accepted layout is transport request state, not saved runtime history.

### Proposed nonoverlapping source/test ownership

All paths below are exact proposed assignments, not current permission to write.
New paths are proposals requiring coordinator confirmation before creation.
No two workers own the same source/test file. Every CMake/manifests/vendor pin/
export/install/codegen runner exception belongs I irrespective of containing root.

| Direction / proposed owner | Existing files / new zones | Sole-owned targeted tests / handoff |
| --- | --- | --- |
| Control codecs / K3 | New libs/bdsp_protocol/include/bdsp_protocol/{control_message,control_codec,control_limits}.h; new src/{control_message,control_cbor_codec,control_protobuf_codec}.cpp; new schema/control.proto + control.options; new generated/control.pb.h/.c. Wire DTO and bounded caller-owned decode arena, no Qt/OS/application executor ownership. Existing stream_codec/byte_stream/stream_fragments are read-only unless coordinator explicitly assigns a framing delta. | New tests/control_cbor_codec_tests.cpp, control_protobuf_codec_tests.cpp, control_codec_golden_tests.cpp. Supplies exact wire schema/API, limits/status/atomic failure semantics, generated sources and golden bytes. Does not edit desktop transport or application service. |
| Desktop control/network / T3 | Existing adapters/desktop/include/pipeline/{engine_client,audio_pipeline,stream_transport,network_runtime,serial_config}.h; runtime/src/{engine_client,audio_pipeline}.cpp; platform/pipeline/src/{stream_transport,network_runtime}.cpp. Existing presentation/ports/include/pipeline/{engine_port,engine_models}.h; apps/desktop_core/main.cpp. Registry metadata files adapters/desktop/include/aggregator/{capabilities,packet_codec,utils}.h and platform/aggregator/src/{utils,aggregator_server,aggregator_client}.cpp only for negotiated control features/binding metadata. New std-only adapters/control/include/control/processing_message_mapper.h + src/processing_message_mapper.cpp for wire↔application mapping; new desktop include/control/{control_session,legacy_control_bridge}.h + runtime/src/{control_session,legacy_control_bridge}.cpp. | Existing tests/network_runtime_tests.cpp, stream_transport_tests.cpp, capabilities_tests.cpp, registry_discovery_tests.cpp, engine_typed_views_tests.cpp; new tests/desktop_control_session_tests.cpp, legacy_control_bridge_tests.cpp, resource_reconciliation_tests.cpp. Owns negotiated TCP/UART service/client, legacy byte characterization, pending layout map keyed device/epoch/request, selective resource changes. No JSON persistence/projection implementation, DSP/data-runtime or WLED codec edits. |
| Project storage / P3 | Existing adapters/desktop/include/config/{config_document,config_migration}.h; include/desktop/graph_document.h; src/graph_document.cpp; gui_config/include/core_app/config_manager.h + src/config_manager.cpp. Existing apps/desktop_gui/include/core_app/{core_application,desktop_engine_provider}.h + src/core_application.cpp only projection/display-settings/one-document composition. New include/desktop/project_display_settings.h + src/project_display_settings.cpp for local settings classification/merge without widget internals. | Existing tests/config_document_tests.cpp, config_migration_tests.cpp (including ConfigManager variant), graph_document_adapter_tests.cpp, console_config_tests.cpp; new tests/project_display_settings_tests.cpp, project_atomic_save_tests.cpp. Owns one JSON preservation/migration/atomic save and typed processing projection in both directions. No remote session state, transport resource reconciliation or editor/rendering feature work. |
| Integration / I | All **/CMakeLists.txt, tests/*.cmake, cmake/**, adapters/esp_idf source/install manifests, SDK export/install. Proposed libs/bdsp_protocol/vendor/{tinycbor,nanopb}/ dependency source inventory/license/pin manifests and tools/control_codegen/** runner/tool-pin manifest are I-owned. K3 owns schema/options/generated payload files, I coordinates reproducible generation without simultaneous writes. | Exact target registration, dependencies, immutable upstream commit/hash pins, generated source build wiring, installed external consumers, whole desktop/DLL and narrow integration after owner freeze. No worker source fixes. |

application/processing_control and graph_lifecycle, domain descriptors/field classification,
R2 signal/runtime, D2 helpers and T2 data delivery bodies are read-only baselines.
If a semantic contract change is needed, coordinator assigns one owner and exact
API before writes; other workers consume it through handoff. T3 alone writes
engine_client/EnginePort/audio_pipeline; P3 supplies projection/settings functions
and T3 integrates calls. P3 alone writes graph_document/ConfigManager; T3 passes
typed accepted replies/request identity, never saves processing-only JSON.
Qt editor/catalog/renderers/i18n, firmware/ESP-IDF/NVS/provisioning remain later
scope; a concrete necessary caller change requires separate exact assignment.

### Contract handoffs required before dependent implementation

1. K3↔T3: exact WireControlMessage schema, stable command/status IDs, codec/version
   negotiation, session/epoch/request/revision/token fields and limits. CBOR default
   common codec; Protobuf usable fallback; no common version gives explicit refusal
   before mutation. uint64/finite/optional/default/error semantics agree. Caller
   buffers/arena, consumed/produced sizes, depth/string/node/edge/extension budgets,
   duplicate/truncated/trailing checks and unchanged output on error are explicit.
   DTO contains no layout/history/GUI state. Boundary validation precedes mapping
   into accepted ProcessingControlService; application still checks meaning.
2. I↔K3: requested TinyCBOR 7.0 and nanopb 0.4.9.2 require verified immutable upstream
   commit and source hashes (no invented pin or new version choice). Pin sources,
   licenses and generator/tools; checked-in proto/options/generated C sources;
   documented separate generation command reproduces output bytes. Ordinary
   SDK/desktop build/install never invokes Python/protoc/generation or downloads.
   Pin resolution/fetch/generation starts only with explicit Slice 3 handoff.
3. T3↔application ports: bounded serialized polling outside ISR/DSP callback;
   TCP/UART same BDSP framing/control codec, incremental assembly, partial writes,
   timeout/cancel/disconnect limits. Legacy Qt bytes characterized first and mapped
   to identical typed commands/service. Reconnect reads state/caps/epoch before
   mutation; no stale token/request replay or implicit streaming-control activation.
4. T3↔P3: typed ProcessingDTO is processing-only both directions; local display
   keys cannot sneak through opaque extensions. P3 owns descriptor-based projection
   and preservation, T3 owns pending {device_identity, epoch, request_id} layout
   association. Accepted reply alone attaches matching layout to accepted graph
   revision; reject/timeout/disconnect/out-of-order/reused node identity/new graph
   cannot attach last submitted layout. Local draft remains preserved.
5. T3 reconciliation: compare stable endpoint/binding identity and resource-affecting
   transport settings; stage acquisition then commit. Gain/routing/display edits
   preserve unaffected TCP/UART bindings. Removed/incompatible paths change only
   affected resources; rejected acquisition leaves working bindings. Explicit Stop
   and device change are separate; post-commit failure identifies the resource.
6. P3 storage: one active absolute JSON path and atomic save; processing/presentation/
   app/presets/draft/applied are logical sections of that document. Preserve unknown
   fields/stable IDs/explicit overrides; reject malformed/future versions before
   apply/overwrite; up/down migration errors explicit. Runtime/history not serialized.
   Display-only changes do not prepare, bump processing revision, reconnect or persist
   implicitly. No test replacement of user config.json/output.

### Dispatch sequence / acceptance proposal

Gate 2 acceptance and stable source/API handoff are prerequisites; this inventory
creates no worker chats. Coordinator confirms K3/T3/P3 file lists and names per
phase. At most three active worker chats including I (coordinator excluded):
contract phase may run K3 + P3 + I; dependent desktop phase T3 + P3 + I only after
K3 freezes/idles. Any K3 repair/review work displaces another active worker, not
adds a fourth slot. Independent reviewers follow the same slot budget.

Start new behavior with focused failing tests; capture legacy control bytes first.
K3: CBOR/Protobuf semantic/golden parity, bounded malformed/overflow/truncation/
duplicate/unknown mandatory/trailing, atomic short-buffer failure, no hidden heap.
T3: negotiated/no-common/old-peer/fragmented TCP/UART, two clients/stale revision/
conflict/reconnect, bounded queues, selective unchanged bindings/failure rollback,
legacy bytes and request-specific accepted/rejected/timeout/out-of-order layout.
P3: projection excludes presentation in both directions, one atomic save failure,
unknown fields/old settings/overrides/draft/active path preserved. I after source
freeze builds app + installed SDK consumers and runs short affected tests only.
Actual independent source/repro review follows, not acceptance by author greens.
No full/UI/native/FPS/soak/Linux/hardware acceptance inferred from these checks.
Status: PROPOSED DISPATCH DOCUMENT READY / NOT STARTED; I DOC COMPLETE / IDLE.

### Slice 2 — R2 authorfix frozen/current I integration, 2026-10-06

Coordinator confirmed R2 F1/F2 AUTHORFIX SOURCE FROZEN and assigned repeat own
portable/installed consumers only. Read node-runtime-contracts.md section 13 and
build-refactor-runtime-slice2/source-hashes-authorfix.txt. Additive HistoryView
zero-row masks and apply_history_route query are R2/T2 API; I changed no R/D/T
source or consumer fixture. Independent R2 repeated acceptance runs separately.

Own portable Release C++20/no Qt/no RTTI/exceptions libraries; assertions-enabled
exact targets node_routing_tests bdsp_protocol_tests stream_fragment_tests
signal_contracts_tests domain_contract_tests application_lifecycle_tests
dataflow_runtime_tests scalar_reduce_tests wled_protocol_tests: build exit 0,
29 steps; CTest exact regex exit 0, 9/9 PASS, 0.22 s. Existing dataflow harness
exception use remains separate from no-exception runtime library compilation.
All SDK libraries explicitly built; embedded_support current-interface rebuild
exit 0. SDK install exit 0. Five external installed consumers sdk/control/runtime/
scalar/wled rebuilt (7 steps), exit 0; CTest 5/5 PASS, exit 0, 0.17 s. Consumers
compile C++20 -UNDEBUG -fno-exceptions -fno-rtti against installed includes/archives.
Runtime consumer reuses current node_routing regressions, including authorfix.

All six authorfix source/test files match published frozen hashes. All four
runtime/signal header/source files in installed ESP source package match those
hashes, and both installed public headers match. Evidence under
build-refactor-integration: build-slice2-authorfix.log, tests-slice2-authorfix.log,
portable/slice2-authorfix.xml, build-sdk-authorfix.log, install-sdk-authorfix.log,
build-consumer-authorfix.log, tests-consumer-authorfix.log,
consumer-build/authorfix-consumers.xml, installed-source-hashes-authorfix.txt.
These are current author integration checks, not independent F1/F2 acceptance.
Earlier 9/9+5/5 logs remain historical pre-fix evidence; this section is current.

ProposedSlice3 dispatch documentary map is ready/NOT STARTED; no implementation
or pin/download work authorized before Gate 2. T2 full desktop freeze still absent:
no desktop build/Qt/UI execution. GATE 2 NOT ACCEPTED; independent repeated R2
verdict and T2 freeze/review/integration remain pending. No source changes, tool
installs, user config/output writes, commits/branches/ZIP/full/Linux/hardware checks.
I CURRENT PORTABLE READY / IDLE.

### Slice 2 — partial independent R2 acceptance / F1/F2 CLOSED

Coordinator read and accepted bounded independent R2 FINAL PASS; I read actual
build-gate2-runtime-review/rereview-report.md. Six frozen authorfix hashes match
source-hashes-rereview.txt. Original unchanged independent repro compile/run 0;
Gate/Switch [1,2,0,0], surviving histories2/2→3/3 with no resets. Separate fixed-size
24-tick local/external logical-row oracle covers wrap/reopen/prepare/independent
output masks/producer changes/stale query refusal/retain/budgets with failures0,
allocations0 (run-extended.ps1, extended-build.log, extended.log). Affected own
review CTest7/7 PASS0.22s (rereview-build.log/rereview-ctest.log). No new finding.
This supersedes R2 FAIL for the reviewed F1/F2 repair, not T2 acceptance.

T2 retains explicit obligation: fence current LOCAL delivery.epoch/applied revision
and graph/endpoint/cache before serialized apply_history_route; keep owning_source
and RuntimeContext alive until borrowed row() reading/retention. Producer metadata
must not substitute for local accepted revision. Reviewer did not assert changing
T2 caller code fulfills this. D2 and R2 are partial accepted directions only;
GATE 2 NOT ACCEPTED, desktop/T2 integration and independent review remain required.

### Slice 2 — desktop build GREEN; tests STOPPED at T2 startup failure

T2 source freeze and R2 freeze allowed own I desktop configure/build/deploy.
Port8213 checked free before tests; no process killed. Registered five focused
CTest cases and enabled assertions for nine affected targets. Configure exit0;
missing optional Vulkan header warning does not prevent build.
Initial build failed codec_buffer_tests link because new port_delivery_codec.cpp
was attached only to audio_pipeline. I CMake repair moved its single implementation
to portable_stream_codec (desktop and no-Qt test fallback), no source-owner edits.
Audit includes: std/domain/signal/BDSP only, no Qt/OS. PUBLIC graph_domain/bdsp deps
suffice. New wire source fallback compiled C++20 -UNDEBUG -fno-exceptions -fno-rtti;
legacy stream_codec wrapper and allocation test guard intentionally throw and are
not claimed as no-exception SDK sources. SDK library targets remain strict.
Portable codec_buffer_tests after wiring repair:1/1 PASS0.05s, exit0.

Repeated desktop build exit0: all five apps core_app, qt-application, pipeline_cli,
aggregator_server_app, aggregator_client_app plus typed_graph_tests,
engine_typed_views_tests, registry_discovery_tests, desktop_history_demand_tests,
stream_transport_tests, capabilities_tests, codec_buffer_tests,
graph_continuity_tests, graph_preparation_tests. DLL deployment completed;
dxcompiler.dll/dxil.dll warning remains unverified native-render scope.
EXE/DLL absolute directory: C:/Users/Koba/CLionProjects/qt-visualization/build-refactor-integration/bin;
platform plugin: that directory/platforms/qwindows.dll. Inventory artifacts list
individual absolute EXE/DLL paths, lengths and EXE timestamps.

CTest exact13 requested+legacy cases --stop-on-failure: exit8, only4 attempted.
PASS codec_buffer_tests0.02s, typed_stream_transport16.79s, registry_capabilities0.34s.
FAIL engine_typed_views0.05s: "Default graph preparation failed", thrown at
adapters/desktop/runtime/src/audio_pipeline.cpp:148. Remaining9 canonical/history/
continuity/preparation cases NOT RUN, no full13 PASS claimed. Coordinator routed
source failure to T2 and narrowly released its freeze for authorfix. Shared
builds/tests HELD until T2 READY/FROZEN; no source fix by I.

Evidence under build-refactor-integration: configure-desktop-slice2.log,
build-desktop-slice2-initial-link-failure.log, build-desktop-slice2.log (GREEN),
build-codec-wiring-repair.log, tests-codec-wiring-repair.log,
portable/codec-wiring-repair.xml, tests-desktop-slice2.log (STOPPED),
slice2-desktop.xml, Testing/Temporary/LastTest.log, application-inventory-slice2.txt,
dll-inventory-slice2.txt, plugin-inventory-slice2.txt, commands-desktop-slice2.log,
commands-codec-wiring-repair.log, source-hashes-desktop-slice2.txt (39-file capture
baseline preceding T2 narrow authorfix; not a later frozen-source assertion).
No userconfig/output/speaker/nativegraphics/hardware/longload/installtools/commits.
Offscreen is not native acceptance. ProposedSlice3 map ready/NOT STARTED.
I INVENTORY/DOC COMPLETE / IDLE; T2 repair/re-freeze/integration GREEN needed
before a new independent T2 reviewer starts; GATE 2 NOT ACCEPTED.

### Slice 2 — Qt slots macro integration defect / narrow header repair hold

2026-10-07, Europe/Moscow. Coordinator identified exact startup cause: Qt's
slots macro expands the public BufferPool constructor parameter named slots
and initializer slots_(slots), producing slots_() and an empty slot span in a
Qt translation unit. The first unavailable marker retain returns exhausted5.
I verified the current public buffer_pool.h parameter/initializer and T2's new
engine_typed_views_tests --default-preparation-only branch by read-only inspection.
The focused CTest registration is pending the next R/T frozen handoff; do not run
it before then. No configure/build/tests or R/T source edits in this update.

R owns narrowly assigned signal_contracts buffer_pool public-header macro repair;
runtime algorithm changes are not assigned. T owns default-preparation fixture
and diagnostic changes. Both narrow zones are unfrozen; shared builds/testholds
remain in effect. Prior bounded independent R2 algorithm/F1/F2 PASS remains valid
for its reviewed scope, but does not accept this new public-header integration fix.
New header repair requires narrow independent repeated review with Qt macro
integration and fresh desktop rebuild of all header dependents after R/T freeze.
I checks cannot substitute for that review. Gate 2 remains NOT ACCEPTED.
Old build/source-hash baseline predates this repair; recapture after fresh build.
I DOC UPDATE COMPLETE / IDLE; max active R/T/I, no commits/source fixes.

### Slice 2 — Qt macro narrow independent PASS / fresh I integration hold

2026-10-07. Coordinator accepted bounded independent R Qt-macro PASS; I read
build-gate2-runtime-review/slots-narrow-report.md. Old/current headers tested with/
without empty slots macro: old+macro compile0/run1 RED, other three compile0/run0;
macro unchanged, caller slots/refcounts/exhaustion/atomic failure ownership verified.
Own reviewer signal test1/1 PASS0.04s. Source hashes match two-file narrow freeze.
API parameter types/order/noexcept/layout unchanged; no Qt dependency or production
macro workaround. Prior R2 algorithm/F1F2 acceptance remains current for repaired
public header. This is not T2/default startup/Gate2 acceptance.

I consumed R §14/T §12 refreeze; registered engine_default_preparation case with
assertions enabled. Fresh desktop target rebuild recompiled header consumers
runtime/buffer_pool/audio_pipeline/engine_client/Qt presentation/apps/tests.
Initial archive replacement failed at libdataflow_runtime.a (file cannot be removed).
Old process finished exit1; process inventory found no live CMake/Ninja/ar/compiler
or competing same-directory build. One exact retry completed exit0, all five apps
and nine requested test binaries deployed. Failure preserved separately; no stale
archive copied or entire build deleted. Retried deployment dxcompiler/dxil warning
remains native-render scope. Bin directory unchanged; refreshed inventory below.

Before engine run, T narrowly refroze test-only default WLEDOutput ID10→13 correction;
I rebuilt engine test source (3 steps) from current file. Product graph/schema/limits
unchanged. Focused engine_default_preparation PASS1/1, exit0,0.08s. NEXT normal
engine_typed_views FAIL exit8,2.30s: "remote stopped view did not clear" after
NetworkInput Stopped/Running transitions. Remaining desktop14 suite not launched;
no full14 PASS implied. Concrete failure routed coordinator/T2; shared desktop
checks held pending source repair/refreeze, no foreign patch by I.

Affected portable pool/runtime/node tests fresh build exit0,3/3 PASS0.09s. SDK
libraries rebuilt/install exit0; affected installed sdk/control/runtime consumers
fresh build exit0,3/3 PASS0.08s, C++20/assertions/no RTTI/exceptions. Unrelated scalar/
WLED tests not repeated. Installed public + ESP source buffer_pool.h hash equals
D794C62208003C50A8D180C92A673F354066AB62A097C6E10FDD8F4E8EDB8013.

Evidence build-refactor-integration: build-desktop-slots-repair-file-lock.log,
build-desktop-slots-repair.log (GREEN), build-engine-default-repair.log,
tests-engine-default-repair.log + engine-default-repair.xml (PASS),
tests-engine-normal-repair.log + engine-normal-repair.xml (FAIL),
build-portable-slots-repair.log, tests-portable-slots-repair.log +
portable/slots-repair-portable.xml, install-sdk-slots-repair.log,
build-consumer-slots-repair.log, tests-consumer-slots-repair.log +
consumer-build/slots-repair-consumers.xml, installed-pool-hash-slots-repair.txt,
source-hashes-desktop-slots-repair.txt (41-file current capture),
application-inventory-slots-repair.txt, dll-inventory-slots-repair.txt,
commands-desktop-slots-repair.log. EXE/DLL paths all absolute under
C:/Users/Koba/CLionProjects/qt-visualization/build-refactor-integration/bin;
platforms/qwindows.dll present from successful deploy. All prior failures preserved.
I evidence not independent T acceptance; offscreen not native acceptance. No source
patches/tool installs/userdata/commits. GATE2 NOT ACCEPTED; new T independent review
waits fix+integration GREEN. ProposedSlice3 ready/NOT STARTED. I REPORT READY / IDLE.

Coordinator confirmed focused actual Qt default startup PASS as I integration evidence for the repaired header. Stop-clearing regression is assigned to narrowly unfrozen T2; normal failure and unexecuted remainder remain separate. I IDLE, no further tests or portable/SDK repetition until new T2 re-freeze handoff.

### Slice 2 — T Stop repair reintegration / next preparation-history failure

2026-10-07. T Stop repair re-frozen: engine_client.cpp and engine_typed_views_tests.cpp
plus desktop contract §13. I read exact handoff/docs/source-hashes-stop.txt; own
source-hashes-stop-freeze-check.txt matches all three published hashes. Registered
engine_stopped_delivery (--stopped-delivery-only), preserving assertions and bounded
CTest environment. No foreign source edits.

Legacy17 boundary is explicit: server fences obsolete local pipeline epochs before
serialization and ordered TCP delivers serialized Stop clear before subsequent
Running status. Legacy17 has no separate local Stop epoch, so arbitrary same-session/
revision injected stale clear cannot be distinguished by that wire alone. Canonical20
retains nonce/session/sequence/local stamp fences. T author GREEN not used as I proof.

Own desktop target build/deploy exit0, new engine_client and engine test source
compiled, affected apps/tests relinked. All five app EXEs + requested binaries ready,
DLL deploy complete. Normal engine_typed_views standalone PASS1/1, exit0,0.98s;
previous remote Stop-clear failure no longer reproduces in this current normal run.
Then exact15 CTest cases --stop-on-failure exit8,19.68s; only7 attempted:
codec_buffer_tests PASS0.03s; typed_stream_transport PASS17.10s;
registry_capabilities PASS0.43s; engine_typed_views PASS0.86s;
graph_disconnect_continuity PASS0.06s; registry_endpoint_discovery PASS0.88s;
graph_preparation_transaction FAIL0.09s. Assertion at tests/graph_preparation_tests.cpp:179:
desktop.latest_views()[1].history==history.history, after NetworkInput History→
SpectrogramView apply/start/feed. Remaining8 canonical/default/stopped/history/demand/
routing cases NOT RUN. The newly registered Stop case is not claimed executed.
Concrete log/line routed coordinator/T2; all further dependent desktop checks held.
No expectation weakened, no R/D/T source patch by I.

Evidence build-refactor-integration: build-desktop-stop-repair.log (GREEN),
tests-engine-stop-normal.log + engine-stop-normal.xml (PASS),
tests-desktop-stop-repair.log + slice2-stop-repair.xml (STOPPED),
Testing/Temporary/LastTest.log, source-hashes-stop-freeze-check.txt,
source-hashes-desktop-stop-repair.txt (41-file current baseline),
commands-desktop-stop-repair.log, application-inventory-stop-repair.txt,
dll-inventory-stop-repair.txt. Absolute app/DLL directory remains
C:/Users/Koba/CLionProjects/qt-visualization/build-refactor-integration/bin;
qwindows plugin present. Previous startup/Stop/wiring/lock failure logs preserved.
Portable/installed SDK unchanged since header repair: no unrelated reruns.
R/D partial acceptance remains bounded; GATE2 NOT ACCEPTED, no independent T review
before integration GREEN. ProposedSlice3 ready/NOT STARTED. Offscreen not native
acceptance. No userconfig/output/speakers/tools/commits. I REPORT READY / IDLE.

Coordinator assigned History preservation assertion repair to narrowly unfrozen T2. Stop finding is closed for current normal integration only; independent T gate remains pending. Current exact15 result remains6PASS/1FAIL/8NOTRUN. Compatible prepare/full-History snapshot characterization must not be weakened for PASS. I IDLE until new T freeze; no suite/portable/SDK continuation.

### Slice 2 — T History repair current desktop integration GREEN

2026-10-07. Consumed T HISTORY repair RE-FROZEN: audio_pipeline.cpp and
 graph_preparation_tests.cpp; desktop contracts §14 plus later memory clarification.
Product source hashes match source-hashes-history-repair.txt:
audio_pipeline.cpp 2C9D5D40549072FBA6DAE998BFCE5D2F8E60E0DD24C0AF29CE04DAA401569DC4;
graph_preparation_tests.cpp 53DC5BDB824004D4E935407659731AC4DE339C1A33EA317F1DE6F1F898422EBF.
Doc-only memory appendix changed document hash to CAAECD88A3E9DDE595C3C98CA6E46E5BFE50D390DCDA2752E9B8A07B1A84346A;
product source unchanged. No original preservation assertion weakened by I.

I registered graph_external_history (--external-history-only), assertions enabled.
Own cached Qt6.10.2/MinGW13.1/Ninja Release desktop build exact five apps+nine
assigned test binaries exit0; audio_pipeline and graphprep test source freshly
compiled, dependent applications/tests relinked, DLL deployment complete. First
failed graph_preparation_transaction and new external-history focus: exit0,
2/2 PASS0.13s. Then CTest exact16 bounded assigned cases --stop-on-failure:
exit0,16/16 PASS24.72s. Cases: codec_buffer_tests, typed_stream_transport,
registry_capabilities, engine_typed_views, graph_disconnect_continuity,
registry_endpoint_discovery, graph_preparation_transaction, desktop_history_demand,
typed_graph_port_identity, engine_canonical_ports, engine_default_preparation,
engine_stopped_delivery, graph_external_history, registry_canonical_streams,
desktop_canonical_history, stream_typed_routing (4.76s counterwrap/timing case).
All previously NOTRUN assigned cases now actually executed, including new Stop
and external History cases. This supersedes previous I stopped runs for current
source integration only; independent T source acceptance is still required.

Actual LastTest allocation output: idle Mel/local History/external History0,
processing allocated bytes3304 at both History capacities8/400. Measurement is
active legacy subscribe_typed(node0) desktop owning display snapshots/vectors/maps/
output events/cache projections counted through operator new. Equal3304 proves
only capacity-independent compatibility work; not Qt/system allocator absence,
whole feed_typed/capture/network/GUI zero allocation, or the complete no-full-copy
goal. Portable process_ports/PreparedGraph and canonical newest-row retention are
separate prepared-pool boundaries; fresh independent T review must verify new
canonical callback remains compliant. Explicit full query flatten/materialization
is outside process under evaluation mutex. Existing desktop external owning matrix
flatten/copy and legacy owning projections remain temporary compatibility adapters.
FINAL SLICE5 OBLIGATION: remove/optimize those remaining conversions/allocations
and independently verify final boundary/no-full-copy behavior. This partial GREEN
does not mark that remaining goal complete or authorize Slice5 work now.

Current evidence in build-refactor-integration: build-desktop-history-repair.log,
tests-history-repair-focused.log + history-repair-focused.xml,
tests-desktop-history-repair.log + slice2-history-repair.xml,
LastTest-desktop-history-repair.log, source-hashes-desktop-history-repair.txt
(41-file current baseline), source-hashes-history-freeze-check.txt,
commands-desktop-history-repair.log, application-inventory-history-repair.txt,
dll-inventory-history-repair.txt, plugin-inventory-history-repair.txt.
All five EXE and Qt/ADS/MinGW DLL absolute paths listed under
C:/Users/Koba/CLionProjects/qt-visualization/build-refactor-integration/bin;
platforms/qwindows.dll present. Prior logs/XML for wiring/lock/startup/Stop/history
failures remain preserved. Optional Vulkan/dxcompiler/dxil warnings stay native scope;
offscreen is not native Windows/OpenGL acceptance.

Portable/SDK current earlier evidence unchanged; no reruns or I foreign source
patches/tools/userdata/speakers/commits. R/D partial acceptance current, T fresh
independent review can now be dispatched by coordinator. GATE2 NOT ACCEPTED by I;
full goal/Slice5/native/hardware incomplete. ProposedSlice3 documentary dispatch
ready/NOT STARTED. I CURRENT DESKTOP INTEGRATION GREEN / READY / IDLE.

### Slice 2 — independent desktop FINAL FAIL / F1-F2 repair gate

2026-10-07. Coordinator read independent build-gate2-desktop-review/review-report.md;
I read actual report for this doc-only update. Frozen41 hashes all matched and
reviewer's own assigned16 cases passed24.96s, but two independent contract oracles
exit1. Independent T2 acceptance withheld; these greens do not override defects.

| Finding | Actual contract/reproduction | Owner / repeat-review gate |
| --- | --- | --- |
| F1 P1 registry canonical stream limits wrong schema | Agreed stream_limits max_frame_bytes/max_stream_id_bytes/max_streams differ from Engine18–20 message/endpoints limits. capabilities.h26–29 requires and NetworkRuntime.cpp249 advertises max_message_bytes/max_endpoints instead. Documented Scalar provider refused; undocumented schema accepted. repro-registry.log contract_schema_accepted0/undocumented_schema_accepted1. | Narrowly unfrozen T2: consistent agreed validator/producer/consumer schema, contract-derived oracle; do not silently rewrite contract. Repeat independent --registry oracle on new hashes + affected registry cases. |
| F2 P1 empty complete20 erases selected canonical cache | Positive ACK for explicit main/1/Input0, valid available first complete20 caches1; next same-session/localepoch/revision higher-sequence complete20 contains0 entries. Per-entry checks vacuously pass and deliver_ports clears cache0, despite unchanged selection. | Narrowly unfrozen T2: complete selected membership check before advancing accepted fences/cache; initial and replacement incomplete snapshot atomic rejection. Repeat independent snapshot oracle plus focused canonical tests. |

Evidence under build-gate2-desktop-review: review-report.md, source-hashes.txt,
independent_repro.cpp, run-repro.ps1, repro-command.txt, repro-build.log,
repro-registry.log, repro-snapshot.log, build.log/tests.log. Reviewer script exits0
while collecting children; relevant oracle child outcomes are exit1, not script0.
O1 unknown feature optional/critical semantics is an observation, NOT third blocker:
feature string criticality has no defined bit/prefix/representation; a test word
'mandatory' alone does not prove mandatory semantics. Coordinator/owner clarify
advertisement/negotiation without adding new wire; optional unknowns are not
blanket-rejected. FAIL rests only on F1/F2.

T2 narrow F1/F2 unfreeze authorized; I shared builds/tests held until re-freeze.
I previous desktop16GREEN and review16GREEN are historical regression evidence
for the tested pre-fix snapshot, not accepted T behavior. R/D partial acceptance
remains scoped. GATE2 NOT ACCEPTED; ProposedSlice3 documentary ready/NOT STARTED.
After repairs, repeat exact independent oracles and affected tests on new hashes;
closing F1/F2 alone does not accept all outstanding desktop requirements.
Remaining independent obligations include adversarial fragments/replay/budget,
source-owner lifetime, type-remap/malformed metadata atomic replacement/reconnect
incarnation, deterministic WLED300/301/failure/rollover, plus final Slice5 adapter
allocation/full-copy removal and boundary verification. Native/hardware/full goal
not completed. This update ran no builds or product source fixes/commits.
I DOC UPDATE COMPLETE / IDLE.

### Slice 2 — F1/F2 author repair fresh I integration GREEN

2026-10-07. Coordinator confirmed T2 F1/F2 SOURCE FROZEN/IDLE. Read desktop
contract §15 and build-refactor-delivery-slice2/f1f2-source-hashes.txt; all nine
published hashes match (source-hashes-f1f2-freeze-check.txt). O1 clarified without
new wire: unknown additional feature strings are optional advertisements; critical
semantics are not inferred from a word/prefix. Agreed limits/version/type/known
port-identity requirements remain mandatory. Author oracles are not independent
acceptance; Gate2 desktop FAIL is not closed by this integration alone.

I added engine_complete_coverage (--complete-coverage-only) and
stream_limits_admission (--stream-limits-only) CTest registrations, preserving
existing signatures, assertions and bounded environment. No product source edits.
Own integration desktop CMake regeneration/build exact five apps+nine affected
binaries exit0. Inline validator header consumers, StreamTransport/NetworkRuntime/
EngineClient and changed tests genuinely recompiled, dependent apps relinked.
All five EXEs and DLL deployment ready. No stale archive/source copies used.
New focused cases2/2 PASS0.89s, exit0. Full agreed16+two cases exact18 regex
--stop-on-failure exit0,18/18 PASS25.56s; full per-case output/XML saved.
Includes legacy codec/transport goldens, registry normative schema/admission,
normal/default/Stop/complete coverage engine, canonical ports/types/history,
continuity/preparation/external owner, and WLED routing timing4.73s.

Evidence in build-refactor-integration: build-desktop-f1f2-repair.log,
tests-f1f2-focused.log + f1f2-focused.xml, tests-desktop-f1f2-repair.log +
slice2-f1f2-repair.xml, LastTest-desktop-f1f2-repair.log,
source-hashes-f1f2-freeze-check.txt (nine frozen matches),
source-hashes-desktop-f1f2-repair.txt (41-file current snapshot),
commands-desktop-f1f2-repair.log, application-inventory-f1f2-repair.txt,
dll-inventory-f1f2-repair.txt, plugin-inventory-f1f2-repair.txt. Absolute EXE/DLL
paths listed under C:/Users/Koba/CLionProjects/qt-visualization/build-refactor-integration/bin;
platforms/qwindows.dll present. Prior independent FAIL and all historical I failure/
GREEN artifacts retained. Optional native deployment warning/offscreen do not
prove native GUI/OpenGL/hardware acceptance.

Current author integration GREEN is ready for independent T2 repeat of original
F1/F2 oracles plus remaining requirements. GATE2 NOT ACCEPTED; independent desktop
acceptance withheld until repeated review. R/D accepted scope unchanged. Portable/
SDK sources unchanged, no reruns. Memory/legacy adapter Slice5 obligations remain
explicit and incomplete; ProposedSlice3 documentary ready/NOT STARTED. No source
patch/tool install/userconfig/output/speaker/commits/branches/ZIP.
I CURRENT F1/F2 DESKTOP INTEGRATION GREEN / READY / IDLE.

### Slice 2 — independent T repeat closes F1/F2, new F3 P1 incarnation FAIL

2026-10-07. Coordinator reports repeated independent T acceptance closed original
F1 registry schema and F2 complete selected-cache coverage, but found F3 P1.
I read independent incarnation_repro.cpp/run-incarnation.ps1/incarnation-result.log
under build-gate2-desktop-review. Own harness compile0/run1: registry incarnation
changes77→88, exact new subscription/ACK established, first DATA on that new session
still carries epoch77 and is accepted available. NetworkRuntime does not pass the
expected registry incarnation into Subscriber, so first DATA epoch is unconstrained.
This is stale producer acceptance after reconnect, not an O1 feature-list question.

F3 owner T2, address-specific repair authorized; only that source scope is unfrozen.
Require expected incarnation handoff/admission and independent repetition of the
original reconnect oracle against new frozen hashes plus affected regression checks.
No unrelated wire/API/runtime changes inferred. Other independent desktop
obligations remain unaccepted. Original F1/F2 closure does not make T2/GATE2 PASS.
I18/18 GREEN describes current pre-F3-fix integration evidence only, not independent
T acceptance. R/D bounded acceptance unchanged; GATE2 desktop FAIL / NOT ACCEPTED.
Slices3–5 NOT STARTED (ProposedSlice3 documentary ready only; remaining Slice5
memory/boundary obligations still future). Shared I builds/tests HELD until new
T freeze; this update made no builds/product source fixes/commits.
I DOC UPDATE COMPLETE / IDLE.

### Slice 2 — F3 author repair fresh I integration GREEN

2026-10-07. T2 F3 READY/SOURCE FROZEN/IDLE consumed: six-file
f3-source-hashes.txt all match (source-hashes-f3-freeze-check.txt). Read actual
f3-author-report.txt and desktop contract §16. StreamTransport additive expected
publisher incarnation overload, NetworkRuntime confirmed-registry handoff, early
mismatched DATA discard, focused tests; legacy expected0 overloads retained.
T author original-oracle GREEN is not independent proof of F3 closure.

I registered engine_registry_incarnation (--registry-incarnation-only) and
stream_incarnation (--incarnation-only) in own tests/CMakeLists, preserving
assertions/bounded environment. No product code changes. Fresh own integration
five-app+nine-binary build/deploy exit0; StreamTransport/NetworkRuntime and header
consumers/changed test sources recompiled, affected apps relinked. All five EXEs
and DLL deployment ready. Focused2/2 PASS2.27s, exit0. Full prior18+two exact20
cases CTest --stop-on-failure exit0,20/20 PASS27.63s, including legacy/canonical
registry/engine/history/Stop/coverage/limits/incarnation and WLED4.71s cases.
Current source integration GREEN supersedes earlier18 snapshot checks for I
regressions; independent repeated F3 oracle and remaining desktop review still
required. No acceptance inferred from author greens or matched hashes alone.

Evidence build-refactor-integration: build-desktop-f3-repair.log,
tests-f3-focused.log + f3-focused.xml, tests-desktop-f3-repair.log +
slice2-f3-repair.xml, LastTest-desktop-f3-repair.log,
source-hashes-f3-freeze-check.txt (six frozen matches),
source-hashes-desktop-f3-repair.txt (41-file current snapshot),
commands-desktop-f3-repair.log, application-inventory-f3-repair.txt,
dll-inventory-f3-repair.txt, plugin-inventory-f3-repair.txt. Individual absolute
EXE/DLL paths under C:/Users/Koba/CLionProjects/qt-visualization/build-refactor-integration/bin;
platforms/qwindows.dll present. All previous failure/review/snapshot logs retained.
Offscreen/deployment is not native/hardware acceptance; Slice5 remaining desktop
adapter allocation/full-copy completion obligation unchanged.

R/D accepted scope unchanged; GATE2 NOT ACCEPTED pending independent T repeat and
remaining requirements. Slices3–5 NOT STARTED; ProposedSlice3 documentary ready.
No portable/SDK reruns (unchanged), foreign source patches/tool installs/userdata/
speakers/commits/branches/ZIP. I CURRENT F3 DESKTOP GREEN / READY / IDLE.

### Slice 2 — independent F3 CLOSED; new F4/F5 desktop FAIL

2026-10-07. Coordinator reports independent F3 incarnation oracle CLOSED, then
new independent contract_repro.cpp findings in build-gate2-desktop-review. I read
actual stopped-poison-result.log, interleave-result.log and bounded fixture source.

| Finding | Independent reproduction / impact | Owner / required repeat gate |
| --- | --- | --- |
| F4 P1 Stop admission fence poisoning | contract_repro noarg exit1. Stopped available canonical batch with local epoch999 is rejected for callbacks yet advances accepted fences; later valid epoch11 unavailable clear and epoch12 resumed data are dropped. Log stopped_available_rejected1/valid_later_clear_delivered0/valid_resumed_frame_delivered0. | T2 narrow repair: reject before accepted fence/state mutation; original stopped-poison oracle repeat and current Stop/canonical regression checks on fresh hashes. |
| F5 P2 fragment assembly permits interleaved status7 | contract_repro --interleave exit1. While fragment assembly for DATA20 is active, status7 changes accepted graph revision. Log active_partial_20_interleaved_status7_accepted1; violates agreed section9 single-message assembly rule. | T2 narrow repair: enforce agreed fragment/interleaving admission boundary without weakening framing contract; original interleave oracle repeat plus affected fragmented delivery regressions. |

F3 closure does not accept F4/F5 or remaining desktop obligations. Source freeze
released only for assigned T2 repair; I builds/tests held until next freeze.
Current20PASS remains pre-repair author integration evidence, not desktop
acceptance. GATE2 desktop FAIL / NOT ACCEPTED; R/D scope unchanged, slices3–5
NOT STARTED (ProposedSlice3 documentary ready). No new wire/other source work
inferred. This update ran no builds/product fixes/commits. I DOC COMPLETE / IDLE.

### Slice 2 — F4/F5 author repair fresh I integration GREEN

2026-10-07. T2 F4/F5 SOURCE FROZEN/IDLE consumed. Read actual three-file
f4f5-source-hashes.txt, f4f5-author-report.txt and desktop contracts §17;
all three frozen hashes match (source-hashes-f4f5-freeze-check.txt). Shared
client/server Wire source engine_client.cpp and engine test changed; no
portable runtime/control/BDSP/product files edited by I. Author unchanged
F1–F5 oracle copies GREEN are not independent acceptance.

I registered engine_stopped_poison (--stopped-poison-only) and
engine_fragment_interleave (--fragment-interleave-only), assertions/bounded
CTest environment retained. Fresh own five-app+nine-binary target build/deploy
exit0; engine source/test genuinely recompiled, affected apps/tests relinked,
DLL deployment complete. New focus2/2 PASS1.02s, exit0. Prior20+two exact22
cases --stop-on-failure exit0,22/22 PASS28.49s, with full per-case log/XML.
Stopped future epoch/sequence poisoning and client/server assembly interleave
cases ran alongside normal completion/legacy/canonical/registry/history/WLED
regressions. No test expectation or source patch by I.

Evidence build-refactor-integration: build-desktop-f4f5-repair.log,
tests-f4f5-focused.log + f4f5-focused.xml, tests-desktop-f4f5-repair.log +
slice2-f4f5-repair.xml, LastTest-desktop-f4f5-repair.log,
source-hashes-f4f5-freeze-check.txt (three matches),
source-hashes-desktop-f4f5-repair.txt (41-file current baseline),
commands-desktop-f4f5-repair.log, application-inventory-f4f5-repair.txt,
dll-inventory-f4f5-repair.txt, plugin-inventory-f4f5-repair.txt.
Individual EXE/DLL absolute paths under
C:/Users/Koba/CLionProjects/qt-visualization/build-refactor-integration/bin;
platforms/qwindows.dll present. Prior failure/review/snapshot evidence preserved.
Current22GREEN is I author integration, not independent closure of F4/F5 or
remaining desktop requirements; repeated unchanged oracles/new review required.
GATE2 NOT ACCEPTED/pending; R/D bounded acceptance unchanged, slices3–5 NOT STARTED,
ProposedSlice3 documentary ready. Slice5 memory/full-copy final obligations and
native/hardware acceptance remain incomplete. No portable/SDK reruns (unchanged),
product patches/tool installs/userdata/speakers/commits/branches/ZIP.
I CURRENT F4/F5 DESKTOP GREEN / READY / IDLE.

### Slice 2 — independent exact F1–F5 CLOSED; whole T INCOMPLETE

2026-10-07. I read actual build-gate2-desktop-review/f4f5-rereview-report.md.
Independent unchanged fixtures compile/run0: F1 agreed schema accepted/Engine
schema refused; F2 empty COMPLETE retains cache1; F3 registry88 rejects DATA77;
F4 valid11clear/12resume survive rejected stopped999; F5 partial20 interleaved7
not accepted. Original fixture hashes unchanged, current I41+T3 source hashes
all match. Evidence f4f5-registry-green.log, f4f5-snapshot-green.log,
f4f5-incarnation-green.log, f4f5-stopped-green.log, f4f5-interleave-green.log,
f4f5-review-source-hashes.txt and exact original scripts in reviewer directory.
This is bounded independent regression closure, NOT whole T acceptance.

Operational limitation explicitly recorded: transition to publisher queue
experiment ended in reported systemError/cybersecurity flag; underlying error
payload/code/trace was not exposed. Publisher bytesToWrite/framing/fragment
queue-size experiment not completed and has NO test result. Coordinator forbids
repeating or bypassing that experiment. No workaround/retry is authorized or
performed by I. This limitation is neither product defect proof nor PASS.

Remaining whole desktop independent obligations are unaccepted: broader COMPLETE/
delta atomic matrix; malformed fragments/normal completion/timeout/combined queues
and both ends beyond F5; source/plan/engine destruction lifetime and exhaustion;
remapped kind/type/direction/port/unavailable matrix; registry malformed replacement/
retired binding/internal retry/current producer continuity/lower-limit refusal;
deterministic WLED300/301/failed-attempt counter/rollover/cadence with substituted
clock/transport. Blocked publisher queue-budget experiment remains explicitly untested.
Coordinator created independent chat01a11343-19cf-7b13-8fea-6fbe963352ae for only
local unit/static ownership/lifetime and WLED fakeclock/faketransport, NO real sockets.
Primary reviewer IDLE. Source stays frozen; I no further builds/source fixes before
new findings/handoff. Current I22PASS/app-deploy are integration evidence, not T
acceptance; Gate2 NOT ACCEPTED/remaining scope INCOMPLETE. Slices3–5 NOT STARTED;
Slice5 legacy allocation/full-copy final obligations unchanged.
I DOC UPDATE COMPLETE / IDLE; no builds/product fixes/commits.

### Slice 2 — complementary socket-free independent reviewers

2026-10-07. Coordinator created second reviewer
01a11344-2e6e-79b0-9d0a-04be7a83b9ce: static source review and socket-free unit
codec/registry contracts, own build-gate2-delivery-contract-review. Scope:
validation ordering, COMPLETE/delta, lengths/fragments/limits/types/metadata/retry.
This complements reviewer01a11343-19cf-7b13-8fea-6fbe963352ae's local unit/static
ownership/lifetime and fakeclock/faketransport WLED, not a second implementation owner.
Both inspect frozen sources read-only and write only independent own artifacts.
Static findings and actually executed unit results must be reported separately;
unexecuted branches explicitly UNVERIFIED. Real sockets, blocked queue experiment,
repeat/bypass and source-copy networking workaround are prohibited. No new wire or
source changes authorized. Primary desktop reviewer remains idle; max active3
confirmed by coordinator. I documentation-only/IDLE, implementation frozen,
Gate2NOTaccepted/wholeT incomplete; slices3–5NOTstarted.

### Slice 2 — complementary independent reports: bounded ownership/WLED PASS; C1/C2 FAIL

2026-10-07. Coordinator read both formal reports; I read actual
build-gate2-ownership-wled-review/review-report.md and
build-gate2-delivery-contract-review/review-report.md. Bounded ownership/WLED
scope accepted, not whole desktop acceptance. 185 enumerated product hashes
unchanged; independent fresh lifetime/retain oracle compile0/run0, prepared
callback allocations0 within its operator-new scope, retained identities/payloads
survive source/plan/context/engine destruction, atomic pool refusal and masked
external History/independent count lifetimes verified. Actual unedited WLED source
compiled with fakeclock/faketimer/faketransport headers, no real sockets: deterministic
300live/301silence, invalid freshness, private bytes,17msPreciseTimer configuration,
failed-attempt counter/error/stop/restart/rollover267 attempts PASS. This proves
adapter policy control flow, not native Qt scheduling/OS send behavior/hardware.
Evidence run-ownership.ps1/ownership-result.log, run-wled.ps1/wled-result.log,
freeze-result.txt/source-hashes-before/after in that reviewer directory.

Contract reviewer bounded FAIL has two assigned findings:

| Finding | Evidence class / defect | Owner / required gate |
| --- | --- | --- |
| C1 P2 registry normative version subset | EXECUTED public validator contracts.cpp --normative-versions exit1,4checks/3failures. Validator requires both2+3 and admits unsupported99; normative nonempty actual subset allows Scalar3-only or Mel2-only. | Narrow T2: integer nonempty supported subset and each advertised selected version membership; exact normative socket-free proof repeated, contract unchanged. |
| C2 P1 all-ports delta membership/fence poisoning | STATIC SOURCE PROOF only, transport NOT EXECUTED/UNVERIFIED. selected.empty delta bypasses configured endpoint admission (snapshot-only check), absent node accepted; processor Input may enter set reserved for actual Output/SinkInput. Accepted fences/cache can mutate for an unconfigured endpoint. | Narrow T2: shared pure public admission helper approved by coordinator, exact configured membership on COMPLETE/delta before state mutation; fresh independent static/socket-free helper checks; no networking copy workaround. |

Contract default534checks PASS, atomic DTO/byte codecs and public assembler geometry
executed separately; not transport-state proof. Evidence contracts.cpp/run.ps1,
bounded.log/normative-versions.log, hashes-before/after and report in
build-gate2-delivery-contract-review. T freeze released ONLY C1/C2 assigned zones.
Other source/accepted R/D/ownership-WLED scopes unchanged; I builds held until freeze.

Unresolved STATIC queue arithmetic item remains: original payload_size+256 preflight
may miss rewritten id and35fragment headers already280bytes at2MiB, before framing;
combined publisher/Engine pending/assembly budget not independently proved sufficient.
This is not a completed experiment or silently closed finding. Actual blocked
publisher queue experiment remains untested; repeat/bypass/real sockets/source-copy
networking workaround prohibited. Wider fragment timeout/metadata/retry transport
branches remain UNVERIFIED where only static source was inspected.

GATE2 desktop FAIL/whole scope pending; current22PASS is pre-C1/C2 integration
snapshot only. Slice5 legacy allocations/full input copy/final boundary verification
obligation unchanged; slices3–5NOTstarted, ProposedSlice3 documentary ready.
This documentary update performs no builds/product patches/commits.
I DOC UPDATE COMPLETE / IDLE.

## Gate 2 residual requirement → evidence matrix (docs-only, 2026-10-07)

Authority: original Slice2 plan/runtime+desktop contracts, not a reduction to the
current green test list. Evidence sources: build-gate2-ownership-wled-review/
review-report.md (OW), build-gate2-delivery-contract-review/review-report.md (DC),
build-gate2-desktop-review/f4f5-rereview-report.md (DR). The table distinguishes
independent execution from static observations. Proposed seams are documentary
options requiring exact coordinator assignment if production changes are needed;
a new seam/API is not mandatory merely to unit-test a requirement. Existing public
validators/codecs/FrameAssembler/try_advertise can be exercised directly socket-free
after authorization. Complete static invariant proof of current production arithmetic,
state transitions and all actual call sites is admissible policy evidence, honestly
distinct from executed tests/native measurement. No permission to implement/run or
bypass blocked work is implied. T2 owns desktop semantics; independent reviewers
own their own proofs; I owns only build/manifests/packaging/docs.

| Gate2 requirement | Current evidence / status | Unmet part: owner and permitted proof seam |
| --- | --- | --- |
| Typed node+direction+port delivery, History0/count1 and sink Input, known-zero/unknown-unavailable, compatible state | R bounded independent algorithm/F1F2 PASS; OW independently executed four endpoints/count/sink/closed rows/two sources/lifetimes. I22 regression PASS is separate integration. | Broader desktop configured/remapped endpoint membership is not accepted by those routes. T2 owns repair; DC reviewer existing descriptor/public membership helper tables plus complete actual catalog/receiver wiring proof, no transport extraction/copy or additional mandatory API. |
| Borrowed callback ownership, retained identity/payload after source/plan/context/engine destruction | OW independently executed PASS after destruction/ring wrap/copy/release, pool refusal atomic and all slots released;185hashmatch. | General endpoint/identity/payload/control-reserve exhaustion permutations are not all executed. T2 source owner; OW reviewer actual public retain_port_frame/BufferPool/AudioPipeline fixtures with bounded caller pools and synthetic source, no sockets. Any missing reserve policy seam requires coordinator approval. |
| Canonical prepared retention copies newest History row only, full query demand outside processing | OW actual source inspection + independent count-only warmticks/portable process+retain operator-new0; current local/external owner queries PASS. | No whole desktop allocator guarantee inferred. Canonical admission/exhaustion matrix still must not grow processing containers; T2/OW public prepared-retain fixtures. Legacy adapter completion remains separate Slice5 row below, not excuse for canonical failure. |
| Registry actual payload-version subset/selected membership | C1 P2 OPEN: DC executed validator normativeversions4checks/3failures; valid3-only/2-only refused, unsupported99 accepted. Other public malformed decode cases PASS atomically. | T2 exact C1 repair; DC reviewer same unmodified public aggregator_capabilities::valid/decode oracle + all integer/nonempty subset/advertised stream selected-version cases. No sockets needed. |
| Allports COMPLETE/delta configured membership and before-fence atomic admission | C2 P1 OPEN STATIC source proof; transport UNVERIFIED. Explicit selected empty COMPLETE F2 independent closed; codec duplicates/truncation/short-buffer atomic proofs PASS. | T2 pure shared public admission helper approved. DC reviewer calls actual production helper with configured descriptor catalog/cache/selection/stamps for absent nodes, processor Input vs SinkInput, kind/type/port mismatch and future fences; helper must be used by receiver before mutation. No copied networking receiver or private-seam simulation. |
| Initial/partial/malformed/repeated COMPLETE + delta atomic state/fences | DR exact F2/F4 CLOSED unchanged oracles; DC codec534checks PASS, static initial-delta/revision/Stop ordering observed. Full state matrix still UNVERIFIED. | T2 owner; DC reviewer direct codec/membership tables plus full invariant proof of ACK/Frames/session/status/catalog/cache/callback mutation paths. A new production state-transition API is optional only for stronger executed matrix, not required by Slice2. Distinguish static admission proof from socket dispatch execution. |
| Engine fragment lengths/offsets/overflow/allowed kinds/single-message/normalcompletion/timeout on both ends | DR exact F5 client interleave CLOSED executed; DC public FrameAssembler120005bytes/offset/lower-limit PASS. Shared Wire guards/timer5s/reset are STATIC; normalcompletion/server/malformed/timeout wider matrix not independently executed (I focus success is author integration). | T2 owner; DC reviewer use existing actual public FrameAssembler + byte codec for geometry; complete static shared Wire finite-state/preflight/reset/release/timer/client-server call-site proof is admissible. An additional real production envelope/assembly seam is optional for executed policy matrix only; DATA FrameAssembler is not Engine Wire. No real sockets/source-copy networking/forbidden experiment; static proof is not native timer execution. |
| Registry/stream frame-id-count limits and immutable bindings | F1 normative schema exact independent CLOSED; DC lowerID/metadata malformed atomic output PASS; broader actual lower frame/count refusal is pending. | T2 owner; DC actual public StreamLimits/capabilities/pure admission helper unit tables for exact min/max/overflow/id bytes/catalog128→129/selected version. Network connection side effects remain UNVERIFIED unless a permitted actual policy seam is approved. |
| Registry incarnation/reconnect/internal retry/current producer continuity/remapped bindings | DR exact F3 CLOSED unchanged oracle. DC STATIC observes expected-incarnation check before lineage/sequence and fingerprint/retry; wider runtime matrix UNVERIFIED. | T2 owner; DC approved actual public pure producer-admission/binding fingerprint/lineage state seam, synthetic registry+DATA DTOs for old/future/current epochs, retired producer/high seq/gen, explicit reset vs internal retry, type/kind/endpoint remap. No actual socket reconnect, no copied NetworkRuntime/Subscriber workaround. |
| Malformed registry replacement preserves prior accepted binding/cache | DC public decode atomic PASS, server/client validation-before-assignment STATIC. Full live-resource/cache effect not independently executed. | T2 owner; DC existing public decode atomic refusal plus complete server/client/setter store-order/error/disconnect/resolve invariant proof. New replacement seam not mandatory. Preserve distinction: invalid setter can retain live metadata, explicit server disconnect legitimately changes availability; no blanket live-ingress preservation claim. |
| WLED private bytes,17ms policy, stale300/301, invalid freshness, unavailable silence, failed attempt counter/rollover | OW independently executed actual unedited adapter with fakeheaders:267attempts, producer201/private0,300live301silence, failed-write error/stop/restart/rollover PASS. | Assigned deterministic policy scope accepted. Native event scheduling/OS send/hardware not proved and not silently required as fakeclock proof; native/hardware scope remains original later acceptance. OW notes concrete Qt internals testability limitation, not demonstrated Gate2 blocker. |
| Bounded combined publisher/outbound/assembly queues and exact admission arithmetic | UNRESOLVED DC STATIC: payload_size+256 before id rewrite/35fragment headers280 at2MiB, extra framing unknown; Engine32/fragment before COBS also unproved. Actual publisher queue experiment operationally blocked/no result. | T2 owns source arithmetic/admission repair or proof; coordinator must assign exact scope. Permitted options: complete static exact worst-case framed-size inequalities plus all actual sender/channel/transport/assembly wiring, or an optional actual public pure budget calculator using finalized byte length/fragment count/header+framing upper bounds/current pending numeric budgets. No packet-encoding substitute/real sockets/bytesToWrite experiment/source-copy transport or retry/bypass of blocked action. A pure bound proof must not be labeled actual Qt queue measurement. If actual queue behavior remains required by original plan, Gate2 cannot be completed until authorized evidence/constraint resolution, not waived by this matrix. |
| DATA fragment timeout vs Engine5s policy | DC STATIC subscriber ACK cancels handshake timer; public DATA assembler has no clock. Required DATA lifetime/expiry contract remains clarification/pending; no execution claim. | First trace original DATA authority separately from explicit Engine5s; do not invent DATA5s or a new timer/blocker. T2/coordinator clarify whether separate DATA expiry is required. If required, complete static deadline/reset wiring proof is admissible; optional clock seam only for deterministic execution. No sockets or silent waiver. |
| Five desktop apps build/deploy, affected integrations, stable existing APIs/goldens | I current fresh five apps+D​​LL build GREEN,22/22 targeted PASS/current hashes/inventory. Author integration, not independent semantic acceptance. | I rebuild/short affected tests only after C1/C2 newfreeze; independent accepted-source comparisons remain reviewer responsibility. No broad/native/full/soak repeats inferred. |
| Final legacy owning projections/external fullinput copy/GUI materialization allocations | OW independently documents idle0 and active legacy3304/3304, only capacity independence. Existing desktop adapter full-copy/allocation paths remain. | Separate explicit Slice5 obligation: T/desktop/UI owner to be assigned at that slice, I wiring, independent final boundary verification. NOT STARTED and not achieved now. Do not move new canonical processing defects or Gate2 bounded queue/admission requirements into this row. |

Gate2 completion rule: C1/C2 must be repaired/re-frozen and independently closed;
remaining Gate2 admission/type/fragment/retry/budget policy obligations need complete
evidence at their actual boundary. A complete static invariant proof plus actual
wiring may suffice without a new API or native Qt measurement; partial observations
do not. Any measured queue experiment remains unexecuted and cannot be claimed
completed or substituted by a bypass. Original authority determines whether a
separate measured result is required, with coordinator resolution if unavailable.
Bounded OW PASS/F1–F5 closure/I22GREEN do not combine
into wholeGate2PASS by arithmetic. Blocked actual queue experiment stays untested
and prohibited to repeat/bypass; no hidden relocation to Slice5. Static observation
is never labeled executed PASS. Slices3–5 remain NOT STARTED, full goal incomplete.
I MATRIX DOC COMPLETE / IDLE; no product edits/builds/tests/commits.


2026-10-07 matrix method correction: read actual build-gate2-delivery-contract-review/residual-proof-options.md. Existing public try_advertise/limits may be checked on unopened publisher without listen/connect/send after authorized fixture build; Subscriber::start opens transport and is not a socket-free test. Complete arithmetic+production-wiring proof may establish bounded policy invariant without mandatory new API/native Qt measurement. Current queue insufficiency still unresolved, actual blocked experiment remains unexecuted/prohibited; C1/C2 pending. Static/executed boundaries unchanged, no new PASS/waiver. Docs-only, no product/build/test.

### Slice 2 — C1/C2 frozen; consolidated review before next integration build

2026-10-07. Coordinator confirms seven-file C1/C2 freeze (T artifacts
c1c2-source-hashes.txt/c1c2-author-report.txt), root verified hashes. Author
socket-free cases GREEN and engine test compile completed; no runtime socket
rerun claimed. Independent C1/C2 closure remains PENDING.
Reviewer01a11344-2e6e-79b0-9d0a-04be7a83b9ce now performs consolidated remaining
read-only review, continuing other independent areas rather than stopping at
first finding. I integration build HELD until consolidated findings collected
and T repairs them as one assigned batch/re-freezes. Next gate: consolidated
independent inventory → assigned batch repairs/freeze → bounded I fresh integration
→ independent closure, not repeated full builds for each small finding.
No new I build/product edit or extra matrix revision in this doc-only update.
Gate2NOTaccepted, slices3–5NOTstarted. I DOC COMPLETE / IDLE.
# Slice 3 dependency handoff (current)

I owns `libs/bdsp_protocol/vendor/{tinycbor,nanopb}`, explicit
`tools/control_codegen` runner/tool manifest, and CMake/export/ESP source mappings.
K retains ownership of schema/options, generated/control.pb.c/.h, control message
and codec implementations/tests. No generated control outputs were written by I.
Minimal C99 runtimes compiled; caller-buffer CBOR/varint smoke and undefined
allocator-symbol audit PASS. Pinned generator bootstrap/version check PASS with
Python -S and project-local protobuf/protoc; no global installation occurred.
Archive/wheel retrieval was explicitly authorized developer-tool acquisition,
not a user ZIP deliverable. Runtime and tools manifests record source revisions,
licenses and hashes; normal build/install needs neither Python nor generation.
Evidence lives in `build-refactor-integration/control-deps`; this does not accept
codec semantics, firmware compilation, full app, Gate2, or Slice5 allocations.
Stage3 independent dependency groundwork STARTED/READY, Gate2 pending. I idle
handoff permits K implementation within the three-worker limit. P3 wiring is
deferred to the next consolidated integration batch; T C3–C10 full build hold stays.

# Current consolidated T/P integration (2026-10-07)

Coordinator released T C3–C10 single17-file freeze plus P S1/S2 seven-file repair
freeze for one I batch. All24 SHA256 match before/after. I CMake wiring only:
graph_document_adapter PUBLIC graph_application + project_display_settings.cpp;
display tests link adapter, atomic save QtCore+adapter includes. Fresh five apps
and affected libraries/tests build exit0, Qt/MinGW DLL deployment complete.
18 scoped socket-free CTest PASS0.80s + capabilities --metadata-only exit0.
No live K source/generated/test hashing or compilation; target closure verified.
Engine/stream/registry/TCP network scenarios freshly buildable but execution
PENDING; no broad all-green/native queue pressure claim. Blocked publisher
experiment never repeated or bypassed. C6/C7/C9 source proof awaits reviewer.

Independent P rereview report read; coordinator accepts bounded P3 source/storage/
projection, S1/S2 CLOSED: original38/10/13, adjacent26, existing6 + manager path PASS,
seven hashes unchanged. WholeGate3/production integration remains open. Gate2
pending consolidated independent C1–C10 review; author I results are separate.
User-authorized independent parallelSlice3 override permits K codec/P storage
work; dependent T3 network still gated. Slice5 allocation/fullcopy work remains.
Commands/results/XML/app/DLL/plugin inventories/hashes/report are under
build-refactor-integration/consolidated-tp, integration-report.md is the single
batch handoff. I READY / COMPLETE / IDLE; reviewer slot can proceed.

# Current K3 integration and Gate2 development transition

2026-10-07. K14/T17/P7 freezes match before/after. I CMake/manifests only integrate
three codec cpp + generated C/private include/pinned runtime links and three tests;
ESP generated/vendor source/include package mappings complete. Portable3/3PASS0.12s,
desktop3/3PASS0.13s + fresh fiveapp/DLL deployment; installed external consumer BOTH
real codecs encode/decode without Qt/exceptions/RTTI PASS. Normal host build/install
no Python/protoc/control generation/download; pinned vendor/tools unchanged.
Explicit existing-toolchain ESP32 optimized4096+transports codec objects compile,
but full archive FAILED domain graph_definition default_text aggregate -Werror.
Owner failure routed via coordinator; no self-fix/warning suppression; ESP full
archive/firmware/hardware remains pending. Exact commands, manifests/hashes/logs/
inventories and gapmatrix: build-refactor-integration/codec-integration/integration-report.md.

Coordinator now authorizes Gate2 DEVELOPMENT TRANSITION to dependent T3 AFTER
independent codec acceptance. Ref: build-gate2-delivery-contract-review/repair-recheck-report.md
plus consolidated-tp integration18checks: C1/C2+C3–C10 bounded CLOSED,17hashstable;
prior R/D/ownership-WLED PASS preserved. This supersedes development-transition
hold only, not whole-goal/native execution obligations. Actual socket/timer/replay/
serial/slowpeer/partialwrite tests BUILD-ONLY/PENDING final automatic acceptance;
native queue experiment blocked/unverified, no requirement removed or substituted.
C6/C7/C9 static closure is not executed event-loop proof. Slice5 fullhistorycopy/
alloc unfinished. P3 bounded accepted, wholeGate3productionpending. K author/I
integration GREEN not independent codec acceptance; T3 contractprep only until
review. Independent parallelSlice3 user override persists. I READY / REPORT / IDLE.

# Current bounded domain retry / independent K3 acceptance

2026-10-07. R single domain initializer freeze22338569D3F14EFC5C6CC7E390A7C78FBBAFE5DBEDBF4BD669D56433B2246D61
matches before/after; K14/T17/P7 unchanged. Exact existing ESP32 optimized4096+
transports incremental target PASS16actions, FULL libportable_sdk.a and BDSP/
ESP-DSP/stream adapter archives produced; no warning suppression/source fix by I.
Host affected domain3actions/1testPASS, SDK install+actual installed graph/application
consumerPASS, five desktop apps relink/DLL deployment7actions. T3 live zones excluded,
previous codec3/18 suites not repeated. Binary/source manifests and commands:
build-refactor-integration/domain-retry/integration-report.md. Prior ESPRED logs
preserved; full-archive build gap now closed, native firmware/hardware remains open.
Domain source independent review still pending; author equivalence not acceptance.

Full K3 independent review report read; coordinator ACCEPTS bounded codecs/deps
under stable DISJOINT caller storage.14hashmatch,20,934 public API probes/4tests/
installed consumer/two exact generations/vendor-tool pins PASS/no findings.
Runtime overlap rejection not mandatory original plan, not implemented/accepted;
controllers must honor precondition. Host stack frames encode3920B/callback2496B/
decode1824B + nested frames carried to Slice4taskstack composition, not ESP peak.
T3 phaseA newfile mapper/controller active; existing Engine/audio/network readonly
until R legacycontract. WholeGate3/goal/native socket-timer-queue acceptance open;
execution gapmatrix/final automatic requirements preserved, Slice5fullcopyalloc
unfinished. I retry complete/READY/IDLE.

# Current R legacy application ABI integration

2026-10-07. R4 legacy freeze verified before/after, K14/T17/P7 and domain freeze
unchanged. Actual runtime-port vtable ABI changed; ControlSessionSlot MinGW x64
696→704 (+8), policy4B, service416/reply280 unchanged. sizeof-based existing budgets
include +8/slot (+16two/+64default8), no new limits/defaults. Coordinator-reported
T3 ACCESS_VIOLATION from old I archive/new header was artifact ABI mismatch, not
a product finding; no invalid-mixture reproduction or source masking by I.

Normal shared registered closure rebuilt current application/adapters/callers;
five apps relink+Qt/MinGW deployment, portable SDK/current installed consumers and
ESP application object/FULL SDK archive updated. Processing/lifecycle2/2PASS in
desktop0.09s/portable0.06s, installed sdk/control consumers2/2PASS0.04s. Other tests
build-only, previouscodec3/18 not repeated. Live T3 source excluded from command
closure; T3 must rebuild its own caller artifacts against matching current ABI.
Exact commands, hashes/member/appDLL inventories and report:
build-refactor-integration/legacy-abi/integration-report.md. R legacy delta new
independent review PENDING; author/I GREEN not independent acceptance.

Coordinator ACCEPTS narrow domain initializer semantics-preserving compile fix:
build-gate3-control-codec-review/domain-fix/review-report.md independent strict
current0/priorRED1, meaningful current/prior fixturePASS, all changed initializers/
ABI equivalent and actual ESP archive verified. Numeric support not implied.
Mandatory new numeric foundation separates element type/rank/semantics/PCM
representation and explicit Converter. K old-model codecPASS is historical bounded
scope, not new-model acceptance. Numeric writer exclusively edits implementation-plan.md
and numeric-data-contracts.md now; I baseline-only doc update. WholeGate3/goal/new
numeric and Slice5fullhistorycopy/alloc unfinished; hardware/native stack/socket/
timer/replay/serial/slowpeer/partialwrite and final gapmatrix obligations preserved.
Blocked native queue experiment untouched, no source/schema/numeric changes,
userconfig/output/audio/UI/toolinstall/commit. I batch COMPLETE / READY / IDLE.

# N1a numeric foundation integration and legacy policy disposition

2026-10-07. I registered frozen R6 new data_contract.cpp/data_frame.cpp in existing
signal_contracts and ESP source manifest, numeric_contract_tests in portable tests
with assertions/C++20/no exceptions/RTTI. Existing public-header/export/source install
rules retained. Five desktop apps freshly relinked plus DLL deployment; desktop and
portable numeric suite1/1 each PASS0.03s; installed artifact-only numeric consumer
1/1PASS0.02s verifies exact UInt64max, retain/copy/move and owned metadata after donor
mutation, noQt. Existing ESP optimized4096+transports actual new2objects/FULL SDK
archive3actions exit0; archiveSHA78267DA31408EFAC733E68C93BB8B29028C9620FC4C15BE451C7A2B16CED536A.
R6/K14/R4/T17/P7/domain49 frozen inputs unchanged; live Dnumeric/T3 files excluded
from registered closures. Full commands/logs/hash/appDLL/archive/install receipts:
build-refactor-integration/numeric-foundation/integration-report.md.

Coordinator ACCEPTS bounded legacy application policy after full independent
build-gate-legacy-policy-review/review-report.md, no actionable delta findings,
strict current processing/oracles PASS. O1 pre-existing control snapshot typed1/
legacy1 allocation is scope-specific; whole control allocation-free is not accepted.
This supersedes pending legacy-policy review above, not whole application acceptance.

N1a author/I GREEN awaits NEW independent foundation review. Existing apps/domain/
runtime not yet migrated merely by linking new objects. history_row keeps full backing,
retain_frame(history_row) full snapshot; N1b prepared compact gather mandatory.
Numeric DSP/storage/delivery/codecs/registry/runtime/domain pending; CMake assigns no
new wire IDs/versions. All native/hardware/task-stack/final gapmatrix/blocked queue and
Slice5 old fullcopy/allocation obligations preserved. I READY / REPORT / IDLE.

# N1a DSP numeric batch: host GREEN, ESP RED; foundation review FAIL

2026-10-07. I registered frozen D3new cpp/private arithmetic in existing dsp_kernels,
PUBLIC signal_contracts/export dependency, three meaningful noexceptionsRTTI/assert
C++20 tests, ESP source manifest3cpp. Desktop/portable3/3PASS0.09s each; installed
numericconsumer1/1PASS0.03s actual Converter exactInt64>2^53/BE and MaxAbs Int64min→
UInt64 exact2^63, noQt. Five desktop apps actualfreshrelink+DLLdeployment once.
D21 and R6/K14/R4/T17/P7/domain frozen inputs unchanged through final stack checks.

ESP configure0/build1: newkernel_support object compiled, numeric_convert fails
numeric_arithmetic.h std::fenv_t/fegetenv/fesetround/feclearexcept/fesetenv and constants
absent in existing actual Xtensa profile; fetestexcept/FE_INEXACT absent likewise.
No newFULLSDKarchive/no ESPnumeric acceptance. Consolidated log returned via root
to Downer; no sourcepatch/flag suppression/toolinstall or second failed retry by I.
Host actual-O3 individual frames prepareConvert1920/prepareReduce1808/entries1680/
reduceRun1456, R dataFrame max880; excludes libc/callchain/taskpeak.8192reference
reservation never certified ESPbudget. Commands/logs/hashes/sourceclosure/export/
stack/report: build-refactor-integration/numeric-dsp/integration-report.md.

Root reports NEW foundation independent FAIL2P2 (History elementsemantic/axis1;
history_row writable output/source alias mutation), actual report read. Used frozen
R6 known-defective snapshot; N1a/I hostGREEN not acceptance. R6 explicit release
after allchecks allows narrow owner repair/refreeze; later affected integration and
independent recheck required. D source portability repair separately required.
N1b compact row/convertedring, genericHistoryUnsupported, app/domain/runtime numeric
migration and all oldnative/fullcopy/alloc/device/final gates remain open.
I boundedbatch COMPLETE / READY / REPORT / IDLE; no wholeDSP/goal PASS.

# Consolidated repaired R/D and lossless P integration

2026-10-07. Coordinator ACCEPTS bounded R6F1/F2 closure after full independent
rereview-report.md (originalunchangedoraclesPASS) and bounded P3 parserfoundation
after full build-gate-project-source-review/review-report.md (7independentgroups/
9authorgroupsPASS/no findings). No productionnumeric/storage/N1b acceptance implied.
I registered standalone noQt/C++20/noexceptionsRTTI project_storage and test, no
DSP/domainJSONdependency or mandatoryPortableDSPexport. R/D latestrepairedfreeze
and P3 sources integrated; live T3/newdomainprocessing_contracts excluded.

One final desktop/portable R1/P1/D3 targetbatch5/5eachPASS0.19s, installedactual
SDKconsumer1/1PASS0.05s: Rretained UInt64max metadata/lifetime +ConverterBE exact64/
UInt64MaxAbs +positive softwareFloat32MeanRMS with float64capfalse. Fiveappsactual
freshrelinkonce/QtADS DLL andruntimepluginsdeployed. ExistingESPoptimized4096+
transports configure/build0,6actions repairedR2+D3objects/FULLSDKarchive;
SHA C8AC800BD7E827FF3294BEE9E3EC0F4D29E3B167E7CC53FEC36CD2A45940039C size22770864.
Actualtargetprofileprobecompile0/relocatablefullSDKlink0, nohardwareexecution.
Source54fixedreceipts+Dcurrent21 stable; oldDfreeze noauthority. Commands/logs/
hash/export/appDLL/archive/probe/fullsupportmatrix/currentreport:
build-refactor-integration/numeric-consolidated/integration-report.md.

D640bit softwareFloat32MeanRMS nowbothprofiles; nativeIEEE64 modes refuse when
compiledfacilityabsent, existingESP builds without fenvshim. Actualtargetindividual
stackframes prepareConv1728/prepareReduce1792/convert1456/reduce1440/specialized1152/
Rdataframe672B; host1920/1808/1680/1680/specialized1728/R880B. Excludes fullchain/
libc/interrupt/hardwaretaskpeak;8192REFERENCE notcertifiedESPbudget. P small integrated
sample requestedpayloadpeak2494B/final_live0 and WindowsprocessWSpeak4575232B case-only;
payloadledgerexcludes allocatoroverhead/recursionstack, parserdepth128 can129valueframes
subtotal33024B atactualO3. No throughput/saturation/resourceuniversalguarantee.

Previous IESPcompileRED superseded onlycurrentcrossbuildgap; immutableREDlogsretained.
FreshDSPindependentreviewPENDING. N1b compactgather/historyconvertedring, domain/runtime/
numericmapping/protocol/storage/UI/device andalloldSlice5/native/queue/finalgaps remain.
GenericHistoryUnsupported; appslinkage notproductionnumericprocessingclaim; source
installmaycopyliveunregisterednewdomainheaders, nocompiledcontractclaim. No old18/
codec3/control2/fullsuite/nativeexperiment/toolinstall/sourcefix/configoutput/commit.
I consolidatedbatch COMPLETE / READY / REPORT / IDLE; wholegoalunmet.

# Current owning-domain / F3 DSP / Snapshot-repaired DATA integration

2026-10-07. Root FULLread/ACCEPTS bounded domainF1–F4 (rereview-report.md),
DSPF1/F2/F3 (f3-recheck-report.md) and portableDATAF1 (f1-recheck-report.md,
original5976/0+adjacent119/0). R6repair/Pparser boundedaccepted preserved.
I registers newdomainprocessing_contracts.cpp in graph_domain, numericdomain test,
newDATA2cpp in bdsp_protocol/DATA test; sameESPmanifest sources, directoryexports
automatic. No newJSON/DSP/domain/Qtdependency. Live Knumericcontrol/Rnewgather/T3
unregistered; copiedinstalledsources do not establish compiledsupport.

Finaldesktop7/7PASS0.22s/portable7/7PASS0.20s; installedactualconsumer1/1PASS0.03s
Rownerlifetimes/exactUInt64, owningDomainSourceSink/caprefusal/atomicowner+peakbudget,
Converter/MaxAbs exact64/Float32positivefalsefloat64cap, F3exactfenvflag+rounding4modes,
DATAtenmanualpayloadcontracts/UInt64max/contextstereo256=2048+187=2235roundtrip.
Ownreferencefixture initiallyomittedrequiredstamp.sequence (invalid_argument2),
correctedartifacttoacceptedbase sequence7/unchanged2235+limits; no productfinding,
sourcefix/expectedweakening/stalearchive. Onlyconsumerrebuild, no sevenchecksrepeat.
Fiveappsactualfreshrelinkonce+DLL/pluginsdeployment0. ESPconfigure/build0 sevenactions
domain1+DATA2+currentDSP3+FULLSDKarchive SHA
25E7093F8BC9088DE994F0657284AF583B342B0E47D324DC5337D550D19E8310,size25365466.
Targetprobecompile/currentSDKrelocatablelink0, noexecution/hardwareclaim.
Currentdomain4/DATAf1seven/Dfenv21+fixed54 beforeafterstable. Exactreports/logs/receipts:
build-refactor-integration/numeric-boundaries/integration-report.md.

Actualtargetindividualstack maxima domain1360/DATAcodec1120/message560/DSPconvert1728/
reduce1792B, entries1456/1440/spec1152; excludes caller/libc/interrupt/hardwarepeak.
8192REFERENCE uncertified. Domainmetadata/candidatebudget notactualruntimepools;
DATAcallerarena owner/dense logical2235 nottrafficqueueproof; Pledger exclusionskeep.
N1bPreparedGraph/runtime/gather/incrementalring/affectedtransfer/fullhotpathalloccopy,
numericcontrol/protocolsessions/delivery/JSON/UIdevice andoldnative/Slice5/finalgaps
remain. Wholegoal notPASS. I finalcompiledinputsreleasedtoROOT afterreport;
COMPLETE / READY / REPORT / IDLE, no commonNinjaactive.


## 2026-10-07 — I combined V1 composition receipt

Root explicitly expanded I ownership to CoreApplication header/cpp, narrow
presenter persistence/bootstrap guards and the exact U editor constructor guard.
Source integration registers the U numeric editor/helper, P lossless document and
portable legacy adapter, and R gather on existing targets/ESP manifests. Root-owned
source baseline and P registered settings/presentation deltas serve all numeric
save paths; future/malformed numeric files never authorize legacy fallback.

Actual desktop core_app/DLL build and eight targeted V1 tests passed. Application
roundtrip checks autosave/Save/Save As, exact UInt64/opaque lexemes, layout/UI,
empty typed draft, external-file refusal and no Apply. Real save_preset inserts
its first array; extending an existing array explicitly refuses without a disk
write. That limitation remains open for independent V1 review.

Portable changed closure (no exceptions/RTTI) + two targeted tests passed;
fresh installed consumer without Qt passed. Actual ESP archive linked after P
fixed three misleading-indentation warnings, with no warning suppression. I test
fixture compile/oracle corrections are documented, not presented as TDD RED.

Authoritative compact report and 40-input/artifact hashes:
[combined integration report](../../build-refactor-integration/v1-composition/integration-report.md).
P helper final 80764DFA34CA7A77…; document A8132C21C3F2F35C….
All six R checkpoint hashes matched at release (runtime A734F89DD986DA2B…,
gather 324EC57A48AD6492…). R released after actual build/test/hash receipt;
subsequent live R source is outside compiled evidence. Fresh V1 acceptance,
full V2 execution bridge, native/hardware and full-goal checks remain pending.
No user config/output, branch, commit, broad UI/FPS/soak or speaker playback.

## Canonical numeric FINAL integration — 2026-10-07

I integrated current whole owner blocks and refreshed production desktop, installed portable SDK and actual Xtensa SDK/NVS/stock TCP main archive + UART main object. Canonical full CTest44/44 PASS7.78s; latest RU/EN editor refresh1/1PASS; final incarnation presenter/renderer relink0 and affected application/rejected/editor/workspace4/4PASS3.31s. Silent eight-tone CLI750ms exit0. External installed find_package consumer build/run0. Native effective C++20 independently verified after overriding later IDF6.1 std26 per canonical component.

Current requirement-to-evidence mapping and acceptance boundaries:
[combined integration report](../../build-refactor-integration/numeric-combined-integration-report.md).
Architecture independent A1–A5 CLOSED bounded scope. Runtime/device and desktop independent final reports still pending at this entry; selected-device lower-epoch restart repair compiled and targeted independent oracle active. Whole package acceptance belongs to coordinator, not author PASS. No native hardware/stack/flash/WLED timing/RSS proof inferred.

Final SHA256 artifacts:
- core_app.exe:5F14A6FF9226103054112D270B6F930C86E32CFCCE8F390C87ACF1396886D7E6
- pipeline_cli.exe:F642BCD3FDE248F278853ECB0463C0545175DD45D36A3C24E562AFE09920F9BB
- aggregator_server_app.exe:0B7639470FFCA2D8A04C3483C255C56F874168D3A45085F1A75A42C63C840BA4
- native libportable_sdk.a:21CDB4045BAD97D2426D078E10BA8C1AAF5F89CBE74E28CB32B487D30426D5A2
- native libmain.a:87382B5853EB064A21F6CF46265E76C5706BF050E324BA34CBDA49B59670B2DC

No user config/output replacements, branches, commits, ZIP or installs of tools.

FINAL closure addendum: independent desktop and runtime reviewers both compiled/ran current fake presenter incarnation oracle0/failures0; lower service epoch/same graph, observed or coalesced retire, first Frame/History before snapshot, compatible revision without clear. Exact oracle retained in tests/numeric_presenter_incarnation_tests.cpp and registered; final build0/CTest1of1PASS0.11s. Current45registered tests have original44batch +postrepairaffected4 +newregression1 evidence. Independent final reports architecture A1–A5 closed, runtime/protocol/storage/device and desktop no unclosed reproduced assigned findings, each bounded with hardware/native gaps. Whole final acceptance remains coordinator action.

## Compact FINAL integration addendum — 2026-10-07

Current production build0 desktop/portable SDK install/external consumer/native Xtensa SDK+NVS+TCP archive/UART object. Full desktop44/45 log preserved; only failing fixture's server codec mask repaired (no production change), targeted build0/1of1PASS0.25s closes registered45-case evidence. Latest actual-main component build0/6of6PASS1.09s includes stricter MAX physical32KiB stack guard. Silent8tone CLI750ms exit0.

Ordinary full stereoFFT2048 eight-node graph + BOTH CONTROL + TCP/UART +128ticks/StopStart/ExplicitPersist/reboot PASS in supplied320KiB host budget. Declared working ledger326522 <=327680; atomic replacement rejects exhausted preserving working state. Explicit1024KiB profiles retain replacement/FFT4096 PASS. Compact independent architecture/SDK and runtime/protocol/device reports scopedPASS/no open actionable finding; historical desktop bounded acceptance preserved. Board actual heap/allocator/task-stack/flash/native graphics unverified. Whole acceptance remains coordinator action. Requirement-to-evidence mapping: [current combined report](../../build-refactor-integration/numeric-combined-integration-report.md). Prior artifact hashes remain historical; current snapshot below.

Algorithm Hash                                                             Path
--------- ----                                                             ----
SHA256    2846F645D964EBA05AFFEAB6E82CF3EAC2954B05464D2A971DED6004127A5204 C:\Users\Koba\CLionProjects\qt-visualization…
SHA256    A9575DB1AB46A3C1DE55946231421B5BDBE05B63D9692C9575BCD8208DC19DA6 C:\Users\Koba\CLionProjects\qt-visualization…
SHA256    21F61EF571829219B0F2E670A37D540E39DB3138C7EE4C39373FE986CAA1C8AB C:\Users\Koba\CLionProjects\qt-visualization…



Desktop delivery refreshed to build-gui/bin: three application SHA256 match current built binaries;16 DLLs+16 plugin files hash-identical,0 mismatches. Evidence numeric-compact-deployment-hashes.json/numeric-compact-deployment.log. No config/output copied; no application kill needed. Current compact independent verdicts and native build now reflected in embedded documentation.
