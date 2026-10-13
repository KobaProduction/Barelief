# Формат проекта

[Конфигурация и хранение](../architecture/configuration-and-storage.md) · [Протокол](protocol.md)

Текущая desktop-граница принимает UTF-8 JSON: document **4**, graph **5**,
processing **4**. CONTROL **4** и DATA **5** описывают отдельные сетевые границы.
Автоматического перехода со старых редакций и downgrade нет.
Источник формата — [NumericProjectDocument](../../adapters/desktop/src/numeric_project_document.cpp)
и [domain DTO](../../domain/include/domain/processing_contracts.h).

## Структура

Минимальный проект с загружаемым пустым draft:

```json
{
  "schema_version": 4,
  "scene_graph": {
    "version": 5,
    "processing_version": 4,
    "graph_id": "main",
    "nodes": [],
    "connections": [],
    "configuration_bindings": []
  }
}
```

`scene_graph` — draft; необязательный `pipeline_graph` — отдельный applied
snapshot. Документ без scene допустим при создании, но не имеет загружаемого
draft. Структурное чтение не заменяет admission перед Apply; черновик может
оставаться семантически незавершённым.

Canonical-узел содержит `id`, `kind`, `enabled`, `parameters`, `inputs`,
`outputs`. Порт содержит `id` и owning-контракт; направление задаётся списком.
Связь использует `out_id`, `out_index`, `in_id`, `in_index` и необязательную
политику `conversion`. `configuration_bindings` отдельно связывает SampleClock
или FrequencyGrid с потребителем; эти связи не являются аудиопроводами.

`execution_interval` — сокращённая положительная дробь секунд с точными Int64
numerator и UInt64 denominator. Допустим интервал от 1 мс до 60 с; при отсутствии
поля используется **1/120 с**. Fs захвата и GUI FPS этим полем не задаются.

## Точные численные значения

Int64/UInt64 и floating-point literals не проходят через JSON double:

```json
{"dtype":"uint64","decimal":"18446744073709551615"}
{"dtype":"int64","decimal":"-9223372036854775808"}
{"dtype":"float32","bits":"7fc00001"}
{"dtype":"float64","bits":"8000000000000000"}
```

`decimal` — каноническая строка соответствующего диапазона; `bits` — 8 или
16 строчных hex-цифр IEEE-754. Поля взаимоисключающие. Хранение NaN или
отрицательного нуля не означает, что любой DSP-узел допускает такое значение.
Применимость определяют реальный контракт операции и её conversion policy.

## Presentation, адаптеры и пресеты

Позиции находятся в `scene_graph.nodes[].position`, UI-поля — в `presentation`.
`ui.editor_view` версии 1 хранит масштаб, центр и настройки геометрии редактора;
`ui.workspace`, `graph_views` и `graph_rendering` — размещение и вид графиков.
Язык, тема и autosave относятся к настройкам приложения. Поле
`ui.pipeline_auto_apply` сохраняется и читается форматом; текущий GUI
принудительно включает автоматическое применение валидных вычислительных
правок и скрывает ручную кнопку Apply. Значение `false` этого поля сейчас
не отключает применение. Чтение JSON само по себе не вызывает Apply;
startup-сценарий GUI отдельно отправляет локальный draft выбранному ядру,
в том числе удалённому TCP или serial. Принятие зависит от прав, ревизии,
поддержки metadata и admission; сохранение на удалённом устройстве требует
отдельного Persist, если ядро его поддерживает.

`nodes[].adapter` хранит зарегистрированные привязки: `provider_uuid`,
`stream_id`, `host`, `port`, `device_identity`, `remote_endpoint`.
Адрес порта данных задаётся `graph_id`, `node_id`, `direction`, `port_index`.
Частичный draft допустим; готовность маршрута проверяет routing-адаптер.
Эти поля не входят в processing-параметры CONTROL.

`ui.pipeline_presets` содержит записи версии 1 с уникальным непустым `id`,
`name`, `graph` и `view`. Необязательный `presentation` версии 1 хранит снимок
графиков и workspace; глобальные theme/language/autosave и transport в него
не входят. Перестановка и переименование пресета сохраняют identity.

`decode_numeric_editor_graph` принимает canonical5 и typed QtNodes visual5:
в visual snapshot узел содержит `internal-data.model-name` и строку
`internal-data.numeric_processing` с canonical typed node. ID и kind должны
совпадать. Документные `scene_graph`/`pipeline_graph` принимают canonical5.

## Отказы и сохранение

Reader ограничивает исходный файл 16 МиБ, проверяет редакции, дубликаты известных
ключей, диапазоны и зарегистрированные структуры. При ошибке результат чтения
не публикуется частично. Неизвестные расширения поддержанной редакции остаются
source-owned и сохраняются с точными raw-значениями; typed snapshot не является
полным writer исходного файла.

Save проверяет baseline и допустимость назначения, затем использует атомарную
замену `QSaveFile`. Future/malformed/неоднозначный документ и `SourceChanged`
требуют явного решения вызывающего приложения. API чтения, staging и записи
объявлен в [numeric_project_document.h](../../adapters/desktop/include/desktop/numeric_project_document.h).
