# Узлы и редактор: один автономный исполнитель

Актуальный запрос пользователя — 2026-10-08. Этот scope заменяет параллельную
оркестрацию UI. Один новый чат реализует весь пакет, собирает приложение и
передаёт результат пользователю на ручную приёмку. Новые подробные UI-тесты
не писать. Координатор не реализует код и не запускает другие рабочие чаты.

## Обязательные правила

Прочитать AGENTS.md, docs/architecture-map.md и полностью
https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md.
Сначала проверить HEAD/status; сохранить все текущие незакоммиченные правки.
Не создавать ветки/коммиты/PR. Не изменять пользовательские config.json/output.
Не использовать OS input, экран, курсор или воспроизведение звука.
Qt presentation не выполняет DSP; переносимое ядро не зависит от Qt.
Стабильные node/port/dock IDs сохраняются. RU/EN, общая ui::Theme, общий стиль.

## Пакет реализации — от общего к частному

Статус на 2026-10-08: предыдущий результат не принят пользователем; corrective
implementation и integration выполнены в том же чате (evidence ниже).
Checkboxes сценариев оставлены открытыми до ручной приёмки пользователя;
авторская сборка и автоматические проверки не означают принятие GUI.

- [ ] **Роли и визуализация.** Source создаёт данные, processor преобразует,
  sink принимает. Только явные graph/view sinks открывают графики по клику,
  хоткеям и context menu. TestSignal, AudioSource, InputSelector и остальные
  processors не превращаются в графики. На них допустима компактная диагностика
  (состояние, принятые кадры, последние данные/время, параметры), без plot action.
  Проверить registry, renderer binding, меню и restore старого layout: не оставлять
  обходные пути создания графика по любому processing output.
- [ ] **Источники и каталог.** Два уровня группировки по роли и направлению:
  источники (аудио/генераторы/транспорт), обработчики (DSP/маршрутизация/накопление),
  получатели (графики/транспорт/устройства). Sidebar и контекстное меню используют
  один каталог. AudioSource — общий интерфейс с выбором loopback или микрофона
  и конкретного устройства; TestSignal отдельный источник. Транспорт назвать
  конкретно в UI: socket/TCP, UART по реально поддерживаемому адаптеру; сохранять
  стабильный NetworkInput ID, не создавать ложных UART-возможностей. Preview
  транспорта показывает состояние/приём/последний кадр, график подключается отдельно.
  WAV упомянут как будущая возможность: сейчас новую file-source функцию не вводить.
- [ ] **Числовые преобразования на связи.** Обычные числовые dtype-преобразования
  scalar/vector/matrix при неизменных semantic/rank/shape/clock/layout описать
  политикой связи и исполнять в runtime, а не скрытым DSP в UI. Поддержать
  проектные int/uint8/16/32/64 и float32/64 там, где их поддерживают kernels.
  byte — uint8 alias, int требует явно определённой ширины. Безопасная точная
  связь имеет понятный статус; потенциально lossy/narrowing преобразование
  требует видимой настройки политики и диагностики (диапазон, округление,
  переполнение, precision). Не молча clamp/wrap. Hover/настройки связи показывают
  вход/выход, преобразование и риск. Цвета только из общей темы, дополнить текстом.
  Scalar→vector, накопление→History, изменения смыслового типа/пакетные кодеки
  остаются явными processing nodes; PCM/Spectrum/Mel не совместимы по одной форме.
  Использовать общие контракты/существующие conversion kernels; не имитировать
  совместимость одним цветом без работающей связи и runtime. Проекция processing
  сохраняет необходимые параметры преобразования; UI-метаданные туда не уходят.
- [ ] **Понятные настройки.** Основные частые поля сверху, остальные в сворачиваемых
  разделах: источник/маршрут, обработка, контракт данных, дополнительные параметры,
  диагностика/отображение. У всех полей ясное название и единицы на RU/EN:
  scalar/vector/matrix/history не повторяются загадочными числовыми подписями.
  В блоке показывать только выбранные пользователем короткие параметры через
  общую настройку видимости, а не весь inspector. Reset не доминирует над настройками.
  InputSelector 2→1 представить очевидным переключателем входа с именами источников;
  Switch 1→N отдельно и с прежней семантикой.
- [ ] **Общие контролы и колесо.** Общий reusable control/event policy для combo,
  spin и других селекторов: прокрутка inspector не меняет значение при наведении,
  даже если control имеет фокус. Wheel передаётся scroll-контейнеру; явные клики,
  клавиатура и раскрытый выбор остаются рабочими. Не плодить фильтры на каждом поле.
- [ ] **Геометрия портов и блока.** Один порт по вертикальному центру доступной
  области, несколько равномерно распределены; симметричные стороны. Подпись
  порта компактными строками: semantic/name, dtype, shape/extent и нужные единицы,
  вместо длинной горизонтальной строки. Согласовать paint/layout/hit-test.
  Увеличить видимый порт и удобную область попадания; размеры/spacing/масштаб
  элементов редактора сделать настройками представления. Ctrl+drag переставляет
  визуальный порядок портов на своей стороне, сохраняет stable port ID и привязку
  соединений, не меняет обработку/selected_input. Порядок хранится в presentation.
- [ ] **Интеграция и передача.** Собрать canonical release/bin/core_app.exe в
  cmake-build-release с нужными DLL. Один исполнитель владеет и CMake/Ninja.
  При необходимости закрыть приложение перед заменой: пользователь ранее разрешил,
  кратко предупредить; config сохранять, не подменять тестовым. Быстро проверить
  базовые данные/совместимость, числовой converter, selector и History через
  существующие целевые core/integration проверки. Новые проверки только для
  существенной processing-логики, не подробные pixel/gesture UI тесты. Не гонять
  full97/старый UI-профиль каждый раз. UI принимается пользователем после сборки.

## Границы и итог

### Замечания ручной приёмки — 2026-10-08

Пользователь не принял предыдущий результат. Тот же единственный исполнитель:

- [ ] Связь Float32→UInt16 на новом снимке не выделяет риск преобразования.
  Нужен постоянный стиль exact/safe/lossy/failure из общей темы и фактической
  conversion policy, не только цвет. Наведение сразу даёт короткое приглашение
  к просмотру; выбор линии открывает source→destination/policy/rounding/overflow/
  precision/diagnostic в существующем инспекторе. Реальные последствия объяснить.
- [ ] Единое краткое форматирование портов: lowercase float32/uint16 и остальные
  стандартные dtype; число/массив256/матрица400×50 с понятной формой и единицами.
  Семантику PCM/Spectrum/Mel/History сохранить в контракте совместимости,
  сделать подписи понятными/компактными, полные подробности доступны в инспекторе.
  Не переименовывать wire/JSON IDs и не разрешать semantic conversion одним cast.

- [ ] Видимый основной переключатель на InputSelector с именами источников,
  выбранной ветвью и действительным selected_input. Не скрывать его в advanced.
  Не путать InputSelector 2→1, Switch 1→N и Splitter.
- [ ] Накопленные данные спектрограммы — processing результат History/2D;
  график спектрограммы — отдельный view sink без собственного накопления.
  Проверить настоящую доставку History port0 и renderer, не путать с count port1.
  На снимке связь уже идёт от накопителя: не менять правильную связь по догадке,
  устранить неясные названия/роли и проверить execution.
- [ ] Многовыходной preview не перекрывает строки и подписи; геометрия учитывает
  число портов, многострочные статусы и компактную диагностику.
- [ ] Сверить фактическую реализацию ВСЕХ исходных пунктов этого файла,
  исправить пропуски, собрать canonical release и передать человеку на приёмку.

Evidence пользователя: clipboard-780a8402-1c09-46c9-a06d-628b24a79a9e.png
и clipboard-00c69db9-21cf-46e0-953a-91efbe370528.png,
дополнение clipboard-da72b078-3335-4868-931e-5a71617e29f2.png
в C:/Users/Koba/AppData/Local/Temp.
Новых чатов/субагентов/независимой ревью-цепочки/детальных UI-тестов не создавать.

Исполнитель самостоятельно выбирает реализацию внутри пакета и ведёт этот checklist.
Не запускать субагентов, новые чаты или независимую ревью-оркестрацию.
Не расширять scope новой сетью/протоколом/долговременным хранилищем.
Работать крупными связными блоками; сообщать только существенные находки,
реальный блокер и итог. Вопросы отправлять координатору только при необходимом
решении, не ради подтверждения обычных обратимых правок.

Итог координатору: выполненные пункты, изменённые зоны, результат сборки,
краткие core проверки, оставшиеся ограничения и 5–8 действий для ручной приёмки.
Функциональность интерфейса не объявлять принятой до подтверждения пользователя.
Если нативный update_plan доступен — вести один крупный план. Если нет — честно
указать отсутствие, использовать этот один файл; не создавать дополнительные планы.

## Evidence предыдущей поставки — 2026-10-08

| Scope | Implementation / integration | Acceptance |
|---|---|---|
| Роли и графики | Registry role guard, editor actions, renderer creation/restore и workspace menus допускают только явные view sinks | Native click/hotkey/menu/restore — ручная приёмка pending |
| Каталог и источники | Общий role/direction tree для sidebar/context; AudioSource loopback/mic/device; NetworkInput подписан TCP; InputSelector отображает имена подключённых источников | Device capture и RU/EN UI — pending |
| Dtype связи | Переносимый admission и lowering в prepared Converter; четыре политики; ошибки доходят до sink diagnostics; source graph сохраняет публичные IDs; JSON/CBOR/Protobuf сохраняют conversion | Core regressions PASS; настройка/hover GUI — pending |
| Inspector и колесо | Сворачиваемые разделы, единицы, общее visibility menu; одна opt-in event policy для combo/spin, scroll forwarding | Native focus/popup/keyboard — pending |
| Геометрия | Общие layout/paint/hit-test, центральные/равномерные порты, многострочные labels, настройки вида; Ctrl+drag сохраняет presentation port order | Native drag/save/restore/theme — pending |
| Сборка и короткие проверки | `cmake --preset release` и release targets завершены с exit 0; текущий `cmake-build-release/bin/core_app.exe` и deployment DLL/plugins готовы; 8/8 CTest PASS, 2.93 s | Offscreen bounded evidence; native/hardware не проверены |

Сборка: `cmake-build-release/node-editor-rework-build.log` (последний incremental
product build). Проверки: `cmake-build-release/node-editor-rework-ctest.log`.
Восемь targets: `numeric_runtime_scenario_tests`, `dataflow_runtime_tests`,
`numeric_control_codec_tests`, `numeric_processing_contracts_tests`,
`numeric_project_document_tests`, `numeric_runtime_startup_tests`,
`numeric_application_first_run`, `numeric_editor_model_tests`.
Новые processing regressions проверяют exact/rejected precision, ties-to-even,
truncation, overflow rejection/saturation, scalar/vector/matrix/History conversions,
отсутствие hot-path allocations и утечки internal converter IDs в delivery.
Существующие модельные проверки адаптированы к общей настройке видимости и
dtype-совместимости; подробные pixel/gesture tests не добавлялись, full97 не запускался.

Ограничения: автоматические dtype-связи требуют Native/full-width представления
и неизменных semantic/rank/shape/clock/layout/normalization; кодеки, scaling и
смена смысла остаются явными nodes. Четыре edge modes: exact/reject,
nearest ties-to-even/reject overflow, toward-zero/reject overflow,
nearest ties-to-even/saturate overflow. Nonfinite отклоняются. TCP NetworkInput
не обещает UART node routing; существующие TCP/UART device endpoints сохранены.
WAV source не добавлен. Deployment предупреждает об отсутствующих dxcompiler/dxil;
исходный duplicate `Unspecified` translation warning сохранён. Это не доказательство
ошибки native renderer и не результат его приёмки.

Нативного `update_plan` в этом чате нет; общий план координатора не изменялся.
Новые ветки/коммиты/чаты/субагенты не создавались. Пользовательские config/output
не заменялись; пользовательское приложение и desktop/input не затрагивались.

## Ручная приёмка после сборки

1. Проверить click/Alt+G/context menu и восстановление layout: source/processors
   показывают диагностику; график открывают только явные view sinks.
2. Сравнить role/direction каталог sidebar и context menu в RU/EN, поиск по группе;
   проверить AudioSource loopback/mic, выбор и обновление списка устройств.
3. Подключить два источника к InputSelector, проверить названия входов и 2→1
   переключение; отдельно проверить прежнюю 1→N семантику Switch.
4. Прокрутить inspector над combo/spin с фокусом и без него: значение сохраняется,
   scroll работает; раскрытый popup, клики и клавиатура меняют выбор явно.
5. Соединить разные dtype при одинаковом контракте, прочитать hover и сменить
   conversion policy; проверить precision/range diagnostic. Несовместимые
   PCM/Spectrum/Mel и изменение rank/shape требуют явного processing node.
6. Проверить центр одного порта, spacing нескольких, hit area и настройки вида;
   Ctrl+drag переставляет порты на своей стороне, кабели и selected_input сохраняются;
   save/reload восстанавливает порядок и настройки.
7. Проверить Light/Dark/System, новые/disabled controls, открытый popup, inspector,
   dock separators, редактор и подписи OpenGL в обычном native запуске.

## Corrective evidence — 2026-10-08

Все три пользовательских снимка просмотрены. Отметки замечаний остаются открытыми
до user acceptance; таблица фиксирует выполненную реализацию и предел проверки.

| Проверенный пункт | Реальная реализация / bounded evidence | Что принимает человек |
|---|---|---|
| Роли и restore графиков | Повторно сверены `graph_available`, `is_graph_view`, renderer creation/restore и workspace menu guards; processor не становится plot. `SpectrogramView` и fixed dock названы «График спектрограммы» | Click/hotkey/context/layout restore native |
| Каталог и источники | Общий role/direction formatter остался единым; существующая проверка теперь рекурсивно сравнивает ВСЕ leaf kinds с каталогом (старое сравнение видело только пустые group IDs). AudioSource loopback/mic/device и TCP label сохранены | Real capture/device refresh и обе локали |
| Основное routing управление | InputSelector всегда показывает две кнопки с названиями источников, filled/empty маркером и pressed state; click пишет actual selected_input, undo синхронизирует обе кнопки. Switch/Gate primary controls и Splitter output count не требуют visibility opt-in; у Splitter нет exclusive выбора | Удобство native управления и routing end-to-end |
| Данные спектрограммы | Default example уже имеет `Spectrogram` History output0→`SpectrogramView` input0, count output1 отдельно. Domain History admission, prepared History callbacks к sink8, MainWindow input binding, adapter cold snapshot/compact row и renderer `update_frame_history` сверены. Existing startup/adapter checks проверяют 2D значения/порядок и продолжение History. Нового intermediary node нет | Native график из History, работа Apply/Stop на пользовательской схеме |
| Связи dtype | Общий connection painter: exact solid/quiet, exact widening dashed «↔ Тип», potential loss dash-dot «! Риск», observed error dotted «× Ошибка». Цвета QPalette, selection/hover halo общей темы. Immediate short invitation на hover, cleanup leave/drag/focus. Выбор связи открывает existing inspector: source→dest contracts, actual Edge.conversion, rounding/overflow/precision, диапазон и зарегистрированная ошибка. Policy edit/undo/processing metadata проверены | Native hover/click/zoom/contrast, реальный поток ошибок |
| Понятные формы / wheel | Primary поля видимы, редкие adapter/scaling/replacement поля перенесены в свернутые Additional settings. Убрана схлопывавшаяся до нуля label-wrapper; стандартные QLabel в общей форме. Общая wheel policy и visibility menu сохранены | Focus/popup/keyboard/scroll в native inspector |
| Компактные порты / geometry | Общий formatter: lowercase dtype, простые purpose captions, symbols + число/массив/матрица400×50; техническая semantic сохраняется в admission и раскрывается в inspector. Port0 History помечен «Данные спектрограммы», count — «Число кадров». Body height рассчитывается по фактическому height-for-width; queued LayoutRequest обновляет geometry, положение QWidget и связей. Inspector/inline field labels и 1/2/3-port Draft/Stop layout просмотрены в Light/Dark offscreen renderer | Native выбранный порядок портов, настройки размеров, Ctrl+drag и save/reload |
| Стабильные IDs и тема | Processing IDs/enum/wire/JSON имена не менялись; Ctrl reorder меняет только presentation arrays; editor settings сохраняются registered mapper. Custom node/edge painter использует актуальную QPalette. Light/Dark offscreen картинки проверены, System/native/hardware не заявляются | User native theme/renderer acceptance |

Дополнительно исправлены два lifetime дефекта, выявленных проверкой:

- Serializer числового контракта при encode больше не перенаправляет view на
  staging channels/clock, которые освобождались перед validate_contract. Decode
  сохраняет своё прежнее staging/retain поведение. Это устраняет случайный пустой
  snapshot при save/copy; malformed/future rejection и unknown fields сохранены.
- RU/EN callback combo теперь удерживает QPointer модели, сохраняя названия
  подключённых источников и прекращая доступ после удаления модели.

Canonical release `core_app.exe` и DLL/plugins собраны, exit 0.
`cmake-build-release/node-editor-corrective-build.log` — final product link/deploy.
`cmake-build-release/node-editor-corrective-ctest.log` — **10/10 PASS, 6.35 s**:
прежние восемь targets плюс `editor_tests` и `numeric_view_adapter_tests`.
После lifetime fixes `numeric_editor_model_tests` отдельно прошёл десять
последовательных запусков (4.49 s). До исправлений были bounded failures; они
не переписаны как native PASS. Короткие model/editor state checks проверяют
кнопку selected_input, edge risk/observed overflow, selected-edge inspector,
policy/undo и настоящий catalog leaf inventory. Новых подробных pixel/gesture
tests и full97 нет.

Temporary renderer probe/source/logs находятся только в `cmake-build-release`;
изображения `node-editor-layout-light.png` и `node-editor-layout-dark.png` показывают
конкретный bounded layout, не рабочий desktop и не hardware rendering acceptance.
Light snapshot предшествует последней правке QLabel; Dark snapshot включает её.
Дополнительный запуск заново собранного probe после final build не завершился;
остановлены только его собственные процессы. Нового PASS для этого запуска нет.
Обычный экземпляр приложения закрыт штатно перед заменой EXE; конфигурация
не подменялась тестовой. Новых веток/коммитов/чата/субагентов нет. Pinned DDD
в этой редакции уже прочитан, повторно не читался. Native update_plan по-прежнему
не предоставлен; этот файл — evidence, не Markdown UI fallback.

Ручной сценарий повторной приёмки:

1. Два PCM источника→InputSelector: сразу видны названия и выбранная кнопка;
   выбрать другую ветвь, Apply, проверить реальные данные и undo.
2. Проверить Switch 1→N и Splitter: main control подписан, значения меняются
   явно; Splitter раздаёт данные всем выходам без ложного выбора одного.
3. Mel/Spectrum→накопитель→History port0→График спектрограммы; count port1→Value
   graph отдельно. Apply/Stop/Start: History/graph и счётчик соответствуют смыслу.
4. Float32→uint16: «! Риск» виден постоянно; hover даёт приглашение сразу;
   click открывает правый inspector с range/rounding/overflow/precision и policy.
   Проверить rejection/rounding/saturation и undo политики на реальном потоке.
5. Сравнить exact/no-conversion и safe widening, selected/hover/failure styles,
   компактные lowercase/shape labels; семантически разные массивы не соединяются.
6. Прокрутка focused controls/popup/keyboard, rare collapsed fields, 1/2/3-port
   Draft/Stop layout, geometry settings и Ctrl visual reorder/save/reload.
7. Light/Dark/System, RU/EN, native graph axes/доки/контролы/меню и restore layout;
   plots только на explicit view sinks. Общая user acceptance остаётся pending.

### Corrective: точность и разметка осей, заполнение FFT dock

Снимок `ef156503-221d-4209-b11a-a5920cbd60ae` подтверждён кодом активного
OpenGL renderer: шесть Y ticks на 0..2 дают **0, 0.4, 0.8, 1.2, 1.6, 2**,
но глобальный `axis_precision=0` округлял их до повторяющихся целых. X ages
теряли дробную часть тем же способом. Новый общий `axisTickLabels` выбирает
точность по реальным ticks (включая нелинейные metadata coordinates), сохраняет
заданную точность как минимум, различает соседние деления и использует scientific
notation для малых/больших масштабов. RU/EN locale задаёт десятичный разделитель.
Он применяется к 3D X/Y/Z и легенде, а также к QValueAxis 2D графиков при изменении
range/tick count. Численные диапазоны, семантика и History ordering сохранены.

Разметка исправляет отдельное расхождение: padding рамки ошибочно растягивал
grid ticks относительно mesh/labels. Tick coordinates теперь соответствуют
реальному mesh, padding остаётся поперёк осей. Исправлен знак поворота при расчёте
screen-space bounds относительно glyph shader. Общий layout ограничивает подписи
viewport, сохраняет ближайшую привязку и пропускает конфликтующие промежуточные
labels вместо перемещения к чужому делению. Все три названия осей проходят общий
placement с проверкой занятых областей, включая легенду/diagnostic readout.

Важно: текущий 3D «Auto Y» — фиксированные 0..2 в `geometry()`/`y_range()`,
а не фактический min/max данных. Это явно проверено; DSP/gain/данные/камера,
manual bounds и auto range behavior этим пакетом не изменяются. 2D auto range
по фактическим данным продолжает работать как прежде.

Снимок `0665f893-b3ac-48b9-88dd-57e7a149177b` воспроизведён коротким
offscreen layout case существующего `qt_workspace_resize_tests`: после скрытия
Data Flow FFT dock вырос до 636 px, panel до 635 px, но chart/settings splitter
остался 372 px. Пустой `numericExactValue` QLabel, добавленный host к общему
QVBoxLayout GraphPanel с нулевыми stretch, получал лишнюю высоту. Исправлен общий
контейнер: splitter получает stretch=1, status row — естественную высоту.
Workspace splitter proportions, saved state, manual ranges и side ratio не
сбрасываются; новые fixed/max heights или центровка не добавлены.

Итоговые команды (из корня, release preset и настроенное локальное Qt/MinGW
окружение):

```text
cmake --build --preset release --target qt_workspace_resize_tests numeric_spectrogram_axis_tests spectrogram_geometry_tests --parallel 4
ctest --preset release -R '^(qt_workspace_resize_chart|numeric_spectrogram_axis_tests|spectrogram_geometry_tests)$' --output-on-failure
cmake --build --preset release --target core_app --parallel 4
```

**3/3 PASS, 4.04 s**. Pure axis checks используют фактические scene ticks,
metadata coordinates, малые/большие/signed значения, RU separator, заданную
точность, negative zero и недоступные координаты. Geometry checks сохраняют
reverse X/Y/Z/newest age0; короткий chart case проверяет resize, tab switch,
hide/show Data Flow, restore и floating→tab, chart/settings fill и малый 2D
range без изменения его границ. До stretch fix этот же case падал на hide
Data Flow с указанными выше фактическими размерами.

`cmake-build-release/axis-layout-ctest.log` — итог checks;
`axis-layout-checks-build.log` — их build; `chart-fill-before.log` — reproduce;
`axis-layout-release-build.log` — final canonical core_app link/deploy, exit 0.
Scoped whitespace check PASS (CRLF учтён). EXE/DLL/plugins находятся в
`cmake-build-release/bin`. Конфигурация/output не подменялись. Не добавлялись
новые docs, чаты, агенты, ветки, коммиты, full97 или подробная UI test chain.

**Нативная визуальная приёмка остаётся pending**: открыть тот же History и FFT,
сравнить реальные grid/tick labels всех осей, их названия и отступы при прежней
камере/проекции/размере; проверить заполнение FFT с settings и сохранённые
пропорции при переключении/скрытии/возврате dock. Offscreen layout и pure checks
не объявляются native GL PASS.

### Ручная приёмка 2026-10-09: повторные замечания остаются открытыми

Пользователь отверг предыдущую поставку. Снимки
`abd25bf1-6236-43cc-b263-58f4af5bf07f` и
`87be22e5-57af-4c04-bc94-5ee3f2bc1dfc` просмотрены: InputSelector обрезает
двухстрочные кнопки, preview status и значения; после выбора loopback видны
неактивный draft и пустой FFT. Надписи сами по себе не считались root cause.
Приёмка прежних осей/разметки/fill dock также остаётся открытой; старые author
PASS не означают user acceptance. Новых причин менять прежние диапазоны,
координаты, камеру, DSP, gain и splitter proportions по этим снимкам нет.

Root cause переключения: registered `selected_input` edit создавал обычный
draft; timer отправлял Apply только при `pipeline_auto_apply=true`. Disk autosave
не отправляет граф исполнителю. InputSelector теперь имеет live routing Apply
для работающей схемы, когда draft отличается от текущего графа только выбором
ветви и не содержит других processing/adapter edits. Применение использует
существующие presenter request/GraphLifecycle/PreparedGraph границы; undo/redo
проходит тот же путь, без Stop/Start и без включения общего auto-Apply/autosave.
Посторонний структурный draft не применяется неявно.

Root cause геометрии: обычная QPushButton geometry не обеспечивала высоту
wrapped многострочного содержимого, а обе preview columns имели `Ignored`
size policy, позволяя value column сжаться до нуля. Новый общий multiline button
использует native style/palette и height-for-width содержимого; общий embedded
height учитывает wrapping, minimum и size hints. Preview сохраняет два
измеряемых столбца. Размеры не заданы под один screenshot, zoom не меняется.

Целевая composition-root проверка дополнительно выявила дефект live capture
reconfigure: заменённый capture input сбрасывал sequence, но сохранял старую
producer_epoch вместе с callback token. Кадры нового счётчика отвергались как
старые. Первоначальная попытка исправить только нулевую epoch не закрыла этот
случай и повторный bounded run снова упал. При принятой live
реконфигурации новому потоку теперь назначается новая producer epoch, без
переоткрытия совместимых остальных источников и без глобального сброса History.

Latest live route intent сохраняется до принятого runtime snapshot; undo/другой
выбор при in-flight Apply согласуется с новым snapshot, а rejection и replacement
документа очищают intent. Embedded geometry также учитывает minimum width текста,
а не только preferred node width; изменение status/value вызывает перерасчёт.

Итоговые адресные проверки: **3/3 PASS, 22.59 s** —
`numeric_editor_model_tests`, `numeric_application_startup_selector`,
`numeric_application_startup_selector_unavailable`. После добавления явной
проверки Qt FFT container два composition-root cases отдельно **2/2 PASS,
21.28 s**. Они используют настоящий `CoreApplication`, отдельный temporary config,
реальные presenter/control session/GraphLifecycle/GraphExecutor/view adapter,
но **injected capture factory**, без hardware driver и speaker playback.

Проверены embedded button clicks при auto-Apply OFF и autosave OFF,
generator→loopback→undo(generator)→redo(loopback)→generator, новые runtime revisions
и Running state, различимые фактические PCM ветвей, delivery до FFT sink и
непустые `value_count`/`peak_value` Qt chart, реальные selector preview values.
History ordinal/sequence продолжаются через совместимые переключения; нет общего
сброса. При injected unavailable нет подмены данными: выбранная ветвь unavailable,
FFT container очищен, capture Failed содержит `Injected loopback unavailable`,
возврат generator восстанавливает FFT. Также проходит отдельный live capture
reconfigure/current source preview case, выявивший sequence/epoch дефект.
Temporary config не создаётся при autosave OFF — runtime Apply не принят за disk
autosave. Геометрический model case проверяет длинные RU/EN имена, wrapped button
height, видимые value columns, containment preview и QtNodes body bounding rect.
Это проверки state/geometry, не нативная визуальная приёмка и не FPS benchmark.

```text
cmake --build --preset release --target core_app numeric_application_tests numeric_editor_model_tests --parallel 4
ctest --preset release -R '^(numeric_editor_model_tests|numeric_application_startup_selector|numeric_application_startup_selector_unavailable)$' --output-on-failure
cmake --build --preset release --target numeric_application_tests --parallel 4
ctest --preset release -R '^(numeric_application_startup_selector|numeric_application_startup_selector_unavailable)$' --output-on-failure
```

Evidence в `cmake-build-release`: `selector-live-final-build.log` (canonical
core_app EXE/DLL/plugins build/deploy exit 0), `selector-live-final-ctest.log`,
`selector-chart-check-build.log`, `selector-chart-ctest.log`. Предшествующие
bounded failures не объявляются PASS. Scoped whitespace check PASS, CRLF учтён.
Пользовательское приложение закрывалось только штатно перед заменой; config.json
и output не подменялись, OS input/screens/audio и новые агенты/чаты/коммиты не
использовались. Прежние оси/fill dock tests не повторялись и их ручная приёмка
не закрыта.

Остаётся ручная проверка новой поставки: тот же working generator→реальный
loopback→generator с undo/redo и без ручного Apply; доступность/причина отказа
реального устройства; multiline controls/preview при пользовательском DPI/zoom,
состояниях и названиях; ранее открытые 3D grid/title layout и FFT dock fill.


## Current cohesive corrective package — author implementation/integration verified, acceptance OPEN

Native update_plan/forced compaction are unavailable in this chat. Current scope replaces the previous selector micro-fixes: (1) dynamic N→1 selector, stable retained ports and compact source-independent RU/EN controls; (2) embedded bodies without default diagnostic cards; (3) continuous processing with valid startup/live transactional edits, independent disk autosave; (4) compare old/new DSP and renderer, fix empty/zero surface, Y control and overlay causes; (5) canonical product build and a short targeted verification. Previous author PASS is historical evidence only. Native visual/hardware and user acceptance remain OPEN.


### Corrective delivery evidence (2026-10-09)

Implemented together: InputSelector accepts 1..8 inputs under the current editor admission limits, rather than fixed two; the canonical inputs array is the count and `selected_input` remains the stable zero-based field. All input contracts and the output must match exactly, with no implicit conversion inside the selector. The UI and physical port captions use short localized Input 1..N, with a combo and +/remove-last controls. Inspector exposes count and registered contracts. Adding/removing tail ports preserves retained IDs; selection clamps to the last retained input; undo restores removed connections and selected value. QtNodes would otherwise shift higher indices: the model restores original endpoints around structural updates. Compatible selector input-count changes preserve its logical output clock; route changes still mark discontinuity appropriately without resetting compatible History.

Default embedded bodies no longer create NodePreviewCard. Diagnostics remain available in inspector/details and subscriptions retain truthful failed/unavailable status. Opt-in parameter visibility remains. Source-name discovery and neighbor-dependent selector captions were removed; themes use the common palette and controls, with RU/EN labels.

Valid projects now start through composition-root transactional admission, including existing files, not only the first demonstration. Start/Stop buttons and the user auto-apply toggle are removed. Valid graph/adapter edits apply while processing runs, including reconciliation of newer edits during an outstanding request. Default/demo replacement also starts its admitted graph. Internal lifecycle stop/shutdown/error APIs remain. Old persisted `pipeline_auto_apply` stays readable but no longer disables the product's live processing policy. Autosave remains separate; the existing-config injected-unavailable case proves that runtime revisions can change without modifying the original file. Invalid drafts do not replace the admitted working graph.

Old/new findings: `b18cfbf` uses the same FFT magnitude `/N`, triangular Mel weights, Hann window and RMS merge conventions as the current numerical path. The earlier native Qt renderer at `93139d1` explicitly sets Y to 0..2 as well, so a universal historical 0..1 convention is not substantiated by these sources. No DSP gain, clamp, camera default, axis relabeling or history order reversal was introduced. Current history remains owned `[frame][band]`; explicit GUI transpose and genuine age/coordinate semantics remain.

Confirmed renderer defects: configured dimensions generated a zero mesh even with empty data; stale selection could print Value 0 over Waiting for data; the background floor/grid at Y=-1 shared the zero surface's depth. Empty data now produces no mesh, clears selection/slice state and suppresses readout; valid zero frames remain valid data. Background floor/grid is separated at -1.01 while wall signal ticks retain exact coordinates. Native artifact appearance still requires user acceptance.

Y Auto previously ignored data and always used 0..2. It now uses actual finite extents including zero; empty/all-zero fallback is 0..1, signed data retains its negative extent. Extents are cached at the GUI handoff. Manual intervals below 1e-6 were reset to 0..1 by settings normalization and flattened by the geometry denominator. Those absolute thresholds are removed in favor of distinct representable float endpoints; range-control steps follow the interval. Manual persisted settings and legend mapping remain aligned. Public display API, small manual interval, real range-control signals and settings restoration are tested; offscreen intentionally has no native 3D controls/container, so integrated native Y controls and GPU output are not claimed tested.

Final canonical build/deploy exit 0: `cmake-build-release/continuous-selector-accepted-build.log`; product `cmake-build-release/bin/core_app.exe`, 7,946,160 bytes, DLL/plugins deployed beside it. No user core_app process was present for replacement; no user process was terminated or launched. No OS capture/input, speaker playback, other chats/agents, branches/commits, config/output replacement, SDK/embedded or Linux acceptance was performed.

Final **8/8 PASS, 37.99 s** in `continuous-selector-final-ctest.log`: dataflow_runtime_tests, portable_dsp_tests, numeric_application_startup_selector, numeric_application_startup_selector_unavailable, numeric_editor_model_tests, numeric_view_adapter_tests, surface_settings_helpers_tests, numeric_spectrogram_axis_tests. Runtime checks include no hot allocations, third input, typed silence/unavailable and retained history bits/sequence. Composition-root checks use bounded isolated temporary projects and injected capture, exercise generator→capture→undo→redo→generator, N growth and selection, edits during an Apply, capture reconfigure, FFT sink/container, and actual History→renderer projection before GL. The existing-config case verifies automatic startup with old auto-apply=false and unavailable capture while generator/core continue; its disk file is unchanged. Retained DSP checks include DFT/DC normalization and Mel filter oracles. Model checks cover multi-tail shrink/undo, canonical save/load, localized physical labels and absence of embedded cards. A preceding 6/8 run is not accepted evidence: newly added tests assumed available History after an intentional outage and native controls in offscreen, causing one failure and one segfault. Corrected probes passed 2/2 before the final whole targeted set.

```text
cmake --build --preset release --target core_app numeric_application_tests numeric_editor_model_tests numeric_spectrogram_axis_tests dataflow_runtime_tests numeric_view_adapter_tests portable_dsp_tests surface_settings_helpers_tests --parallel 4
ctest --preset release -R '^(numeric_editor_model_tests|numeric_application_startup_selector|numeric_application_startup_selector_unavailable|numeric_spectrogram_axis_tests|dataflow_runtime_tests|numeric_view_adapter_tests|portable_dsp_tests|surface_settings_helpers_tests)$' --output-on-failure
```

Author implementation and desktop integration are verified in the stated scope. Independent/manual acceptance and the large visual regression remain **OPEN**. User acceptance must cover the native zero/empty surface and depth artifacts, Auto/Manual/reversed Y and persisted explicit settings, overlays at their DPI/camera, compact N selector controls and real loopback availability. This evidence does not close the coordinator's overall checklist or imply native OpenGL/hardware acceptance.


### 2026-10-09 — whole-node pins, per-input selector, full numeric path: integration pending

This package is **not ready / acceptance OPEN**. Geometry now uses the whole visible node center for each side independently, symmetric equal pitch, caption-height-aware vertical text centering and common painted button rectangles/hits. Selector inline combo and permanent +/- were replaced by themed per-input buttons and context menu add/arbitrary remove. Sparse stable PortId mapping was added at Qt slot/canonical DTO boundaries, connection restoration/undo, visual preset decoding and domain/runtime selector admission, including removal of original input zero. Remaining inputs and routes keep IDs. Targeted first probe passed numeric_editor_model_tests and both startup-selector cases; dataflow_runtime_tests failed on a fixture's duplicate ID15 introduced by default Bark restoration, subsequently corrected to16. That run is partial evidence, not package acceptance.

Full old route discovery corrects the preceding report's incomplete kernel-only comparison: baseline b18cfbf3c2ac62b4baf022de5348f5a29d46e2cc GraphProgram::defaults returns a version1 document canonicalized with Window/Hann and explicit BarkWeighting after every StereoFFT. New numeric default had omitted BarkWeighting. The explicit default/demo role has been restored as node15 without renumbering nodes1..14. Existing explicit user graphs/config files are not rewritten. Historical /N FFT normalization does not mean that each tone peak or Mel sum must equal1. Hann coherent gain is approximately0.49987793 at4096 samples; a resolved sinusoid's magnitude is roughly amplitude*gain/2 before weighting. Bark intentionally changes frequency-dependent amplitude; Mel sums weighted magnitudes, not PCM or squared power.

Independent historical PreparedGraph was compiled from git-show snapshots under cmake-build-release/baseline-b18cfbf, using the historical signal_contracts/domain/application/runtime/kernels sources. Vendor FFT backend was linked from the existing host library. Current canonical runtime was compiled into an isolated research runner before the later concurrent runtime edits. Both received identical eight moving tones in distinct stereo planes,8192 PCM samples with varying127/256/317/80 packet sizes,4096 Hann window,50 Mel bands and400 History capacity. Rates48k and44.1k exercise10/11 emitted windows respectively. No audio playback or hardware capture was used.

| Stage maximum,48k | Historical full route | Current compiled snapshot |
| --- | ---: | ---: |
| Window |0.303107142|0.303107142|
| Hann |0.302064329|0.302064329|
| FFT /N |0.0147538660|0.0147538669|
| Bark |0.375076473|0.375076503|
| RMS merge |0.375076443|0.375076503|
| Mel/History |1.12718666|1.12521803|

Window and Hann arrays are exactly equal for both rates. FFT maximum element error1.9e-9; Bark/merge<=6e-8. Mel differs by up to0.0134021341 at48k /0.009307161 at44.1k: the evolving DSP implementation uses continuous Hz filter boundaries; baseline rounded boundary locations to integer FFT bins. This is a distinct filter convention, not unexplained global amplitude loss. Without explicit Bark, the same historical test route peaks at0.0442855544 in Mel at48k and0.0436266325 at44.1k. Each executor's emitted History agrees exactly with its own Mel rows; the new full matrices retain oldest-to-newest chronology. Research fixture numbers are not the screenshot's generator waveform and do not establish native visual acceptance.

Evidence: numeric-path-comparison-48000.json, numeric-path-comparison-44100.json, numeric-history-chronology.json, numeric-path-snapshot-sha256.json, baseline/current-numeric-*.csv and baseline-unweighted-*.csv in cmake-build-release. Scripts and snapshot harness sources there preserve reproducibility. The historical capture adapter and current adapter both request float32 stereo with the same miniaudio0.11.25 revision/channel mixing; the new adapter can request the declared processing rate instead of always using native endpoint rate. Hardware/native endpoint behavior remains untested.

A bounded hidden native-FBO mode and opt-in exact GPU VBO readback check are implemented but **not built or executed** after discovery of parallel writes. They consume actual canonical History CSV, check band/age peak and full400x50 triangle count without desktop capture, OS input or showing a window. They are pending integration, not GL evidence.

Concurrency blocker: coordinator confirmed the user-owned chat ! Анализ проекта (01a11d67-ad2d-7ee3-84e0-c53a03704c78) has two active executors editing overlapping runtime/DSP/capture/lifecycle and editor files. The build pin-selector-integrated-build.log stopped on evolving EngineCommand enum/caller mismatch. The next source read already had the conflicting request branches removed without this chat changing them. This chat has stopped all writes to those APIs/models and shared Ninja builds until ownership or completion is resolved. Runtime apply/shutdown, desktop adapter and embedded wrapper integration edits plus teardown/re-admission test adaptations are preserved, not independently accepted. Remaining work includes API/fixture reconciliation, RU Remove input localization with its owner, retained relevant tests, actual canonical core_app/DLL/plugin build and native-FBO probe. No user core_app process was present at the process check; no user process was terminated. No branches, commits, ZIP, new chats/agents or user config/output replacement were performed. Overall checklist and independent/manual acceptance remain OPEN.


Independent capture-format probe (miniaudio0.11.25, MA_NO_DEVICE_IO, no hardware) exit0: signed16 values[-32768,-16384,0,16384,32767] become[-1,-0.5,0,0.5,0.999969482]. Stereo interleaved48k->48k retains every PCM float exactly (8192 frames; RMS0.0811839114, peak0.303107142). Default miniaudio44.1k->48k conversion consumes8192/produces8917 frames; this deliberately broad eight-tone fixture's RMS changes0.0812019488->0.0721095385, peak0.293031722. Thus capture-rate adaptation itself can attenuate higher tones and must not be treated as bit-identical to the old native-rate path or compensated by blind gain. The historical native44.1k and canonical declared48k comparisons above deliberately test both runtime clocks separately; they do not claim an end-to-end hardware/resampled equivalence. Source/log: cmake-build-release/capture-format-probe.cpp, capture-format-build.log, capture-format-probe.log. Full actual capture->runtime->GL on the integrated final code remains pending ownership resolution.


### 2026-10-09 — autonomous continuation, native upload evidence

Continued at the user's explicit instruction. Preserved evolving changes from the other chat and avoided a second shared Ninja while its final application build runs. Independent renderer-only build completed before that build. Fixed an additional sparse-selector bug in capability/resource estimation: history contract probing still dereferenced numeric port0; it now resolves an existing primary contract when input0 was removed. Reset selector settings preserves input/output topology and stable IDs. Added RU Remove input; owned model/GUI probes cover sparse visual/canonical round trips and actual per-pin Qt viewport mouse events (within isolated Qt tests, no OS input).

Hidden native FBO probe **build0 / run0 PASS**, native-path-probe-final.log: actual canonical snapshot History10x50, configured400x50, peak1.12521803 at age7/band41;117306 vertices; exact byte-for-byte GPU VBO readback. Native app-owned framebuffer saved as cmake-build-release/native-history-frame.png; no show(), user window activation/input or desktop capture. Default Qt surface format matches the product. A first attempt incorrectly requested Core Profile3.3 and failed shader compilation; that test override was removed. An intermediate image-export harness failed to compile due to a missing QFileInfo include, then compiled and passed after correction; only the final build/run pair is accepted. This verifies actual upload and tested indexed peak/topology counts, not the screenshot's mesh appearance, depth/shading or manual/native product acceptance. Those remain OPEN. Final integrated release and current-code targeted tests still pending the other owner's shared Ninja completion.

### 2026-10-09 — integrated author verification complete

Canonical release build exit0: pin-selector-accepted-build.log. core_app.exe is 7,980,665 bytes, SHA256 2715c9f786945563c5cf6153c7c71aea95fab76356e4e77d0f3ab65ada702078; deployed Qt DLLs and qwindows plugin hashed in pin-selector-product-manifest.json. Eight targeted current-system CTest cases PASS (23.37s), pin-selector-accepted-ctest.log: runtime, portable DSP, startup selector with both available/unavailable capture fixtures, editor model, view adapter, surface settings and spectrogram axes. This is not a full host-suite result or independent acceptance.

Two integration fixture defects fixed without weakening product admission: existing-config GUI fixture overlapped every node at (0,0), so actual per-pin mouse events hit another node; explicit distinct test positions now preserve the original-file-unchanged oracle. normalized_pcm_fft reused borrowed channel/clock metadata after replacing its owner; local channels/static clock domain now remain alive. detached_preparation_ownership supplied 1600 samples to a 256-sample fixture allocation; its declared/allocated ingress bound is now4096 and Pcm::send checks capacity before writing. Initial failed logs are retained. The final tests include new parallel-owner normalized PCM, coordinate, interval and detached ownership regressions.

Final-code old/current runtime comparison recompiled both probes successfully; all four runs exit0. 48k: Window/Hann arrays exact; FFT max error1.9e-9; Bark/Merge6e-8; Mel/History peaks1.12718666 vs1.12521803 (max element difference0.0134021341). 44.1k: Window/Hann exact; FFT1.9e-9; Bark/Merge<=6e-8; Mel/History peaks1.11056721 vs1.11161757 (max difference0.009307161). Continuous-Hz Mel boundaries explain the retained Mel difference. Every full History matrix equals the chronological concatenation of actual current Mel frames (10/11 rows), numeric-history-chronology.json. No blind gain, clamp or current-maximum normalization added; explicit user schemes/settings untouched.

Final hidden native FBO build/run exit0, native-path-probe-accepted.log: runtime History10x50, configured400x50, peak1.12521803 age7/band41,117306 vertices, exact GPU VBO bytes. App-owned native-history-frame.png remains an isolated renderer artifact; no desktop capture, show(), OS input, user-window activation, speaker playback or hardware-capture claim. Manual/native screenshot appearance and independent package acceptance remain OPEN. Overall coordinator checklist is not closed by this author result. Shared Ninja was released before probe execution.
