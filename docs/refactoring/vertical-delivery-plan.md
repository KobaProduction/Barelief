# Актуальная организация вертикальной поставки

Дата: **2026-10-07, Europe/Moscow**. Текущий прямой запрос пользователя:
автономно завершить новый canonical numeric продукт. Этот порядок заменяет прежние
промежуточные TDD, compatibility gates, freeze/receipt loops и микроконтроль.
Общий scope — [implementation-plan](implementation-plan.md), статусы —
[progress-checklist](progress-checklist.md), native UI — [codex-plan-tracking](codex-plan-tracking.md).

## От общего к частному

Каждый владелец самостоятельно завершает весь назначенный функциональный блок:
реализация → подключение → сборка у I → независимый аудит готового блока →
исправления владельцами. Новые тесты, адаптация полезных тестов новой системы
и согласованные полные проверки выполняются на FINAL после общей интеграции.
Промежуточные TDD/characterization и повторные авторские тестовые циклы отменены.
Уже выполненные проверки сохраняются как исторические факты, не становятся
обязательными повторными gates и не объявляют готовой всю систему.

Один итоговый отчёт владельца: сделано, точки подключения, реальные ограничения;
либо один неразрешимый в его границах блокер. Промежуточные успехи и status spam
координатору не отправляются. Общий API или конфликт владения согласуется напрямую
с соответствующим владельцем одним конкретным сообщением. Координатор организует
работу, не утверждает каждую функцию и не реализует продуктовый код.

Единственный путь — canonical numeric. Старые режимы, конфиги, схемы, wire/protocol,
legacy adapters, migrations, inverse/downgrade и schema1 goldens исключены из текущих
обязанностей. Совместный P/R grant legacy helper отменён. Владельцы могут узко убрать
устаревшие реализации и legacy-only тесты; I регистрирует изменения targets.
Архивы, пользовательские config.json/output и тесты массово не удаляются.
Сохраняются новые format versions и reject будущего/некорректного формата,
один JSON, lossless exact numeric types, atomic save/apply, revision/session ownership,
admission и bounded memory. Типовая совместимость новых портов остаётся обязательной.

## Пакеты и текущий статус

| Пакет | Результат нового продукта | Текущий статус |
| --- | --- | --- |
| V1 | Numeric редактор/versioned JSON/projection/presets | Completed: software scope integrated и accepted root |
| V2 | Canonical runtime/application/Converter/DSP/routing | Completed: current compact implementation/integration/independent acceptance |
| V3 | History/count/ownership/renderer/reconnect | Completed: current software scope accepted root |
| V4 | Control/DATA/device admission/NVS/SDK | Completed в host/fake/crosscompile software scope; physical/manual UNVERIFIED |
| FINAL | Current integrated software + independent compact amendments + coordinator acceptance | Completed: root accepted all mandatory software deliverables; manual/hardware отдельно |

Все mandatory software deliverables и current compact independent evidence приняты root; whole V1–V4/FINAL completed, hardware/manual проверки отдельно.

## Итоговая software приёмка — completed

**2026-10-07, Europe/Moscow. Root FINAL ACCEPTANCE:** все обязательные software
deliverables актуального canonical scope приняты; actionable findings отсутствуют.
V1, V2, V3, V4 и FINAL completed. Аппаратная/native приёмка этим решением не заявлена.

Root полностью прочёл текущий
[I combined report](../../build-refactor-integration/numeric-combined-integration-report.md),
оба independent compact amendments к
[architecture/SDK report](../../build-final-architecture-review/review-report.md) и
[runtime/protocol/storage/device report](../../build-final-runtime-protocol-review/review-report.md),
FFT/ownership/remote Persist fixtures/logs и deployment hashes. Прежняя
[desktop independent acceptance](../../build-final-desktop-review/review-report.md),
включая P1 incarnation closure, сохранена. Контракты/runtime/control/storage/Qt/embedded/
FINAL сверены с этой evidence и latest compact.

| Проверенный результат | Итоговый evidence scope |
| --- | --- |
| Тесты | Full batch44/45 сохранил RED; test-only fix focused1/1 author0.25s и independent0.14s. Итого45 cases verified across batches; полного45/45 log нет. Registered session independent1/1 и current component6/6 PASS |
| Desktop delivery | Actual current deployment build-gui/bin: 3 exe hashes совпали, 16 DLL +16 plugins, 0 mismatches |
| SDK/native | SDK install consumer accepted; current Xtensa C++20 archives/UART object build0 и независимая перепроверка accepted в software/crosscompile scope |
| Compact default | Full eight-node stereo FFT2048 TCP/UART, BOTH CONTROL, ExplicitPersist/restore accepted на host/fake resources с supplied320KiB; исчерпанный old+new replacement честно rejected атомарно |
| Extended profile | Explicit1024KiB FFT4096 и atomic replacement accepted; бюджет не подразумевает наличие такой памяти на обычной плате |
| Runtime/codecs | R compact ownership/runtime и K exact measure affected integration + independent analytic FFT/ownership/remote persistence accepted |
| Решение | Все mandatory software deliverables приняты root; whole V1–V4/FINAL completed, физические/manual проверки отдельно |

Проверенный default: CONTROL wire4KiB, primary arena8KiB, packet1024, read chunk512.
Shared cold workspace разделяет decode/NVS phases; immutable capabilities и exact
BOTH reply preflight предшествуют preparation. Actual supplied memory ограничена
configured ceiling; PSRAM не является обязательной portable зависимостью.
Host ledger326522 <=327680 bytes включает full32KiB stack, cold owner/DTO snapshots;
1158 bytes headroom — ledger, а не измерение heap платы.

## Morning/manual проверки — UNVERIFIED

- [ ] Physical heap/allocator/contiguous memory/stack на реальном ESP32.
- [ ] Firmware ELF/link, flash, power-cut NVS, реальные UART/TCP и WLED timing.
- [ ] Native GUI/OpenGL/Light-Dark-System/themes/popups и physical capture.

Host/fake-device/crosscompile/offscreen PASS не закрывают эти пункты.
Коммиты/push/новые установки и замена пользовательских config/output не выполнялись.
## Владельцы и границы

| Владелец | Ответственность |
| --- | --- |
| R | Назначенные runtime/gather/history и application processing_control/graph_lifecycle headers/sources; ready mutable implementation общего numeric API |
| P | Назначенные project_storage, desktop graph/config/numeric_project_document adapters; canonical storage cleanup без legacy helper |
| G | Назначенные numeric ports/renderer/lifecycle и четыре spectrogram graph/modifier metadata файла; U/R файлы не передаются |
| T | Назначенные desktop control mapper/session; разрешённые новые numeric_remote_endpoint и numeric_control_server h/cpp в desktop control include/runtime |
| E | Назначенные embedded numeric admission/persistence NVS и canonical device |
| K | Numeric CONTROL BOTH CBOR/Protobuf и назначенный Native descriptor fix; DATA4 Native bytes refusal сохраняется |
| U | Editor и назначенные RU/EN captions; прежний editor block сохраняется, соседние файлы не передаются |
| I | Local engine/GUI/CLI composition roots, все CMake/registration/manifests/export/install, SDK и единственный shared Ninja; baseline-and-ownership |
| Root | Организация работы, статус крупных результатов и независимая итоговая приёмка |

Точные grants имеют приоритет над названиями каталогов. Параллелизм определяется
владением, ресурсами и конкретными API-зависимостями. I согласует только короткое
необходимое окно чтения действительно меняющихся compiled inputs; группа не стоит
на общем freeze. Новые рабочие чаты без реальной причины не перезапускаются.
Три FINAL reviews и latest compact amendments приняты root; I shared build/integration завершены в software scope. Координаторская финальная software приёмка завершена.

## Приёмка готовой системы

I подключает результаты и собирает actual desktop и SDK в доступном окружении.
Независимое code review готового крупного блока сохраняется; findings исправляет
владелец. FINAL после общей интеграции включает новые тесты и сохранённые полезные
oracles, согласованные полные проверки и итоговые независимые architecture,
runtime/protocol/storage, desktop/UI/device reviews. Author GREEN не равен acceptance.
Offscreen/host archive не доказывает native GUI/hardware/task-stack; реальные
непроверенные ограничения остаются явными. Без установок, веток, коммитов, push,
новых документов на микрозадачи или Markdown UI fallback.

Опорные контракты: [numeric-data-contracts](numeric-data-contracts.md),
[foundation](numeric-foundation-handoff.md), [DSP](numeric-dsp-handoff.md),
[domain/runtime](numeric-domain-runtime-handoff.md). Их старые compatibility/process
пункты не расширяют актуальный scope. Каждый новый implementation/review чат читает
AGENTS.md и полностью
[закреплённый Clean Architecture / DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).

## Историческая evidence, не текущие gates

### Исторический compact snapshot до окончательной приёмки

Все whole owner implementation блоки R/P/U/G/T/E/K готовы. Desktop P1 закрыт
независимой приёмкой; все три прежних FINAL review приняты root только в назначенных
bounded границах. Оставшийся реальный блок — compact ESP32, затронутая интеграция
и независимая перепроверка текущего состава. Whole V1–V4/global не completed.

| Направление | Актуальный результат | Что осталось |
| --- | --- | --- |
| Прежняя architecture/SDK приёмка | [FINAL report](../../build-final-architecture-review/review-report.md) принят root: A1–A5 closed, installed SDK/consumer/source/crosscompile в перечисленном scope | Новый compact R/K/T состав требует affected build/SDK/native refresh и независимой перепроверки |
| Прежняя runtime/control/storage/device приёмка | Official FINAL report прочитан root; bounded functional/fake-device scope и latest incarnation приняты | Runtime reviewer сейчас выполняет независимые текущие binaries и analytic FFT oracle для compact ESP32 |
| Desktop приёмка | [Amended FINAL report](../../build-final-desktop-review/review-report.md) принят root; P1 same-device incarnation закрыт, bounded current reconnect/UI/editor/presentation/storage scope принят | Native UI/hardware ограничения сохраняются; прежнее закрытие не подменяет независимую проверку нового compact состава |
| Compact ESP32 I author integration | I active turn: full stereo FFT2048/320KiB по TCP и UART + BOTH CONTROL + ExplicitPersist/restore author PASS; replacement under320 честно rejected без mutation; extended4096/replacement проходят | Implementation/integration evidence, НЕ independent acceptance; runtime current binary/analytic oracle verdict и обновлённые I SDK/native evidence ожидаются |
| R/K compact реализации | R compact runtime ready; K exact Codec::measure BOTH canonical wire/scratch/arena/aggregate text, noalloc/no writes/aligned decode footprint и arena_bytes enforcement ready | Affected final build/rereview не закрыты; прежняя codec acceptance относится к прежнему snapshot |
| Итоговое решение | Автономные владельцы, I единственный shared build owner; независимые проверки текущего compact состава идут | Координаторская whole/global приёмка после current integration и независимых verdict; whole checkboxes открыты |

2MiB external storage нельзя предполагать на обычном ESP32. Actual capabilities,
memory/storage, atomic rejection при нехватке и full2048 default profile остаются
требованиями. Малый RAW-only профиль не заменяет full pipeline; scope не сокращён.
Native hardware/heap-stack/firmware-link proof остаётся UNVERIFIED, host/fake-device/
objects/crosscompile evidence не расширяется до аппаратной приёмки.


### История предыдущих статусных снимков (ненормативна)

| Направление | Evidence / назначение | Граница статуса |
| --- | --- | --- |
| I combined integration | Root fully read [combined report](../../build-refactor-integration/numeric-combined-integration-report.md): core_app/pipeline_cli/registry build0; portable C++20 no exceptions/RTTI SDK build/install + installed canonical consumer run0; silent CLI 8 tones/750ms exit0; registry metadata + actual CONTROL discovery/disconnect 2/2 PASS | Current desktop/U preview built and bounded accepted; latest R affected rebuild/SDK refresh, WLED wiring and stock ESP NVS restore/retire pending; whole acceptance pending |
| Independent architecture/SDK | Root fully read UPDATED [final report](../../build-final-architecture-review/review-report.md) after official FINAL ready: assigned bounded architecture/source/installed SDK/crosscompile ACCEPTED; A1–A5 CLOSED, including native effective C++26 override repaired to C++20 | Bounded runtime/control/storage/fake-device scope accepted; desktop bounded closure accepted; ordinary ESP32 no-external full2048 completion gap pending; whole V1–V4/global unaccepted |
| Independent runtime/protocol/storage/device | Root fully read official FINAL review report + reconnect log failures0/persistence54/affected4/4 current PASS; listed bounded runtime/control/storage/fake-device scope ACCEPTED; latest incarnation independently PASS | Desktop bounded closure accepted; whole global completion unaccepted pending ordinary ESP32 profile audit |

| Independent desktop/UI | Root read amended FINAL report current requirements/findings/evidence; reviewer completed; bounded P1 reconnect closure ACCEPTED: current presenter/adapter oracle compile0/run0 and independent copied/rebuilt runtime0 | All three final reviews bounded ready/accepted; global completion remains open for actual ordinary ESP32 profile/completion audit |
| Desktop current reconnect closure scope | Same graph/lower epoch, observed/coalesced retirement, first Frame/History before snapshot, compatible revision no clear; current affected4/4 author + independent app normal/rejected/editor/workspace + engine8tones current libraries successfully reread | Accepted only listed current bounded desktop scope; previous snapshot logs unchanged |
| Current completion audit | Three independent final reviews bounded accepted; I retained new test1/1 ready report read; I latest turn active memory investigation | FULL global completion open only actual ordinary ESP32 profile/completion requirements; whole packages not checked complete |
| Desktop accepted execution scope | Original adapter/engine oracles; current editor/model/literal; 489 presentation checks; 13 axes checks; offscreen workspace; application/rejected/presets; pure WLED policy — 12 ledger entries PASS | Only listed bounded source/execution scope accepted |
| Desktop evidence limits | Native Light/Dark/System/popups/3D, physical capture, multidock A-B-A, real-transport WLED timing/RSS, ESP hardware UNVERIFIED | Offscreen/pure policy PASS does not close these scenarios or whole packages |
| Current audit/FINAL scope | E canonical persistence/admission/session/NVS: own 3 target builds0, CTest3/3 PASS0.25s; BOTH persistence/fake NVS restore/exact64/FFT profiles/packet fragmentation/noheap covered; independent current rerun pending | FINAL active; stock ESP default NVS/boot restore/selective retirement у I active; whole packages unaccepted |
| Embedded evidence | Actual Xtensa archive predates latest R/E, refresh required; host fake-IDF UART/TCP compile0; E new tests author PASS scoped | Independent current device rerun and stock ESP composition pending; not hardware/task-stack proof |
| T repairs/seams | Remote retirement/per-port unavailable/queued generation fences ready; I registry2/2 PASS; pure WLED policy + discard_outbound_data selective boundary seam ready | I actual WLED wiring/final checks pending |
| Independent findings | Architecture A1–A5 independently CLOSED; exact installed72 canonical/36 public/11 composition; RAW/2048/4096 consumers zeroalloc512ticks; History140 exactly1row/noalloc; exported find_package consumer exit0 | Accepted only assigned architecture/SDK scope; bounded runtime/control/storage/fake-device accepted; desktop bounded closure accepted; ESP no-external completion gap pending |
| Current FINAL automated checks | Root read actual numeric-final-all-ctest.log: 100% passed, 0 failures/44 tests, 7.78s в предыдущем snapshot; I confirms current actual desktop build and stock host RAW/spectral2048/4096 within existing plan/stack bounds | Previous snapshot host/test PASS; новые desktop changes не покрыты прежними44/44, whole closure pending |
| Current final refresh | Actual57-step Xtensa SDK/NVS/TCP/UART objects build0; independent rules/elf32 verified in updated accepted architecture report | Crosscompile accepted in assigned scope; native hardware/heap-stack/firmware link remain unverified; no global closure |
| Revised R owned History API | NodeId/OwnedDataFrame/DeliveryStamp/callbacks/optional ordinal/shared arena owner; DATA4 ordinal required, borrowed History rejected | I + runtime/architecture reviewers notified; current integration and independent closure pending |
| U FINAL editor evidence | Root read numeric-final-editor-ctest.log: editor_tests/model/literal exactly3/3 PASS0.40s. Current I desktop build integrates numeric_frame/History preview bridge, bounded exact text, shared endpoint union, hidden/draft clear, fences, RU keys; author covers real QtNodes clipboard/undo/exact64/save/reload/preset/apply/preview/lifetime/live palette RU/EN | Previous bounded desktop PASS retained for its snapshot; current desktop P1 REOPENED; no whole V1 PASS |
| P FINAL storage evidence | P whole owner ready; canonical-only fixtures/commands handed reviewers/I. Root read numeric-final-storage-ctest.log: document PASS0.19s, configmanager PASS0.53s, WLEDpolicy PASS0.02s; application/rejected first TIMEOUT missing offscreen plugin | Это не whole5/5 PASS исходного CTest log; independent full review pending |
| P unchanged-binary retry | Root read final-application-retry.stdout.log and final-rejected-retry.stdout.log: canonical application/presets/restart PASS, rejected startup PASS при installed QT_PLUGIN_PATH | I repeatable CTest environment fix active; old timeout не переписан как PASS |
| ESP FINAL completion gap | Ordinary ESP32 no-external profile gap routed I/E: fixed CONTROL1120KiB + adapters64KiB + stack32KiB exceeds ordinary internal RAM before processing | Honest actual profile/result required; fake2MiB/low-heap reject/source SDK PASS does not prove default full2048 without PSRAM; no-external proof pending |
| Independent storage/editor composition | Root fully inspected storage_oracle.cpp + storage_oracle.run.log: 100 checks/0 failures; all10 dtype/rank0..2 roundtrip, exact64/NaN bits/-0 parameters, unknown raw lexemes, draft/applied separation, invalid/future/duplicate-version rollback, atomic external file conflict refusal, stable preset reorder/name/unknown preservation | Accepted only bounded independently passed storage/editor composition; whole V1/global acceptance pending |
| Independent desktop storage runs | Root read run-application.log, run-application-rejected.log, run-presets.log: all PASS on copied current binaries with installed offscreen | U inline preview/theme/ports/workspace author coverage recorded; previous bounded desktop PASS retained for its snapshot; current desktop P1 REOPENED; whole V1/global reports pending; whole package checkbox remains open |



V1: [composition report](../../build-refactor-integration/v1-composition/integration-report.md);
[presets report](../../build-refactor-integration/v1-presets/integration-report.md) и
[ctest.log](../../build-refactor-integration/v1-presets/ctest.log): actual build0,
save_preset FAIL98 expected2/read1, rejected/config PASS2. Последующий P visual3 repair
author-ready по final-source-sha256.txt, actual RED → focused GREEN; I closure ACK.
Новой whole V1 acceptance это не даёт. Очередь промежуточных unchanged oracles отменена.

V4: [mask-rereview](../../build-gate-numeric-control-review/mask-rereview/review-report.md)
bounded BOTHcodec принят: P2 closed, 66 checks/0 failures, 11-file snapshot unchanged.
Это не whole V4/SDK/hardware acceptance. Поздний Native CONTROL Unsupported и новый
K fix рассматриваются в текущем составе отдельно. Исторические FAIL/PASS не переписаны.