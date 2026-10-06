# Независимое первичное ревью Qt integration

Текущий результат: **READY в согласованном Qt/offscreen scope — P2 закрыт
независимым повторным probe**. Поздний valid TypedViews после подтверждённого
Stop отвергается; Recovering unavailable marker сохраняет доставку. Последний
раздел содержит актуальный snapshot и narrow verification. Ниже сохранён исходный
RED и первоначальный отчёт; native graphics/hardware acceptance не заявляется.

## Snapshot и границы

- HEAD: `0387743d734b34f6c8bd86e0f84032dc3e2206e6`.
- Baseline: `3de4de8`; проверен actual dirty source, а не только HEAD diff.
- SHA256 scoped source manifest: `aeabd031071f8d47af9458cf44e935b58a752a6025f03f96e719064ca0e61d9b`.
  Manifest: `review-qt-integration/source-manifest.sha256`; включает Qt presentation,
  GUI composition/configuration, затронутые fixtures/CMake и EngineClient boundary.
  Это идентификатор проверенного среза, не Git commit всей dirty migration.
- Git blob hashes: presenter `f822a6ddef8a0f6ea10fac75a7a8d46a3684dde4`,
  dock workspace `4a27a69d9d1df8da09903271412e24889849937b`,
  EngineClient `5062a456e27aea30988a57408a17f0a86fb512e8`,
  ui fixture `b80c2d067cf6e9ba91e23df98502e46952eb2f26`,
  workspace fixture `8d49edefc8ac961789c78426f92dbc55638b4162`,
  GUIViews fixture `36c9b9ef963d0d3105dfbf45e42db774c5f50781`.

Прочитаны AGENTS.md и полностью обязательные clean-architecture-ddd-reference.md,
architecture.md, architecture-map.md, network-stream-v2.md. Выводы основаны на
исходниках, сравнении с baseline и собственных процессах в отдельном build.
Другие review-файлы и переписки не открывались; присланные авторские результаты
не использованы как доказательство. Portable/DSP/prepared allocation не ревьюировались.
Production и общие tests не изменялись; собственный probe и логи находятся только
в отдельном `review-qt-integration`, отчёт — в этом файле.

## Исходный P2/RED: TypedViews может вернуть pre-Stop значения в stopped GUI

Место: `adapters/desktop/runtime/src/engine_client.cpp:656-658`;
presentation boundary `presentation/qt/src/presenter/gui_presenter.cpp:145-147`.

Ветка packet 17 проверяет subscriptions, session epoch и graph revision, но
принимает данные при `Analyzer == Stopped`. Presenter повторяет только epoch/revision
check и эмитит `node_views_ready`. `accept_snapshot` правильно очищает views на
Stopped, однако эта очистка не блокирует последующую доставку. Session epoch и
graph revision остаются теми же при Stop, поэтому сохранённый до Stop пакет
проходит эти проверки. В legacy packet 8 есть дополнительная проверка Running.

Собственный воспроизводитель: `review-qt-integration/own_qt_probe.cpp`.
Он создаёт реальную CoreApplication с injected synthetic capture, реальный
AudioPipeline/EngineServer и remote GuiPresenter по loopback. Сохраняет один
реально полученный FFT frame и encode-ит valid packet 17 по существующему codec.
Затем останавливает server capture/analyzer, ждёт stopped snapshot и empty chart,
проверяет ещё 100 ms отсутствия обычного callback revival. После этого через
реальный server TcpBDSPSocket отправляет сохранённый пакет. Parser и presentation
callbacks не подменяются.

На актуально пересобранном snapshot:

```text
OWN real externalStop / quietAfterStop PASS
OWN delayed pre-Stop TypedViews: delivered=1 count=2048 peak=0.041764 Analyzer=0 old_generation=1 new_generation=2
Probe exit: 2
```

Ожидание: после stopped snapshot поздний data packet не заполняет FFT; actual:
непустой ненулевой график при Analyzer Stopped. Сценарий детерминированно задаёт
позднюю доставку сохранённого пакета; он **не утверждает**, что обычный текущий
EngineServer сам переупорядочивает TCP или регулярно отправляет этот пакет после
Stop. Обычная server Stop последовательность отдельно прошла. Дефект относится
к отсутствующему presentation receive fence для explicitly requested late data
scenario, а не к DSP или переносимому scheduler.

Рекомендация: запрещать presentation data delivery при explicitly stopped
Analyzer на typed receive boundary (и сохранить generation/stream semantics при
дальнейшем расширении fencing). Добавить regression с delayed valid packet 17
после stopped snapshot. Не заменять Stop общим clear на каждом graph ACK.

## Подтверждённые результаты и assertions

- MainWindow baseline, собранный из main_window/view_updates/dock_workspace/node_views,
  совпадает по поведению; значимые изменения — deferred topLevelChanged refresh и
  эквивалентные Unicode literals. Контекст queued refresh привязан к dock; проверяет
  текущие floating/visible, не сохраняет устаревший bool. Own probe подтвердил detach
  неактивной вкладки public `setFloating(true)` без меню, доставку обоим renderers,
  return-to-tabs exposure false, отсутствие hidden data delivery и viewport Paint
  за 80 ms, hide до queued refresh без revival.
- GuiPresenter Stopped snapshot очищает локальные и дополнительные charts;
  внешняя остановка реального remote engine очищает FFT и Mel в GUIViews fixture.
  Source unavailable и отсутствующий per-node frame обнуляют только затронутую
  chart geometry; возобновление данных и независимый FFT проверяются реальной сетью.
- Settings fixture мигрирует удалённые checkbox/menu/title controls в текущие
  controls без ослабления результата: manual Apply проверяет accepted ACK с request
  id, отсутствие pending, compile requested/working graph и same_processing_as;
  успешный warning скрыт/пуст, rejected draft оставляет рабочую схему и ошибку.
  Это соответствует actual editor status channel: success не отображается,
  ошибка хранится в tooltip/accessibility. Переводы RU/EN, темы и отсутствие DSP
  revision изменения проверяются fixture. Release test compile commands содержат
  `-O3 -DNDEBUG ... -UNDEBUG`; require/throw assertions также активны.
- Source factory проходит через CoreApplication → GuiPresenter → EngineClient,
  включая возврат к local device. SyntheticCapture использует настоящий eight-tone
  in-memory worker, сохраняет выбранный source identity и никогда не делегирует
  физическому capture. `deliver_audio=false` — fixture control, не production флаг.
- WLED settings fixture отключает только два MainWindow start/restart bindings и
  собирает real EngineClient StartWled с LocalHost/ephemeral UDP port. Start,
  restart, packets, disabled state и сохранение параметров остаются проверяемыми.
  Начальное enabled=false не открывает LAN output до fixture composition.
  Production interface selection/broadcast этим тестом не доказаны.
- Workspace fixture проверяет default graph tabs сверху/Data Flow снизу, placement,
  manual save, floating restore в дочернем процессе, layout/preset persistence.
  Own probe дополнительно сохранил неизвестные root/nested UI JSON через actual
  saveSettings action. ConfigManager использует QSaveFile и recursive overlay.
- Slot focus/context creation, editor splitter collapse/width persistence, footer
  и single-pane chrome рассмотрены по actual source и assertions navigation/dock/
  resize fixtures; эти пять ранее проверявшихся случаев повторно не запускались.
  Скрытые charts выключают updates/timer, surface scheduling проверяет exposure;
  render exposure отделён от сохранённого `!isHidden()` состояния.
- Central ui::Theme задаёт Fusion/palette/QSS; стандартные controls наследуют её,
  QtNodes берёт semantic roles и реагирует на palette change, renderer/theme
  updates не меняют DSP/camera или explicit user colors. Local hardcoded theme
  palettes при canonical move не добавлены.

## Собственные команды и результаты

Toolchain: Windows, Qt `C:/Qt/6.10.2/mingw_64`, GCC
`C:/Qt/Tools/mingw1310_64/bin`, CMake `C:/Qt/Tools/CMake_64/bin/cmake.exe`,
Ninja `C:/Qt/Tools/Ninja/ninja.exe`. Только отдельный build; shared Ninja не запускался.

```powershell
$env:PATH='C:/Qt/Tools/mingw1310_64/bin;C:/Qt/6.10.2/mingw_64/bin;'+$env:PATH
cmake -S . -B review-qt-integration -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe `
  -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/mingw_64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
  -DFETCHCONTENT_SOURCE_DIR_BDSP=C:/Users/Koba/CLionProjects/qt-visualization/cmake-build-release/_deps/bdsp-src `
  -DFETCHCONTENT_SOURCE_DIR_MINIAUDIO=C:/Users/Koba/CLionProjects/qt-visualization/cmake-build-release/_deps/miniaudio-src `
  -DFETCHCONTENT_SOURCE_DIR_QTNODES=C:/Users/Koba/CLionProjects/qt-visualization/cmake-build-release/_deps/qtnodes-src `
  -DBUILD_LEGACY_APP=OFF -DENABLE_NATIVE_QA=OFF -DENABLE_DEVICE_QA=OFF
cmake --build review-qt-integration --target core_app ui_tests workspace_tests gui_views_tests qt_workspace_resize_tests gui_presenter_tests -j 3
cmake --build review-qt-integration --target core_app ui_tests workspace_tests gui_views_tests -j 3
ctest --test-dir review-qt-integration -R '^(settings_interactions|workspace_persistence|gui_independent_views)$' --output-on-failure
```

Использовался полный путь к cmake/ctest, показанные короткие имена — для читаемости.
Initial build PASS; incremental build PASS после concurrent source updates.
Три targeted cases: PASS, 7.48 / 9.04 / 1.69 s, 18.23 s total.
Логи: `build.log`, `incremental-build.log`, `targeted-tests-current.log`.

Own probe собран отдельно с compile flags gui_views_tests из compile_commands.json,
своим object вместо test main и теми же actual static libraries; команды сохранены
в `own-compile.cmd`, `own-link.cmd`. Повторная линковка после incremental build:

```powershell
cd review-qt-integration
cmd /c own-compile.cmd
cmd /c own-link.cmd
$env:QT_QPA_PLATFORM='offscreen'
$env:QT_PLUGIN_PATH='C:/Qt/6.10.2/mingw_64/plugins'
$env:QT_FORCE_STDERR_LOGGING='1'
./bin/own_qt_probe.exe
```

Own directFloat/tabReturn/hiddenDelivery/hiddenPaint/queuedHide: PASS;
manualSave/unknownJson: PASS; ordinary remote Stop/quietAfterStop: PASS;
late packet receive fence: FAIL (exit 2). Лог: `own-probe-current.log`.

Offscreen доказывает Qt state/data/paint-event сценарии, не native window-manager
drag/reparent, OpenGL framebuffer, popup appearance/System OS scheme. Native GUI,
cursor/screenshots, physical capture, hardware WLED, stress/full suite и Linux
не запускались. В первоначальном snapshot READY не был выдан ввиду P2;
исправлений production в этом review нет.

## Независимая повторная проверка после owner freeze

HEAD остаётся `0387743d734b34f6c8bd86e0f84032dc3e2206e6`.
Актуальный scoped manifest `review-qt-integration/source-manifest-fence.sha256`:
SHA256 `54bf62ac40fc0b90323ff1c71a930c8a1b9a631ff57a70d5df43f53d8ee453a7`.
Он дополнительно включает runtime/engine_transport fixtures. Новые Git blobs:
EngineClient `78952846267fc1e0a3e99b26c0c5f93f5b81a862`,
engine_transport fixture `cb6a50e27730e609bc8255ffb6d0ddeb811cab7f`,
runtime fixture `4c7df4edb77b03926c2f09149ee1e3c8a8c47e38`.

Проверена actual receive ветка EngineClient: новое условие
`cache.statuses.value("Analyzer").state != State::Stopped` закрывает позднюю
data delivery при explicit Stop, сохраняя typed unavailable markers в Recovering.
Wire/container/API не менялись. Regression в engine_transport_tests сохраняет
positive Running delivery, rejected stopped late packet и positive Recovering
unavailable delivery; generation 77 намеренно не приравнивается к analyzer
generation — per-stream provenance не подменяется общей generation.

Собственный отдельный build актуального core_app/gui_views_tests: PASS
(`fence-build.log`). Оригинальный own_qt_probe.cpp не изменён, первоначальные
RED логи сохранены. Повторно слинкован с актуальными actual libraries:

```text
OWN real externalStop / quietAfterStop PASS
OWN delayed pre-Stop TypedViews: delivered=0 count=0 peak=0.000000 Analyzer=0 old_generation=1 new_generation=2
Original probe exit: 0
```

Лог `own-probe-fence.log`. Отдельный `own_fence_probe.cpp` продолжает тот же
real-loopback сценарий: после отрицательной stopped проверки передаёт monotonic
Recovering snapshot и valid unavailable packet 17 через реальный server socket.

```text
OWN Recovering unavailable marker PASS delivered=1
Fence probe exit: 0
```

Лог `own-fence-probe.log`; compile/link команды сохранены в
`own-fence-compile.cmd` / `own-fence-link.cmd`. Это собственное подтверждение
границы обоих состояний; результаты owner не использованы как proof.

Runtime fixture delta также проверен по source. Новая explicit
`presenter.set_visible_views(Views | TypedViews | Previews)` находится перед
direct callback lifecycle loops: fixture сам является потребителем и подписывается
на сигналы, не выставляя скрытые docks видимыми. Clear/cancel/restart assertions
сохранены; это соответствует текущему subscription contract. Эти старые lifecycle
случаи в данном narrow rerun не исполнялись.

WLED delta первоначально ошибочно ожидал replacement от второго Start. Собственный
exact-block probe воспроизвёл RED (`own-wled-before-restart.log`), actual
AudioPipeline::start_wled имеет намеренный idempotent guard. В окончательном
fixture restart восстановлен как Stop → wait Stopped → explicit localhost Start;
production не изменён ради ошибочного ожидания.

Для granular независимой проверки WLED block из runtime_tests.cpp перенесён
**дословно** (с нормализацией CRLF) в build-only `own_wled_probe.cpp` с настоящими
CoreApplication, GuiPresenter, AudioPipeline, EngineServer/EngineClient и UDP.
Совпадение current source block с probe проверено программно. Повторно исполнены
пять Start/restart/double-Stop циклов, accepted request ACK, real packets в оба
ephemeral receivers, исчезновение пакетов в старом endpoint, тишина после Stop
и disabled restart. Итог:

```text
OWN exact WLED fixture block PASS
WLED probe exit: 0
```

Лог `own-wled-probe.log`; команды `own-wled-compile.cmd` / `own-wled-link.cmd`.
Все endpoints явно LocalHost; физический capture отсутствует. Этот probe
подтверждает actual app pipeline worker/control transport, а не LAN interface
selection или enabled convenience restart wrapper (последний покрывает ранее
исполненный settings fixture). Server использует callbacks app pipeline; значения
и результаты worker не подменены counters/status stubs.

Полный runtime/transport suite, старые пять navigation/resize случаев и native
QA повторно не запускались. Новых P1/P2 в узком frozen delta не найдено.
P2 closed; Qt slice READY относится только к явно разрешённым source/offscreen
проверкам с указанными native/hardware ограничениями. Production/tests этим
reviewer не исправлялись.
