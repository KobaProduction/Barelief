# Формат числового проекта

Текущий проект — UTF-8 JSON с `schema_version: 2`. Графы используют
`version: 3` и `processing_version: 2`. Старые форматы не поддерживаются:
автоматической миграции, downgrade и восстановления legacy-схем нет.
Неизвестная версия или неоднозначное известное поле приводят к отказу загрузки.

## Документ, draft и applied

Минимальный документ с загружаемым пустым draft:

```json
{
  "schema_version": 2,
  "scene_graph": {
    "version": 3,
    "processing_version": 2,
    "graph_id": "main",
    "nodes": [],
    "connections": []
  }
}
```

`scene_graph` хранит редактируемый draft; `pipeline_graph`, если присутствует,
хранит отдельный applied snapshot в том же canonical-формате. Изменение или
сохранение draft не заменяет applied. Загрузка документа, пресета или applied
snapshot не выполняет Apply. Подготовку и запуск разрешает отдельная команда
приложения. Draft может быть семантически незавершённым; структурное чтение
JSON не заменяет проверку контрактов и ресурсов перед выполнением.

`read_numeric_draft` возвращает owning `NumericProcessingGraph`.
`read_numeric_applied` возвращает `optional<NumericProcessingGraph>`: отсутствие
секции успешно даёт пустой optional, ошибка сохраняет прежний результат.
В `NumericDraftPresentation.applied_graph` передаётся только явно сохраняемый
успешно применённый snapshot; отсутствие значения сохраняет прежнюю секцию.
Документ без `scene_graph` допустим при создании, но не имеет загружаемого draft.

При отсутствии локального config приложение создаёт новый TestSignal demo
через `numeric_test_graph` и `create_numeric_startup_project`: восемь меняющихся
тонов, canonical preset `builtin-test-signal` и включённые waveform/FFT/Mel/History
представления. Builder не пишет файл и не выполняет Apply. Отдельная startup-команда
приложения запускает это демо после сборки GUI, а редактор один раз раскладывает
узлы после готовности viewport. Этот режим не применяется к существующему,
повреждённому или remote/serial config; явно сохранённые настройки сохраняются.

## Узлы, связи и точные значения

Canonical-узел содержит `id`, `kind`, `enabled`, `parameters`, `inputs`, `outputs`.
Порты содержат стабильный `id` и owning контракт: dtype, форму, семантику,
представление, координаты и часы. Направление задаётся списком inputs/outputs.
Связь содержит `out_id`, `out_index`, `in_id`, `in_index`. Идентификаторы графа,
узлов и портов сохраняются; дубликаты известных идентификаторов отклоняются.
Изменение ID уже сохранённого графа требует явного создания нового проекта.

Int64/UInt64 и floating-point literals не проходят через JSON double:

```json
{"dtype":"uint64","decimal":"18446744073709551615"}
{"dtype":"int64","decimal":"-9223372036854775808"}
{"dtype":"float32","bits":"7fc00001"}
{"dtype":"float64","bits":"8000000000000000"}
```

`decimal` — каноническая десятичная строка соответствующего диапазона.
`bits` — 8 или 16 строчных hex-цифр, задающих точные IEEE-754 биты,
включая NaN payload и отрицательный ноль. Тег и payload должны соответствовать;
`decimal` и `bits` взаимоисключающие. Хранение битов не разрешает автоматически
их использование любым DSP-узлом: применимость определяют контракт и policy.
Остальные bounded integer-поля читаются с проверкой диапазона из raw JSON.
Числовые контракты поддерживают все десять dtype от Int8 до Float64.

## Layout, UI и пресеты

Позиция хранится в `scene_graph.nodes[].position: {x,y}`. Настройки находятся
в `core`, `ui`, `wled_ar_server`; editor view — в `ui.editor_view`
(`version: 1`, `scale`, `center_x`, `center_y`). Явные изменения передаются через
`NumericDraftPresentation` и `ConfigManager::numericSettingsDelta`.

`scene_graph.nodes[].presentation.inline_parameters` — массив стабильных UI-ключей
параметров, показываемых на блоке. Пустой массив явно скрывает их все. Эти флаги
передаются в `NumericDraftPresentation.node_presentations` по десятичным node ID;
они не входят в typed processing graph и CONTROL. В visual3 snapshot те же флаги
находятся в `internal-data.presentation` и сохраняются с clipboard/undo/пресетом.
Неизвестные поля presentation сохраняются отдельно от редактируемого списка;
дубликаты ключей и неверный тип известного списка отклоняются атомарно.

`ui.pipeline_presets` — массив записей с `version: 1`, обязательным уникальным
непустым строковым `id`, пользовательским `name`, snapshot `graph` и `view`.
Добавление, редактирование, перестановка и удаление согласуются по ID,
а не по имени или позиции в массиве.

Необязательный `preset.presentation` версии 1 хранит `graph_views`,
`graph_rendering`, `workspace` и восемь `show_*` флагов отображения. Это полный
снимок известных полей: пустой объект очищает прежнее состояние соответствующего
раздела. Глобальные theme/language/autosave и настройки транспорта в него не входят.
Отсутствующий снимок использует стандартное состояние UI; неверная или будущая
версия отклоняется до изменения графа. `decode_numeric_preset_presentation`
проверяет границу и возвращает owning UI-проекцию, сохраняя out при отказе.

Snapshot пресета может быть canonical3 или новым QtNodes visual3. В visual3
узел имеет `id`, `internal-data.model-name` и строку
`internal-data.numeric_processing` с точным typed node JSON; связи используют
`outNodeId`, `outPortIndex`, `inNodeId`, `inPortIndex`. Typed ID/kind должны
совпадать с visual ID/model-name. Смешение представлений отклоняется.
`decode_numeric_editor_graph` переводит оба snapshot-представления в owning DTO;
это отдельная граница адаптера. **Документные `scene_graph` и `pipeline_graph`
принимают только canonical3.**

Метаданные ApplicationAdapter находятся в `nodes[].adapter` canonical3 или
`nodes[].internal-data.adapter` visual3. Зарегистрированы `provider_uuid`,
`stream_id`, `host`, `port`, `device_identity` и `remote_endpoint`
(`graph_id`, `node_id`, `direction`, `port_index`). Частичный draft допустим;
готовность маршрута проверяет routing adapter. Строки непустые и ограничены
256 UTF-8 байтами, UUID валиден и ненулевой, port — 1..65535, node_id —
0..INT32_MAX, port_index — 0..65535, direction — `input` или `output`.
Эти поля не входят в ProcessingParameters, typed processing snapshot или CONTROL.
`NumericDraftPresentation.node_adapters` передаёт полный снимок известных полей
по десятичному node ID; `{}` очищает привязку. Неизвестные поля сохраняются
source-owned, включая расширения внутри remote_endpoint. Декодер
`decode_numeric_node_adapter` возвращает только зарегистрированную проекцию.

## Неизвестные поля и атомарная запись

`NumericProjectDocument.source` владеет исходным UTF-8 и token tree.
Неизвестные поля и точные raw-лексемы остаются source-owned, включая числа
больше точности double и не представимые в нём экспоненты. UI получает только
безопасную проекцию. Изменение известных полей сохраняет неизвестные поля
соответствующих сохранившихся записей; удаление записи удаляет её расширения.
Неизменённые безопасные значения UI также сохраняют исходные лексемы.
Неподдерживаемая замена opaque-структуры отклоняется атомарно. Это не обещание
байтовой идентичности всего JSON после любого редактирования.

`stage_numeric_draft`/`stage_numeric_presentation` готовят локальный кандидат.
`ConfigManager::saveNumeric`/`saveNumericAs` публикуют его только после успешного
`QSaveFile` commit; direct-write fallback отключён. Ошибка не меняет документ,
его disk baseline или активный путь. Файл ограничен 16 MiB.

Запись по активному пути сравнивает содержимое с точными байтами baseline.
Внешнее изменение или удаление дают `SourceChanged`; требуется явный reload
или Save As. Save As может заменить существующий поддерживаемый документ,
но не повреждённый, неоднозначный или future-format файл. Проверка перед commit
является best effort, не межпроцессным CAS. Явный путь имеет приоритет;
путь по умолчанию — `config.json` рядом с исполняемым файлом.
Отказ загрузки в приложении включает защиту исходного файла от последующей
автоматической записи; повреждённый источник не превращается в пустой проект.

## API и проверенные сценарии

- [Публичный storage API](../adapters/desktop/include/desktop/numeric_project_document.h)
  и [ConfigManager](../adapters/desktop/gui_config/include/core_app/config_manager.h).
- [Canonical storage fixtures](../tests/numeric_project_document_tests.cpp):
  exact64/float bits, неизвестные raw-поля, draft/applied, snapshot-декодер,
  пресеты, UI-векторы, SourceChanged и реальный отказ Windows replacement.
- [Реальное приложение](../tests/numeric_application_tests.cpp): первое и повторное
  сохранение пресета, загрузка/удаление, Save As, restart, точные значения,
  отдельный applied и защищённый отказ startup.

Эти FINAL-сценарии прошли авторские и независимые ограниченные проверки
2026-10-07. Приложение проверялось с silent synthetic capture и установленным
Qt offscreen plugin. Результат не доказывает нативные стили или 3D-отрисовку.
Добавленные 2026-10-08 проверки presentation и adapter — отдельный текущий scope;
исторический PASS их не подтверждает.
Подробности доказательств — в
[storage handoff](refactoring/numeric-project-storage-handoff.md).
