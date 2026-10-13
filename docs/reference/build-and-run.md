# Сборка и запуск

[Документация](../README.md) · [Qt-слой исполнения](../architecture/desktop-runtime.md) · [Embedded SDK](embedded-sdk.md)

Команды выполняются из корня репозитория. Требуются CMake 3.30+, Ninja,
совместимый C/C++ compiler и Qt 6.7+ для desktop. Qt/compiler задаются окружением
или локальным `CMakeUserPresets.json`; `QT_ROOT` используется общим preset.
GUI дополнительно использует Widgets, Qml, Quick, QuickWidgets и SerialPort.
Точные зависимости задаёт [CMakeLists.txt](../../CMakeLists.txt).

## Presets

| Preset | Результат | Каталог |
| --- | --- | --- |
| `release` | Desktop Release | `cmake-build-release` |
| `debug` | Desktop Debug | `cmake-build-debug` |
| `sdk` | Portable SDK, без Qt, exceptions и RTTI | `cmake-build-sdk` |

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Для Debug или SDK заменить имя preset. SDK устанавливается командой
`cmake --install cmake-build-sdk` в `cmake-build-sdk/install`.
Один build-каталог обслуживает один Ninja-процесс; параллельные агенты согласуют
сборку с интегратором. DLL и Qt plugins размещаются рядом с desktop targets
в `bin`; exe запускается оттуда.

## Графическое приложение

```powershell
./cmake-build-release/bin/core_app.exe --config ./cmake-build-release/bin/config.json
```

`-c`/`--config` выбирает файл. Без пути GUI использует `config.json` рядом с exe.
`--remote-host HOST --remote-port PORT` выбирает отдельное ядро, default port
8213. Подключение к ядру по serial выбирается общими serial-флагами ниже; одновременный выбор
remote host и serial override отвергается.

GUI работает как управляющий клиент: при startup пытается Apply локального
редакторского draft к выбранному локальному или удалённому ядру (TCP/serial). Валидные
вычислительные правки применяются автоматически; кнопка Apply скрыта, а поле
`ui.pipeline_auto_apply` сейчас не отключает применение. Удалённый запрос может
быть отклонён из-за read-only режима, конфликта ревизии, adapter metadata или
admission. Успешный Apply меняет рабочую схему; Persist её на устройстве —
отдельная возможность ядра.

`--help` и `--version` печатаются в терминал. Обычный запуск GUI требует
окружения без тестовых переопределений `QT_QPA_PLATFORM` и `QT_PLUGIN_PATH`.

## Консольное Qt-приложение

```powershell
./cmake-build-release/bin/pipeline_cli.exe --config ./cmake-build-release/bin/config.json
./cmake-build-release/bin/pipeline_cli.exe --synthetic --duration 3000
./cmake-build-release/bin/pipeline_cli.exe --synthetic --server --allow-mutations
```

`--synthetic` подаёт восемь изменяющихся тестовых тонов внутри обработки, без
воспроизведения через динамики. Не задавайте проект с внешними выходами для
изолированной проверки. CLI без `--config` строит стандартную схему; если
`--synthetic` не выбран, используется физический источник этой схемы.

| Флаг CLI | Поведение |
| --- | --- |
| `--config FILE` | Читает canonical-проект; неподдержанный/невалидный файл даёт отказ |
| `--preset ID/NAME` | Требует config: сначала точный stable ID, затем единственное совпадение имени |
| `--duration MS` | Завершает после MS; 0 означает непрерывную работу |
| `--analysis-rate N[/D]` | Положительная частота анализа, например `120` или `30000/1001` |
| `--analysis-interval-ms MS` | Интервал 1…60000 мс; несовместим с `--analysis-rate` |
| `--server` | Открывает CONTROL/DATA server только для локального engine |
| `--listen ADDRESS`, `--port PORT` | Адрес и порт сервера ядра; defaults `127.0.0.1`, `8213` |
| `--allow-mutations` | Разрешает удалённые processing-команды server |
| `--remote HOST` | Подключается к удалённому ядру и явно отправляет выбранную схему на Apply |
| `--registry-host ADDRESS`, `--registry-port PORT` | Явно подключает локальное ядро к реестру; default registry port `8212` |
| `--uuid UUID` | Задаёт ненулевую identity регистрации; без флага создаётся новая |

`--remote` является управляющим запуском: он не ограничивается просмотром
удалённой схемы. CLI возвращает 2 при ошибке аргументов/чтения и 3 при отказе
runtime/Apply; при bounded duration отсутствие успешного Apply тоже даёт 3.

Общие serial-флаги GUI и CLI: `--serial-port NAME`, `--serial-baud BAUD`
(default 115200), `--serial-input-node ID`. Они уточняют runtime-копию
`network.serial` и не переписывают файл. CLI не допускает одновременно
подключение к ядру по serial и `--remote`.

## Реестр обнаружения

```powershell
./cmake-build-release/bin/aggregator_server_app.exe run --port 8212
./cmake-build-release/bin/aggregator_client_app.exe run --server 127.0.0.1 --port 8212
```

Оба приложения требуют позиционную команду `run`. У server `-p`/`--port`
задаёт порт, `-t`/`--registration-timeout` — 0…60000 мс, default 1000;
0 отключает registration timeout. У client `-s`/`--server` задаёт IP-адрес,
`-p`/`--port` — порт реестра, `-u`/`--uuid` — identity,
`-t`/`--type` — `core`, `data_source`, `data_processor` или `data_sink`,
`-c`/`--component-port` — объявляемый порт компонента.
`--infinite-reconnect`, `--reconnect-tries`, `--reconnect-interval` и
`-w`/`--wait-for-server` задают политику подключения.
См. [server parser](../../apps/registry/main_server.cpp) и
[client parser](../../apps/registry/main_client.cpp).

Точная parser-граница: [CLI main](../../apps/desktop_core/main.cpp),
[GUI arguments](../../apps/desktop_gui/src/core_application.cpp),
[serial options](../../adapters/desktop/include/pipeline/serial_config.h).
Тестовые процессы используют отдельный config, ограниченное время и собственную
телеметрию. Offscreen PASS не подтверждает native renderer, физический захват
или производительность платы.
