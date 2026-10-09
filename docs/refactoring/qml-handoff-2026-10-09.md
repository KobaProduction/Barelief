# Сжатый контекст перед QML — 2026-10-09

Сводка подготовлена отдельным саб-агентом по текущим отчётам и исходникам.
В этом шаге реализации QML, сборок и повторных тестов не было.
Последнее указание пользователя: сначала сжать контекст агентам.

## Scope следующего этапа

Все представления графиков визуализации: Scalar → метрики;
Vector → line/points/bars/pie; Matrix → image2d/surface3d.
Редактор схемы QtNodes, DSP, накопление History, протокол и форматы документов
в перенос не входят. Реализацию следующего этапа пока не запускать.

## Принятая база

- `NodeParameterModel` не зависит от QWidget/QtNodes; `NodeParameterForm`
  создаёт Widgets, inspector и inline используют общую модель.
- `UiCommandRegistry` централизует QAction, сочетания и контекст команд.
- Directory model/discovery port отделены от AggregatorClient; конкретный
  клиент собирается desktop composition root.
- UI preferences сохраняются дельтой независимо от валидности draft и Apply;
  unknown fields сохраняются; preset не меняет геометрию главного окна.
- FieldRow/sections/errors/sliders пока являются Widgets-компонентами;
  непосредственно в QML не переиспользуются. Полные help-тексты ещё не написаны.
- Immutable GUI-owned matrix snapshot общий для adapter/image/surface/native;
  transpose — индексное представление, pause — frozen + один latest pending.

## Evidence, которое не нужно перепроверять без новой причины

Authoritative статусы и пути: верхние блоки
[progress-checklist](progress-checklist.md), а не только ранний UI separation.

| Блок | Принятый scope |
| --- | --- |
| UI separation | Release и полный host 100/100; независимое круговое ревью. `cmake-build-release/ui-separation-1791518587407`. |
| Shared matrix snapshot | Release, host 100/100, SDK 32/32. |
| History startup/resize | Release, host 100/100, SDK 32/32, installed component 6/6, внешний SDK consumer PASS. `%TEMP%/qt-history-startup-20261009-4e14d43af44f417aaf0bcf64c587ca4f`. |
| Spectrogram selectors | Release и полный host 100/100. `%TEMP%/qt-spectrogram-axes-20261009-65638428c71842709553c03950d46518`. |
| Последний native axes follow-up | Native OpenGL default graph/History400: шесть поз/дистанций камеры, точка и два среза, осмотр собственного framebuffer. Host 94/100, затем шесть исправленных проверок PASS и 12 fixture consumers PASS. Итог 100/100 — комбинированные результаты, не второй полный прогон. `%TEMP%/qt-native-axes-fix-20261009-ff219b2db3cf4106866f4f69dc75973f`. |

Нативная приёмка existing OpenGL axes не принимает будущий QML renderer.
Offscreen PASS не доказывает native pixels, hardware или FPS.

## Обязательные инварианты графиков

1. History предоставляет полную typed-zero capacity до первого ingress;
   occupancy/ordinal остаются нулевыми. Resize сохраняет свежие строки,
   обрезает/дополняет старый хвост без advancement. GUI отображает authoritative
   snapshot, не реконструирует padding и не накапливает свою History.
   Unavailable очищает stale изображение.
2. Rank определяет семейство renderer. Нет неявного flatten/scalar-to-series.
   Метрики имеют независимые stable input ports и точные строковые значения.
   Int64/UInt64 нельзя передавать через JavaScript Number с потерей точности.
3. Hertz/seconds берутся только из source metadata; Mel — реальные band centres.
   Недоступные координаты явно недоступны. Пока timestamps не покрывают всю
   History, используются frame positions. Temporal frame 0 — новейший.
   Generic matrix не приобретает ring-семантику.
4. `frequency_axis_mode`/`time_axis_mode` — presentation preferences, общие для
   image2d/surface3d. Restore и capability change не записывают preferences
   и не меняют sample owner/revision.
5. Grid divisions зависят от viewport/font metrics, не камеры. Камера меняет
   projection/visible walls, не tick values/policy/grid buffers. Crop/reverse
   согласованы для grid/labels/selection/slices; integral frames — integral.
6. Сохраняются amplitude autorange, pause/resume, theme/language, plane/channel,
   названия/visibility, exact diagnostics, bounded ownership и revision/device
   fences. Скрытие графика прекращает отрисовку, не DSP.

## Незавершённая граница и исходные точки

UI separation не завершил graph presentation boundary. `GraphPanel : QWidget`
возвращает concrete chart/spectrogram/image/pie/metrics. `NumericRenderTarget`
содержит concrete widget pointers; node_views/workspace используют casts.
Quick/Qml пока не подключены: renderer использует Charts/DataVisualization/
OpenGLWidgets/OpenGL. Это база для переноса, не готовый QML frontend.

- `presentation/qt/src/renderer/graph_panel.cpp`,
  `presentation/qt/include/core_app/graph_panel.h`: modes/preferences/settings.
- `presentation/qt/src/renderer/node_views.cpp`: endpoint/plane binding,
  metrics inputs, subscriptions, dispatch и composition.
- `presentation/qt/src/renderer/numeric_view_adapter.cpp`,
  `presentation/qt/include/core_app/numeric_view_adapter.h`: delivery/fences,
  HistoryPresentation, projection/cache и нынешний concrete target.
- `presentation/qt/include/visualization/numeric_matrix_snapshot.h`:
  immutable projection/shared transpose; не подмена source exact dtype.
- `presentation/qt/include/visualization/numeric_{image,pie,metrics}_view.h`,
  `presentation/qt/src/renderer/charts/`, `src/renderer/abstract/`,
  `src/renderer/spectrograms/` и соответствующие headers.
- `presentation/qt/include/visualization/{amplitude_range,axis_tick_labels,
  axis_control_widget,scene_aspect_control,surface_settings}.h`.
- `presentation/qt/src/presenter/gui_presenter.cpp`: abstract ports/fences.
- `presentation/qt/src/workspace/dock_workspace.cpp`, `main_window.cpp`:
  dock restoration/exposure/navigation.
- Переводы: `presentation/qt/i18n/ui_ru.ts`.

## Будущие блоки владения

- Общий bridge: render data/state/actions отдельно от GraphPanel и
  NumericRenderTarget; bounded snapshots и точные scalar strings.
- 2D/метрики: line/points/bars/pie/scalar/image2d, controls/readout/selection/
  ranges/labels через общий bridge.
- 3D: surface3d/camera/projection/grid/cache/point/slices через те же контракты;
  отдельная native приёмка.
- Координатор: общие API, Quick/Qml CMake/deployment/composition, dock wiring,
  Theme/language bridge, persistence, FINAL tests и независимое ревью.

Перед кодом каждый исполнитель полностью читает закреплённый
[Clean Architecture / DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md)
и актуальный AGENTS.md. Qt/QML остаются вне portable domain/application/runtime.
Только координатор запускает CMake/Ninja и меняет shared CMake/composition.
Не откатывать чужие изменения, не создавать ветки/коммиты/ZIP,
не подменять пользовательские config/output, не управлять рабочим столом.
Native update_plan в этом чате отсутствует; Markdown UI fallback не открывать.
Отдельный lineage follow-up unused direct `reset_history` не считать принятым:
фактические UI Apply/resize revision fences уже проверены в указанном scope.
