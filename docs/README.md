# Документация Qt Visualization

Начните с [карты архитектуры](architecture/README.md): она связывает переносимое
ядро, графический и консольный Qt-runtime, интерфейс и embedded-сборку.
Перед диаграммами объяснены [термины и роли](architecture/README.md#термины).
Страницы ниже объясняют текущее устройство и способы использования системы.

## Архитектура

| Раздел | Что объясняет |
| --- | --- |
| [Карта системы](architecture/README.md) | Общие границы и путь данных |
| [Переносимое ядро](architecture/portable-core.md) | Контракты, DSP, runtime и управление схемой |
| [Qt-слой исполнения](architecture/desktop-runtime.md) | Общий слой графического и консольного приложений, устройства и цикл исполнения |
| [Интерфейс](architecture/interface.md) | Редактор, рабочая область, presenter и renderer |
| [Конфигурация и хранение](architecture/configuration-and-storage.md) | Документ проекта, применение и сохранение состояния |
| [Embedded и обмен](architecture/embedded-and-exchange.md) | Общий SDK, платформенные адаптеры, CONTROL и DATA |

## Руководства и справочники

- [Сборка и запуск](reference/build-and-run.md).
- [Работа с интерфейсом](guides/interface.md) и [графиками](guides/graphs.md).
- [Runtime API](reference/runtime-api.md).
- [Формат проекта](reference/project-format.md).
- [Сетевой протокол](reference/protocol.md).
- [Embedded SDK](reference/embedded-sdk.md).

## Развитие проекта

[История](history/README.md) сохраняет основные изменения архитектуры,
а [уроки работы с агентами](engineering/agent-lessons.md) — повторяющиеся ошибки.
Открытая работа, будущие направления и доказательства приёмки находятся в
[issues](https://github.com/ArthurKoba/qt-visualization/issues).
