# Независимый первичный review: test fixtures и capture injection

Дата: 2026-10-06. Repository HEAD при старте: `0387743d734b34f6c8bd86e0f84032dc3e2206e6`; baseline: `3de4de8`. Проверялся реальный dirty working tree, включая untracked `tests/synthetic_capture.h`, а не только HEAD.

## Решение в назначенном scope

Открытых P1/P2 в synthetic capture factory, composition injection и включении assertions не обнаружено. Найденный P2 потери first-run coverage исправлен координатором и независимо проверен. Этот вывод — первичный QA slice; он не означает успешную общую GUI/full-short приёмку или финальную трёхчатовую приёмку.

Прочитаны AGENTS.md и весь `docs/clean-architecture-ddd-reference.md`, сверена ссылка на pinned reference: https://github.com/ArthurKoba/ai-agent-workflow/blob/9bc1b1ff27f7204fa946253a4d5612fd80102e8f/skills/software-engineering/clean-architecture-ddd.md. Reference использован как справочник границ, не как authority проекта. Авторский `docs/test-fixture-audit.md` и переписки до собственных выводов не читались; авторские результаты не использованы как acceptance evidence.

## Найденный и исправленный P2

**P2: migration убрала missing/default configuration bootstrap из автоматического startup smoke.** Первоначально `tests/CMakeLists.txt:40` регистрировал `core_startup_shutdown` с `--config tests/fixtures/test-signal.json`, как `silent_gui_pipeline`. Существующая first-run ветка `tests/core_smoke.cpp:106` включается только через `QT_VISUALIZATION_EXPECT_FIRST_RUN`, а этот environment нигде не регистрировался. Поэтому две автоматические проверки шли через TestSignal config; сбой создания fresh default AudioSource workspace мог остаться незамеченным. Синтетическая injection уже позволяла сохранить исходную цель без физических устройств.

Probe: свежий собственный CTest manifest + поиск `EXPECT_FIRST_RUN` по tests/scripts/.github. Исправление координатора: `tests/CMakeLists.txt:40–41` теперь запускает `core_smoke` без config args и включает `QT_VISUALIZATION_EXPECT_FIRST_RUN=1`. Ветка создаёт config в QTemporaryDir и использует injected factory (`tests/core_smoke.cpp:107`); default AudioSource остаётся source=0. `silent_gui_pipeline` отдельно сохраняет TestSignal fixture и EXPECT_TEST_SIGNAL.

После fresh reconfigure/build: **core_startup_shutdown PASS, 1.78 s**. В собственном LastTest.log присутствует: `Fresh configuration: default source with synthetic adapter, Analyzer running, Spectrogram above Pipeline`. Замечание закрыто.

## Проверка реализации и сохранения целей

- `tests/synthetic_capture.h:10–22`: все source kinds идут через `make_test_signal_source()`, без ветки к `default_audio_source_factory`/Miniaudio. `_kind` сохраняет SystemOutput/Microphone/TestSignal identity. Diagnostics честно описывают in-memory generator. Configure, status и lifecycle handlers передаются worker; stop делегирует join/cancellation worker. Destruction unique_ptr вызывает деструктор TestSignalSource с stop/join.
- `tests/synthetic_capture.h:18–19`: suppress-audio удаляет только audio handler. Worker запускается и сообщает готовность; manual feed сценарии pipeline/runtime/workspace остаются изолированными от автоматически произведённых PCM. Синтетический сигнал — существующие восемь меняющихся тонов без playback.
- Production composition seam: `apps/desktop_gui/src/core_application.cpp:59–61,117–118` → пятая factory в `GuiPresenter` → factory overload `EngineClient` → `AudioPipeline`. Presenter хранит factory и повторно использует её при возврате к локальному устройству. Remote constructors продолжают подключаться к remote endpoint; локальный capture engine в них не создаётся.
- Production defaults сохранены: обычный CoreApplication delegates empty factory; обычный EngineClient delegates empty factory; `AudioPipeline` заменяет empty factory на прежнюю default production factory. `default_audio_source_factory` продолжает выбирать generator только для TestSignal, остальные kinds — Miniaudio. Не вводились зависимости Qt/miniaudio в переносимые domain/application порты ради теста; seam находится в desktop composition/adapters.
- Injection diff сохраняет default AudioSource documents и проверки маршрута/контрактов. `default_runtime_start` независимо проверяет запрос именно SystemOutput. Remote engines в migrated fixtures также injected. Callback thread assertion в engine_transport усилена; сравнение `views == views_before_block` сохраняет цель отсутствия callback во время заблокированного owner event loop. Readiness ожидания capabilities и Stop snapshot проверяют состояние, а не только появление placeholder/пакета.
- Inspector assertion в editor_tests изменяет ожидаемое collapse поведение и добавляет явную проверку восстановления divider click. Это отдельная UI семантика; hard assertion не удалён. Её соответствие UI требованиям не является задачей данной capture migration приёмки.

## Изоляция автоматического short inventory

Свежий независимый configure с `ENABLE_DEVICE_QA=OFF`, `ENABLE_NATIVE_QA=OFF`: **63 registered tests**, без `capture_lifecycle`, native render или soak registration. Native executables могут собираться, но не входят в automatic CTest inventory.

Source audit зарегистрированных desktop сценариев, включая indirect Apply/AutoApply, не обнаружил открытие физического источника:

- CoreApplication tests и local Presenter tests, способные применить AudioSource graph, используют injected synthetic factory; workspace child processes запускают тот же injected test executable.
- GUI CLI help/version/invalid arguments завершаются при parsing до создания Presenter/capture; schema subprocess запускает injected core_smoke.
- graph CLI children используют `--synthetic`; CLI останавливает capture/analyzer до Apply и разрешает source overrides до явного старта.
- console configuration children выбирают TestSignal graphs/presets или NetworkInput-only UART graph. Invalid selections завершаются до capture startup.
- distributed CLI children используют TestSignal либо NetworkInput graphs; remote StartCapture адресован TestSignal source. Remote fixture engines injected.
- Оставшиеся default-constructed AudioPipeline instances в graph_preparation, typed_graph, desktop_serial_stream, detached transport probe и TestSignal tests не применяют физический producer: это NetworkInput/manual feed/TestSignal либо только analyzer startup. Constructor сам capture не открывает.
- qt_workspace_resize создаёт Presenter с default graph, но не initialize/Apply; node model/editor geometry не выполняют DSP. Editor default-source interactions используют scripted remote peer; local editor fixture bootstrap — TestSignal.

Это source audit плюс короткие выбранные исполнения; весь process tree всех 63 тестов инструментально не трассировался и полный suite мной не запускался.

## Независимые исполненные проверки

Собственный каталог: `review-test-fixtures/`, единственный мой Ninja build: `review-test-fixtures/build`. Release, Qt 6.10.2 MinGW, GCC 13.1, собственный fresh configure. Общие build-gui/Ninja каталоги не использованы. Third-party source directories использованы read-only из существующего dependency checkout; сборочные outputs полностью отдельные.

1. Сборка `core_app`, `pipeline_cli`, fixture probe и выбранных test executables — PASS на первичном snapshot; `core_smoke` дополнительно собран после исправления регистрации.
2. Собственный QCore-only `review_fixture_probe` — PASS: identity всех трёх kinds, configure/amplitude/PCM metadata, status/lifecycle ready и producer-error forwarding, handler removal/replacement, stop cancellation, restart, destruction join, suppress-audio, EngineClient factory invocation, Presenter bootstrap Apply при run flags=false. На обновлённых параллельных production sources fixture probe повторно PASS (exit 0).
3. `independent_pipeline` — PASS 2.43 s.
4. `pipeline_graph` — PASS 1.86 s.
5. `engine_transport` — PASS 9.05 s на исходном snapshot до нового unrelated subscription RED case.
6. `console_config_selection` — PASS 1.51 s.
7. `default_runtime_start` — PASS 0.46 s.
8. Исправленный `core_startup_shutdown` — PASS 1.78 s, конкретный offscreen first-run case.

Пять non-GUI CTest cases: 15.33 s, 0 failures. Все **58 test translation units** в own Release compile database имеют `-UNDEBUG` после `-DNDEBUG`; без override — 0. Собственный probe дополнительно содержит compile-time error при NDEBUG. Assertions в Release не выключены; production библиотеки сохраняют обычные Release flags.

## Fresh source proof и границы

Локальные evidence: `review-test-fixtures/source-proof-before.json`, `source-proof-after.json`, `source-proof-final.json`, `source-proof-latest.json`, `assertion-flags.json`, `ctest-manifest.json`, `probe-result.txt`, `probe-final-result.txt`, `targeted-tests.log`, `first-run-test.log`, `binary-proof.json`. Probe source и CMake hook также сохранены в этом каталоге.

Во время первичной сборки synthetic_capture, CoreApplication, Presenter, EngineClient, AudioPipeline и TestSignal sources оставались неизменными по SHA256; tests/CMakeLists изменился по принятому P2. Последующие параллельные application repairs изменили production sources и добавили local Subscribe(0) RED case в engine_transport. Fresh rebuild нового case независимо дал failure `Queued local view escaped Subscribe(0)`, exit 1. Это отдельный application subscription gate, переданный координатору; исходный PASS не выдаётся за PASS нового executable. Начатый дополнительный собственный queued-unsubscribe probe после параллельного production repair вернул 0 callbacks/exit 0, но дальнейшую application acceptance и повторный полный engine_transport gate выполняет его назначенный reviewer после owner freeze.

Другие общие GUI failures и их исправления принадлежат координатору/назначенным reviewers; не использованы здесь как мои независимые выводы. По запросу не выполнялись физический capture, native GUI, screenshots, long/soak tests, Linux, полный suite, subagents, создания чатов, commits/branches. Пользовательские config.json/output не менялись.

**Готовность:** назначенный injection/assertions slice и закрытие P2 first-run подтверждены. Общий working-tree/global GUI gate и финальная приёмка остаются открытыми вне этого slice; первичный отчёт не подменяет их.

