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

Для MinGW на Windows установленный LLD можно использовать вместо GNU ld,
сохраняя компилятор, ABI и проверки. Это локальная настройка CMake cache;
пути машины не добавляются в общий preset. Если `ld.lld.exe` доступен в PATH:

```powershell
$lldBin = (Split-Path (Get-Command ld.lld.exe).Source).Replace('\', '/')
$linkFlags = "-fuse-ld=lld -B`"$lldBin/`""
cmake --preset debug "-DCMAKE_EXE_LINKER_FLAGS_DEBUG=$linkFlags"
cmake --build --preset debug
```

Для возврата к GNU ld задайте `-DCMAKE_EXE_LINKER_FLAGS_DEBUG=`.
Замена линковщика требует повторной проверки собранных программ.

Обычные `release`, `debug` и `sdk` CTest presets выбирают короткие
`quick` unit/component/core/node проверки. Все тесты остаются
зарегистрированными; более широкие прогоны выбираются явно:

```powershell
ctest --preset release-kernel       # численные DSP kernels
ctest --preset release-node         # контракты и допуск узлов
ctest --preset release-integration  # взаимодействие подсистем, включая UI
ctest --preset release-ui           # UI primitives и широкие UI-сценарии
ctest --preset release-audit        # сбор машинных UI evidence
ctest --preset release-long         # длительные DSP/analysis/component проверки
ctest --preset release-all          # все зарегистрированные CTest
```

Для полного Debug/SDK прогона используйте `debug-all`/`sdk-all`.
`release-integration` и `release-ui` могут пересекаться; это выбор сценария,
а не разбиение полного набора. Opt-in native OpenGL/устройство probes
собираются и запускаются отдельно с собственным evidence; offscreen CTest
не подтверждает native renderer.
Короткий `plot_viewport_component_tests` проверяет только общий нормализованный
crop, pan/zoom, выбор осей и границы. Отображение явных координат/value series
в line/bars и жесты Qt Quick проверяются отдельными UI-сценариями.
Составные dataflow/continuity/single-stream, полные musical/numeric pipelines
и многорежимный cadence относятся к `release-integration`; небольшие проверки
DSP kernels, numeric contracts и отдельных узлов остаются в обычном preset.
Один build-каталог обслуживает один Ninja-процесс; параллельные агенты согласуют
сборку с интегратором. DLL и Qt plugins размещаются рядом с desktop targets
в `bin`; exe запускается оттуда.

`scripts/verify-desktop-runtime.ps1 -Preset release` проверяет PE imports
и наличие deployed DLL/plugins в соответствующем `bin`, без запуска приложения.
Для Debug задайте `-Preset debug`. Инструмент берётся из `-Objdump`, `OBJDUMP`,
`CMAKE_OBJDUMP` build cache или `PATH`. Эта проверка файлов не подтверждает
нативную отрисовку и работу устройств.

## Графическое приложение

```powershell
./cmake-build-release/bin/Barelief.exe --config ./cmake-build-release/bin/config.json
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


## Имя продукта, лицензии и пакеты

Пользовательское приложение — Barelief, исполняемый файл — `Barelief.exe`;
CMake target остаётся `core_app`. `VERSION.txt` задаёт версию приложения,
Windows VERSIONINFO и CPack. Разработчик и издатель — Koba Production,
репозиторий — `KobaProduction/Barelief`.
Имя рабочего каталога существующего checkout не определяет название продукта.
Публикация релиза выполняется отдельно.

CPack настроен для portable ZIP и Windows NSIS. Для NSIS требуется установленный
NSIS; установщик использует общий логотип, MIT и ярлык Barelief, не меняет PATH.
Windows-путь установки задаётся как `Koba Production\Barelief`: NSIS удаляет `/`
из `InstallDir`, склеивая имена каталогов. Ключ реестра установки задан отдельно
и остаётся `Koba Production/Barelief`, чтобы обновление находило прежнюю копию.
NSIS собирает единый пакет с быстрым zlib-сжатием. Deployment GUI сканирует
QML-импорты и включает необходимые модули. Для `cpack -G NSIS -B <каталог>`
выбирайте короткий выходной путь: NSIS ограничивает длину путей ресурсов Windows.
Пакеты собираются из install tree с Qt runtime и каталогом `licenses`, а не из
всей build-папки с пользовательскими конфигурациями и тестовыми EXE.
Создание архивов и установка выполняются только по отдельному запросу владельца.
Проверка сборки приложения не означает приёмку install/upgrade/uninstall.

### Linux и WSL

Для Ubuntu 24.04 CI использует Qt 6.10.2, Ninja, CMake 3.30+ и GCC.
Помимо development-пакетов OpenGL, PulseAudio, XKB и zlib, для упаковки нужны
`patchelf` и `dpkg-dev`; для изолированных GUI-проверок — `xvfb`, `xauth`,
`openbox`, шрифты DejaVu и XCB runtime dependencies. Список пакетов закреплён
в [workflow](../../.github/workflows/windows.yml).

В WSL используйте отдельную Linux-копию исходников: Windows и Linux не должны
делить CMake cache или каталог сборки. Задайте `QT_ROOT` для Linux Qt и выполните
`cmake --preset release`, затем `cmake --build --preset release`. Desktop FFT
capacity в preset равна 16384; `BUILD_GUI=OFF` отключает только GUI.

Тесты по умолчанию используют `offscreen`. Для оконных сценариев Linux задайте
`-DBARELIEF_TEST_QPA_PLATFORM=xcb` при configure и запускайте CTest через
`xvfb-run --auto-servernum` с отдельным Openbox. Тестовый guard разрешает X11
только при `BARELIEF_TEST_VIRTUAL_DISPLAY=1` и локальном номере дисплея не меньше
99; обычный WSLg `:0` не подходит. После создания QApplication проверяется также
фактически выбранная платформа Qt. Используются виртуальные аудиоустройства;
тестовые сигналы не воспроизводятся через динамики.

Linux GUI упаковывается командой:

```sh
cpack -G DEB --config cmake-build-release/CPackConfig.cmake -B cmake-build-release/packages
```

Пакет размещает приложение и Qt runtime под `/opt/barelief`. QML imports
определяются `qmlimportscanner`; необходимые модули и зависимости их plugins
попадают в пакет с относительными runtime paths. Отсутствующие необязательные
платформенные стили Qt не считаются обязательными imports. Системные зависимости
DEB вычисляются `dpkg-shlibdeps`, включая зависимости XCB plugins.

`scripts/verify-linux-package.py PACKAGE --evidence-dir DIRECTORY` запускается
в отдельном Xvfb с тем же явным guard. Он распаковывает DEB во временный каталог,
проверяет ELF dependencies без окружения Qt SDK, запускает упакованный GUI
с отдельным синтетическим консольным ядром и каноническим проектом
`tests/fixtures/linux-package.json` (восемь меняющихся тонов без аудиовыхода).
Счётчики обычных кадров, строк History и её snapshots сохраняются отдельно;
приёмка требует начатого sample и доставки хотя бы по одному из этих путей.
Это проверка извлечённого runtime; она не подтверждает upgrade/uninstall,
физические устройства или производительность нативной отрисовки.

Оформление About, разделение лицензий и ограниченная проверка GitHub Releases
адаптированы по подходу RedmiBook Manager. Автоматическая проверка раз в сутки,
каналы prerelease, загрузка/запуск установщика и подписание релизов в Barelief
пока не реализованы. Проверка обновлений доступна вручную из «О программе».
