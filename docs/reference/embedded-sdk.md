# Подключение Embedded SDK

[Архитектура embedded](../architecture/embedded-and-exchange.md) · [Сборка проекта](build-and-run.md)

SDK предоставляет C++20-контракты данных, domain/application, `dsp_kernels`,
`dataflow_runtime`, `bdsp_protocol` и `embedded_support`. Сборка `sdk` обходится
без Qt и включает режим без exceptions/RTTI. Подготовка всё ещё использует
платформенный allocator; его поведение при исчерпании памяти определяет host.

## Host package

Из корня репозитория в настроенном compiler/CMake окружении:

```powershell
cmake --preset sdk
cmake --build --preset sdk
cmake --install cmake-build-sdk
```

Preset устанавливает package в `cmake-build-sdk/install`. Consumer указывает этот
prefix через `CMAKE_PREFIX_PATH`:

```cmake
find_package(PortableDSP CONFIG REQUIRED)
target_link_libraries(consumer PRIVATE
    PortableDSP::embedded_support
    PortableDSP::portable_numeric_control_session)
target_compile_features(consumer PRIVATE cxx_std_20)
```

`PORTABLE_DSP_MAX_FFT_SIZE` выбирает compiled capacity 512/1024/2048/4096;
активное окно схемы задаётся отдельно. ANSI backend включён по умолчанию.
`DSP_KERNELS_BACKEND=ESP_DSP` использует уже предоставленный внешний target
ESP-DSP; библиотека не устанавливает платформу или toolchain.

Install также размещает исходники, публичные заголовки, component manifests и
пример в `share/PortableDSP/embedded`. Прошивка компилирует эти исходники своей
toolchain: host static archive нельзя просто переложить в firmware.
Руководство в установленном пакете находится в `docs/reference/embedded-sdk.md`.
Ссылки из общей документации на исходники desktop/GUI рассчитаны на полный
checkout репозитория; source package содержит переносимую и embedded-части.

## ESP-IDF source components

Stock-пример — [examples/esp32_streamer](../../examples/esp32_streamer/CMakeLists.txt).
Собственная прошивка регистрирует в `EXTRA_COMPONENT_DIRS`:

```text
adapters/esp_idf/bdsp
adapters/esp_idf/portable_sdk
adapters/esp_idf/stream_adapter
adapters/esp_idf/persistence
```

BDSP предоставляется через `BDSP_SOURCE_DIR`; root FetchContent закрепляет
revision `2c68056dfaeffba2f34fc64eee9bc9e331e83c7c`. Пример по умолчанию ожидает
компонент `esp-dsp`. `PORTABLE_SDK_ESP_DSP=OFF` выбирает bundled ANSI backend.
Используйте уже настроенное совместимое ESP-IDF окружение:

```powershell
idf.py -C examples/esp32_streamer -DBDSP_SOURCE_DIR=C:/path/to/pinned/BDSP set-target esp32
idf.py -C examples/esp32_streamer -DBDSP_SOURCE_DIR=C:/path/to/pinned/BDSP menuconfig
idf.py -C examples/esp32_streamer -DBDSP_SOURCE_DIR=C:/path/to/pinned/BDSP build
```

`CONFIG_PORTABLE_SDK_MAX_FFT_SIZE` или CMake `PORTABLE_SDK_MAX_FFT_SIZE` ограничивает
FFT capacity; `CONFIG_DSP_MAX_FFT_SIZE` ESP-DSP должен её покрывать. Общие backend
tables нельзя переинициализировать или освобождать, пока существуют prepared plans.

## Library-only cross-build

Композиция [adapters/esp_idf/library](../../adapters/esp_idf/library/CMakeLists.txt)
использует native CMake v2 API ESP-IDF 6.1. Она собирает ESP32 SDK/BDSP archives и
опциональные transport/NVS adapters, без firmware ELF, bootloader и flashing.
ESP-IDF задаётся через `ESP_IDF_SOURCE_DIR` или `IDF_PATH`; compiler — через
активное окружение или `ESP32_TOOLCHAIN_DIR`. Композиция закрепляет ESP-DSP 1.8.2
revision `a53a0756833c045311ea1d79a2badf495cdfde4c`.

### Поиск существующего PlatformIO toolchain

В Windows корень пользователя получайте через `$env:USERPROFILE`; проверяйте
его `.platformio/tools` наряду с `.platformio/packages`. В частности,
компиляторы могут находиться в:

```text
.platformio/tools/toolchain-xtensa-esp-elf/bin
.platformio/tools/xtensa-esp-elf/<version>/xtensa-esp-elf/bin
```

`xtensa-esp32-elf-g++` предназначен для ESP32, `xtensa-esp32s3-elf-g++` — для S3;
современный общий `xtensa-esp-elf` toolchain предоставляет target-specific wrappers.
`xtensa-lx106-elf` предназначен для ESP8266 и не заменяет ESP32 compiler.
Проверяйте версию самого executable: имя package и активная версия могут отличаться.
Выбирайте версию, которую требует конкретный ESP-IDF, а не просто первый найденный
каталог. Library-only composition сейчас выбирает `esp32`; S3 задаётся отдельно
в firmware composition.

Перед новым setup прочитайте существующие локальные CMake caches: там могут быть
готовые `ESP32_TOOLCHAIN_DIR`, `ESP32_SDK_IDF_PATH`, `ESP32_SDK_PYTHON` и
`ESP32_IDF_TOOLS_PATH`. Сохранившийся IDF Python environment и constraint cache
можно использовать повторно; старые build-каталоги при этом не удаляются.
Эти параметры доступны root target `esp32_sdk` и задаются локально через
`cmake --preset release -D...`, окружение или `CMakeUserPresets.json`.
В общий preset абсолютные пути машины не добавляются.

```powershell
cmake -S adapters/esp_idf/library -B cmake-build-release/esp32-sdk -G Ninja
cmake --build cmake-build-release/esp32-sdk --target esp32_sdk
cmake --install cmake-build-release/esp32-sdk --prefix cmake-build-release/esp32-sdk/prefix
```

Из настроенного desktop build доступен target `esp32_sdk`:

```powershell
cmake --build --preset release --target esp32_sdk
```

Опции композиции: `PORTABLE_SDK_MAX_FFT_SIZE`, `PORTABLE_SDK_ESP_DSP`,
`PORTABLE_SDK_TRANSPORTS`, `PORTABLE_SDK_PERSISTENCE` и
`PORTABLE_SDK_EXAMPLE_ENTRYPOINT`. Последняя по умолчанию OFF; ON добавляет stock
entrypoint objects и требует transports/persistence, но не исполняет `app_main`.
Source component допускает FFT capacity512…16384 (степени двойки), default4096.
Внешний `CONFIG_DSP_MAX_FFT_SIZE` должен покрывать capacity. Большая capacity
не означает, что произвольный план поместится во внутреннюю RAM: проверяйте
workspace, caller buffers, общие таблицы и contiguous heap. PSRAM автоматически
не подключается. Extended FFT/GMF stand использует минимальные таблицы одного
маршрута; работающая многоплановая композиция сохраняет общие таблицы до teardown.
Install кладёт archives в `lib/esp32`, headers в `include`, сведения о конфигурации
в `share/PortableDSP/esp32`. Финальная прошивка должна иметь совместимые ABI,
IDF configuration, framework libraries и linker scripts.

При включении source components в собственную ESP-IDF firmware после
`include($ENV{IDF_PATH}/tools/cmake/project.cmake)` и до `project(...)` передайте
`idf_build_set_property(PORTABLE_SDK_ESP_DSP ${PORTABLE_SDK_ESP_DSP})`.
Это сообщает early dependency discovery, требуется ли внешний `esp-dsp`;
обычные CMake variables в раннем script-pass недоступны. Примеры streamer и
FFT benchmark выполняют этот шаг в composition root.

## Контракты собственной прошивки

Один `GraphExecutor`, `GraphLifecycle` и `ProcessingControlService` обслуживают
активную схему. `SyntheticPipeline::prepare_source` и `tick_source` дают только
source кадры; host передаёт их в `GraphExecutor::process_numeric_ports`.
Не запускайте рядом второй helper `prepare`/`tick`, который владеет собственным
prepared graph. Точные версии CONTROL4/DATA6 описаны в [протоколе](protocol.md).

[host_hooks.h](../../examples/esp32_streamer/main/host_hooks.h) задаёт точки расширения:

| Hook | Обязанность host |
| --- | --- |
| `streamer_host_setup` | Инициализация WiFi/I2S/Bluetooth и другой периферии |
| `streamer_host_memory_budget` | Доступный этой композиции бюджет после остальных владельцев |
| `streamer_host_acquire_socket` / `release_socket` | Неблокирующее получение соединения и закрытие host-owned descriptor |
| `streamer_host_read_pcm` | При `STREAMER_EXTERNAL_PCM` — Native planar PCM с настроенными Fs и block size; borrowed память жива до возврата processing |
| `streamer_host_report_processing`, `report_processing_errors`, `report_node_stats` | Приём измерений и bounded snapshot диагностики |
| `streamer_host_persistence` / `restore_processing` | Опциональное host-owned хранение вместо stock NVS |

ISR/capture callback заполняет ограниченную очередь и уведомляет task. DSP,
allocation и transport не выполняются в ISR. Нет готового универсального
I2S/Bluetooth driver: hook — граница интеграции устройства.

Stock Kconfig: UART1 TX17/RX16, 2 Mbaud 8N1; mono 8 кГц, 256 отсчётов, восемь
тестовых тонов; analysis default 120 Гц. Spectral mode добавляет FFT/Mel8,
History выключена. Memory ceiling — 320 KiB, CONTROL wire/arena — 4/8 KiB;
реальный доступный бюджет может быть меньше. Значения меняются через menuconfig.
Admission проверяет память и полную стоимость публикации до Prepare и binding.

## Проверка интеграции

`scripts/verify-portable-sdk.ps1` использует preset `sdk`, выполняет его сборку
и CTest, устанавливает пакет в `cmake-build-sdk/install`, затем собирает
consumer и установленный host component внутри `cmake-build-sdk`. Компиляторы
и Ninja для этих подпроектов берутся из cache основного SDK. Окружение и
локальные presets задают toolchain; параметры `CxxCompiler`, `CCompiler`
и `BdspSourceDirectory` допускают явный выбор без machine paths в репозитории.
Скрипт запускается только на этапе согласованных интеграционных проверок.

Host-тесты `embedded_numeric_persistence_tests`, `embedded_numeric_session_tests`
и `embedded_nvs_adapter_tests` проверяют программные границы. Harness
`tests/embedded_sdk_component` компилирует stock components с fake-IDF и принимает
`SDK_ROOT` для проверки установленного source tree. Их результаты не заменяют
Xtensa compilation, firmware link и запуск на плате. Фактические heap/stack,
timing, UART/WiFi throughput и устойчивость NVS к отключению питания измеряются
на выбранной аппаратной конфигурации.
