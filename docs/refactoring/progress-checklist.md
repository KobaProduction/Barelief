# Рефакторинг: видимый прогресс

### QML graph migration — 2026-10-09

- COMPLETED in migration / recorded automated acceptance scope: all graph families: scalar
  metrics; vector line/points/bars/pie; matrix image2d/surface3d. QtNodes editor,
  DSP, History accumulation, protocol and document formats remain outside scope.
- Fresh agents created with `fork_turns=none`, using only the condensed handoff
  `qml-handoff-2026-10-09.md` and current rules/evidence. The handoff's old
  instruction to defer implementation is superseded by explicit user approval.
- Ownership: qml_vector_matrix2d — 2D/metrics; qml_surface3d — surface renderer;
  qml_graph_bridge — graph presentation/controls/shared Quick helpers; root —
  shared CMake/composition/workspace/integration/tests/docs and sole build owner.
- Overview: implementation INTEGRATED; release build PASS (`build15.log`);
  FINAL automated acceptance COMPLETE in the scope below. Circular independent source reviews accepted the 2D/metrics,
  bridge, surface and root integration after owner fixes. Supplemental independent
  review accepted persistence, teardown, palette, explicit FBO viewport, snapshot
  notifications and stable sidebar layout. Earlier accepted
  History/axes/UI blocks retain only their recorded scope, not future QML pixels.
- Evidence directory: `cmake-build-release/qml-migration-20261009-220014`.
  Native update_plan is unavailable; no Markdown UI fallback opened.
- Shared integration removes Charts/DataVisualization/OpenGLWidgets from live
  targets and deploys QML imports. Regression fixtures target presentation ports;
  new QML checks cover Ready/errors, exact scalars, peak-preserving budgets,
  commands, axes, theme/language and lazy surface state. Build attempts exposed
  remaining old test API/includes; these are adapted without restoring fallback
  renderers. Focused workspace/settings/axes/QML checks: 4/4 PASS. Native 2D
  scenes/controls/metrics/image pixels: PASS. Build13 full host: 99/101 PASS;
  UI persistence test isolated the synchronous preference commit from unrelated
  autosave; gesture restart fixture now uses real child processes. Both source
  adaptations independently accepted; final affected recheck 14/14 PASS
  (`host-recheck.log/xml`, 109.16s). Latest outcomes across the full run and
  rechecks: **101/101 PASS**, combined evidence, not a second full suite run.
  Machine-readable summary: `acceptance-summary.json` in the evidence directory.
- Native 3D FINAL scenario PASS (`native3d-final.out.log`, empty stderr): actual
  geometry, exact GPU VBO data, physical/index roundtrip, six camera poses,
  point/both slices, resize, hide/show and dock reparent. Physical/roundtrip use
  the same 801x800 logical viewport, 1602x1600 FBO and 14 time-axis sections.
  Root and independent bridge reviewer inspected the owned reparent framebuffer
  and accepted the recorded native scenario evidence. This is a bounded native
  scenario, not general visual/product signoff.


### Native axes regression follow-up — 2026-10-09

- Implementation integrated / independently source accepted / FINAL host and
  native axes scenario accepted in this regression scope.
  Earlier 100/100 is retained as offscreen evidence only: it did not accept
  these newly demonstrated native scenarios.
- Default/editor Mel declares explicit source band-centre Hertz; History
  declares source timestamps in seconds and preserves frequency coordinates.
  Saved user config received only missing coordinate declarations (25 axes),
  with an exact original backup and structural comparison of unrelated fields.
  Unavailable physical coordinates are explicit in disabled selector entries;
  arrival/removal changes capability without writing graph preferences.
- Grid sections depend on viewport/font metrics, never camera pose/distance.
  Height retains at least four sections when space permits; integral frame
  positions remain integral. Camera updates projection/visible boundary walls,
  not numeric tick policy or grid buffers. Source values/DSP are unchanged.
- Release configure/build PASS. Native real-default-graph/400-row-History
  OpenGL PASS: physical/index selectors and tick values, six actual v2 camera
  poses/distances, unchanged grid/policy revisions, point and both slices,
  unchanged matrix owner/revision/amplitude. Root inspected own framebuffer;
  independent harness oracle review accepted. Grid pixel counting is not an
  automated oracle; telemetry and actual framebuffer inspection are retained.
- Full host run: 94 PASS / 6 failures; corrected missing explicit-coordinate
  test payloads and refusal/capability test oracles, then all six rechecks PASS
  (24.26 s). No production guard weakened. All 12 rebuilt shared-GUI-fixture
  consumer checks PASS (131.20 s). Latest outcomes across the full run and final
  rechecks: **100/100 PASS**, recorded explicitly as combined evidence rather
  than a second complete suite run. Saved-project editor/engine verification PASS.
- Evidence: `%TEMP%/qt-native-axes-fix-20261009-ff219b2db3cf4106866f4f69dc75973f/`
  (`native.log`, `native/*.png/json`, `host-tests.log/xml`, `host-recheck.log/xml`,
  `fixture-consumers.log/xml`, `host-final-summary.json`,
  `project-verify-out/err.log`, `user-config-before.json`, `config-repair.json`).
  No SDK production sources changed; no firmware/native hardware claim.
- Owners: renderer grid policy; source contract factories/propagation; independent
  native regression harness; root — control capability/config/integration/build.
  Native update_plan is unavailable. User windows/input are not manipulated.

### Spectrogram axes and coordinate selectors — 2026-10-09

- Implementation completed / independently source accepted /
  FINAL automatic integration accepted in the presentation scope.
- Temporal frame replaces Age in axis/readout labels. Per-graph frequency and
  time selectors choose declared Hertz/seconds or positions/frames across both
  renderers, with source metadata retained separately from display choices.
  Missing physical coordinates fall back to positions; incomplete History uses
  frame positions until real timestamps cover capacity. Generic matrix row
  direction is retained. Mel Hertz values are source-provided band centres;
  no DSP/filter bank computation was changed.
- Sections adapt independently to projected 3D axis length/viewport and actual
  font metrics; grid and labels share divisions, including crop/reverse and
  partial-known time. Image axes adapt to viewport and label size. Native mode
  combos now synchronize during restore without preference writes.
- Ownership: root — metadata/selectors/image/localization/integration;
  adaptive_spectrogram_ticks — projected native ticks/grid and geometry oracles.
  Processing data, camera, History storage and portable SDK stay unchanged.
- Root independently accepted adaptive grid/cache changes after owner fixes for
  wall-side mirroring, bounded sections and GL cache cleanup. The owner accepted
  source/effective metadata, shared selectors/restore, image and pause behavior.
- FINAL release configure/build PASS; full Windows host **100/100 PASS**, 230.59 s.
  Evidence: `%TEMP%/qt-spectrogram-axes-20261009-65638428c71842709553c03950d46518/`
  (`configure.log`, `target-build.log`, `release-build.log`, `host-tests.log/xml`).
  Regression oracles cover nonuniform frequencies, selectors without sample
  owner/revision changes, restore without writes, frozen/pending metadata,
  generic reversed row indices, adaptive image size and projected 3D policy.
  GUI acceptance is offscreen/pre-GL; native 3D pixels are not claimed. No SDK
  rebuild required for Qt-only changes. User closed PID 66144; no user process
  was terminated by the agent. User config/output untouched; no commits.
  Native update_plan remains unavailable.

### History startup/resize and preset diagnostics — 2026-10-09

- Implementation completed / independently source accepted /
  FINAL automatic integration accepted in the startup/resize/editor scope.
- Authoritative full-capacity typed-zero History is available before first
  ingress, with occupancy/ordinal zero. The renderer projects every source cell;
  axes cover capacity immediately, using frame ages until real timestamps cover
  the full history. Shrinking removes the oldest age tail; extending adds typed
  zeros there without processing or advancing the ordinal. Replacement continues
  after capacity is reached. Unavailable without payload clears stale rendering.
- Owners: runtime/history; projection/axes; shipped presets/admission diagnostics;
  I: editor diagnostic placement, integration, independent review and builds.
- Existing completed blocks remain accepted in their recorded scope. Previous
  padding-only tests did not cover authoritative cold startup and growing axes.
- Shipped factories were valid. The user's saved scene_graph and pipeline_graph
  node 17 incorrectly used MelView for a Matrix input; only those two kind values
  were repaired to SpectrogramView, preserving every other value and unedited
  byte. The original configuration was backed up. PID 29768 was terminated with
  explicit user permission before rebuilding. Test runs used isolated configs.
- Connection diagnostics identify nodes and ports in the editor toolbar tooltip;
  the duplicate permanent global footer labels were removed.
- Final release build PASS; full Windows host **100/100 PASS**, 223.04 s;
  SDK build/install and **32/32 PASS**, 9.83 s; installed SDK component/fake-IDF
  **6/6 PASS**, 3.75 s, including the ordinary 320 KiB TCP/UART profiles;
  external installed SDK consumer configure/build/run PASS, exit 0.
  An isolated copy of the repaired actual project also passed engine/editor
  verification with advancing History frames and capacity 400.
- Evidence: `%TEMP%/qt-history-startup-20261009-4e14d43af44f417aaf0bcf64c587ca4f/`
  (`release-accepted-build.log`, `host-accepted-tests.log/xml`, `sdk-build.log`,
  `sdk-tests.log/xml`, `sdk-install.log`, `component-configure-final.log`,
  `component-build.log`, `component-tests.log/xml`, `consumer-*.log`,
  `repaired-project-tests.log`, `config-repair.txt`, original/repaired config copies).
  The initial host 98/100 exposed two stale test oracles for removed footer labels
  and cleared unavailable history; both were strengthened and independently
  reviewed before the final full pass. The initial component configure missing
  BDSP_SOURCE_DIR is retained alongside the corrected configure evidence.
- GUI coverage is offscreen/pre-GL, not native 3D or hardware acceptance.
  Separate follow-up: the unused direct reset_history API needs explicit lineage
  when resetting ordinal within the same producer generation; this acceptance
  covers actual UI Apply/resize revision fencing, not that direct API scenario.
  No commits were created. Native update_plan remains unavailable.

### Desktop: shared matrix snapshot — 2026-10-09

- Implementation completed: GUI-owned immutable snapshot shared by adapter,
  image, surface and native renderer; transpose is an indexed view. Pause holds
  frozen and latest pending snapshots without a queue. Per-frame projection cache.
- Scope: Qt presentation only; DSP/runtime/protocol/embedded sources unchanged.
- Independent source review accepted, including equal-content owner replacement,
  lifetime, axes/slices, pause/resume, unavailable and limits.
- FINAL automatic integration accepted: release build PASS; full Windows host
  **100/100 PASS** (263.40 s), including shared-owner/equal-content/pause and
  authoritative History ordering checks; existing SDK **32/32 PASS** (10.40 s).
  No portable production changes or firmware rebuild were required.
  Evidence: `%TEMP%/qt-shared-projection-20261009-ee5b85f1d2a24e5a87bd432aa936e38f/`
  (`release-build.log`, `release-final-build.log`, `host-tests.log/xml`,
  `sdk-tests.log/xml`). GUI coverage is offscreen/pre-GL; native pixels,
  hardware and FPS improvement are not claimed. Remaining HistoryPresentation
  storage and renderer image/GPU resources are intentional separate ownership.

### Размерность графиков, метрики и согласованные срезы — 2026-10-09

- **Implementation completed / independently source accepted /
  FINAL automatic integration accepted в scope этого блока**.
  Новый scope уточняет предыдущие универсальные графики: PCM/FFT/Mel метки
  по-прежнему не блокируют числовые данные, но rank задаёт семейство:
  Scalar → метрики; Vector → line/points/bars/pie; Matrix → image2d/surface3d.
- Владельцы: renderer readout/slices; editor rank/ports/names; portable extractor
  и per-input sink delivery. I — GraphPanel, metrics projection/composition,
  единственные builds/tests и независимое ревью.
- План: (1) контракты/извлекатель и входы метрик, (2) отображение и согласованные
  срезы, (3) интеграция/независимое ревью, (4) итоговые host/SDK проверки.
  Native update_plan отсутствует; прежние evidence сохранены.
- Реализованы IndexExtract (точный dtype, logical row/column/channel),
  ScalarView с независимыми одноканальными входами, именами/видимостью/Undo
  и сохранением; графики не меняют размерность и не накапливают значения.
  Readout без рамки/ellipsis; срезы следуют проекции своей оси.
  Первый полный host 99/100: адаптирован oracle порядка bypass для wrapper
  ScalarView. Первый installed component 4/6: устранено превышение 320 KiB
  на 97 bytes за счёт компактных cold metadata, без увеличения профиля.
  Первый переход dense→ring исправлен с сохранением строгой последующей
  проверки курсора. Все owner fixes независимо source accepted.
  Evidence: `%TEMP%/qt-ranked-graphs-20261009-05bab55df63c4d38a0ba14f250c6d795/`.
- Итог: release build PASS (`release-final2-build.log`, test-only
  `release-accepted-build.log`); полный Windows host **100/100 PASS**, 107.63 s
  (`host-accepted-tests.log/xml`); SDK build и **32/32 PASS**, 4.90 s
  (`sdk-final-build.log`, `sdk-accepted-build.log/tests.log/xml`);
  installed SDK component/fake-IDF **6/6 PASS**, 8.36 s
  (`component-accepted-build.log/tests.log/xml`), включая 320 KiB TCP/UART.
  Внешний installed SDK consumer configure/build/run PASS, exit 0.
  Промежуточные ошибки сохранены; последняя фикстура проверяет точный отказ
  базового контракта многоканального Scalar и атомарность прежнего owner.
  GUI acceptance — offscreen/pre-GL; native OpenGL, реальное аудио и ESP32
  hardware этим прогоном не подтверждаются. Пользовательские файлы и процессы
  не заменялись/не завершались, коммиты не создавались.

### Универсальные графики, соединения и инспекция — 2026-10-09

- **Implementation completed / independently source accepted / FINAL automatic
  integration accepted в scope этого блока**: графики
  принимают числовые scalar/vector/matrix независимо от назначения PCM/FFT/Mel;
  dtype, форма и физические координаты сохраняют проверку корректности.
  Накопление выполняется явным processing History, GUI получает готовые снимки.
- Три владельца: primitive contracts/runtime/wire; editor/adaptation/names/clipboard;
  renderer/modes/selection. I — snapshot composition, независимое ревью,
  единственные release/SDK builds и итоговые автоматические проверки.
- Результаты: line/points/bars/pie/2D/3D, синхронное имя блока и графиков,
  сохранение conversion policy и IDs при copy/paste, перемещение выбранной точки
  и компактные вертикальные показания. Названия сохраняются как presentation
  metadata, включая явный сброс; DSP и неизвестные поля документа сохраняются.
  Финальные release build и полный Windows host-прогон **100/100 PASS**
  (118.80 s); SDK build и **32/32 PASS** (4.27 s); installed SDK
  component/fake-IDF **6/6 PASS** (17.06 s), внешний SDK consumer
  configure/build/run PASS. Первый host-прогон 84/100 и промежуточные ошибки
  сохранены; исправлены повторное применение surface overrides, сохранение
  display_name и сброс 3D→Line при пустых настройках пресета. Фикстуры
  используют активный renderer, зарегистрированные presentation deltas,
  actual subscription ACK и обновлённую layout geometry.
  Evidence: `%TEMP%/qt-universal-graphs-20261009-56a58e430f0449c397e10229f87403c8/`
  (`release-final-build.log`, `host-final-tests.log/xml`, `sdk-final-build.log`,
  `sdk-final-tests.log/xml`, `component-tests.log/xml`, `consumer-test.log`).
  Автоматическая GUI-проверка — offscreen/pre-GL; native OpenGL, реальные
  аудиоустройства и ESP32 hardware acceptance этим блоком не заявлены.
  Native update_plan недоступен; отдельный Markdown UI fallback не создаётся.

### Стабильный автодиапазон и удаление нижней полосы — 2026-10-09

- **Implementation completed / independently source accepted / bounded automatic
  integration accepted**:
  единая политика автодиапазона амплитуды для chart и
  спектрограммы, сохранённый интервал пересчёта (default 10 s), запас по
  амплитуде; табы и посторонние настройки не сбрасывают удержание диапазона.
- Диагностическая подпись `numericExactValue` переносится из нижней строки
  всего dock в боковую панель. Текущее скалярное значение остаётся внутри графика.
- Qt owner — диапазоны/controls/persistence/targeted tests; I — footer relocation,
  composition, независимая приёмка и единственные builds/tests.
  Независимая приёмка исправлений паузы, конечности отображаемых координат,
  первого ненулевого сигнала, локализации и сохранения интервала завершена.
  Release build PASS; адресные settings/render/axes/adapter/geometry/UI проверки
  **11/11 PASS** (52.82 s). Детерминированные clocks проверяют рост и сжатие
  только на границе интервала, накопленные пики и cold zero first-fit;
  actual chart control и surface settings signal сохраняют разные интервалы
  7.5/6.5 s через явный Save/load. Проверены sidebar ancestry и отсутствие
  нижней строки outer layout. Первые неуспешные builds/tests сохранены;
  исправлены размещение тестового кода, наследуемый clock seam и корректная
  offscreen проверка surface persistence через рабочий settings signal.
  Evidence: `%TEMP%/qt-stable-range-20261009-834a2414c195414180d748d06466e951/`
  (`release-accepted2-build.log`, `release-final-build.log`,
  `targeted-accepted-tests.log/xml`). Native 3D controls/OpenGL acceptance
  не заявлены; DSP/SDK не менялись. Native update_plan недоступен.

### Контекстное меню и фиксированная история спектрограммы — 2026-10-09

- **Implementation completed / independently source accepted / FINAL automatic
  integration accepted в scope этого блока**: переименование удалено из области
  графика/Placement и доступно в боковых настройках графика и инспекторе
  графического узла пайплайна. Меняет только presentation preferences;
  regression проверяет неизменность документа, Undo и применённой DSP-ревизии.
- History: ещё до первого сигнала известная геометрия имеет полную capacity
  с типизированными нулями, включая midpoint беззнакового PCM;
  новые строки заменяют старые. Resize сохраняет свежую часть, обрезает старый
  хвост или добавляет нулевой старый хвост. `actual=capacity` отделено от
  реального `filled_count` во всём snapshot/protocol/presentation пути.
- Владение: I — rename/composition/tests/final builds; core owner — runtime/history;
  Qt owner — projection/renderer; History owner — snapshot bridges/protocol metadata.
  Runtime, projection, bridges/protocol, rename и итоговые regression fixtures
  независимо приняты по исходникам. Проверены точное сохранение свежих UInt64,
  timestamps/ordinal, полный rollover, пустой буфер, локальный и удалённый resize,
  owning ingress с partial occupancy и разрыв сетевого PCM без сброса чужой ветки.
- **FINAL evidence**: release build PASS; полный host CTest **100/100 PASS**
  (125.88 s, `-j 2`); SDK build и **32/32 PASS** (9.50 s);
  установленный SDK component build и **6/6 PASS** (3.39 s), включая штатные
  TCP/UART profiles 320 KiB; внешний installed SDK consumer build/run PASS.
  Первые неуспешные прогоны сохранены: исправлены устаревшие growing-history
  test helpers и явный повторный запрос cold snapshot в resize fixture;
  строгие проверки continuity, amplitudes и потери PCM сохранены.
  Evidence: `%TEMP%/qt-fixed-history-20261009-d10e5df08db648a3bd8be8232b6592a2/`
  (`release-accepted-build.log`, `host-accepted-tests.log/xml`,
  `sdk-accepted-build.log`, `sdk-accepted-tests.log/xml`,
  `component-build.log`, `component-tests.log/xml`,
  `consumer-build.log`, `consumer-run.log`). GUI проверки изолированные
  offscreen/fake; native OpenGL/hardware acceptance не заявлены.
  Native update_plan отсутствует; новый файл отчёта не создаётся, коммиты не созданы.

### Клавиатурное управление 3D и стабильный горизонт — 2026-10-09

- **Implementation completed / independently source accepted / bounded automatic
  integration accepted**: после уточнения пользователя WASD следует горизонтальной
  проекции взгляда по X/Z, игнорируя pitch. Первоначальный вариант фиксированных
  мировых направлений отменён. Горизонтальный шаг сохраняет Y и скорость при любом
  наклоне, в том числе на границе дальности. Вращение/загрузка ограничивают pitch
  до ±89° и убирают roll: камера не переворачивается.
  Space поднимает по Y, Ctrl+Space опускает. Bare Ctrl освобождён для shortcuts,
  включая autorepeat ранее удерживаемых WASD. Удерживаемый Space меняет направление
  при нажатии/отпускании Ctrl; Shift ускоряет оба направления. Настройки захвата,
  подписи и сравнение сочетаний различают Space и Ctrl+Space; bare Ctrl не
  принимается как движение, остальные пользовательские поля сохраняются.
- Release build PASS; адресные settings/render/axes/adapter/UI проверки
  **7/7 PASS** (29.61 s). Evidence: `%TEMP%/qt-camera-heading-fix-20261009/`
  (`build.log`, `tests.log`, `tests.xml`). Проверены 30 сочетаний yaw/pitch,
  направление/скорость/высота, отсутствие roll/переворота при больших mouse deltas
  и горизонтальный шаг на границе дальности. Retained native
  lifecycle scenario дополнен направлением взгляда и порядками Shift/Ctrl/Space;
  native OpenGL проверка не запускалась и её приёмка не заявлена.
  DSP/SDK не менялись. Native update_plan недоступен.

### Графики, bypass и частота дискретизации — 2026-10-09

- **Implementation completed / FINAL integration automatically accepted в scope этого блока**: scalar current-value + bounded timestamped
  history; стабильный auto range и сдержанный плоский chart style через Theme;
  настраиваемая source sample rate, переносимый PCM resampler и согласование
  downstream contracts; постоянный bypass на блоках; пользовательские имена
  графиков с сохранением, отменой и восстановлением стандартного названия.
- Владение: Qt owner — charts/adapter; Mel/core owner — sample-rate/resampler/
  domain/editor contracts; History owner — bypass/form/editor; I — graph names,
  переводы, composition, review и единственная итоговая сборка/проверки.
  Qt/bypass, graph names, portable resampler и maintained QtNodes Undo extension
  independently source accepted; rate-editor, deterministic Undo serialization,
  optional resampler state и итоговые regression fixtures также independently accepted. Удалён
  сброс scalar ring на пропуске GUI sequence: реальные timestamps сохраняют
  редкие выборки; unavailable/discontinuity очищают несовместимую историю.
  Source rate и resampler rate распространяются как одна операция с Undo;
  ordinary disconnect сохраняет current clocks. Настройки источника согласованы
  с SDK; ресемплер сохраняет свой диапазон 8–192 kHz и атомарно отвергает
  неподдерживаемую цепочку. Cold-owned FIR state выделяется и учитывается
  только у реальных ресемплеров; независимость snapshot и отсутствие hot-path
  allocations проверены. Стандартный embedded бюджет 320 KiB не увеличен.
- **FINAL integration evidence**: release build PASS, полный host CTest
  **100/100 PASS** (248.55 s); SDK build и **32/32 PASS** (9.14 s);
  установленный SDK component build и **6/6 PASS** (2.95 s), включая штатные
  TCP/UART profiles 320 KiB; внешний installed SDK consumer build/run PASS.
  Первые неуспешные прогоны сохранены отдельно; исправлены owning plane capture
  тестовой фикстуры, явный sample rate SDK factory, рост inline state у чужих
  узлов и недетерминированный порядок сериализации связей. Strict Undo oracle
  сохранён. Evidence: `%TEMP%/qt-graphs-rates-bypass-20261009-148d9b38d7a9471b856739b15151ebf5/`
  (`release-accepted5-build.log`, `host-accepted-tests.log/xml`,
  `sdk-accepted-build.log`, `sdk-accepted-tests.log/xml`,
  `component-accepted-build.log`, `component-accepted-tests.log/xml`,
  `consumer-accepted-build.log`, `consumer-accepted-run.log`).
  GUI проверки изолированные offscreen/fake; native OpenGL и реальное audio
  hardware acceptance не заявлены. Общий редизайн отложен пользователем.
  Native update_plan недоступен; новый отчёт не создаётся.

### Геометрические пропорции спектрограммы — 2026-10-09

- **Implementation completed / independently source accepted**: пользователь уточнил, что проблема — отношение
  длины/ширины к высоте при crop истории400 до1–2%, а не ограничение значений Y.
  Qt owner исправил независимый от sample counts/crop размер сцены и добавил
  явные настройки X/Y и Z/Y с сохранением старого count-based режима как выбора.
- Историческое сравнение: `3de4de8` и `b18cfbf` используют тот же count-based auto
  aspect; `68b12bd` оставлял Q3DSurface defaults (aspect2, horizontalauto).
  Поэтому это существующее ограничение presentation, а не доказанная DSP-регрессия.
- Source review принял также owner fixes наследования связанной группы mode/X/Y/Z/Y:
  индивидуальные пропорции сохраняются при global mode change и unrelated colour
  edit, даже когда global временно совпал с local. Точные ratios не округляются
  при загрузке/несвязанном редактировании. **FINAL integration / automatic acceptance
  completed в scope scene proportions**: release build PASS; адресные
  settings/geometry/renderer/GUI checks **14/14 PASS** (36.67 s).
  Исправлена и независимо принята зависимость общего контрола: только Qt и
  нижние presentation-примитивы, без core_app FieldRow. Проверены crop400 до1–2%,
  неизменность amplitudes/camera, точные ratios и global/per-view persistence.
  Evidence: `%TEMP%/qt-spectrogram-aspect-20261009-da2b3995622840d5bd6657e7c77b55d5/`
  (`release-final-build.log`, `aspect-tests.log`, `aspect-tests.xml`); начальная
  link failure сохранена отдельно. Native OpenGL visual acceptance не заявлена.
  DSP/SDK scope предыдущего блока принят
  и не меняется. Пользовательские configs/camera/ranges не перезаписываются.
  Native update_plan отсутствует; новых файлов отчёта не создаём.

### Разделение UI перед QML — 2026-10-09

Пользователь утвердил первый архитектурный блок без переноса на QML.
A владеет моделями/формами параметров и редактором; B — командами,
главным окном и discovery port/adapter; C — общими контролами, настройками
и workspace persistence. I владеет desktop composition, CMake, тестами,
документацией и итоговой интеграцией. Чужие изменения сохраняются.

- Implementation **completed**: выделение widget-independent presentation
  models, явной metadata полей, общего command registry и компонентов форм;
  исправления группировки, ошибок ввода и контекстов команд.
- Integration / automatic acceptance **completed в scope UI separation**:
  release build PASS; полный host CTest **100/100 PASS**, включая 39 UI
  сценариев, editor/Undo/ports, fake discovery, настройки, workspace/presets,
  exact literals, invalid input и invalid-draft preferences persistence.
- Independent acceptance **source accepted**: A reviews B, B reviews C,
  C reviews A; owner fixes повторно проверены, включая popup origin,
  pending input, capture choices, compact chrome и autosave disable.
- Native visual acceptance **не заявлена**. Используются только собственные
  offscreen/fake сценарии; пользовательские окна/config/output не трогаются.

Native update_plan недоступен. Эта запись фиксирует evidence, не подменяет
встроенный план и не меняет принятые статусы предыдущего core-fixes блока.

Итоговый [отчёт о границах и проверках](../ui-separation-2026-10-09.md).
Evidence: `cmake-build-release/ui-separation-1791518587407/build-accepted.log`,
`cmake-build-release/ui-separation-1791518587407/host-accepted-final.log`,
`cmake-build-release/ui-separation-1791518587407/host-accepted-final.xml`.
Ранние неуспешные прогоны сохранены отдельно; итоговый PASS получен после
owner fixes и устранения гонки ожидания registration/capabilities в тесте.

### Второй сравнительный аудит сигналов и спектрограмм — 2026-10-09

- **Correction implementation completed / independently source accepted**: пользователь назначил трёх новых
  исполнителей для Mel semantics, History/time coordinates и Qt spectrogram
  axes/pause/projection. Взаимное Mel/History review accepted; Qt source review
  accepted после owner fixes duplicate bucket, stale coordinate range и RU key.
  **FINAL integration / automatic acceptance completed в scope signal/axes fixes**:
  release build + **100/100** host CTest PASS, SDK build + **32/32** PASS,
  installed embedded component build + **6/6** PASS, включая stock320KiB.
  Installed SDK consumer no-Qt/no-exceptions/no-RTTI build/run PASS; CLI synthetic
  default1/60 и25ms по1800ms завершились exit0. Все запуски — координатор.
  Пользовательские configs/ranges не заменяются.
  Первый host-прогон98/100: выявлены только новый root PCM fixture max extent
  и старый editor snapshot fixture без новых Explicit Seconds coordinates.
  Оба fixture исправлены и независимо приняты; stage oracle actual PASS.
  Финальный host-прогон100/100 после обеих исправленных fixtures —245.57s.
  По запросу пользователя временный отчёт этого прохода удалён после проверки;
  итоговая evidence-сводка сохранена здесь. Native OpenGL/hardware не заявлены.

- **Comparative source audit completed / read-only**: три новых независимых агента проверили
  (1) PCM/Window/FFT/Mel и масштаб амплитуд, (2) исполнение цепочек,
  History и временные метаданные, (3) renderer спектрограммы, оси и display settings.
- Базы сравнения: `3de4de8` перед выделением domain/application/runtime;
  `b18cfbf` перед текущей незакоммиченной переработкой; `68b12bd` при необходимости
  для исходных алгоритмов. Текущая версия включает tracked и untracked source files.
- Задача: доказать конкретные регрессии и отделить их от намеренных изменений
  формул, единиц и настроек. Предыдущая приёмка core fixes сохраняется в своём
  scope и не считается доказательством визуальной/численной эквивалентности.
- Результаты: Mel formula delta подтверждена и разобрана в
  [контракте DSP](../../libs/dsp_kernels/README.md). Continuous-Hz weights сохранены;
  output — Mel bands, weighted linear magnitude, без скрытого power/gain/dB.
  Исправлены History snapshot/Regular/None time semantics и renderer
  axes/partial fill/pause/readout, vector Regular/explicit coordinates, peak
  reduction и singleton projection. Frames/seconds, Hz/bands подписываются явно.
  Уточнение source-аудита: variable-width History уже отвергался runtime prepare;
  исправлена domain admission parity, а не доказанная corruption допущенного графа.
  Общий новый FFT/PCM gain loss не найден. Loopback source сохранён; output rate
  теперь явно48000 вместо прежнего native rate, возможен resampling.
- Независимый stage oracle сравнивает Window4096/Hann/FFT/Bark/RMS/Mel50/History
  на восьми изменяющихся тонах с собственным DFT; C++ hot-allocation guard PASS.
  Новые графы/History factory используют Explicit Seconds; сохранённые None
  контракты последовательно frames, без миграции пользовательского файла.
- Evidence: `C:\Users\Koba\AppData\Local\Temp\qt-spectrogram-fixes-20261009-9520ad1a1226436faf4acda31e2a2e06`:
  `release-final-build.log`, `release-final-tests.log/xml`, `sdk-build.log`,
  `sdk-tests.log/xml`, `sdk-install.log`, `component-build.log`,
  `component-tests.log/xml`, `consumer-build.log`, `consumer-run.log`,
  `bounded-runs.log`, `cli-default.log`, `cli-25ms.log`. Первый98/100 прогон
  сохранён отдельно как `release-tests.log/xml`; oracle receipt — `oracle-test.log/xml`.
  SDK consumer/component build dirs — `cmake-build-sdk/spectrogram-consumer-9520ad1a`
  и `cmake-build-sdk/spectrogram-component-9520ad1a`.
  Native `update_plan` в текущем чате отсутствует.

### Исправление ядра после углублённого ревью — 2026-10-09

Пользователь назначил двух исполнителей и отложил все builds/tests/app runs до
конца общей реализации. A владеет contracts/domain/application/runtime/DSP;
B — capture/delivery/CONTROL/JSON/embedded/firmware. Координатор интегрирует
composition/presentation и единолично выполняет FINAL CMake/Ninja/проверки.
Native update_plan в этом чате отсутствует; Markdown UI fallback не предлагается.

- Implementation **completed / independently accepted**: concrete findings трёх core reports,
  continuous service, time-based `execution_interval` default Ratio{1,60},
  safe bypass (одинаковый полный контракт — identity; иной — typed silence),
  portable/adapters parity, actual error propagation и актуальная спецификация.
- Integration / FINAL regression **completed в scope core fixes**: согласованный
  API и ownership, owner fixes и независимое review; release 98/98,
  SDK 32/32, installed component 6/6 PASS. Внешние правки сохранены.
- Independent review desktop/control и portable/firmware **source accepted**
  после owner fixes. FINAL desktop tests и bounded synthetic app launches PASS.
- Hardware MCU acceptance **не заявлена**: host/fake-IDF не заменяет устройство.

Author receipt A: implementation готова, без builds/tests/probes/app runs;
FFT/Mel resources, PCM contracts, History/Window continuity, helper validation,
time interval и continuous lifecycle реализованы. Независимое read-only review
B desktop/control выполнил A: выявлены и переданы owner deadlock при нескольких
CONTROL-командах, JSON interval validation, starvation конечного выхода за
intermediate FIFO и copied terminal callback. B подтвердил закрытие также
generation increments и очистки tap demand при последнем disconnect/close.
Порядок FINAL после завершения обоих implementation blocks: A owns portable
regressions и review firmware; B owns adapter/protocol/embedded regressions и
review A; I owns GUI/composition tests, shared CMake/Ninja и общий прогон.
Этот author receipt фиксировал implementation; итог runtime acceptance ниже.

История интеграции до итоговой приёмки: release/sdk configure PASS; production `core_app`, `pipeline_cli`
и SDK `dataflow_runtime`, `embedded_support`, `portable_numeric_control_session`
собраны. A/B regression sources готовы, новых CMake targets не требуется.
Первый полный compile обнаружил неверный override в CONTROL fake runtime;
owner исправил. Первый GUI прогон выявил отсутствие Qt Test DLL в PATH и stale
UI-oracles (graph-view-only menus, трёхуровневый catalog, отдельная preview card),
а также ожидание Apply до завершения reply. Environment/fixtures исправлены;
первый полный CTest: 77/98 PASS, 21 failures. Повтор после owner fixes:
13 из этих 21 PASS, остались 8 (CONTROL/remote и GUI). Portable PCM/DSP,
контракты, JSON и source sequence проходят. Выявлены и исправлены также
stale preview при повторном Apply идентичной схемы и narrow inspector.
CONTROL reply sizing проходит независимое review с owner correction.
Этот промежуточный прогон ещё не закрывал сценарий; актуальный итог ниже.

На первом FINAL continuation после восстановления user limits оба исполнителя возобновлены.
GUI owner scenarios PASS; SDK no-Qt/no-exceptions/no-RTTI build и 32/32 CTest PASS,
installed consumer build/run PASS, bounded synthetic CLI 25 ms PASS.
Network runtime PASS; remaining host/engine transport oracles и ordinary
320 KiB fake-IDF bootstrap тогда ожидали owner fixes. Stock DTO измерен 7257 bytes,
лимит 6144 непригоден. Acquisition allocation обязан покрывать 24 KiB
(dispatch16 + Converter8); transport allocation 16 KiB. Проверяется сокращение
фактических CONTROL sessions/replay owners, без изменения 320 KiB или oracle;
этот цикл закрыт актуальным evidence ниже.
Hardware acceptance по-прежнему не заявлена.

Актуальный evidence-блок после owner fixes: SDK final build и CTest **32/32 PASS**;
installed component build/CTest **6/6 PASS**, включая strict stock 320 KiB TCP/UART;
installed consumer rebuild/run exit 0, local/installed firmware main.cpp SHA256
совпадают. Стек acquisition 24 KiB и transport 16 KiB сохранён; actual unused
publisher assembly удалён, CONTROL observation не занимает session/replay slot.
Desktop byte transport, engine transport и numeric desktop host regression PASS
после would_block backpressure fix и independent source acceptance. Host scenario
с 60 DATA bindings/History занимает 18.85 s; прежний quick timeout 15 s исправлен
на bounded integration 60 s, assertions сохранены. Полный release build/CTest
**PASS: 98/98**, release build PASS. SDK build/32 CTest и installed component
6 CTest PASS; installed consumer exit 0. Синтетические CLI запуски по 1800 ms
с default 1/60 s и override 25 ms exit 0. Scope этого core-fixes блока принят;
это не закрытие всех V1–V4, native graphics или физического MCU acceptance.
Подробности и каталог logs: [отчёт исправлений](../core-fixes-final-2026-10-09.md).

Tests sources и запуск checks во время implementation не выполняются. User
config.json/output и старые evidence/build не заменяются и не удаляются.

### Независимый анализ ядра и новое требование — 2026-10-09

Дополнительный evidence scope после уточнения bypass/временного исполнения:
[deep PCM review](../core-pcm-deep-review-2026-10-09.md). Три направления завершили
read-only pass: четыре новых семейства дефектов воспроизведены на свежих portable
APIs; clock overflow — compiled arithmetic probe; receive coalescing — extracted
helper probe + source trace. Positive gather atomicity/UInt64 ownership probes
также сохранены. Bypass для несовпадающих input/output contracts пока не выбран.
Это review/probe completion, не implementation/integration/MCU acceptance.

Evidence scope: три параллельных read-only ревью контрактов/application,
runtime/DSP и desktop/embedded lifecycle/delivery завершены. Отчёты:
[первый анализ](../core-analysis-2026-10-09.md),
[непрерывность/cadence/архитектура/MCU](../core-continuous-architecture-review-2026-10-09.md).
Пользователь исключил public Start/Stop: core service живёт до shutdown,
analysis target настраивается в ядре, default 60. Исторические Stop/restart
PASS не подтверждают это новое требование. Найдены mismatch, условный PCM
coalescing defect и оставшиеся дефекты первого анализа. Implementation/fixes,
integrated acceptance и hardware 60 Hz этим блоком **не выполнены**.
Код анализируемого ядра этот чат не менял; внешний InputSelector scope не
принимается данным ревью. Native update_plan здесь отсутствует; это evidence
запись, не новый UI fallback и не обновление плана другого чата.

Снимок: **2026-10-08, Europe/Moscow**. Текущий scope и автономный процесс —
[implementation-plan](implementation-plan.md) и [vertical-delivery-plan](vertical-delivery-plan.md).
Это durable запись, которую пользователь не считает решением native UI.
Настоящий update_plan недоступен; Markdown UI fallback не предлагается и не открывается.

## Крупные этапы

### Новый процесс: один исполнитель, ручная UI-приёмка — 2026-10-08

Пользователь отменил параллельную оркестрацию для текущего scope. Один новый
чат с чистым контекстом исполняет [node-editor-rework](node-editor-rework.md):
роли/plot sinks, каталог/источники, dtype-политики связи, компактные настройки,
wheel policy и геометрия/визуальная перестановка портов. Он же собирает приложение.
Новые детальные UI-тесты не писать; базовые данные/processing проверять адресно.
UI acceptance требует подтверждения пользователя. Исторические bounded PASS
сохранены и не закрывают этот новый scope.

### Текущий приоритет: реальный Windows loopback — 2026-10-08

Уточнение scope: пользователь во время музыки видит unavailable/нули и не может
выбирать устройство. Новый снимок показывает unavailable также у корректного
PCM Float32 Vector[256] TestSignal. Разделить Stopped/Apply/source/preview причины.
Явно запрошен стартовый demo generator + loopback → selector 2→1 → DSP.
- [x] R: новый InputSelector 2→1; существующий Switch 1→N сохраняет смысл.
  Portable independent acceptance: a4d direct canonical dataflow_runtime_tests exit0,
  `review/run-loopback-final-dataflow_runtime_tests.log`, 8 receipt hashes stable.
  История/metadata/discontinuity, disabled typed silence, known disconnected clock,
  selected unavailable, inactive isolation и no hot allocations PASS.
- [x] U: выбор устройства/диагностика/selector UI, достоверный runtime/preview status.
- [x] P: coherent default demo и own-CoreApp переключение источников.

- [x] I: воспроизведение physical loopback, endpoint/init/start/callback/admission,
  адресное исправление и canonical core_app сборка.
- [x] a4d: независимое source/run review и ограниченная реальная capture-телеметрия.

Final bounded acceptance: common build exit0, `loopback-canonical-final-ctest.log`
20/20 PASS 93.81s. a4d independently repeated 11 assigned cases exit0;
59 receipt hashes unchanged before/after (`loopback-final-hashes.json`).
Existing report `review/desktop-review.md`, Independent final Windows loopback / startup selector acceptance.
Actual independent WASAPI48k/stereo: 221 callbacks/106080 frames/silence0,
nonzero PCM/FFT/Mel and finite Mel50 History on both selector branches,
generator continues; Stop/new-provider Restart PASS. Pinned endpoint also nonzero.
Core app SHA256 `7015E1B5BC261328A1FFCC78D5FD40E7A5639641D92ADD21CDCE91BD1D918CCD`.
Accessible explicit menu: Отображение → Пресеты → Генератор и loopback.
Current user app started separately from pwsh and autosaves live config;
no global config immutability claim, no overwrite/restore or OS input by workers.
Full97/backend/native OpenGL/OS gesture acceptance remains outside this closed scope.

Actual capture evidence I: `loopback-music-native.log` WASAPI Realtek 48k/stereo,
callbacks2 / frames960 / nonzero_samples480 / synthetic silence0.
`loopback-actual-selector.log`: same owned CoreApp real factory, input1 music
→ nonzero PCM/FFT/Mel, input0 generator, History ordinal74→138 retained,
generator12→19 continued, callbacks225 / frames108000 / synthetic silence0,
Stop/Restart PASS. Это подтверждённый прогон во время музыки пользователя,
без собственного playback и сохранения payload; extended final acceptance ещё OPEN.
P normal UI selector/preview/device Apply и failed inactive source scenario PASS;
U lifetime crash fixed by QObject-context QtNodes callback and guarded form handlers;
original reproduction retained and independently accepted in final gate above.

Пользователь сообщает, что реальный loopback-пример не работает. Предыдущий
Default PASS использовал synthetic capture и не закрывал physical Windows scope.
Проверять реальные endpoint, PCM rate/channels и доставку; silence отделять
от отказа запуска. Без OS cursor/screen capture, воспроизведения звука и записи
захваченных audio payload. Пользовательский config только read-only.

### Текущий приоритет: стартовые примеры и ручное соединение портов — 2026-10-08

- [x] U implementation: допустимые configured-node contracts, inspector и реальный
  QtNodes port press/move/release; недопустимое изменение не удаляет связи молча.
- [x] P implementation: каждый поставляемый стартовый пример валиден и выдаёт
  synthetic PCM → Window → FFT → Mel → History после Apply/Run.
- [x] I integration: целевые проверки и coherent canonical core_app сборка.
- [x] a4d independent acceptance: свежая проверка gesture/admission/preset delivery.

Final bounded gate: `startup-editor-final-build.log` 22/22 exit0,
`startup-editor-final-ctest.log` 15/15 PASS 41.48s. Независимый a4d повторил
пять canonical executable cases exit0, 29 source/binary hashes совпали до/после.
Evidence: `cmake-build-release/review/run-examples-final-*.log` и существующий
`review/desktop-review.md`, Independent supplied-examples / port-gesture / Save-vs-Apply final bounded acceptance.
Core app SHA256 `42DA7B0C84971F2744F544C30EF927E9B666F15084B95A58DB6EDD81A1A51044`.
Закрыт software scope примеров/ручных связей/admission/Save-vs-Apply;
physical loopback/native input/OpenGL/full backend gate этим не закрываются.

Предшествующий author/integration snapshot: `startup-editor-repair-build` exit0,
5/5 целевых CTest PASS 15.94s. Все четыре примера проверены через Run/Stop
того же CoreApplication с изменяющимися данными на всех 11 стадиях/view-входах,
History/count. Реальный port gesture, output1, отказ без потери edges/undo,
явное восстановление некорректного узла и запуск PASS. Реальные autosave timer
и autoApply OFF/ON, RU/EN, отказ с сохранением Running revision и очистка
предупреждения после восстановления PASS. Final gate закрыт evidence-блоком выше.
Фактический пользовательский config прочитан без записи: draft содержит
неподдерживаемый TestSignal Int64 Scalar и Converter scalar→Mel vector,
applied сохранён как корректная stereo PCM схема. Оба флага отсутствуют,
effective defaults autosave=true/autoApply=true; сохранение invalid draft
не означает успешное применение. Новый UI отклоняет недопустимые правки атомарно.

Новое сообщение пользователя: все примеры не работают, блоки не соединяются;
скриншот содержит TestSignal Int64 Scalar и Converter Int64 Scalar, waiting.
Причину конкретной конфигурации необходимо воспроизвести, не угадывать.
Предыдущая bounded docking-приёмка остаётся закрытой в своём scope и не доказывает
создание связей или запуск примеров. Пользовательские config/output не заменяются,
проверки выполняются app-owned событиями без ОС ввода/захвата экрана.

### Текущий приоритет: UI docking и открытие графиков — 2026-10-08

- [x] Implementation: G исправил floating → существующая группа вкладок,
  detach/reorder и открытие new tab/adjacent/external с сохранением instance/binding.
- [x] Integration: I собрал canonical Release; шесть целевых docking/UI сценариев PASS.
- [x] Independent acceptance: отдельный reviewer a4d проверил фактический
  press/move/release, topology, active tab, освобождение floating host и изоляцию внешних окон.

Приёмка ограничена app-owned Qt events/offscreen: независимые 6/6 exit0,
`cmake-build-release/review/run-ui-gesture-final-*.log`; единый отчёт
`cmake-build-release/review/desktop-review.md`, раздел Independent UI docking final bounded acceptance.
Source/binary manifest совпал до/после. Canonical core_app SHA256
`6BF9BD2639EC57A017B85961D41C0ACDC5A227D1C45705F32B0D0CEE06F126E2`.
SaveSettings → restart → Apply сохраняет группу и оба варианта активной вкладки.
Нативное Windows-перетаскивание/физический тачпад не объявляются проверенными;
общая backend-приёмка и создание/соединение узлов остаются отдельными открытыми scopes.

Подтверждён пробел прежней приёмки: успешный возврат floating проверялся
программным docking API, а не завершённым drag/drop. G/I/reviewer обнаружили,
что широкая верхняя drop-зона перехватывает tabbar и выбирает split.
Исторические bounded UI PASS не закрывают эту регрессию. Проверки выполняются
событиями приложения без захвата экрана или управления рабочим столом пользователя.
Остальные незавершённые backend-правки сохранены; их расширение и общий full gate
отложены на время текущего UI scope. Общая goal остаётся paused, конкретная
UI-работа возобновлена последним запросом пользователя.

### Активный этап: полная функциональная проверка и восстановление состояния — 2026-10-08

Root verified full diagnostic Release CTest: `functional-diagnostic-full-ctest.log`
— 85/88 PASS, 123.71 s, без исключённых targets. Остались38
numeric_history_provider_tests,48 numeric_desktop_host_tests,50
stream_transport_tests. Все остальные текущие UI/editor/core/codec/SDK targets
в этом составе прошли. Состав содержит temporary legacy ACK/trace для
причинной диагностики, поэтому это диагностический full gate, не финальный.
I назначена параллельная передача оставшихся дефектов владельцам, затем
restored final ACK/gap behavior, diag removal, stable build/full88/review/ledger.

New stronger Host warm24 actual RED: available20/24, missing117/119/121/123,
Running/warnings none. Reviewer9505 подтвердил source path потенциальной
потери ACK: sent без outstanding/retry, сервер ACK queue8 может отказать.
Причина конкретного20/24 требует trace; T/I внедряют one outstanding и
idempotent same-ID retry с точными epoch/ACK fences, actual run OPEN.
I local engine200feed/24ports тоже RED; output endpoint coalescing сохраняет
bounded/noalloc ownership, но independent provider-scope приёмка требует
gap/discontinuity semantics для downstream network/DSP, не только GUI latest200.
Эти gates добавлены к полному scope; final full88 ещё не подтверждён.

U independently accepted current bounded software package: a4d получил stable
I handoff, direct nav/editor exit0 со stable SHA; все focus/key/zoom/pan/grid,
camera/docmutation и cold History ordinal/ownership/wire predicates сохранены.
Evidence `review/run-u-final-editor_navigation_tests.log`,
`review/run-u-final-editor_tests.log`, тот же evolving desktop-review.md.
U/G implementation+integration+independent acceptance закрыты в их явно
перечисленных scopes; whole functional plan ещё OPEN по Host/full88/ledger.
I предложен диагностический full88 с известным Host RED после stable build,
чтобы другие неизвестные отказы не ждали закрытия одного fixture.

Correction independent9505/I/T: первоначальный Host remote24 RED был composite:
CONTROL ready не доказывает DATA ACK; 25 subscriptions могут длиться >125ms,
а старый fixture заканчивал feed за100ms. Поэтому тот RED сам по себе не
доказывает единственную причину Arena16/queue8. Их finite ownership/source
findings сохраняются. Новый oracle принимает реальные callbacks всех24,
удерживает24owners доGUIblock, затем проверяет all24 latest20/fresh21 с
монотонным sequence и fixed producer/generation. Source accepted независимым
reviewer; актуальное исполнение ещё ожидается. Не ослаблены blocked/pinning gates.

Root current log: `functional-fifo-grid-ctest.log` — 5/6 PASS, 18.01 s.
Engine TCP, network runtime, distributed transport, UART и editor navigation
PASS; numeric_desktop_host_tests ещё FAILED, T/I продолжают remote24/60 gate.
U final source frozen, навигация/cold-preview текущие бинарники переданы a4d
для независимой source/run приёмки. Общий full88 и final inventory/hashes
не закрываются этим целевым прогоном.

Latest I/T/U evidence: pointer-anchor zoom теперь PASS (измеренный delta3.08);
editor navigation дошёл до grid palette/contrast RED, U продолжает. TCP fixture
без observer проверяет actual callback readiness обоих source/sink ports;
sustained80KB воспроизводит A_sink0/B_sink62/source178available RED — startup
ожидание не закрывает дефект. T подготовил endpoint ordinary coalescing32 и
History8, pre-processing reserve N+97, accounting/reuse; independent source/run
приёмка новых очередей ожидается. Remote24 blocked и remote60 paced остаются
обязательными отдельными gates; 128 blocked fairness не выводится из лимита.

Root прочитал независимый current G packet в `review/desktop-review.md`:
7/7 direct scenarios exit0 со stable EXE SHA before/after. Приняты bounded
gui_views silent Run/zero views/lifecycle, workspace strict A→B→A/no implicit
Apply/disk restart, narrow+windows Light/Dark, adapter generation17 cold→compact,
settings/theme/registry и graph axes/retained renderer predicates. Первый
narrow loader refusal устранён настройкой существующего Qt PATH, бинарник
не изменялся. G scope independently accepted; U wheel anchor/editor и T
transport/ownership/full88/final ledger/hardware остаются отдельными OPEN.

Latest fresh Release: I подтвердил full build exit0, root прочитал
`functional-current-ui-ctest.log` — 4/5 PASS, 16.87 s. Current History demand,
gui_views реальный silent Run/reopen, workspace A→B→A и narrow inspector PASS.
Editor navigation FAILED: Wheel zoom loses pointer anchor (U). G передаёт
current executable hashes reviewer a4d; независимый fresh UI run ещё ожидается.
Transport regression current TCP delivery после изменения reserve ещё OPEN.
Reviewer upstream received queue8 finding передан I/T: bounded per-port latest
coalescing и честный resource admission, не выводить 128 fairness из константы.
Remote24 blocked+pinned и remote60 paced resubscribe+pinned gates обязательны;
полный current Release88 прогон пока не подтверждён.

Final SDK текущего снимка: root прочитал `cmake-build-sdk/functional-final-ctest.log`,
32/32 PASS, 8.42 s. Полная Release-сборка пока RED (Qt slots macro в новом
remote reserve и redeclaration в HistoryDemand fixture), владельцы исправляют;
не считать tail DLL deployment доказательством build exit0. Reviewer 9505
обнаружил high-count active rebind: новый arena не помещается рядом с текущим
даже без retained renderer owners; T готовит bounded reuse/recovery. Final
Release88 execution и независимая приёмка этих границ ещё OPEN.

Current I/G handoff: remote24-port regression воспроизвёл starvation
(`worker-remote-fairness-red-run.log`), T готовит ограниченный резерв до processing.
GDB workspace crash локализован: ADS visibility/render callback во время
частичного удаления docks при preset restore; G добавил restoring guard,
actual A→B→A rerun ожидается. Root прочитал I physical probe: WASAPI SystemOutput
48 kHz, 1331 кадров без ошибки (`functional-physical-capture.log`); device lifecycle
подтверждён, ненулевой звук/визуальная аппаратная приёмка не утверждаются.
Reviewer a4d сверил staged registration Release88/SDK32/component6 без дублей;
семь Release commands ожидают final build/JSON refresh, hash/run evidence OPEN.

Актуальный evidence, 2026-10-08: `functional-repair-ctest.log` — 7/14 PASS,
19.52 s. Прошли workflow/shutdown и все четыре transport-сценария; ещё семь
отказов: console_config, numeric_editor_action, editor_navigation, gui_views,
workspace, graph_settings и qt_workspace_resize_narrow. Исправления P/U/G
переданы единственному владельцу сборки I, повторное исполнение ожидается.
Первичная успешная сеть не закрывает весь worker scope: новая проверка
24 выбранных портов при заблокированном GUI выявила потерю доставки.
I сообщил исправление ограниченной очередью последних значений по портам
и author PASS; восстановление полного History после переполнения и независимая
проверка текущего бинарника ещё OPEN. R: дополнительные ring/restart/FFT
проверки интегрированы, SDK 2/2 PASS по отчёту I. Финальный полный прогон,
актуализация ledger/hash/registration и независимая приёмка остаются OPEN.

Последующая независимая проверка 9505: шесть direct targets PASS со стабильными
SHA256 до/после запуска; current host fairness24/latest20 подтверждён отдельно.
Четыре transport targets и worker WLED относятся к предыдущему host relink,
поэтому это bounded acceptance, а не единая финальная сборка. I воспроизвёл
History overflow и сообщил author PASS восстановления всех 64 строк/producer
с продолжением следующей компактной строкой. G выявил отдельный GUI-пробел:
полный cold History снимок отвергается адаптером; исправление в работе.
Также G исправляет чтение отсутствующего graph_views без вставки null,
мешавшей сохранить пресет. Current execution/independent acceptance этих
двух GUI-исправлений пока не подтверждены.

Latest cold projection run: `functional-cold-projection-ctest.log` — 2/8 PASS,
24.96 s. Numeric view adapter и Windows resize прошли; editor navigation,
reopen latest delivery, preset A topology, GUI external processor discovery,
narrow inspector и staged editor cold fixture ещё FAILED. Это новый состав,
не ухудшение сопоставимого полного набора; каждый отказ остаётся у владельца.
9505 независимо подтвердил current Host local+TCP remote full80/64 rows17..80,
producer701 и continuation81 (стабильный exe SHA before/after). Unchanged-producer
resync принят в этом bounded scope; producer/generation restart P1 ещё OPEN.

Повторный camera repair run: `functional-camera-repair-ctest.log` — 3/5 PASS,
4.23 s; numeric_editor_action, qt_workspace_resize_windows и editor_tests
прошли, editor_navigation и qt_workspace_resize_narrow ещё FAILED.
I отдельно сообщил текущий CLI PASS. Cold History callback подключается в
renderer/editor; финальная проверка views/workspace/settings ожидается.

Новый independent P1 9505: resync ordinal fence хранится по endpoint и не
учитывает смену producer_epoch/generation/discontinuity. После снимка ordinal80
источник с новым producer и Row1 может блокироваться до80 под прежним graph
epoch. I назначено исправление и local/remote restart regression до финального
прогона. Reviewer a4d отдельно оставил OPEN inline editor cold History preview:
нужны корректный ordinal/fence и подключение snapshot-входа, а не generic frame
preview. U/G/I согласуют это в текущем пакете. Общая приёмка не закрыта.

Текущий handoff: I сообщил local/remote baseline repair PASS, reviewer 9505
независимо подтвердил combined producer+generation+discontinuity restart;
изолированные переходы усилены и ждут нового исполнения. U подключил cold
preview и исправил minimum preview-card/port-contract labels. Reviewer a4d
прочитал navigation reorder и подтвердил сохранение применимых predicates
(focus/keys/text/held/hide-stop/camera); native reactivation не утверждается.
G исправил ADS toggle уже открытой панели и deferred exposure; source review
без новых замечаний. Фактический интегрированный UI-прогон и current88-target
ledger/hash/registration acceptance пока OPEN.

Следующий I handoff: UI stable repair 11 случаев — 6 PASS/5 FAIL.
Live TestSignal Apply, editor cold preview/action/adapter, Windows resize и
graph settings PASS. Workspace SIGSEGV остаётся у G; editor navigation,
narrow inspector и GUI reopen/demand проходят следующий цикл исправлений.
Изолированные resync discontinuity/generation/producer — author build/run0
(`worker-resync-boundaries-build2/run2`), reviewer повторная проверка ожидается.
Новый T/I ownership gate: remote arena16 может исчерпываться при GUI mailbox32;
нужны bounded fix и фактический >16 remote-port regression. Full scope OPEN.

Пользователь расширил scope: проверить все переключения и отображения, подключить
и адаптировать прежние актуальные тесты ядра и UI. Историческая приёмка ниже
не закрывает новые дефекты. Loopback — подтверждённый пользователем источник
неработающего захвата. Старый legacy-режим не восстанавливается.

- [ ] P/U/G: переключение пресетов восстанавливает графики, основные вкладки,
  presentation и расположение блоков; стандартная схема не требует ручного
  упорядочивания/центрирования, явные позиции сохраняются.
- [ ] R/I/G: редактирование работающей схемы и разрыв/восстановление связей
  сохраняют совместимое накопление и состояние; неизвестные/отключённые
  маршруты обрабатываются согласно контрактам, Explicit Stop проверяется отдельно.
- [ ] I: воспроизвести loopback preset → Apply → Run, исправить захват и
  проверить доставку; синтетический сигнал не подтверждает аппаратный звук.
- [ ] I/transport: проверить прежнюю распределённую обработку через registry,
  provider/source binding и три ядра; обнаруженное отсутствие автоматического
  связывания в NumericEngine остаётся OPEN до восстановления canonical-пути
  и фактического end-to-end прогона. Удаление старого API не отменяет функцию.
- [ ] U/P/G/R: инвентаризировать все прежние тесты своих зон, адаптировать и
  подключить актуальные UI/core сценарии; каждый исключённый obsolete тест
  объяснить. Регистрацию и общую сборку выполняет I.
- [ ] I: полный автоматический host/core/UI прогон после интеграции,
  рабочий канонический Release с DLL; ошибки исправлены либо явно открыты.
- [ ] Независимый проверяющий: новая приёмка актуального diff и фактических
  сценариев после сборки; замечания возвращены владельцам и перепроверены.
- [ ] Координатор: сверить требования, отчёты, результаты и ограничения;
  завершённость не выводится из авторского GREEN.

Работа назначена прежним отдельным чатам P/U/G/R/I автономными пакетами;
независимый reviewer готовит сценарии. Владение зонами сохраняется, общий
Ninja использует только I. Без пользовательского config/output, рабочего
стола, screen capture, системного ввода и перехвата фокуса. Implementation,
integration и independent acceptance пока не завершены.

Evidence K, 2026-10-08: подготовлены inventory/adaptation пяти старых codec
источников (`cmake-build-release/codec-test-inventory.md`), canonical проверки
обоих кодеков и сохранённый BDSP COBS-ZPE framing fixture. Пакет передан I
для регистрации и финального прогона. Это implementation-only handoff:
сборка, исполнение и независимая приёмка данного пакета пока не подтверждены.

Evidence I/R, 2026-10-08: composition-интеграция GUI/metadata/registry/router
в работе, CLI ещё не завершён. R воспроизвёл RED сохранения History при
совместимых изменениях Gate/window; исправление source clock в работе.
Совместимые amplitude/tone/sweep-правки TestSignal должны сохранять clock,
producer incarnation/epoch и downstream History. Изменения геометрии требуют
адресной проверки совместимости; Explicit Stop/смена устройства отдельны.
Общие Release/SDK прогоны и независимая приёмка остаются OPEN.

Goal audit 2026-10-08: фактический `rg --files tests` для cpp/cmake/py/CMakeLists
содержит 111 файлов; configured inventory 51 targets не является полным
source inventory. I собирает disposition всех источников с replacement
predicates и run evidence, независимый reviewer сверяет с деревом файлов.
Актуальные чаты подтверждены active. Новые findings: application admission
переполненного memory requirement (R), CLI неоднозначность ID/имени пресета
(P→I), очистка binding только основного вместо всех dock instances (G).
Canonical navigation/input/editor и codec/framing пакеты подготовлены;
итоговый build/run/independent acceptance ещё не подтверждены.

Inventory handoff: I создал `cmake-build-release/functional-test-inventory.json`
с 150 rg-visible путями tests/scripts/cmake и SHA256; dispositions пока pending.
Reviewer baseline `review/test-material-inventory-before.json` расширен до
167 материалов с CI/dependency/auxiliary references. Требуется reconciliation
множеств baseline∪current∪delivered fixtures, а не сравнение счётчиков.
U семь targets/P пакет source-ready; I добавил ещё не собранный regression
живых параметров TestSignal с сохранением phase/offset/fence и History.

Independent path-set evidence: reviewer сверил актуальный baseline167 и
I ledger168 (без duplicate paths): все baseline paths включены, дополнительный
путь — generated Python cache. Root прочитал текущий ledger: 168 dispositions
ещё `pending-owner-reconciliation`. Полнота перечня подтверждена для этого
снимка; актуальность, замены, регистрация, исполнение и итоговая приёмка OPEN.

P handoff прочитан root в `numeric-project-storage-handoff.md`, раздел
Current storage/preset inventory 2026-10-08: десять прежних sources имеют
конкретные retained/replacement predicates; presentation snapshot/adapter
metadata/CLI IDpriority пакет source-ready. Current application_lifecycle
уже canonical, регистрация обязательна; старое имя не означает obsolete.
R сообщил author PASS двух независимых FFT-окон и побитного сохранения History
при совместимых edits. Полный интегрированный прогон и независимая приёмка
этих изменений остаются OPEN.

Portable integration I: текущий SDK configure exit0/build exit0 (33 steps),
full SDK CTest 32/32 PASS 3.17s. Root прочитал configure/build logs, I передал
`cmake-build-sdk/functional-{configure,build,ctest}.log`; reviewer получил
стабильный portable handoff для независимых source/targeted execution проверок.
Installed consumer/component checks ещё в работе; desktop/общая приёмка OPEN.

Current desktop integration RED: root прочитал `functional-build.log` —
numeric_remote_endpoint.cpp использует incomplete QJsonArray, build остановлен;
desktop full CTest ещё не выполнен. Независимое source-review выявило слабые
oracles: codec optional reply subtree semantics и CLI ID/name precedence с
одинаковыми графами. K/P получают замечания; исправления и повторный review
обязательны до финального PASS, targets не отключать ради сборки.

Reviewer SDK snapshot: шесть независимых targeted executions PASS (lifecycle,
cadence, compatible History, runtime, framing, Native mapper). Source-review
дополнительно требует наблюдаемого возврата новых nonzero данных после reopen/
reconnect, не только роста/сохранения прежних строк; замечание R. P усиливает
CLI selection и SaveAs atomic refusal/retry, K optional reply semantics/ownership.
Это bounded acceptance прежнего SDK snapshot, не final после новых test edits.
Недостающий QJsonArray include исправлен T; I собирает product targets отдельно
от ещё изменяемых K/P test sources, затем полный test build/run.

K/P rereview handoff: замечания в тестовых источниках исправлены и переданы I.
K — full Reply optional semantics/ownership, complex proper-prefix/short-wire;
P — наблюдаемый ID/name selection и SaveAs malformed/future rollback+retry.
Execution/rereview этих новых assertions пока не подтверждены. Root прочитал
текущий inventory: классификация начата, 109 paths всё ещё pending; финальная
инвентаризация не завершена. Installed SDK consumer compile/link log прочитан,
отдельное runtime/exit evidence запрашивается при окончательном I handoff.

Independent rereview: K/P четыре source-level findings и R resume-nonzero
finding SOURCE ADDRESSED по frozen reread; actual refreshed execution PENDING.
Reviewer appended same desktop-review.md; старые шесть SDK результатов не
переписаны как проверки новых assertions. Desktop retry прошёл сетевые ошибки,
но RED на node_views.cpp incomplete SchemeEditorWidget; передано G для
адресного compile fix, полный desktop suite ещё не запускался.

Retained pipeline-test finding I: WLED policy/update/send привязаны к GUI
drain/timer, поэтому blocked GUI останавливает network output при работающем
DSP. Это функциональный gap OPEN. I исправляет desktop worker ownership,
T добавляет actual blocked-GUI UDP oracle. Portable executor не получает Qt,
thread/socket I/O; prepared callback ограничен bounded передачей состояния,
сетевой I/O выполняется вне engine lock. Требуется failing case и actual PASS.

Actual blocked-GUI WLED RED: build exit0/run exit1,
`wled-blocked-red-{build,run}.log` — nonzero UDP не получен без GUI event loop.
Обнаружен общий аналогичный gap taps/control/router/RemoteEndpoint timers:
весь desktop network composition должен жить независимо GUI. I реализует
worker-owned engine/network resources и owning queued EnginePort bridge;
T проверяет blocked-GUI UDP/DATA/TCP/control/registry routing. Ограниченный
WLED-only fix не закрывает исходное требование. Portable DSP не получает Qt/I/O.

I installed SDK handoff: внешний installed consumer configure/build/run exit0;
installed component build85 steps exit0 и шесть component checks PASS1.18s.
Логи `cmake-build-sdk/functional-component-*` и `functional-consumer-*`.
Desktop product compile продвинулся, test build RED на editor_navigation
QSize/QPointF shared auto declaration, исправление передано U. Новый canonical
numeric_shutdown_tests восстанавливает exception autosave enabled/disabled и
защиту pending position/workspace; actual execution ещё не подтверждено.

Independent GUI finding P2: renderer replacement early-return не передаёт
сохранённое dock.renderExposed новому renderer; скрытые instances могут рисовать,
visible GraphPanel timer может не стартовать. G получил адресный fix и hidden/
exposed replacement oracle. Второй reviewer Oct8 source audit T/R без новых
findings, execution текущих интегрированных binaries ожидается; прежняя
приёмка не закрывает worker repair/blocked-GUI WLED/network новые сценарии.

Ownership update: финальный functional-test-inventory.json передан P для
автономной classification/reconciliation всех пока pending paths; I временно
не пишет ledger и занимается worker/network integration/build. P согласует
handoff с I, затем возвращает frozen inventory для добавления фактических
run evidence и independent review. Один итоговый ledger сохраняется.

New worker source audit P1/P2: bounded reply queue теряет реальный completion
ID при overflow; уже popped data suffix может пережить reentrant Stop/Apply/
disconnect; старый reply snapshot может откатить более новое applied state.
I получил три finding и обязательные admission/correlation, callback-fence,
latest-state regression predicates. NumericDesktopHost пока UNBUILT; прежний
transport PASS не закрывает новые ownership/queue/GUI-independent сценарии.

Worker lifetime review: четвёртый finding — reentrant destruction из handler
может продолжить drain по freed Impl; требуются guard/local handler copies
и адресный regression. Payload/arena ownership и обычное worker teardown
по source audit корректны; это не behavioral acceptance. Root прочитал
worker-host-build.log: новая worker/routing библиотека скомпилирована и linked,
но полный app/test build и исполнение worker сценариев пока не подтверждены.

Worker rereview: четыре первоначальных finding structurally addressed.
Дополнительные OPEN observations — data overflow теряет terminal Unavailable
и сохраняет старое available, reentrant handler unset/rebind не отменяет
batch-local старый callback. I получил оба случая. Root прочитал
worker-integration-build.log: stream_transport/distributed_transport/
numeric_desktop_host targets compiled+linked; actual execution ещё ожидается.

Worker actual integration log root-read: host PASS0.67s, blocked-GUI WLED
PASS0.45s; distributed_transport RED3.15s на directory stream discovery.
Это 2/3, не полный PASS. T получил failure. Latest independent source rereview
закрыл все шесть host findings структурно, но fresh behavioral evidence для
последних deltas и registry/160-view blocked DATA ещё требуется.
P ledger теперь классифицирует все текущие paths; два applicability gaps
(mixed runtime/telemetry) остаются OPEN, R/I получили их адаптацию.

P classification: все 170 материалов получили disposition, проверены
JSON/unique paths/baseline∪current completeness. Independent semantic audit
назначен reviewer; run evidence и acceptance ещё pending. I зарегистрировал
новые bounded telemetry/performance и canonical LatestDelivery проверки для
найденных gaps. T объяснил discovery RED обязательным payload_version4,
добавил его только в fixture; exact64 predicates сохранены. Новые host terminal/
handler-generation и 160-frame blocked-GUI DATA assertions требуют fresh run.

Fresh worker-integration-ctest2 root-read: 4/5 PASS (performance telemetry,
runtime LatestDelivery, host queue/callback repairs, blocked-GUI WLED).
Distributed теперь проходит discovery, но RED6.26s на three-engine relay/fanout
exact64; T получил изменившийся failure. Numeric expectations не ослаблять.
Reviewer final inventory audit подтвердил set completeness, отмечает stale
dispositions по уже адаптированным runtime/telemetry/render cases для P refresh.
R получил прямой P chat ID и сверяет оставшиеся portable predicates.

P source-classification frozen handoff обратно I: 170 unique materials,
baseline167/current155 без omissions/duplicates/pending-owner dispositions.
Reported ledger SHA256 AE7B07D1B028A1F484543D52007BE77DFCABC597EA74A35C8F42BE5F0EE5415F.
P writes stopped; I теперь единственный writer final run evidence/source hash
refresh. Все run_evidence пока пустые, independent semantic audit и portable
mixed-runtime predicate reconciliation продолжаются. Classification не PASS.

SDK review-refresh root-read: 4/4 PASS0.48s новых K/R assertions на этом
бинарном снимке (`functional-review-refresh-ctest.log`). Последующие R follow-up
ring resize/repeated StopStart additions ещё требуют финального refresh.
GUI build56/56 log compiled+linked, I получил предложение независимого UI
handoff параллельно distributed debug. Final inventory audit подтвердил пути,
но отметил stale runtime/performance/axes mappings и 32 изменённых hash rows:
final generated registration/source hashes/run attachments остаются OPEN.

Actual UI integration root-read: functional-ui-build56/56 завершён;
functional-ui-ctest 14/24 PASS, 10 failures21.72s. Repairs распределены
автономными пакетами: U editoraction/navigation2; G gui_views/workspace/
graph_settings/narrow/windows5 плюс A→B→A topology oracle; P console1;
I desktop_workflow/shutdown2 и общий worker/composition/build. Exact semantic
wait вместо obsolete synchronous fixture предпосылки допустим, ожидания
поведения/числа/dock identities не ослаблять и targets не отключать.
Independent current config-manager PASS принял P2 SaveAs bounded storage;
CLI/P1 и общий UI/global gate остаются OPEN.

Relay diagnostic root-read: RB1178/RC1172 accepted, no rejects, fanout observed
exact9007199254740993; failing scalar oracle использовал query_numeric,
который поддерживает только full History. T заменил этот fixture path на
owning latest-one tap FrameProbe (worker mutex-safe), сохранив exact64/nonzero/
fences/offline expectations; full History queries unchanged. Independent
source rereview и fresh execution назначены, это не ранее объявленный PASS.
G A→B→A topology oracle source reviewed; reviewer требует revision baseline
до первого save/load для отсутствия implicit Apply, execution pending.

### Срочный активный фикс: открытие графиков — 2026-10-07

- [x] U/G/I: воспроизвести фактический QAction/context menu → endpoint → dock
  для открытия здесь, в новой вкладке, рядом и во внешнем окне.
- [x] Исправить доказанный разрыв действий/создания/binding/видимости; сохранить
  INPUT identity графиков-потребителей и OUTPUT identity источников/обработчиков.
- [x] I: Release rebuild и короткие адресные action/integration проверки.
- [x] Независимая приёмка изменённого пути открытия.

Рабочий стол, экран и системный ввод не используются. Пользовательские конфиги
и существующая раскладка сохраняются. Предыдущая приёмка не закрывает новый баг.

Root acceptance нового action scope: источник отказа до Apply — поиск только
в пустой runtime-схеме; теперь представление создаётся по editor draft без
неявного Apply/Run и получает authoritative binding после Apply. Явное новое
открытие не переиспользует уже открытые, в том числе неактивные, вкладки.
Исправлены eligibility и Input/Output identity для sinks/sources/processors.
Release build55 шагов exit0, четыре затронутых проверки PASS; финальный
numeric_graph_action_tests 1/1 PASS2.59s. Независимый проверяющий исполнил тот же
current canonical action fixture exit0 и проверил источник/условия; прочитаны
`review/desktop-review.md` и `review/run-numeric-graph-actions.log`.
Тест охватывает реальные QAction, здесь/вкладку/рядом/внешнее окно, повторное
открытие неактивной вкладки, endpoint binding после Apply, внешний host редактора,
сохранение Stopped до и после Apply. Экран/системный ввод не использовались.
Current core_app SHA256:
`4E678010D02C505AD314C6E13EAC5E867B3BD2575EE92C1030A787F6AFD2AB3A`.

### Текущий этап после рефакторинга: работоспособность и GUI — 2026-10-07

Предыдущая software приёмка остаётся историческим результатом своего scope.
Пользователь подтвердил запуск нового Release и сообщил новые дефекты; запуск
не подтверждает корректность первого пресета, обработки и редактирования.

- [x] I + прежние U/P: отсутствующий конфиг открывает стартовый TestSignal preset;
  узлы разложены и центрированы после готовности viewport. Явные настройки
  пользователя не перезаписываются.
- [x] G + прежний U: простые блоки, полные параметры в inspector, отдельные
  сохраняемые presentation-флаги «Показывать на блоке», синхронное управление.
- [x] R: проверить актуальный processing path и свои тесты; исправить
  воспроизводимые runtime ошибки, передать интегратору regression cases.
- [x] I: проверить Apply/Run/живую доставку synthetic frames в GUI и диагностировать
  capture. Аппаратный захват не считать доказанным синтетическим прогоном.
  Дополнение пользователя: разрешены короткие запуски GUI и необходимых сервисов;
  проверить локальное ядро и внешний synthetic CLI-core по TCP, фактические кадры
  и меняющиеся графики. Собственные тестовые процессы завершить после проверки.
  Последующее уточнение: без захвата рабочего стола и Computer Use/системного
  ввода; app-owned telemetry и offscreen renderer, изолированные конфиги и
  процессы, анализ одного отчёта. Новое управление ПК требует отдельного разрешения.
- [x] Владельцы + I: инвентаризация актуальных/отключённых/устаревших тестов,
  восстановление полезных new-system проверок, адресные regression tests,
  итоговый полный автоматический host-прогон и Release rebuild с DLL.
- [x] Независимая приёмка startup/flags/capture/transport и backend/delivery
  в перечисленном программном scope после готовности авторов.
- [ ] Ручное визуальное подтверждение расширения спектрограммы по времени
  на новой сборке. Не подменяется backend/offscreen telemetry.

Implementation, integration и independent acceptance отмечаются отдельно по
фактическим результатам. I — единственный владелец общего release build;
G/R — автономные назначенные рабочие чаты с согласованием прежних владельцев.
Принудительный межчатовой compact недоступен; владельцам передано требование
обновить краткий рабочий контекст до реализации, без фиктивного подтверждения
сжатия. Прежнее откладывание тестов отменено прямым указанием пользователя.

Evidence текущего блока: Release build прошёл; после исправлений целевые 12/12
PASS (8.74 s), полный автоматический host-набор 49/49 PASS (12.64 s), фактические
журналы `cmake-build-release/desktop-check-affected-fixed.log` и
`cmake-build-release/desktop-check-full-ctest.log` прочитаны координатором.
Пользователь подтвердил в открытом приложении: «графики меняются, все заработало,
я вижу работоспособность». Это подтверждает наблюдавшийся локальный сценарий;
внешний CLI-core и независимая приёмка нового блока пока не завершены.

Новый пользовательский дефект: «накопление в спектре не работает». G/R/I
проверяют фактическую цепочку accumulator → History delivery → spectrogram
renderer на изолированном запуске и телеметрии. Пользователь уточнил:
спектрограмма меняет Mel на одной временной позиции, но история по времени
визуально не растёт. Приоритет новых тестов — backend: источник → маршруты и
Splitter/Switch/Gate → накопление Window → FFT/Mel → History; численные
значения, размерности, порядок/счётчик кадров и сохранение предыдущих строк.
Новые тесты графиков пока не добавлять. При исправном ядре адресно исправить
передачу/отображение истории. Общая динамика Mel и прежние 49/49 не доказывают
этот сценарий.

**Root bounded acceptance — 2026-10-07:** прочитаны отчёты runtime,
интеграции и независимой приёмки. Дефект воспроизведён: ядро сохраняло 39×50
значений, но desktop delivery добавлял 108 ошибочных History-маркеров, сбрасывая
накопление. NumericEngine исключает общий hot delivery для Semantic::History;
компактные строки/lifecycle и полный cold query сохранены. Приложение пересобрано
(66 шагов, exit0), затронутые 5/5 PASS (2.38 s); Splitter/Switch 1/1 PASS
(0.04 s). Проверяющий независимо исполнил provider и route cases: рост 21→38
строк, старые значения сохранены, новые отличаются, generic/errors=0;
Splitter 1→8 и все Switch routes сохраняют exact UInt64, identity и stamps,
неактивные ветви получают типизированный ноль, hot allocations не растут.
Приняты именно эти software/backend/delivery границы, не визуальная приёмка.

Фактическая app-owned telemetry: 178 строк за 3 s, ordinal/filled=179,
gaps=0, generic markers=0. Offscreen projection assertion остался RED:
выбранная вкладка spectrogram имела renderExposed=0, поэтому отрисовка
пропускалась по штатному visibility guard. Этот результат не является
подтверждением native pixels/роста видимой временной оси. Проверки renderer
дальше не расширяются по указанию пользователя. Рабочий стол не захватывался.
Документы: `cmake-build-sdk/audit-runtime/report.md`,
`cmake-build-release/desktop-check-report.md`,
`cmake-build-release/review/desktop-review.md`.

- [x] Foundation — completed только в исторически принятом bounded scope.
- [x] V1 — completed: root final software acceptance, current integrated/independent evidence.
- [x] V2 — completed: root final software acceptance, current integrated/independent evidence.
- [x] V3 — completed: root final software acceptance, current integrated/independent evidence.
- [x] V4 — completed: root final software acceptance, current integrated/independent evidence.
- [x] FINAL — completed: root final software acceptance, current integrated/independent evidence.

Все этапы software overview completed по явной root acceptance. Hardware/manual пункты отдельно UNVERIFIED.

## Текущие блоки

| Блок | Implementation | Integration | Independent acceptance |
| --- | --- | --- | --- |
| V1 | P/U completed | Current desktop/storage/editor composition integrated | Accepted software scope |
| V2 | R completed | Compact runtime/ownership/Converter integrated | Accepted software scope |
| V3 | G/R completed | History/renderer/owning delivery/reconnect integrated | Accepted software scope |
| V4 | T/E/K completed | Compact default/control/DATA/persist/SDK integrated | Accepted host/fake/crosscompile software scope; board/manual unverified |

T: 01a116dd-2b72-7490-8472-6d2ce1e5b4bc; E: 01a116dd-3ca9-76c2-aed9-c05919486823;
G: 01a116dd-5177-7471-bb3b-bed85a373888. IDs фиксируют назначение, не completed.
Три FINAL reviews и latest compact amendments приняты root. Mandatory software scope completed; аппаратные ограничения сохранены.

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
## FINAL: completed software scope

- [x] Все canonical блоки подключены; actual desktop и SDK собраны I.
- [x] Новые tests и полезные retained oracles проверяют ten dtypes/rank0..2,
  exact64/PCM24, Converter policies, semantics/axes, runtime ownership/noalloc,
  checked memory/admission и atomic apply/save/revision/session.
- [x] BOTH codecs/new DATA, новый JSON/presets/projection, remote/device и GUI проверены
  согласованными финальными unit/integration/full checks.
- [x] Независимые architecture/runtime/protocol/storage/UI/device findings устранены.
- [x] Native/hardware/task-stack ограничения отражены без расширения host/offscreen proof.
- [x] Итоговый scope явно принят координатором по независимой evidence.

Legacy migrations/inverse/oldconfigs/oldprotocol/schema1 goldens и сохранение старых
режимов исключены. P/R legacy helper grant отменён. Промежуточные TDD,
characterization, unchanged oracle repeats и micro freeze/gates отменены.
Тесты адаптируются/пишутся на FINAL; obsolete legacy-only случаи удаляются узко
владельцами с I registration. Пользовательские config/output и архивы сохраняются.

## Исторически принятая bounded evidence

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



- [x] Bounded numeric CONTROL BOTHcodec mask:
  [independent report](../../build-gate-numeric-control-review/mask-rereview/review-report.md),
  P2 closed, 66 checks/0 failures, 11-file snapshot unchanged; принят координатором.
  Этот scope не закрывает новый Native fix или весь V4.
- V1 прежний [composition report](../../build-refactor-integration/v1-composition/integration-report.md)
  GREEN ограничен своим snapshot. Поздний
  [presets ctest](../../build-refactor-integration/v1-presets/ctest.log):
  actual build0, save_preset FAIL98 expected2/read1, rejected/config PASS2.
  P actual visual3 repair author-ready (RED → focused GREEN), I closure ACK;
  whole integration/acceptance pending. Исторические результаты не переписаны как новый PASS.
- Ранние runtime/history/codec/compatibility proofs остаются в исходных отчётах
  только в прежних границах; старый compatibility PASS не доказывает новый продукт.

## Обновление evidence

Владелец завершает весь блок автономно и передаёт один итоговый отчёт либо настоящий
блокер. Общие API/ownership вопросы решаются напрямую с владельцем без status spam.
Координатор обновляет крупные implementation/integration/acceptance по evidence,
сохраняет один обзорный in_progress и исторические bounded completed.
I единолично владеет shared Ninja/CMake и baseline-and-ownership.
Правила native tracking: [codex-plan-tracking](codex-plan-tracking.md).

Evidence R, 2026-10-08, core editing/retained backend source handoff:
`cmake-build-sdk/audit-runtime/report.md` and `backend-source-inventory.csv`
(104 source/script records, exact R retained/excluded predicates, other owner
reconciliation explicitly open). Fixed ancestor blanket reset, compatible History
continuity/sequence and preparation-port overflowed memory acceptance. Adapted five
old portable sources; seven final direct Windows current-source author tests PASS.
I registered five targets and reports SDK32/32 before final test-only additions;
final dataflow/control/resume predicate delta awaits I refresh. Independent reviewer
resume assertion gap fixed with finite positive and changing newest row oracle.
Implementation R stable; whole integration/independent acceptance remain OPEN.
No native update_plan available in R chat; no coordinator plan update claimed.

Evidence R mixed-runtime follow-up, 2026-10-08: P exact old predicate list reconciled.
Added canonical ring wrap/grow-shrink explicit reset/write position and ten real
8-tone DSP Stop/Start cycles to dataflow_runtime_tests; filled radix4 Hartley exact
DFT/DC normalization oracle in portable_dsp_tests. Both direct author build0/run0,
production unchanged. I receives two test-only refresh/hash requirements; P reports
no other known portable remainder. LatestDelivery/telemetry/async adapter remains I,
camera/chart remains G. Current integrated and independent semantic acceptance open.

Evidence R full88 provider fixture correction, 2026-10-08: failure originated in
Apply revision2 ordinal42→45 before Stop, sharedbad mislabelled it as Stop delivery.
Test now strictly validates within-revision ordinal+sequence, deliberately fences
oldqueuedpresentationrows while retaining ALL45coldHistoryrows exact, and separately
proves postStopRows/postStopScalars0 + unavailable cold query/newproducerrestart.
Author current test build0/run0; injected within-revision row10 loss still RED9→11.
No production change. I targeted refresh and independent semantic acceptance pending;
full88 baseline85/88 remains historical diagnostic, not overwritten as full PASS.
